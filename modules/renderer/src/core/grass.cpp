// GRASS: the field round the camera and the spawn of its blades (grass.h; NGG design §2.6 "Grass").
#include "grass.h"
#include "frame_upload.h"
#include "log.h"
#include "shaders.h"
#include "pipeline_cache.h"
#include "../format/ngg_format.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

const DXGI_FORMAT kFieldFormats[3] = { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM };
constexpr uint32_t kInstBytes = 32;
constexpr uint32_t kArgsBytes = 64;   // kLods DRAW records (16 bytes), then the raw counts at 48

D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

ComPtr<ID3D12Resource> Upload(ID3D12Device* dev, const void* data, size_t bytes)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const D3D12_RESOURCE_DESC desc = BufferDesc((std::max)(bytes, (size_t)16));
    ComPtr<ID3D12Resource> res;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&res))))
        return nullptr;
    void* mapped = nullptr;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(res->Map(0, &none, &mapped))) return nullptr;
    if (data && bytes) memcpy(mapped, data, bytes);
    res->Unmap(0, nullptr);
    return res;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

void Mul4x4(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float acc = 0.0f;
            for (int k = 0; k < 4; ++k) acc += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = acc;
        }
}

// The frustum's planes (row-vector convention: from the matrix's columns), normalised; inside: dot >= 0.
void Planes(const float* m, float p[6][4])
{
    auto col = [&](int c, int r) { return m[r * 4 + c]; };
    for (int i = 0; i < 4; ++i)
    {
        p[0][i] = col(3, i) + col(0, i);
        p[1][i] = col(3, i) - col(0, i);
        p[2][i] = col(3, i) + col(1, i);
        p[3][i] = col(3, i) - col(1, i);
        p[4][i] = col(2, i);
        p[5][i] = col(3, i) - col(2, i);
    }
    for (int i = 0; i < 6; ++i)
    {
        const float len = sqrtf(p[i][0] * p[i][0] + p[i][1] * p[i][1] + p[i][2] * p[i][2]);
        if (len > 1e-9f) for (int k = 0; k < 4; ++k) p[i][k] /= len;
    }
}

} // namespace

constexpr uint32_t Grass::kCap[Grass::kLods];

void Grass::Reset()
{
    m_loaded = false;
    m_spawned = false;
    m_fieldValid = false;
    m_payload.clear();
    m_types.clear();
    m_typeNames.clear();
    m_maskOf.clear();
    m_maskRec.clear();
    m_replaced.clear();
    m_grid.clear();
    m_gridW = m_gridH = 0;
    m_payloadBuf.Reset();
    m_gridBuf.Reset();
    m_typeBuf.Reset();
    for (uint32_t l = 0; l < kLods; ++l) m_counts[l] = 0;
    m_dropped = 0;
    m_loggedFirst = false;
}

bool Grass::Load(ID3D12Device* dev, const uint8_t* p, size_t n, uint32_t textureCount, uint32_t plantCount,
                 uint32_t variant, const float offset[3])
{
    Reset();
    if (!p) return false;
    if (const char* why = CheckGrassChunk(p, n, textureCount, plantCount))
    {
        Log("scene: NGGF refused (%s) - no grass, the cards it would replace drawn", why);
        return false;
    }
    GrassChunkHeader h;
    memcpy(&h, p, sizeof h);
    if (h.typeCount == 0 || h.maskCount == 0)
    {
        Log("scene: NGGF has %u type(s) and %u mask(s) - nothing grows, no grass", h.typeCount, h.maskCount);
        return false;
    }
    m_payload.assign(p, p + n);
    m_cellSize = h.cellSize;
    m_tileCells = h.tileCells;
    for (int k = 0; k < 3; ++k) m_offset[k] = offset ? offset[k] : 0.0f;

    size_t at = sizeof h;
    const uint32_t types = (std::min)(h.typeCount, kMaxTypes);
    if (h.typeCount > kMaxTypes) Log("scene: NGGF has %u grass types - the first %u drawn", h.typeCount, kMaxTypes);
    for (uint32_t i = 0; i < h.typeCount; ++i, at += h.typeRecordSize)
    {
        if (i >= types) continue;
        GrassType t;
        memcpy(&t, p + at, sizeof t);
        t.name[sizeof t.name - 1] = 0;
        TypeGpu g;
        g.heightMin = (std::max)(0.0f, t.heightMin);
        g.heightMax = (std::max)(g.heightMin, t.heightMax);
        g.density = (std::max)(0.0f, t.density);
        g.width = (std::max)(0.001f, t.bladeWidth);
        g.dryness = t.dryness;
        g.spread = t.colourSpread;
        g.wind = t.windResponse;
        g.seedHeads = (t.flags & kGrassSeedHeads) ? 1.0f : 0.0f;
        m_types.push_back(g);
        m_typeNames.push_back(t.name);
    }
    m_maskOf.assign(textureCount, 0xFFFFFFFFu);
    for (uint32_t i = 0; i < h.maskCount; ++i, at += h.maskRecordSize)
    {
        GrassMask m;
        memcpy(&m, p + at, sizeof m);
        if (m_maskOf[m.texture] != 0xFFFFFFFFu)
            Log("scene: NGGF has two masks for texture %u - the first kept", m.texture);
        else
            m_maskOf[m.texture] = i;
        m_maskRec.push_back(m.dataOffset);
        m_maskRec.push_back((uint32_t)m.width | ((uint32_t)m.height << 16));
        m_maskRec.push_back((std::min)(m.type, types - 1));
    }
    // The paint: the tiles of this variant, on a grid over their extent.
    const uint32_t bit = variant < 32 ? (1u << variant) : 0u;
    std::vector<GrassTile> mine;
    for (uint32_t i = 0; i < h.tileCount; ++i, at += h.tileRecordSize)
    {
        GrassTile t;
        memcpy(&t, p + at, sizeof t);
        if (t.variants & bit) mine.push_back(t);
    }
    if (!mine.empty())
    {
        int32_t x0 = INT32_MAX, z0 = INT32_MAX, x1 = INT32_MIN, z1 = INT32_MIN;
        for (const GrassTile& t : mine)
            x0 = (std::min)(x0, t.tileX), z0 = (std::min)(z0, t.tileZ), x1 = (std::max)(x1, t.tileX), z1 = (std::max)(z1, t.tileZ);
        const uint64_t w = (uint64_t)(x1 - x0) + 1, hh = (uint64_t)(z1 - z0) + 1;
        if (w * hh > 4u * 1024u * 1024u)
            Log("scene: NGGF's tiles span %llu x %llu - the paint left out", (unsigned long long)w, (unsigned long long)hh);
        else
        {
            m_gridX0 = x0, m_gridZ0 = z0, m_gridW = (uint32_t)w, m_gridH = (uint32_t)hh;
            m_grid.assign((size_t)(w * hh), 0xFFFFFFFFu);
            for (const GrassTile& t : mine)
                m_grid[(size_t)(t.tileZ - z0) * m_gridW + (size_t)(t.tileX - x0)] = t.dataOffset;
        }
    }
    if (m_grid.empty()) { m_gridW = m_gridH = 1; m_grid.assign(1, 0xFFFFFFFFu); }
    for (uint32_t i = 0; i < h.plantCount; ++i, at += 4)
    {
        uint32_t pl;
        memcpy(&pl, p + at, 4);
        m_replaced.push_back(pl);
    }
    std::sort(m_replaced.begin(), m_replaced.end());
    m_replaced.erase(std::unique(m_replaced.begin(), m_replaced.end()), m_replaced.end());

    m_payloadBuf = Upload(dev, m_payload.data(), (m_payload.size() + 3) & ~(size_t)3);
    m_gridBuf = Upload(dev, m_grid.data(), m_grid.size() * 4);
    m_typeBuf = Upload(dev, m_types.data(), m_types.size() * sizeof(TypeGpu));
    if (!m_payloadBuf || !m_gridBuf || !m_typeBuf)
    {
        Log("scene: NGGF - the grass's buffers could not be made, no grass");
        Reset();
        return false;
    }
    std::string names;
    for (const std::string& s : m_typeNames) names += (names.empty() ? "" : ", ") + s;
    Log("scene: NGGF grass - %zu type(s) (%s), %u mask(s), %zu of %u paint tile(s) in this variant, %zu plant(s) "
        "replaced, %.1f MB", m_types.size(), names.c_str(), h.maskCount, mine.size(), h.tileCount, m_replaced.size(),
        n / (1024.0 * 1024.0));
    m_loaded = true;
    return true;
}

D3D12_VERTEX_BUFFER_VIEW Grass::InstanceView() const
{
    D3D12_VERTEX_BUFFER_VIEW v = {};
    if (!m_inst) return v;
    v.BufferLocation = m_inst->GetGPUVirtualAddress();
    v.SizeInBytes = (UINT)m_inst->GetDesc().Width;
    v.StrideInBytes = kInstBytes;
    return v;
}

bool Grass::CreateGpu(ID3D12Device* dev)
{
    m_gpuTried = true;
    // The field's targets and their depth (the highest ground wins: drawn from above).
    D3D12_HEAP_PROPERTIES dh = {};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (int k = 0; k < 3; ++k)
    {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = kClip;
        d.Height = kClip;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = kFieldFormats[k];
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = d.Format;
        if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                                IID_PPV_ARGS(&m_field[k]))))
            return false;
    }
    {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = kClip;
        d.Height = kClip;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_D32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = d.Format;
        if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                IID_PPV_ARGS(&m_fieldDepth))))
            return false;
    }
    m_fieldState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 3;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)))) return false;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_dsvHeap)))) return false;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 3;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)))) return false;
        const UINT rs = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const UINT ss = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (int k = 0; k < 3; ++k)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE r = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            r.ptr += (SIZE_T)k * rs;
            dev->CreateRenderTargetView(m_field[k].Get(), nullptr, r);
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = k == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : kFieldFormats[k];
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE s = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
            s.ptr += (SIZE_T)k * ss;
            dev->CreateShaderResourceView(m_field[k].Get(), &sd, s);
        }
        dev->CreateDepthStencilView(m_fieldDepth.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }
    // The instances, the indirect arguments (and their start, copied in each frame) and a readback of the counts.
    {
        UINT64 total = 0;
        for (uint32_t l = 0; l < kLods; ++l) total += (UINT64)kCap[l] * kInstBytes;
        D3D12_RESOURCE_DESC d = BufferDesc(total, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                nullptr, IID_PPV_ARGS(&m_inst))))
            return false;
        m_instState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        d = BufferDesc(kArgsBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&m_args))))
            return false;
        // The strips' vertex counts (9: 7 triangles, 5: 3, a card's 4) and where each LOD's instances start.
        const uint32_t init[16] = { 9, 0, 0, 0,
                                    5, 0, 0, kCap[0],
                                    4, 0, 0, kCap[0] + kCap[1],
                                    0, 0, 0, 0 };
        m_argsInit = Upload(dev, init, sizeof init);
        D3D12_HEAP_PROPERTIES rh = {};
        rh.Type = D3D12_HEAP_TYPE_READBACK;
        d = BufferDesc(kArgsBytes * 2);   // a range per frame slot (Device::kMaxFrames)
        if (!m_argsInit || FAILED(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &d,
                                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                               IID_PPV_ARGS(&m_argsReadback))))
            return false;
    }
    {
        D3D12_INDIRECT_ARGUMENT_DESC a = {};
        a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC cs = {};
        cs.ByteStride = 16;
        cs.NumArgumentDescs = 1;
        cs.pArgumentDescs = &a;
        if (FAILED(dev->CreateCommandSignature(&cs, nullptr, IID_PPV_ARGS(&m_cmdSig)))) return false;
    }

    // --- the field's root signature: its constants, the material's five SRVs (the scene's heap), the chunk, the grid.
    {
        D3D12_DESCRIPTOR_RANGE tex = {};
        tex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        tex.NumDescriptors = 5;
        tex.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER rp[4] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants.ShaderRegister = 0;
        rp[0].Constants.Num32BitValues = 34;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &tex;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[2].Descriptor.ShaderRegister = 5;
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[3].Descriptor.ShaderRegister = 6;
        rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC s = {};
        s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 4;
        rs.pParameters = rp;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &s;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
            FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_fieldRs))))
        {
            Log("grass: the field's root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
    }
    // --- the spawn's: its constants, the field's three SRVs (our heap), the types, the instances, the arguments.
    {
        D3D12_DESCRIPTOR_RANGE tex = {};
        tex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        tex.NumDescriptors = 3;
        tex.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER rp[5] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants.ShaderRegister = 0;
        rp[0].Constants.Num32BitValues = 44;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &tex;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[2].Descriptor.ShaderRegister = 3;
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp[3].Descriptor.ShaderRegister = 0;
        rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp[4].Descriptor.ShaderRegister = 1;
        D3D12_STATIC_SAMPLER_DESC s = {};
        s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 5;
        rs.pParameters = rp;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &s;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
            FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_spawnRs))))
        {
            Log("grass: the spawn's root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
    }
    // --- the shaders (shaders.txt: grass.hlsl, DXC).
    const std::vector<std::string> args = { "-HV", "2018" };
    ComPtr<ID3DBlob> vs = LoadShader("grass.hlsl", "VSField", "vs_6_0", args);
    ComPtr<ID3DBlob> ps = LoadShader("grass.hlsl", "PSField", "ps_6_0", args);
    ComPtr<ID3DBlob> cs = LoadShader("grass.hlsl", "CSSpawn", "cs_6_0", args);
    ComPtr<ID3DBlob> cc = LoadShader("grass.hlsl", "CSClamp", "cs_6_0", args);
    if (!vs || !ps || !cs || !cc) return false;
    static const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gp = {};
    gp.pRootSignature = m_fieldRs.Get();
    gp.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    gp.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    gp.InputLayout = { layout, _countof(layout) };
    gp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gp.NumRenderTargets = 3;
    for (int k = 0; k < 3; ++k) gp.RTVFormats[k] = kFieldFormats[k];
    gp.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    gp.SampleDesc.Count = 1;
    gp.SampleMask = UINT_MAX;
    gp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gp.RasterizerState.DepthClipEnable = TRUE;
    gp.DepthStencilState.DepthEnable = TRUE;
    gp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    gp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
    for (int k = 0; k < 3; ++k) gp.BlendState.RenderTarget[k].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cp = {};
    cp.pRootSignature = m_spawnRs.Get();
    cp.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    D3D12_COMPUTE_PIPELINE_STATE_DESC cl = cp;
    cl.CS = { cc->GetBufferPointer(), cc->GetBufferSize() };
    PipelineBatch batch(dev);
    batch.Add(gp, &m_fieldPso, "grass: the field's PSO failed - no grass");
    batch.Add(cp, &m_spawnPso, "grass: the spawn's PSO failed - no grass");
    batch.Add(cl, &m_clampPso, "grass: the spawn's clamp PSO failed - no grass");
    batch.Run("the grass's");
    return m_fieldPso && m_spawnPso && m_clampPso;
}

void Grass::DrawField(ID3D12GraphicsCommandList* list, const Terrain& terrain)
{
    // From above: x, z onto the clipmap, the height onto depth (higher nearer: GREATER keeps the top surface).
    const float half = 0.5f * kClip * kTexel;
    const float cx = (float)m_fieldX * kTexel, cz = (float)m_fieldZ * kTexel;
    const float yLo = -2000.0f, yHi = 4000.0f;
    // Row vectors: clip = (x, y, z, 1) * M. x -> (x - cx) / half; z DOWN the image, so the texture's v runs with +z (the
    // spawn reads it at (x - x0, z - z0) / size); depth (y - lo) / range.
    float vp[16] = {};
    vp[0] = 1.0f / half;
    vp[12] = -cx / half;
    vp[9] = -1.0f / half;
    vp[13] = cz / half;
    vp[6] = 1.0f / (yHi - yLo);
    vp[14] = -yLo / (yHi - yLo);
    vp[15] = 1.0f;

    Barrier(list, m_field[0].Get(), m_fieldState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Barrier(list, m_field[1].Get(), m_fieldState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Barrier(list, m_field[2].Get(), m_fieldState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_fieldState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[3];
    const UINT rs = [&] { ID3D12Device* d = nullptr; list->GetDevice(IID_PPV_ARGS(&d));
                          const UINT s = d ? d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) : 0;
                          if (d) d->Release(); return s; }();
    for (int k = 0; k < 3; ++k)
    {
        rtv[k] = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv[k].ptr += (SIZE_T)k * rs;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const float zero[4] = { 0, 0, 0, 0 };
    for (int k = 0; k < 3; ++k) list->ClearRenderTargetView(rtv[k], zero, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    list->OMSetRenderTargets(3, rtv, FALSE, &dsv);
    const D3D12_VIEWPORT v = { 0.0f, 0.0f, (float)kClip, (float)kClip, 0.0f, 1.0f };
    const D3D12_RECT sc = { 0, 0, (LONG)kClip, (LONG)kClip };
    list->RSSetViewports(1, &v);
    list->RSSetScissorRects(1, &sc);
    list->SetGraphicsRootSignature(m_fieldRs.Get());
    list->SetPipelineState(m_fieldPso.Get());
    ID3D12DescriptorHeap* heaps[] = { terrain.heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootShaderResourceView(2, m_payloadBuf->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(3, m_gridBuf->GetGPUVirtualAddress());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &terrain.vbv);
    list->IASetIndexBuffer(&terrain.ibv);
    list->SetGraphicsRoot32BitConstants(0, 16, vp, 0);
    struct Paint { int32_t x0, z0; uint32_t w, h; float cell; uint32_t cells; float ox, oz; } paint = {
        m_gridX0, m_gridZ0, m_gridW, m_gridH, m_cellSize, m_tileCells, m_offset[0], m_offset[2] };
    list->SetGraphicsRoot32BitConstants(0, 8, &paint, 26);
    const float lo[2] = { cx - half, cz - half }, hi[2] = { cx + half, cz + half };
    const D3D12_GPU_DESCRIPTOR_HANDLE heap0 = terrain.heap->GetGPUDescriptorHandleForHeapStart();
    for (const TerrainDraw& d : *terrain.draws)
    {
        if (d.bmax[0] < lo[0] || d.bmin[0] > hi[0] || d.bmax[2] < lo[1] || d.bmin[2] > hi[1]) continue;
        if (d.baseMask == 0xFFFFFFFFu && d.detailMask == 0xFFFFFFFFu) continue;   // nothing grows on it, nor under it
        auto mask = [&](uint32_t m, uint32_t* out) {
            out[0] = m == 0xFFFFFFFFu ? 0xFFFFFFFFu : m_maskRec[m * 3 + 0];
            out[1] = m == 0xFFFFFFFFu ? 0u : m_maskRec[m * 3 + 1];
            out[2] = m == 0xFFFFFFFFu ? 0u : m_maskRec[m * 3 + 2];
        };
        uint32_t c[11] = {};
        c[0] = d.flags;
        mask(d.baseMask, c + 1);
        mask(d.detailMask, c + 4);
        memcpy(c + 7, d.factor, 12);
        list->SetGraphicsRoot32BitConstants(0, 10, c, 16);
        D3D12_GPU_DESCRIPTOR_HANDLE h = heap0;
        h.ptr += (UINT64)d.material * 5u * terrain.stride;
        list->SetGraphicsRootDescriptorTable(1, h);
        list->DrawIndexedInstanced(d.indexCount, 1, d.firstIndex, 0, 0);
    }
    for (int k = 0; k < 3; ++k) Barrier(list, m_field[k].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_fieldState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_fieldValid = true;
}

void Grass::Update(ID3D12GraphicsCommandList* list, const Terrain& terrain, const float* view, const float* proj,
                   float reach, float density)
{
    m_spawned = false;
    if (!m_loaded || reach <= 0.0f || !terrain.heap || !terrain.draws) return;
    ID3D12Device* dev = nullptr;
    list->GetDevice(IID_PPV_ARGS(&dev));
    if (!dev) return;
    if (!m_gpuTried) m_gpuOk = CreateGpu(dev);
    dev->Release();
    if (!m_gpuOk) return;
    ReadStats();   // what this frame slot's last frame counted (it has run: Device::BeginFrame)

    Stamp(list, 0);
    const float tx = view[12], ty = view[13], tz = view[14];
    float eye[3];
    for (int j = 0; j < 3; ++j) eye[j] = -(tx * view[j * 4 + 0] + ty * view[j * 4 + 1] + tz * view[j * 4 + 2]);

    // The field: drawn again once the camera is kRedrawTexels from its centre (or the first time).
    const int32_t ex = (int32_t)floorf(eye[0] / kTexel), ez = (int32_t)floorf(eye[2] / kTexel);
    if (!m_fieldValid || std::abs(ex - m_fieldX) >= (int32_t)kRedrawTexels || std::abs(ez - m_fieldZ) >= (int32_t)kRedrawTexels)
    {
        m_fieldX = ex;
        m_fieldZ = ez;
        DrawField(list, terrain);
    }
    else if (m_fieldState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        for (int k = 0; k < 3; ++k) Barrier(list, m_field[k].Get(), m_fieldState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_fieldState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    // The spawn's arguments start over; the instances become writable again.
    Barrier(list, m_args.Get(), m_frame ? D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT : D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(m_args.Get(), 0, m_argsInit.Get(), 0, kArgsBytes);
    Barrier(list, m_args.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(list, m_inst.Get(), m_instState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_instState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    float vp[16], planes[6][4];
    Mul4x4(view, proj, vp);
    Planes(vp, planes);
    float maxDensity = 1.0f;
    for (const TypeGpu& t : m_types) maxDensity = (std::max)(maxDensity, t.density);
    const float fullTo = (std::min)(15.0f, 0.5f * reach);
    const int32_t px0 = (int32_t)floorf((eye[0] - reach) / kPatch), pz0 = (int32_t)floorf((eye[2] - reach) / kPatch);
    const uint32_t side = (uint32_t)ceilf(2.0f * reach / kPatch) + 2;
    struct SpawnCB
    {
        float clipX0, clipZ0, clipSize, reach;
        float eye[3], fullTo;
        int32_t patchX0, patchZ0;
        uint32_t patchesSide;
        float densityScale;
        float lod0, lod1;
        uint32_t typeCount;
        float maxDensity;
        float planes[6][4];
        uint32_t cap[3], pad;
    } cb = {};
    static_assert(sizeof(SpawnCB) == 44 * 4, "SpawnCB: 44 root constants (grass.hlsl SpawnCB)");
    const float half = 0.5f * kClip * kTexel;
    cb.clipX0 = (float)m_fieldX * kTexel - half;
    cb.clipZ0 = (float)m_fieldZ * kTexel - half;
    cb.clipSize = 2.0f * half;
    cb.reach = reach;
    for (int k = 0; k < 3; ++k) cb.eye[k] = eye[k];
    cb.fullTo = fullTo;
    cb.patchX0 = px0;
    cb.patchZ0 = pz0;
    cb.patchesSide = side;
    cb.densityScale = density;
    cb.lod0 = 0.16f * reach;
    cb.lod1 = 0.4f * reach;
    cb.typeCount = (uint32_t)m_types.size();
    cb.maxDensity = maxDensity;
    memcpy(cb.planes, planes, sizeof planes);
    for (uint32_t l = 0; l < kLods; ++l) cb.cap[l] = kCap[l];

    list->SetComputeRootSignature(m_spawnRs.Get());
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRoot32BitConstants(0, 44, &cb, 0);
    list->SetComputeRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootShaderResourceView(2, m_typeBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, m_inst->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(4, m_args->GetGPUVirtualAddress());
    list->SetPipelineState(m_spawnPso.Get());
    list->Dispatch((side + 7) / 8, (side + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = m_args.Get();
    list->ResourceBarrier(1, &uav);
    list->SetPipelineState(m_clampPso.Get());
    list->Dispatch(1, 1, 1);

    Barrier(list, m_args.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(m_argsReadback.Get(), (UINT64)FrameSlot() * kArgsBytes, m_args.Get(), 0, kArgsBytes);
    Barrier(list, m_args.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    Barrier(list, m_inst.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    m_instState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    m_readbackPending[FrameSlot()] = true;
    ++m_frame;
    m_spawned = true;
    Stamp(list, 1);
}

bool Grass::Timed()
{
    static const bool on = [] { const char* e = std::getenv("NGG_GRASS_TIMES"); return e && *e && *e != '0'; }();
    return on;
}

void Grass::Stamp(ID3D12GraphicsCommandList* list, uint32_t slot)
{
    if (!Timed() || slot >= 6) return;
    if (!m_timer.Ready())
    {
        ID3D12Device* dev = nullptr;
        list->GetDevice(IID_PPV_ARGS(&dev));
        if (!dev) return;
        ComPtr<ID3D12CommandQueue> q;
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) || FAILED(q->GetTimestampFrequency(&m_timerHz)) ||
            !m_timer.Create(dev, 6))
            m_timer.Reset();
        dev->Release();
        if (!m_timer.Ready()) return;
    }
    m_timer.Stamp(list, slot);
    m_stamped |= 1u << slot;
}

void Grass::ResolveStamps(ID3D12GraphicsCommandList* list)
{
    if (!m_timer.Ready() || m_stamped != 0x3Fu) { m_stamped = 0; return; }
    m_timer.Resolve(list, 0, 6);
    m_stamped = 0;
}

void Grass::ReadStats()
{
    // This frame slot's last frame (Device::BeginFrame waited for it).
    if (UINT64 t[6]; m_timerHz && m_timer.Take(t))
    {
        {
            for (int k = 0; k < 3; ++k)
                if (t[2 * k + 1] >= t[2 * k]) m_msSum[k] += double(t[2 * k + 1] - t[2 * k]) * 1000.0 / double(m_timerHz);
            m_bladeSum += double(m_counts[0]) + m_counts[1] + m_counts[2];
            if (++m_msFrames == 300)
            {
                Log("grass: field + spawn %.3f ms, prepass %.3f ms, main pass %.3f ms (means of 300 frames), %.0f instances",
                    m_msSum[0] / 300.0, m_msSum[1] / 300.0, m_msSum[2] / 300.0, m_bladeSum / 300.0);
                m_msSum[0] = m_msSum[1] = m_msSum[2] = m_bladeSum = 0.0;
                m_msFrames = 0;
            }
        }
    }
    const uint32_t s = FrameSlot();
    if (!m_readbackPending[s] || !m_argsReadback) return;
    uint32_t* a = nullptr;
    D3D12_RANGE r = { (SIZE_T)s * kArgsBytes, (SIZE_T)(s + 1) * kArgsBytes };
    if (SUCCEEDED(m_argsReadback->Map(0, &r, reinterpret_cast<void**>(&a))))
    {
        a += s * kArgsBytes / 4;
        m_dropped = 0;
        for (uint32_t l = 0; l < kLods; ++l)
        {
            m_counts[l] = (std::min)(a[12 + l], kCap[l]);
            m_dropped += a[12 + l] - m_counts[l];
        }
        if (!m_loggedFirst && m_counts[0] + m_counts[1] + m_counts[2] > 0)
        {
            m_loggedFirst = true;
            Log("grass: the first frame with grass - %u + %u blades, %u cards", m_counts[0], m_counts[1], m_counts[2]);
        }
        D3D12_RANGE none = { 0, 0 };
        m_argsReadback->Unmap(0, &none);
    }
    m_readbackPending[s] = false;
}

} // namespace core
} // namespace ngg
