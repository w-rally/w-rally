#include "pipeline_cache.h"
#include "log.h"
#include "shaders_embedded.h"

#include <dxgi1_4.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

double NowMs()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

struct Fnv
{
    uint64_t h = 1469598103934665603ull;
    void Bytes(const void* p, size_t n)
    {
        for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const unsigned char*>(p)[i]) * 1099511628211ull;
    }
    template <class T> void Val(const T& v) { Bytes(&v, sizeof v); }
    void Code(const D3D12_SHADER_BYTECODE& c)
    {
        Val((uint64_t)c.BytecodeLength);
        if (c.pShaderBytecode) Bytes(c.pShaderBytecode, c.BytecodeLength);
    }
    void Str(const char* s)
    {
        if (s) Bytes(s, std::strlen(s) + 1);
        else Val(0);
    }
};

// The file: this header, then the library's serialised bytes.
struct FileHeader
{
    char     magic[8];             // "NGGPSO\0\0"
    uint32_t version;              // of this layout
    uint32_t vendor, device, subsys, revision;
    uint32_t pad;
    uint64_t driver;               // the user-mode driver's version (IDXGIAdapter::CheckInterfaceSupport)
    uint64_t shaderSet;            // embedded::kShaderSetHash
    uint64_t size;                 // of the library's bytes
    uint64_t dataHash;             // FNV-1a 64 of them: a damaged file is not handed to the driver
};
const char     kMagic[8] = { 'N', 'G', 'G', 'P', 'S', 'O', 0, 0 };
const uint32_t kVersion = 1;

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

struct Cache
{
    std::mutex                       lock;
    ID3D12Device*                    device = nullptr;    // the one the library is for (not owned)
    ComPtr<ID3D12PipelineLibrary>    library;
    std::vector<char>                blob;                // the library's bytes - it reads them for its whole life
    std::wstring                     path;
    FileHeader                       key = {};
    bool                             dirty = false;       // pipelines stored since the last write
    bool                             stale = false;       // a name already held another pipeline: start again
    std::unordered_map<const void*, uint64_t> rootSigs;   // root signature -> its serialised bytes' hash
    int    count = 0, hits = 0;
    double ms = 0;
    bool   times = std::getenv("NGG_PIPELINE_TIMES") != nullptr;
};

Cache& C()
{
    static Cache c;
    return c;
}

// Writes the library (the caller holds the lock).
void Save(Cache& c)
{
    if (!c.library || c.path.empty()) return;
    if (c.stale)
    {
        // A name held another description, which a library cannot drop: the file goes, the next start makes a new one.
        DeleteFileW(c.path.c_str());
        c.library.Reset();
        c.stale = c.dirty = false;
        Log("pipelines: a cached pipeline did not match its description - the cache starts again next time");
        return;
    }
    if (!c.dirty) return;
    ID3D12PipelineLibrary* lib = c.library.Get();
    const SIZE_T size = lib->GetSerializedSize();
    std::vector<char> data(size);
    if (!size || FAILED(lib->Serialize(data.data(), size)))
    {
        Log("pipelines: the library did not serialise - not cached");
        return;
    }
    FileHeader h = c.key;
    h.size = size;
    Fnv f;
    f.Bytes(data.data(), size);
    h.dataHash = f.h;
    const std::wstring tmp = c.path + L".tmp";
    FILE* out = _wfopen(tmp.c_str(), L"wb");
    if (!out) return;
    const bool ok = fwrite(&h, sizeof h, 1, out) == 1 && fwrite(data.data(), 1, size, out) == size;
    fclose(out);
    if (!ok || !MoveFileExW(tmp.c_str(), c.path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmp.c_str());
        Log("pipelines: could not write the cache");
        return;
    }
    c.dirty = c.stale = false;
    Log("pipelines: cache written (%zu KB)", (size_t)(size / 1024));
}

template <class Desc, class LoadFn, class CreateFn>
HRESULT Create(ID3D12Device* dev, const Desc* desc, uint64_t name, REFIID riid, void** out, LoadFn load,
               CreateFn create)
{
    Cache& c = C();
    std::lock_guard<std::mutex> hold(c.lock);
    const double t0 = NowMs();
    wchar_t wname[24];
    swprintf_s(wname, L"%016llx", (unsigned long long)name);
    HRESULT hr = E_FAIL;
    bool hit = false;
    if (c.library && dev == c.device)
    {
        hr = load(c.library.Get(), wname, desc, riid, out);
        hit = SUCCEEDED(hr);
    }
    if (!hit)
    {
        hr = create(dev, desc, riid, out);
        if (SUCCEEDED(hr) && c.library && dev == c.device)
        {
            ComPtr<ID3D12PipelineState> pso;
            static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&pso));
            if (pso)
            {
                if (SUCCEEDED(c.library->StorePipeline(wname, pso.Get()))) c.dirty = true;
                else c.stale = true;   // the name is taken (by another description)
            }
        }
    }
    const double ms = NowMs() - t0;
    ++c.count;
    c.hits += hit ? 1 : 0;
    c.ms += ms;
    if (c.times) Log("pipelines: %ls %s in %.2f ms", wname, hit ? "from the cache" : "created", ms);
    return hr;
}

uint64_t RootSigHash(Cache& c, ID3D12RootSignature* rs)
{
    std::lock_guard<std::mutex> hold(c.lock);
    const auto it = c.rootSigs.find(rs);
    return it == c.rootSigs.end() ? 0 : it->second;
}

} // namespace

void OpenPipelineCache(ID3D12Device* dev, const std::string& dir)
{
    Cache& c = C();
    std::lock_guard<std::mutex> hold(c.lock);
    c.device = dev;
    c.library.Reset();
    c.blob.clear();
    c.dirty = c.stale = false;
    if (!dev || dir.empty()) return;
    if (const char* e = std::getenv("NGG_PIPELINE_CACHE"); e && !std::strcmp(e, "off"))
    {
        Log("pipelines: no cache (NGG_PIPELINE_CACHE=off)");
        return;
    }
    if (const char* d = std::getenv("NGG_SHADER_DIR"); d && *d)
    {
        Log("pipelines: no cache while NGG_SHADER_DIR compiles the shaders");
        return;
    }
    D3D12_FEATURE_DATA_SHADER_CACHE sc = {};
    ComPtr<ID3D12Device1> d1;
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &sc, sizeof sc)) ||
        !(sc.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY) || FAILED(dev->QueryInterface(IID_PPV_ARGS(&d1))))
    {
        Log("pipelines: no pipeline library on this device - no cache");
        return;
    }

    // The key: this adapter and its driver, and the shaders.
    FileHeader& k = c.key;
    k = {};
    std::memcpy(k.magic, kMagic, sizeof kMagic);
    k.version = kVersion;
    k.shaderSet = embedded::kShaderSetHash;
    {
        const LUID luid = dev->GetAdapterLuid();
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 ad = {};
        LARGE_INTEGER umd = {};
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) || FAILED(adapter->GetDesc1(&ad)) ||
            FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        {
            Log("pipelines: the adapter's driver version is unknown - no cache");
            return;
        }
        k.vendor = ad.VendorId;
        k.device = ad.DeviceId;
        k.subsys = ad.SubSysId;
        k.revision = ad.Revision;
        k.driver = (uint64_t)umd.QuadPart;
    }
    c.path = Widen(dir);
    if (!c.path.empty() && c.path.back() != L'\\' && c.path.back() != L'/') c.path += L'\\';
    c.path += L"pipelines.bin";

    // The file, if it is this key's and whole.
    const char* why = nullptr;
    if (FILE* f = _wfopen(c.path.c_str(), L"rb"))
    {
        FileHeader h = {};
        if (fread(&h, sizeof h, 1, f) != 1) why = "short";
        else if (std::memcmp(h.magic, k.magic, sizeof k.magic) || h.version != k.version) why = "another format";
        else if (h.vendor != k.vendor || h.device != k.device || h.subsys != k.subsys || h.revision != k.revision)
            why = "another adapter";
        else if (h.driver != k.driver) why = "another driver";
        else if (h.shaderSet != k.shaderSet) why = "other shaders";
        else if (h.size > (1ull << 30)) why = "damaged";
        else
        {
            c.blob.resize((size_t)h.size);
            Fnv fh;
            if (fread(c.blob.data(), 1, c.blob.size(), f) != c.blob.size()) why = "short";
            else if (fh.Bytes(c.blob.data(), c.blob.size()), fh.h != h.dataHash) why = "damaged";
        }
        fclose(f);
        if (!why)
        {
            const HRESULT hr = d1->CreatePipelineLibrary(c.blob.data(), c.blob.size(), IID_PPV_ARGS(&c.library));
            if (FAILED(hr))
            {
                why = hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH ? "refused: another driver"
                    : hr == D3D12_ERROR_ADAPTER_NOT_FOUND       ? "refused: another adapter"
                                                                : "refused by the runtime";
                c.library.Reset();
            }
        }
        if (why)
        {
            Log("pipelines: the cache is stale (%s) - built again", why);
            c.blob.clear();
            DeleteFileW(c.path.c_str());
        }
        else
            Log("pipelines: cache read (%zu KB)", c.blob.size() / 1024);
    }
    if (!c.library && FAILED(d1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&c.library))))
    {
        Log("pipelines: no pipeline library - no cache");
        c.library.Reset();
    }
}

void SavePipelineCache()
{
    Cache& c = C();
    std::lock_guard<std::mutex> hold(c.lock);
    Save(c);
}

void ClosePipelineCache()
{
    Cache& c = C();
    std::lock_guard<std::mutex> hold(c.lock);
    Save(c);
    c.library.Reset();
    c.blob.clear();
    c.rootSigs.clear();
    c.device = nullptr;
}

HRESULT CreatePipeline(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID riid, void** out)
{
    if (!dev || !d) return E_INVALIDARG;
    // The name: every field that makes the pipeline, field by field (no padding), the shaders' bytes and the root
    // signature's.
    Fnv f;
    f.Val('G');
    f.Val(RootSigHash(C(), d->pRootSignature));
    f.Code(d->VS); f.Code(d->PS); f.Code(d->DS); f.Code(d->HS); f.Code(d->GS);
    f.Val(d->StreamOutput.NumEntries);
    for (UINT i = 0; i < d->StreamOutput.NumEntries; ++i)
    {
        const D3D12_SO_DECLARATION_ENTRY& e = d->StreamOutput.pSODeclaration[i];
        f.Val(e.Stream); f.Str(e.SemanticName); f.Val(e.SemanticIndex); f.Val(e.StartComponent);
        f.Val(e.ComponentCount); f.Val(e.OutputSlot);
    }
    f.Val(d->StreamOutput.NumStrides);
    for (UINT i = 0; i < d->StreamOutput.NumStrides; ++i) f.Val(d->StreamOutput.pBufferStrides[i]);
    f.Val(d->StreamOutput.RasterizedStream);
    f.Val(d->BlendState.AlphaToCoverageEnable); f.Val(d->BlendState.IndependentBlendEnable);
    for (const D3D12_RENDER_TARGET_BLEND_DESC& b : d->BlendState.RenderTarget)
    {
        f.Val(b.BlendEnable); f.Val(b.LogicOpEnable); f.Val(b.SrcBlend); f.Val(b.DestBlend); f.Val(b.BlendOp);
        f.Val(b.SrcBlendAlpha); f.Val(b.DestBlendAlpha); f.Val(b.BlendOpAlpha); f.Val(b.LogicOp);
        f.Val(b.RenderTargetWriteMask);
    }
    f.Val(d->SampleMask);
    const D3D12_RASTERIZER_DESC& r = d->RasterizerState;
    f.Val(r.FillMode); f.Val(r.CullMode); f.Val(r.FrontCounterClockwise); f.Val(r.DepthBias); f.Val(r.DepthBiasClamp);
    f.Val(r.SlopeScaledDepthBias); f.Val(r.DepthClipEnable); f.Val(r.MultisampleEnable); f.Val(r.AntialiasedLineEnable);
    f.Val(r.ForcedSampleCount); f.Val(r.ConservativeRaster);
    const D3D12_DEPTH_STENCIL_DESC& z = d->DepthStencilState;
    f.Val(z.DepthEnable); f.Val(z.DepthWriteMask); f.Val(z.DepthFunc); f.Val(z.StencilEnable);
    f.Val(z.StencilReadMask); f.Val(z.StencilWriteMask);
    for (const D3D12_DEPTH_STENCILOP_DESC* o : { &z.FrontFace, &z.BackFace })
    {
        f.Val(o->StencilFailOp); f.Val(o->StencilDepthFailOp); f.Val(o->StencilPassOp); f.Val(o->StencilFunc);
    }
    f.Val(d->InputLayout.NumElements);
    for (UINT i = 0; i < d->InputLayout.NumElements; ++i)
    {
        const D3D12_INPUT_ELEMENT_DESC& e = d->InputLayout.pInputElementDescs[i];
        f.Str(e.SemanticName); f.Val(e.SemanticIndex); f.Val(e.Format); f.Val(e.InputSlot);
        f.Val(e.AlignedByteOffset); f.Val(e.InputSlotClass); f.Val(e.InstanceDataStepRate);
    }
    f.Val(d->IBStripCutValue); f.Val(d->PrimitiveTopologyType); f.Val(d->NumRenderTargets);
    for (DXGI_FORMAT fmt : d->RTVFormats) f.Val(fmt);
    f.Val(d->DSVFormat); f.Val(d->SampleDesc.Count); f.Val(d->SampleDesc.Quality); f.Val(d->NodeMask); f.Val(d->Flags);
    return Create(dev, d, f.h, riid, out,
        [](ID3D12PipelineLibrary* lib, const wchar_t* n, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID id,
           void** o) { return lib->LoadGraphicsPipeline(n, desc, id, o); },
        [](ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID id, void** o) {
            return device->CreateGraphicsPipelineState(desc, id, o);
        });
}

HRESULT CreatePipeline(ID3D12Device* dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID riid, void** out)
{
    if (!dev || !d) return E_INVALIDARG;
    Fnv f;
    f.Val('C');
    f.Val(RootSigHash(C(), d->pRootSignature));
    f.Code(d->CS);
    f.Val(d->NodeMask);
    f.Val(d->Flags);
    return Create(dev, d, f.h, riid, out,
        [](ID3D12PipelineLibrary* lib, const wchar_t* n, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID id,
           void** o) { return lib->LoadComputePipeline(n, desc, id, o); },
        [](ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID id, void** o) {
            return device->CreateComputePipelineState(desc, id, o);
        });
}

HRESULT CreateRootSignature(ID3D12Device* dev, UINT nodeMask, const void* blob, SIZE_T size, REFIID riid, void** out)
{
    if (!dev) return E_INVALIDARG;
    const HRESULT hr = dev->CreateRootSignature(nodeMask, blob, size, riid, out);
    if (SUCCEEDED(hr) && out && *out)
    {
        ComPtr<ID3D12RootSignature> rs;
        static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&rs));
        Fnv f;
        f.Bytes(blob, size);
        Cache& c = C();
        std::lock_guard<std::mutex> hold(c.lock);
        if (rs) c.rootSigs[rs.Get()] = f.h;
    }
    return hr;
}

void LogPipelineStats(const char* when)
{
    Cache& c = C();
    std::lock_guard<std::mutex> hold(c.lock);
    if (!c.count) return;
    Log("pipelines: %d made in %.1f ms, %d from the cache (%s)", c.count, c.ms, c.hits, when);
    c.count = c.hits = 0;
    c.ms = 0;
}

} // namespace core
} // namespace ngg
