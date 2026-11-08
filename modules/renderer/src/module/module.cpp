// WRenderer.dll - ngg-core behind wrally/iface/renderer.h (docs/design.md 3).
//
// Composed exactly as ngg-api (api/ngg_api.cpp) composes it - Device, swap chain, core::Renderer and its Scene - and
// driven by the frame the game describes instead of by an editor's calls. Where ngg-api had a call, the module makes
// the same core call in the same order (material creation, instance placement, the frame's recording), so a game
// that moved from ngg-api.dll to this draws the same image. What is new: the swap chain is on the game's own window
// (no child window), models (meshes on a node tree, placed per frame), the stage objects addressed by their key, and
// the UI pass (module/ui.h) after the frame.

#include <windows.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../format/ngg_format.h"
#include "device.h"
#include "log.h"
#include "module/ui.h"
#include "post.h"
#include "renderer.h"
#include "scene.h"
#include "sky.h"
#include "weather.h"
#include "wrally/iface/renderer.h"
#include "wrally/iface/renderer_edit.h"
#include "wrally/png/png.h"

using Microsoft::WRL::ComPtr;
using namespace wrally::iface;

namespace {

const DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
const UINT kBackBufferCount = 2;
constexpr uint32_t kModuleVersion = MakeVersion(0, 1, 0);
constexpr uint32_t kUiTextureBit = wrally::render::UiPass::kTextureBit;
constexpr uint32_t kUnknownInstance = 0xFFFFFFFFu;

// ngg-api's swap chain (api/ngg_api.cpp), on the game's window.
struct SwapChain {
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12Resource> buffers[kBackBufferCount];
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT rtvStride = 0;
    UINT width = 0, height = 0;

    bool Create(ngg::core::Device& dev, HWND hwnd, UINT w, UINT h)
    {
        width = w, height = h;
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = w;
        sd.Height = h;
        sd.Format = kBackBufferFormat;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBackBufferCount;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.SampleDesc.Count = 1;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory->CreateSwapChainForHwnd(dev.queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) return false;
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(sc1.As(&swap))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kBackBufferCount;
        if (FAILED(dev.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)))) return false;
        rtvStride = dev.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        return CreateViews(dev);
    }
    bool CreateViews(ngg::core::Device& dev)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kBackBufferCount; ++i) {
            if (FAILED(swap->GetBuffer(i, IID_PPV_ARGS(&buffers[i])))) return false;
            dev.device->CreateRenderTargetView(buffers[i].Get(), nullptr, h);
            h.ptr += rtvStride;
        }
        return true;
    }
    bool Resize(ngg::core::Device& dev, UINT w, UINT h)
    {
        if (w == 0 || h == 0 || (w == width && h == height)) return false;
        dev.WaitForGpu();
        for (auto& b : buffers) b.Reset();
        if (FAILED(swap->ResizeBuffers(kBackBufferCount, w, h, kBackBufferFormat, 0))) return false;
        width = w, height = h;
        return CreateViews(dev);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE CurrentRtv() const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)swap->GetCurrentBackBufferIndex() * rtvStride;
        return h;
    }
    ID3D12Resource* CurrentBuffer() const { return buffers[swap->GetCurrentBackBufferIndex()].Get(); }
};

struct Model {
    struct Mesh {
        uint32_t mesh;       // the scene's dynamic mesh (1-based)
        uint32_t material;   // the scene's material (1-based)
        uint32_t node;
    };
    std::vector<ModelNode> nodes;
    std::vector<Mesh> meshes;
    uint32_t refs = 0;
    bool released = false;
};

struct Instance {
    ModelHandle model = 0;
    std::vector<uint32_t> objects;   // the scene's object per mesh
    float origin[3] = {};            // where Place last put it (its world's translation)
    bool visible = true;
    bool alive = false;
};

// The host's log, for ngg-core's sink (a plain function: one renderer per process).
HostServices g_logHost{};
void CoreLog(const char* message)
{
    if (g_logHost.log) g_logHost.log(g_logHost.ctx, LogLevel::Info, (std::string("ngg: ") + message + "\n").c_str());
}

// a then b (row vectors: a * b) - phys-drive's Mul44, the arithmetic in the same order.
void Mul44(const float* a, const float* b, float* out)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] + a[4 * i + 3] * b[12 + j];
}

} // namespace

namespace wrally::iface {

struct Renderer {
    HostServices host{};
    HWND hwnd = nullptr;
    ngg::core::Device dev;
    SwapChain sc;
    ngg::core::Renderer core;
    ngg::core::Scene& scene = core.GetScene();
    ngg::core::PostFx& post = core.Post();
    ngg::core::SkyParams weatherSky;       // the weather's sky; a frame's is this plus its time (FrameDesc::skyTime)
    wrally::render::UiPass ui;
    RenderSettings settings{};
    std::string error;

    bool sceneLoaded = false;
    int32_t appliedPreset = -2;            // the weather preset in weatherSky (-1 a WeatherDesc, -2 none yet)
    WeatherDesc appliedDesc{};
    SkyTimeDesc loggedSkyTime{};           // the last FrameDesc::skyTime the log reported
    float sunElevationDeg = 90.0f;         // the last frame's sun over the horizon (RenderStats)
    std::unordered_map<int32_t, std::vector<uint32_t>> byKey;   // NGGK key -> the scene's objects
    std::unordered_map<int32_t, bool> keyVisible;
    std::vector<Model> models;             // by handle - 1
    std::vector<Instance> instances;       // by handle - 1
    bool linesSet = false;

    std::string capturePath;
    ComPtr<ID3D12Resource> captureBuf;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT captureFoot = {};

    Status Fail(Status s, const std::string& what)
    {
        error = what;
        ngg::core::Log("wrenderer: %s", what.c_str());
        return s;
    }
};

} // namespace wrally::iface

namespace {

// ---- the settings, as ngg-api's setters made them
void Apply(Renderer& r)
{
    const RenderSettings& s = r.settings;
    r.core.SetQuality(static_cast<ngg::core::Quality>(s.quality > 2 ? 2 : s.quality));
    r.core.SetPreferRayTracedShadows(s.rayTracing != 0);
    r.scene.SetShadowsOn(s.shadows != 0);
    r.post.SetAutoExposure(s.autoExposure != 0);
    r.core.SetUpscaling(r.dev, s.upscaler == kUpscalerFsr    ? ngg::core::UpscalerKind::Fsr
                               : s.upscaler == kUpscalerDlss ? ngg::core::UpscalerKind::Dlss
                                                             : ngg::core::UpscalerKind::None,
                        s.renderScale);
}

void ToDesc(const ngg::core::WeatherPreset& w, WeatherDesc& d)
{
    for (int k = 0; k < 3; ++k) d.sunDirection[k] = w.sky.sunDir[k], d.groundColour[k] = w.sky.groundColour[k];
    d.sunIntensity = w.sky.sunIntensity;
    d.turbidity = w.sky.turbidity;
    d.groundAlbedo = w.sky.groundAlbedo;
    d.cloudCover = w.cloudCover;
    d.fogVisibility = w.fogVisibility;
    d.fogHeight = w.fogHeight;
    d.rain = w.rain;
    d.snow = w.snow;
    d.exposure = w.sky.exposure;
}

// The frame's weather into weatherSky, when it changed: a preset exactly as ngg-api's load applied the file's default
// (ApplyWeather onto fresh SkyParams); a WeatherDesc as ngg_set_weather applied it.
void ApplyFrameWeather(Renderer& r, const FrameDesc& f)
{
    const auto& presets = r.scene.WeatherPresets();
    if (f.weatherPreset >= 0) {
        if (f.weatherPreset == r.appliedPreset || size_t(f.weatherPreset) >= presets.size()) return;
        r.weatherSky = ngg::core::SkyParams{};
        ngg::core::ApplyWeather(presets[size_t(f.weatherPreset)], r.weatherSky);
        r.appliedPreset = f.weatherPreset;
        return;
    }
    if (r.appliedPreset == -1 && !std::memcmp(&f.weather, &r.appliedDesc, sizeof f.weather)) return;
    ngg::core::WeatherPreset w;
    const WeatherDesc& d = f.weather;
    for (int k = 0; k < 3; ++k) w.sky.sunDir[k] = d.sunDirection[k], w.sky.groundColour[k] = d.groundColour[k];
    w.sky.sunIntensity = d.sunIntensity;
    w.sky.turbidity = d.turbidity;
    w.sky.groundAlbedo = d.groundAlbedo;
    w.cloudCover = d.cloudCover;
    w.fogVisibility = d.fogVisibility;
    w.fogHeight = d.fogHeight;
    w.rain = d.rain;
    w.snow = d.snow;
    w.sky.exposure = d.exposure;
    ngg::core::ApplyWeather(w, r.weatherSky);
    r.weatherSky.exposure = d.exposure;
    r.appliedPreset = -1;
    r.appliedDesc = d;
}

void ForgetScene(Renderer& r)
{
    r.sceneLoaded = false;
    r.byKey.clear();
    r.keyVisible.clear();
    r.models.clear();
    r.instances.clear();
    r.appliedPreset = -2;
    r.linesSet = false;
}

// ---- capture, as ngg-api's (the back buffer copied out while the list is open, written once it is done)
bool RecordCapture(Renderer& r)
{
    ID3D12Resource* bb = r.sc.CurrentBuffer();
    const D3D12_RESOURCE_DESC bd = bb->GetDesc();
    if (!r.captureBuf || r.captureFoot.Footprint.Width != (UINT)bd.Width || r.captureFoot.Footprint.Height != bd.Height) {
        UINT64 bytes = 0;
        r.dev.device->GetCopyableFootprints(&bd, 0, 1, 0, &r.captureFoot, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        r.captureBuf.Reset();
        if (FAILED(r.dev.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                                         nullptr, IID_PPV_ARGS(&r.captureBuf))))
            return r.Fail(Status::ErrDevice, "capture: readback buffer creation failed"), false;
    }
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    r.dev.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = r.captureBuf.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = r.captureFoot;
    src.pResource = bb;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    r.dev.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    return true;
}

void WriteCapture(Renderer& r)
{
    const UINT w = r.captureFoot.Footprint.Width, h = r.captureFoot.Footprint.Height, pitch = r.captureFoot.Footprint.RowPitch;
    const unsigned char* p = nullptr;
    if (FAILED(r.captureBuf->Map(0, nullptr, (void**)&p))) return void(r.Fail(Status::ErrDevice, "capture: readback would not map"));
    {
        // 24-bit BGR, top-down - from the R8G8B8A8 back buffer.
        const size_t stride = (size_t)w * 3;
        std::vector<unsigned char> bgr(stride * h);
        for (UINT y = 0; y < h; ++y) {
            const unsigned char* src = p + (size_t)y * pitch;
            unsigned char* dst = bgr.data() + y * stride;
            for (UINT x = 0; x < w; ++x) {
                dst[x * 3 + 0] = src[x * 4 + 2];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 0];
            }
        }
        if (!wrally::png::WriteBgr(r.capturePath, (int)w, (int)h, bgr.data(), stride))
            ngg::core::Log("wrenderer: capture: cannot write %s", r.capturePath.c_str());
    }
    D3D12_RANGE none = {0, 0};
    r.captureBuf->Unmap(0, &none);
}

// ---------------------------------------------------------------- the table

void WRALLY_CALL Destroy(Renderer* r)
{
    if (!r) return;
    r->dev.WaitForGpu();
    ngg::core::SetLogSink(nullptr);
    g_logHost = {};
    delete r;
}

const char* WRALLY_CALL LastError(Renderer* r) { return r ? r->error.c_str() : "no renderer"; }

Status WRALLY_CALL ApplySettings(Renderer* r, const RenderSettings* s)
{
    if (!r || !s) return Status::ErrInvalidArg;
    if (s->mode != WindowMode::Windowed) return r->Fail(Status::ErrNotSupported, "only windowed so far");
    r->settings = *s;
    Apply(*r);
    return Status::Ok;
}

Status WRALLY_CALL Resize(Renderer* r, uint32_t width, uint32_t height)
{
    if (!r || !width || !height) return Status::ErrInvalidArg;
    if (r->sc.Resize(r->dev, width, height)) r->core.OnResize(r->dev, r->sc.width, r->sc.height);
    return Status::Ok;
}

Status WRALLY_CALL LoadScene(Renderer* r, const void* bytes, uint64_t count, SceneHandle* out)
{
    if (out) *out = 0;
    if (!r || !out) return Status::ErrInvalidArg;
    if (!bytes || count < 16) return r->Fail(Status::ErrFormat, "load: empty buffer");
    ForgetScene(*r);
    if (!r->core.LoadFromMemory(r->dev, bytes, static_cast<size_t>(count)))
        return r->Fail(Status::ErrFormat, "load: the buffer was rejected - see the log for which chunk");
    r->sceneLoaded = true;
    for (uint32_t i = 0, n = r->scene.ObjectCount(); i < n; ++i)
        if (const int32_t k = r->scene.ObjectKey(i); k >= 0) r->byKey[k].push_back(i);
    // The file's default weather until a frame says otherwise - as ngg-api's load.
    r->weatherSky = ngg::core::SkyParams{};
    if (const ngg::core::WeatherPreset* w = r->scene.FindWeather(nullptr)) {
        ngg::core::ApplyWeather(*w, r->weatherSky);
        r->appliedPreset = 0;
    }
    r->error.clear();
    *out = 1;
    return Status::Ok;
}

Status WRALLY_CALL LoadSceneVariant(Renderer* r, const void* bytes, uint64_t count, const char* variant, SceneHandle* out)
{
    if (!r) return Status::ErrInvalidArg;
    r->scene.SetVariant(variant);
    const Status s = LoadScene(r, bytes, count, out);
    r->scene.SetVariant(nullptr);      // a later loadScene draws a file's main variant, as it always did
    return s;
}

void WRALLY_CALL UnloadScene(Renderer* r, SceneHandle s)
{
    if (r && s == 1) ForgetScene(*r);   // (the core keeps the geometry until the next load: nothing draws it)
}

bool SceneOk(Renderer* r, SceneHandle s) { return r && s == 1 && r->sceneLoaded; }

Status WRALLY_CALL SceneBounds(Renderer* r, SceneHandle s, float outMin[3], float outMax[3])
{
    if (!SceneOk(r, s) || !outMin || !outMax) return Status::ErrInvalidArg;
    r->scene.Bounds(outMin, outMax);
    return Status::Ok;
}

Status WRALLY_CALL SceneKeyStats(Renderer* r, SceneHandle s, uint32_t* keyed, uint32_t* keys)
{
    if (!SceneOk(r, s)) return Status::ErrInvalidArg;
    size_t n = 0;
    for (const auto& [k, objects] : r->byKey) n += objects.size();
    if (keyed) *keyed = static_cast<uint32_t>(n);
    if (keys) *keys = static_cast<uint32_t>(r->byKey.size());
    return Status::Ok;
}

uint32_t WRALLY_CALL WeatherPresetCount(Renderer* r, SceneHandle s)
{
    return SceneOk(r, s) ? static_cast<uint32_t>(r->scene.WeatherPresets().size()) : 0;
}

Status WRALLY_CALL WeatherPresetAt(Renderer* r, SceneHandle s, uint32_t index, WeatherDesc* out, char* name, uint32_t nameBytes)
{
    if (!SceneOk(r, s) || index >= r->scene.WeatherPresets().size()) return Status::ErrInvalidArg;
    const ngg::core::WeatherPreset& w = r->scene.WeatherPresets()[index];
    if (out) ToDesc(w, *out);
    if (name && nameBytes) strncpy_s(name, nameBytes, w.name.c_str(), _TRUNCATE);
    return Status::Ok;
}

Status WRALLY_CALL FindWeatherPreset(Renderer* r, SceneHandle s, const char* name, uint32_t* outIndex)
{
    if (!SceneOk(r, s) || !name || !outIndex) return Status::ErrInvalidArg;
    const auto& presets = r->scene.WeatherPresets();
    for (size_t i = 0; i < presets.size(); ++i)
        if (!_stricmp(presets[i].name.c_str(), name)) return *outIndex = static_cast<uint32_t>(i), Status::Ok;
    return Status::ErrInvalidArg;
}

Status WRALLY_CALL CreateTexture(Renderer* r, const void* dds, uint64_t bytes, TextureHandle* out)
{
    if (out) *out = 0;
    if (!r || !out || !dds || !bytes) return Status::ErrInvalidArg;
    if (!r->sceneLoaded) return r->Fail(Status::ErrInvalidArg, "createTexture: load a scene first (textures are its)");
    const uint32_t h = r->scene.AddTexture(r->dev.device.Get(), r->dev.queue.Get(), dds, static_cast<size_t>(bytes));
    if (!h) return r->Fail(Status::ErrFormat, "createTexture: the DDS could not be decoded or uploaded");
    *out = h;
    return Status::Ok;
}

Status WRALLY_CALL CreateTextureData(Renderer* r, const void* dds, uint64_t bytes, TextureHandle* out)
{
    if (out) *out = 0;
    if (!r || !out || !dds || !bytes) return Status::ErrInvalidArg;
    if (!r->sceneLoaded) return r->Fail(Status::ErrInvalidArg, "createTextureData: load a scene first (textures are its)");
    const uint32_t h = r->scene.AddTexture(r->dev.device.Get(), r->dev.queue.Get(), dds, static_cast<size_t>(bytes), true);
    if (!h) return r->Fail(Status::ErrFormat, "createTextureData: the DDS could not be decoded or uploaded");
    *out = h;
    return Status::Ok;
}

Status WRALLY_CALL CreateTextureRgba(Renderer* r, const void* rgba, uint32_t width, uint32_t height, TextureHandle* out)
{
    if (out) *out = 0;
    if (!r || !out) return Status::ErrInvalidArg;
    std::string e;
    const uint32_t slot = r->ui.AddTexture(r->dev, rgba, width, height, e);
    if (!slot) return r->Fail(Status::ErrDevice, e);
    *out = kUiTextureBit | slot;
    return Status::Ok;
}

void WRALLY_CALL ReleaseTexture(Renderer* r, TextureHandle t)
{
    if (r && (t & kUiTextureBit)) r->ui.ReleaseTexture(t & ~kUiTextureBit);   // (scene textures go with the scene)
}

Status WRALLY_CALL UpdateTextureRgba(Renderer* r, TextureHandle t, const void* rgba)
{
    if (!r || !rgba || !(t & kUiTextureBit)) return Status::ErrInvalidArg;
    std::string e;
    if (!r->ui.UpdateTexture(r->dev, t & ~kUiTextureBit, rgba, e)) return r->Fail(Status::ErrInvalidArg, e);
    return Status::Ok;
}

Status WRALLY_CALL CreateModel(Renderer* r, const ModelDesc* d, ModelHandle* out)
{
    if (out) *out = 0;
    if (!r || !d || !out) return Status::ErrInvalidArg;
    if (!r->sceneLoaded) return r->Fail(Status::ErrInvalidArg, "createModel: load a scene first (models are its)");
    if (!d->nodeCount || !d->nodes || (d->meshCount && !d->meshes) || (d->materialCount && !d->materials))
        return r->Fail(Status::ErrInvalidArg, "createModel: a model needs nodes, and its arrays");
    for (uint32_t i = 0; i < d->nodeCount; ++i)
        if (d->nodes[i].parent >= int32_t(i)) return r->Fail(Status::ErrInvalidArg, "createModel: a parent after its child");
    for (uint32_t i = 0; i < d->meshCount; ++i)
        if (d->meshes[i].node >= d->nodeCount || d->meshes[i].material >= d->materialCount)
            return r->Fail(Status::ErrInvalidArg, "createModel: a mesh's node or material out of range");

    Model m;
    m.nodes.assign(d->nodes, d->nodes + d->nodeCount);
    // The materials as ngg-api made them: add (two texture handles and the flags), then the descriptor read back,
    // changed and written (ngg_get_material / ngg_set_material).
    std::vector<uint32_t> mats;
    // The SRV heap rebuilt once for all of them, not twice a material (a car's dozen: ~0.2 s at once).
    r->scene.BeginMaterialBatch();
    struct Batch {
        Renderer* r;
        ~Batch() { r->scene.EndMaterialBatch(r->dev.device.Get()); }
    } batch{r};
    for (uint32_t i = 0; i < d->materialCount; ++i) {
        const MaterialDesc& in = d->materials[i];
        // The interface's flags as the format's: alpha test and two-sided are the same bits; blend is bit 2 here and
        // bit 3 there (the format's bit 2 was v2's single-stage, left unused). Blend wins over the alpha test.
        static_assert(kMaterialAlphaTest == ngg::kMaterialAlphaTest && kMaterialTwoSided == ngg::kMaterialTwoSided,
                      "renderer.h's material flags and ngg_format.h's");
        constexpr uint32_t kMask = ngg::kMaterialAlphaTest | ngg::kMaterialTwoSided | ngg::kMaterialBlend;
        uint32_t flags = in.flags & (ngg::kMaterialAlphaTest | ngg::kMaterialTwoSided);
        if (in.flags & kMaterialBlend) flags = (flags & ~ngg::kMaterialAlphaTest) | ngg::kMaterialBlend;
        const uint32_t h = r->scene.AddMaterial(r->dev.device.Get(), in.baseColour, in.detail, flags);
        if (!h) return r->Fail(Status::ErrDevice, "createModel: the descriptor heap could not be rebuilt");
        ngg::core::Scene::MaterialSlot s;
        if (!r->scene.GetMaterial(h - 1, &s)) return r->Fail(Status::ErrInternal, "createModel: a material vanished");
        auto slot = [](TextureHandle t, uint32_t current) { return t ? t - 1 : current; };
        s.tex[2] = slot(in.normal, s.tex[2]);
        s.tex[3] = slot(in.orm, s.tex[3]);
        s.tex[4] = slot(in.emissive, s.tex[4]);
        s.flags &= kMask;
        for (int k = 0; k < 4; ++k) s.baseColourFactor[k] = in.baseColourFactor[k];
        s.roughness = in.roughness;
        s.metallic = in.metallic;
        s.normalScale = in.normalScale;
        s.occlusionStrength = in.occlusionStrength;
        s.emissiveStrength = in.emissiveStrength;
        s.alphaCutoff = in.alphaCutoff;
        s.clearCoat = in.clearCoat;
        if (!r->scene.SetMaterial(r->dev.device.Get(), h - 1, s)) return r->Fail(Status::ErrDevice, "createModel: material update failed");
        mats.push_back(h);
    }
    for (uint32_t i = 0; i < d->meshCount; ++i) {
        const ModelMesh& mm = d->meshes[i];
        static_assert(sizeof(Vertex) == sizeof(ngg::Vertex), "the vertices are uploaded raw");
        const uint32_t h = r->scene.AddMesh(r->dev.device.Get(), reinterpret_cast<const ngg::Vertex*>(mm.vertices),
                                            mm.vertexCount, mm.indices, mm.indexCount);
        if (!h) continue;   // (ngg-api's caller skipped a rejected mesh too)
        m.meshes.push_back({h, mats[mm.material], mm.node});
    }
    r->models.push_back(std::move(m));
    *out = static_cast<ModelHandle>(r->models.size());
    return Status::Ok;
}

Model* FindModel(Renderer* r, ModelHandle h)
{
    return r && h && h <= r->models.size() && !r->models[h - 1].released ? &r->models[h - 1] : nullptr;
}

void WRALLY_CALL ReleaseModel(Renderer* r, ModelHandle h)
{
    if (Model* m = FindModel(r, h)) m->released = true;   // (its meshes stay in the pool until the scene goes)
}

Status WRALLY_CALL CreateInstance(Renderer* r, ModelHandle h, InstanceHandle* out)
{
    if (out) *out = 0;
    Model* m = FindModel(r, h);
    if (!m || !out) return Status::ErrInvalidArg;
    Instance in;
    in.model = h;
    in.alive = true;
    for (const Model::Mesh& mesh : m->meshes) {
        const uint32_t o = r->scene.AddInstance(mesh.mesh, mesh.material, 0, nullptr);
        if (o == kUnknownInstance) return r->Fail(Status::ErrInternal, "createInstance: the scene refused a mesh");
        in.objects.push_back(o);
    }
    ++m->refs;
    r->instances.push_back(std::move(in));
    *out = static_cast<InstanceHandle>(r->instances.size());
    return Status::Ok;
}

void WRALLY_CALL DestroyInstance(Renderer* r, InstanceHandle h)
{
    if (!r || !h || h > r->instances.size() || !r->instances[h - 1].alive) return;
    Instance& in = r->instances[h - 1];
    for (uint32_t o : in.objects) r->scene.RemoveInstance(o);
    if (Model* m = FindModel(r, in.model)) --m->refs;
    in.alive = false;
}

// One instance's placement this frame: each mesh at its node's world (local * parent's ... ) * the instance's world.
void Place(Renderer& r, const InstanceUpdate& u)
{
    if (!u.instance || u.instance > r.instances.size() || !r.instances[u.instance - 1].alive) return;
    Instance& in = r.instances[u.instance - 1];
    const Model& m = r.models[in.model - 1];
    for (int k = 0; k < 3; ++k) in.origin[k] = u.world[12 + k];
    const bool posed = u.nodes && u.nodeCount == m.nodes.size();
    std::vector<float> nodeWorld(16 * m.nodes.size());
    for (size_t n = 0; n < m.nodes.size(); ++n) {
        const float* local = posed ? u.nodes + 16 * n : m.nodes[n].local;
        if (m.nodes[n].parent < 0) Mul44(local, u.world, &nodeWorld[16 * n]);
        else Mul44(local, &nodeWorld[16 * size_t(m.nodes[n].parent)], &nodeWorld[16 * n]);
    }
    for (size_t i = 0; i < m.meshes.size() && i < in.objects.size(); ++i)
        r.scene.SetObjectTransform(in.objects[i], &nodeWorld[16 * m.meshes[i].node]);
    const bool visible = u.visible != 0;
    if (visible != in.visible) {
        for (uint32_t o : in.objects) r.scene.SetObjectVisible(o, visible);
        in.visible = visible;
    }
}

void MoveStageObject(Renderer& r, const StageObjectUpdate& u)
{
    const auto it = r.byKey.find(u.key);
    if (it == r.byKey.end()) return;
    for (uint32_t o : it->second) r.scene.SetObjectTransform(o, u.delta);
    bool& visible = r.keyVisible.try_emplace(u.key, true).first->second;
    if ((u.visible != 0) != visible) {
        visible = u.visible != 0;
        for (uint32_t o : it->second) r.scene.SetObjectVisible(o, visible);
    }
}

Status WRALLY_CALL RenderFrame(Renderer* r, const FrameDesc* f)
{
    if (!r || !f) return Status::ErrInvalidArg;
    if (!r->sc.swap) return r->Fail(Status::ErrDevice, "renderFrame: no swap chain");
    const bool world = !(f->flags & kFrameNoWorld) && f->scene == 1 && r->sceneLoaded;
    if (world) {
        for (uint32_t i = 0; f->stageObjects && i < f->stageObjectCount; ++i) MoveStageObject(*r, f->stageObjects[i]);
        for (uint32_t i = 0; f->instances && i < f->instanceCount; ++i) Place(*r, f->instances[i]);
        // The car the cube probe is drawn around (core/probe.h).
        if (f->reflectionFocus && f->reflectionFocus <= r->instances.size() && r->instances[f->reflectionFocus - 1].alive) {
            const Instance& in = r->instances[f->reflectionFocus - 1];
            r->scene.SetReflectionFocus(in.objects.data(), static_cast<uint32_t>(in.objects.size()), in.origin);
        } else {
            r->scene.SetReflectionFocus(nullptr, 0, nullptr);
        }
        ApplyFrameWeather(*r, *f);
        r->scene.SetWetness(f->wetness);
        // The frame's local lights, as the core takes them (NGG design 5.4 N2).
        {
            std::vector<ngg::core::LightDesc> lights;
            lights.reserve(f->lightCount);
            for (uint32_t i = 0; f->lights && i < f->lightCount; ++i) {
                const LightDesc& in = f->lights[i];
                ngg::core::LightDesc l;
                l.type = in.type == kLightBeam ? ngg::core::LightDesc::kBeam
                       : in.type == kLightSpot ? ngg::core::LightDesc::kSpot : ngg::core::LightDesc::kPoint;
                for (int k = 0; k < 3; ++k) {
                    l.position[k] = in.position[k];
                    l.direction[k] = in.direction[k];
                    l.up[k] = in.up[k];
                    l.colour[k] = in.colour[k];
                }
                l.intensity = in.intensity;
                l.range = in.range;
                l.radius = in.radius;
                l.innerDeg = in.innerDeg;
                l.outerDeg = in.outerDeg;
                l.shadows = (in.flags & kLightNoShadows) == 0;
                l.lobeCount = (std::min)(in.lobeCount, kMaxLightLobes);
                for (uint32_t k = 0; k < l.lobeCount; ++k) {
                    const LightLobe& o = in.lobes[k];
                    l.lobes[k] = { o.candela, o.halfWidthDeg, o.halfHeightDeg, o.yawDeg, o.pitchDeg, o.sharpness };
                }
                lights.push_back(l);
            }
            r->scene.SetLights(lights);
        }
        if (f->lineVertexCount && f->lines) {
            static_assert(sizeof(LineVertex) == sizeof(ngg::core::Scene::OverlayVertex), "uploaded raw");
            r->scene.SetOverlayLines(r->dev.device.Get(), 0, reinterpret_cast<const ngg::core::Scene::OverlayVertex*>(f->lines),
                                     f->lineVertexCount & ~1u, 0);
            r->linesSet = true;
        } else if (r->linesSet) {
            r->scene.SetOverlayLines(r->dev.device.Get(), 0, nullptr, 0, 0);
            r->linesSet = false;
        }
        if (f->camera.cut) r->core.ResetHistory();
    }

    if (!r->dev.Begin()) return r->Fail(Status::ErrDevice, "renderFrame: the command list would not open");
    D3D12_RESOURCE_BARRIER toRt = {};
    toRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRt.Transition.pResource = r->sc.CurrentBuffer();
    toRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    r->dev.list->ResourceBarrier(1, &toRt);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = r->sc.CurrentRtv();
    // The frame's sky: the weather's, and over its sun the time and place when the frame gives them (NGG design 5.4)
    // - from the weather's sky every frame, so the night's scaling of the light never compounds.
    ngg::core::SkyParams sky = r->weatherSky;
    // The sun's elevation for the stats: the weather's sun (sunDir its light's direction), unless a time sets it below.
    r->sunElevationDeg = static_cast<float>(std::asin(std::clamp(-sky.sunDir[1], -1.0f, 1.0f)) * 57.29578);
    if (world && f->skyTime.set) {
        const SkyTimeDesc& t = f->skyTime;
        ngg::core::SkyTime st;
        st.year = t.year, st.month = t.month, st.day = t.day;
        st.localHour = t.localHour, st.utcOffset = t.utcOffset;
        st.latitude = t.latitude, st.longitude = t.longitude, st.northDeg = t.northDeg;
        const ngg::core::SkyBodies b = ngg::core::ApplySkyTime(st, sky);
        r->sunElevationDeg = b.sunElevationDeg;
        if (std::memcmp(&t, &r->loggedSkyTime, sizeof t)) {   // said once per time, not per frame
            const int minutes = static_cast<int>(t.localHour * 60.0f + 0.5f);
            ngg::core::Log("sky: %04d-%02d-%02d %02d:%02d (UTC%+.1f) at %.2f, %.2f: sun %.1f deg up, moon %.1f deg up "
                           "(%.0f %% lit), the light: %s", t.year, t.month, t.day, minutes / 60, minutes % 60, t.utcOffset,
                           t.latitude, t.longitude, b.sunElevationDeg, b.moonElevationDeg, 100.0f * b.moonIlluminated,
                           sky.lightIsMoon > 0.5f ? "the moon" : "the sun");
            r->loggedSkyTime = t;
        }
    }
    if (world) {
        // The frame, exactly as ngg-api recorded it: sky on, the settings' TAA, ACES.
        r->core.SetSkyOn(true);
        r->core.SetTaa(r->settings.taa != 0);
        r->core.SetCurve(ngg::core::PostFx::kCurveAces);
        r->core.RecordFrame(r->dev, rtv, f->camera.view, f->camera.proj, r->sc.width, r->sc.height, sky);
        // (Depth-tested against the scene's depth, which is the render size: not drawn while upscaling.)
        if (r->core.RenderWidth() == r->sc.width && r->core.RenderHeight() == r->sc.height)
            r->scene.RenderEditorOverlay(r->dev.list.Get(), rtv, f->camera.view, f->camera.proj, r->sc.width, r->sc.height);
    } else {
        const float black[4] = {0, 0, 0, 1};
        r->dev.list->ClearRenderTargetView(rtv, black, 0, nullptr);
    }
    r->ui.Record(r->dev, rtv, r->sc.width, r->sc.height, f->ui);

    D3D12_RESOURCE_BARRIER toPresent = toRt;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    const bool capture = !r->capturePath.empty() && RecordCapture(*r);
    if (capture) toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    r->dev.list->ResourceBarrier(1, &toPresent);
    r->dev.EndAndWait();
    if (capture) WriteCapture(*r);
    r->capturePath.clear();
    if (world) {
        r->scene.LogProbe();
        r->scene.ReadOverlayStats();
    }
    r->dev.DrainMessages();
    return r->dev.CheckRemoved("renderFrame") ? Status::ErrDevice : Status::Ok;
}

Status WRALLY_CALL Present(Renderer* r)
{
    if (!r || !r->sc.swap) return Status::ErrInvalidArg;
    return FAILED(r->sc.swap->Present(r->settings.vsync ? 1 : 0, 0)) ? Status::ErrDevice : Status::Ok;
}

Status WRALLY_CALL Stats(Renderer* r, RenderStats* out)
{
    if (!r || !out) return Status::ErrInvalidArg;
    *out = {};
    ngg::core::Rt& rt = r->core.Raytracer();
    rt.ReadStats();
    out->passMs = (float)r->scene.LastPassMs();
    out->frameMs = (float)r->core.LastFrameMs();
    out->sunElevationDeg = r->sunElevationDeg;
    out->envMs = (float)r->scene.LastEnvMs();
    out->aerialMs = (float)r->scene.LastAerialMs();
    out->rtActive = r->core.RayTracedShadowsActive() ? 1u : 0u;
    out->rtAvailable = r->core.RayTracingAvailable() ? 1u : 0u;
    if (out->rtActive) {
        out->rtMs = (float)rt.LastMs();
        out->aoMs = (float)rt.AoMs();
        out->denoiseMs = (float)rt.DenoiseMs();
        out->lightsMs = (float)rt.LightsMs();
        out->lightCount = (uint32_t)r->scene.GetLights().Count();
        out->rtPixels = rt.TracedPixels();
        out->rtShadowed = rt.ShadowedPixels();
    } else if (const char* why = r->core.RtFallbackReason()) {
        strncpy_s(out->rtFallback, sizeof out->rtFallback, why, _TRUNCATE);
    }
    out->quality = (uint32_t)r->core.GetQuality();
    out->taa = r->post.Taa() ? 1u : 0u;
    out->shadows = r->scene.ShadowsOn() ? 1u : 0u;
    out->ambientOcclusion = r->core.AoApplied() ? 1u : 0u;
    strncpy_s(out->aoSource, sizeof out->aoSource, r->core.AoSourceName(), _TRUNCATE);
    if (!out->rtActive || r->core.GetAoSource() != ngg::core::AoSource::Traced) out->aoMs = (float)r->core.AoMs();
    out->autoExposure = r->post.AutoExposure() ? 1u : 0u;
    out->debugView = (uint32_t)r->scene.DebugMode();
    out->exposure = r->post.LastExposure();
    out->meteredEv = r->post.LastMeteredEv();
    out->adaptedEv = r->post.LastAdaptedEv();
    out->renderWidth = r->core.RenderWidth();
    out->renderHeight = r->core.RenderHeight();
    strncpy_s(out->upscaler, sizeof out->upscaler, r->core.UpscalerName(), _TRUNCATE);
    out->upscalersAvailable = 1u << kUpscalerNone;
    if (r->core.UpscalerAvailable(ngg::core::UpscalerKind::Fsr))  out->upscalersAvailable |= 1u << kUpscalerFsr;
    if (r->core.UpscalerAvailable(ngg::core::UpscalerKind::Dlss)) out->upscalersAvailable |= 1u << kUpscalerDlss;
    return Status::Ok;
}

Status WRALLY_CALL CaptureFrame(Renderer* r, const char* path)
{
    if (!r || !path) return Status::ErrInvalidArg;
    r->capturePath = path;
    return Status::Ok;
}

Status WRALLY_CALL SetDebugView(Renderer* r, int32_t mode)
{
    if (!r) return Status::ErrInvalidArg;
    r->scene.SetDebugMode(mode);
    return Status::Ok;
}

const char* WRALLY_CALL DebugViewName(int32_t mode) { return ngg::core::Scene::DebugModeName(mode); }

Status WRALLY_CALL SetLegacyProfile(Renderer* r, int32_t mode)
{
    if (!r) return Status::ErrInvalidArg;
    r->scene.SetLegacyProfile(mode);
    return Status::Ok;
}

// ------------------------------------------------------- the editor's table (renderer_edit.h)
//
// ngg-api's editor calls (plugin-rbr ngg/api/ngg_api.cpp), over the same core calls, addressed by the scene's object
// index as ngg-api's instance handles were. The overlay's layer 0 is FrameDesc::lines', so the editor's layer L is
// the core's L + 1.

static_assert(sizeof(EditLabel) == sizeof(ngg::core::Scene::Label), "labels are handed over raw");
static_assert(kEditNoDepth == ngg::core::Scene::kOverlayNoDepth && kEditScreenSpace == ngg::core::Scene::kOverlayScreenSpace &&
                  kEditTextShadow == ngg::core::Scene::kOverlayTextShadow,
              "renderer_edit.h's flags and ngg-core's");

bool EditOk(Renderer* r) { return r && r->sceneLoaded; }
bool ObjectOk(Renderer* r, uint32_t o) { return EditOk(r) && o < r->scene.ObjectCount(); }

uint32_t ToTextureHandle(uint32_t index) { return index == ngg::kNoTexture ? 0u : index + 1u; }
uint32_t ToTextureIndex(TextureHandle h) { return h ? h - 1u : ngg::kNoTexture; }

uint32_t WRALLY_CALL EditObjectCount(Renderer* r) { return EditOk(r) ? r->scene.ObjectCount() : 0; }
uint32_t WRALLY_CALL EditVisibleObjectCount(Renderer* r) { return EditOk(r) ? r->scene.VisibleObjectCount() : 0; }

Status WRALLY_CALL EditSetObjectVisible(Renderer* r, uint32_t o, uint32_t visible)
{
    if (!ObjectOk(r, o)) return Status::ErrInvalidArg;
    return r->scene.SetObjectVisible(o, visible != 0) ? Status::Ok : r->Fail(Status::ErrInvalidArg, "setObjectVisible: refused");
}

Status WRALLY_CALL EditSetObjectTransform(Renderer* r, uint32_t o, const float delta[16])
{
    if (!ObjectOk(r, o) || !delta) return Status::ErrInvalidArg;
    return r->scene.SetObjectTransform(o, delta) ? Status::Ok : r->Fail(Status::ErrInvalidArg, "setObjectTransform: refused");
}

Status WRALLY_CALL EditObjectBounds(Renderer* r, uint32_t o, float outMin[3], float outMax[3])
{
    if (!ObjectOk(r, o) || !outMin || !outMax) return Status::ErrInvalidArg;
    return r->scene.ObjectBounds(o, outMin, outMax) ? Status::Ok : Status::ErrInvalidArg;
}

Status WRALLY_CALL EditInstanceObjects(Renderer* r, InstanceHandle h, uint32_t* out, uint32_t capacity, uint32_t* count)
{
    if (count) *count = 0;
    if (!r || !h || h > r->instances.size() || !r->instances[h - 1].alive) return Status::ErrInvalidArg;
    const std::vector<uint32_t>& objects = r->instances[h - 1].objects;
    if (count) *count = static_cast<uint32_t>(objects.size());
    for (uint32_t i = 0; out && i < capacity && i < objects.size(); ++i) out[i] = objects[i];
    return Status::Ok;
}

Status WRALLY_CALL EditFocusPoint(Renderer* r, float out[3])
{
    if (!EditOk(r) || !out) return Status::ErrInvalidArg;
    return r->scene.FocusPoint(out) ? Status::Ok : Status::ErrInvalidArg;
}

Status WRALLY_CALL EditSetSelection(Renderer* r, const uint32_t* objects, uint32_t count)
{
    if (!r || (count && (!objects || !r->sceneLoaded))) return Status::ErrInvalidArg;
    return r->scene.SetSelection(objects, count) ? Status::Ok : r->Fail(Status::ErrInvalidArg, "setSelection: refused");
}

Status WRALLY_CALL EditSetOverlayLines(Renderer* r, uint32_t layer, const LineVertex* v, uint32_t count, uint32_t flags)
{
    if (!r || layer >= kEditOverlayLayers || (count && !v)) return Status::ErrInvalidArg;
    if (count & 1u) return r->Fail(Status::ErrInvalidArg, "setOverlayLines: an odd vertex count (a segment is a pair)");
    if (!r->scene.SetOverlayLines(r->dev.device.Get(), layer + 1, reinterpret_cast<const ngg::core::Scene::OverlayVertex*>(v),
                                  count, flags))
        return r->Fail(Status::ErrDevice, "setOverlayLines: the layer could not be uploaded");
    return Status::Ok;
}

void WRALLY_CALL EditClearOverlays(Renderer* r)
{
    if (!r) return;
    r->scene.ClearOverlays();
    r->linesSet = false;
}

Status WRALLY_CALL EditSetFont(Renderer* r, const void* rgba, uint32_t width, uint32_t height, uint32_t columns,
                               uint32_t rows, uint32_t firstChar)
{
    if (!r || !rgba || !width || !height || !columns || !rows) return Status::ErrInvalidArg;
    if (!r->scene.SetFont(r->dev.device.Get(), r->dev.queue.Get(), rgba, width, height, columns, rows, firstChar))
        return r->Fail(Status::ErrDevice, "setFont: the atlas could not be uploaded");
    return Status::Ok;
}

Status WRALLY_CALL EditSetLabels(Renderer* r, uint32_t layer, const EditLabel* labels, uint32_t count, const char* blob,
                                 uint32_t blobBytes, float pixelHeight, uint32_t flags)
{
    if (!r || layer >= kEditLabelLayers || (count && (!labels || !blob))) return Status::ErrInvalidArg;
    if (count && !r->scene.HasFont()) return r->Fail(Status::ErrInvalidArg, "setLabels: no font yet (setFont)");
    if (!r->scene.SetLabels(r->dev.device.Get(), layer, reinterpret_cast<const ngg::core::Scene::Label*>(labels), count,
                            blob, blobBytes, pixelHeight, flags))
        return r->Fail(Status::ErrInvalidArg, "setLabels: refused - pixelHeight must be positive, every text inside the blob");
    return Status::Ok;
}

Status WRALLY_CALL EditOverlayStatsOf(Renderer* r, EditOverlayStats* out)
{
    if (!r || !out) return Status::ErrInvalidArg;
    out->selectionVisible = r->scene.SelectionPixelsVisible();
    out->selectionOccluded = r->scene.SelectionPixelsOccluded();
    out->linesVisible = r->scene.OverlayPixelsVisible();
    out->linesOccluded = r->scene.OverlayPixelsOccluded();
    out->labelsVisible = r->scene.LabelPixelsVisible();
    out->labelsOccluded = r->scene.LabelPixelsOccluded();
    return Status::Ok;
}

uint32_t WRALLY_CALL EditMaterialCount(Renderer* r) { return EditOk(r) ? r->scene.MaterialCount() : 0; }

Status WRALLY_CALL EditGetMaterial(Renderer* r, uint32_t h, MaterialDesc* out)
{
    ngg::core::Scene::MaterialSlot s;
    if (!EditOk(r) || !h || !out || !r->scene.GetMaterial(h - 1, &s)) return Status::ErrInvalidArg;
    *out = {};
    out->baseColour = ToTextureHandle(s.tex[0]);
    out->detail = ToTextureHandle(s.tex[1]);
    out->normal = ToTextureHandle(s.tex[2]);
    out->orm = ToTextureHandle(s.tex[3]);
    out->emissive = ToTextureHandle(s.tex[4]);
    // The format's blend bit is 3 and the interface's 2 (as createModel maps them); what the renderer derives from
    // the slots stays inside it.
    out->flags = (s.flags & (ngg::kMaterialAlphaTest | ngg::kMaterialTwoSided)) |
                 ((s.flags & ngg::kMaterialBlend) ? kMaterialBlend : 0u);
    for (int k = 0; k < 4; ++k) out->baseColourFactor[k] = s.baseColourFactor[k];
    out->roughness = s.roughness;
    out->metallic = s.metallic;
    out->normalScale = s.normalScale;
    out->occlusionStrength = s.occlusionStrength;
    out->emissiveStrength = s.emissiveStrength;
    out->alphaCutoff = s.alphaCutoff;
    out->clearCoat = s.clearCoat;
    return Status::Ok;
}

Status WRALLY_CALL EditSetMaterial(Renderer* r, uint32_t h, const MaterialDesc* in)
{
    ngg::core::Scene::MaterialSlot s;
    if (!EditOk(r) || !h || !in || !r->scene.GetMaterial(h - 1, &s)) return Status::ErrInvalidArg;
    s.tex[0] = ToTextureIndex(in->baseColour);
    s.tex[1] = ToTextureIndex(in->detail);
    s.tex[2] = ToTextureIndex(in->normal);
    s.tex[3] = ToTextureIndex(in->orm);
    s.tex[4] = ToTextureIndex(in->emissive);
    uint32_t flags = in->flags & (ngg::kMaterialAlphaTest | ngg::kMaterialTwoSided);
    if (in->flags & kMaterialBlend) flags = (flags & ~ngg::kMaterialAlphaTest) | ngg::kMaterialBlend;   // as createModel
    s.flags = flags;
    for (int k = 0; k < 4; ++k) s.baseColourFactor[k] = in->baseColourFactor[k];
    s.roughness = in->roughness;
    s.metallic = in->metallic;
    s.normalScale = in->normalScale;
    s.occlusionStrength = in->occlusionStrength;
    s.emissiveStrength = in->emissiveStrength;
    s.alphaCutoff = in->alphaCutoff;
    s.clearCoat = in->clearCoat;
    return r->scene.SetMaterial(r->dev.device.Get(), h - 1, s) ? Status::Ok
                                                               : r->Fail(Status::ErrDevice, "setMaterial: update failed");
}

Status WRALLY_CALL EditMaterialName(Renderer* r, uint32_t h, char* buf, uint32_t bytes)
{
    if (!EditOk(r) || !h || h > r->scene.MaterialCount() || !buf || !bytes) return Status::ErrInvalidArg;
    const char* name = r->scene.MaterialName(h - 1);
    strncpy_s(buf, bytes, name ? name : "", _TRUNCATE);
    return Status::Ok;
}

Status WRALLY_CALL EditObjectMaterial(Renderer* r, uint32_t o, uint32_t* out)
{
    if (out) *out = 0;
    uint32_t m = 0;
    if (!ObjectOk(r, o) || !out || !r->scene.ObjectMaterial(o, &m)) return Status::ErrInvalidArg;
    *out = m + 1;
    return Status::Ok;
}

Status WRALLY_CALL EditSetLightMode(Renderer* r, int32_t mode)
{
    if (!r) return Status::ErrInvalidArg;
    r->scene.SetLightMode(mode);
    return Status::Ok;
}

const char* WRALLY_CALL EditLightModeName(int32_t mode) { return ngg::core::Scene::LightModeName(mode); }

Status WRALLY_CALL EditSetCull(Renderer* r, uint32_t on)
{
    if (!r) return Status::ErrInvalidArg;
    r->scene.SetCull(on != 0);
    return Status::Ok;
}

Status WRALLY_CALL EditProbePixel(Renderer* r, int32_t x, int32_t y)
{
    if (!r || x < 0 || y < 0) return Status::ErrInvalidArg;
    r->scene.SetProbe(x, y);
    return Status::Ok;
}

const RendererEditApi kEditApi = {
    &EditObjectCount,     &EditVisibleObjectCount, &EditSetObjectVisible, &EditSetObjectTransform, &EditObjectBounds,
    &EditInstanceObjects, &EditFocusPoint,         &EditSetSelection,     &EditSetOverlayLines,    &EditClearOverlays,
    &EditSetFont,         &EditSetLabels,          &EditOverlayStatsOf,   &EditMaterialCount,      &EditGetMaterial,
    &EditSetMaterial,     &EditMaterialName,       &EditObjectMaterial,   &EditSetLightMode,       &EditLightModeName,
    &EditSetCull,         &EditProbePixel};

Status WRALLY_CALL EditApi(Renderer* r, uint32_t editVer, const RendererEditApi** out)
{
    if (out) *out = nullptr;
    if (!r || !out) return Status::ErrInvalidArg;
    if (editVer != kRendererEditVer)
        return r->Fail(Status::ErrVersion, "editApi: this module implements editor table " + std::to_string(kRendererEditVer) +
                                               ", asked for " + std::to_string(editVer));
    *out = &kEditApi;
    return Status::Ok;
}

const RendererApi kApi = {&Destroy,       &LastError,         &ApplySettings,  &Resize,         &LoadScene,
                          &UnloadScene,   &SceneBounds,       &SceneKeyStats,  &WeatherPresetCount,
                          &WeatherPresetAt, &FindWeatherPreset, &CreateTexture, &CreateTextureRgba,
                          &ReleaseTexture, &CreateModel,      &ReleaseModel,   &CreateInstance, &DestroyInstance,
                          &RenderFrame,   &Present,           &Stats,          &CaptureFrame,   &SetDebugView,
                          &DebugViewName, &SetLegacyProfile, &UpdateTextureRgba, &CreateTextureData, &EditApi,
                          &LoadSceneVariant};

} // namespace

WRALLY_EXPORT uint32_t WRALLY_CALL GetWInterfaceVer(ModuleKind kind)
{
    return kind == ModuleKind::Renderer ? kRendererInterfaceVer : 0;
}

WRALLY_EXPORT Status WRALLY_CALL InitWInterface(uint32_t gameVersion, const StructHeader* in, StructHeader* out)
{
    if (!in || !out) return Status::ErrInvalidArg;
    if (in->kind != ModuleKind::Renderer || out->kind != ModuleKind::Renderer) return Status::ErrKind;
    if (in->interfaceVer != kRendererInterfaceVer || out->interfaceVer != kRendererInterfaceVer) return Status::ErrVersion;
    if (in->size != sizeof(RendererInput) || out->size != sizeof(RendererOutput)) return Status::ErrStructSize;
    if (gameVersion < MakeVersion(0, 1, 0)) return Status::ErrGameVersion;
    const auto& input = *reinterpret_cast<const RendererInput*>(in);
    auto& output = *reinterpret_cast<RendererOutput*>(out);
    if (!input.window || !input.width || !input.height) return Status::ErrInvalidArg;
    if (g_logHost.log) return Status::ErrNotSupported;   // one renderer per process (ngg-core's log sink is global)
    try {
        auto r = std::make_unique<Renderer>();
        r->host = input.host;
        r->hwnd = static_cast<HWND>(input.window);
        g_logHost = input.host;
        ngg::core::SetLogSink(&CoreLog);
        auto fail = [&](const char* what) {
            ngg::core::Log("wrenderer: %s", what);
            ngg::core::SetLogSink(nullptr);
            g_logHost = {};
            return Status::ErrDevice;
        };
        LUID luid{input.adapterLuidLow, input.adapterLuidHigh};
        const bool pick = input.adapterLuidLow || input.adapterLuidHigh;
        if (!r->dev.Create(pick ? &luid : nullptr, (input.flags & kRendererDebugLayer) != 0)) return fail("D3D12 device creation failed");
        if (!r->sc.Create(r->dev, r->hwnd, input.width, input.height)) return fail("swap chain creation failed");
        if (!r->core.Create(r->dev, kBackBufferFormat, r->sc.width, r->sc.height, !(input.flags & kRendererNoRayTracing)))
            return fail("renderer creation failed");
        std::string e;
        if (!r->ui.Create(r->dev, kBackBufferFormat, e)) return fail(e.c_str());
        r->settings = input.settings;
        if (r->settings.mode != WindowMode::Windowed) r->settings.mode = WindowMode::Windowed;
        Apply(*r);
        // ngg-api's environment switches, after the settings (a quality tier sets the AO's rays): NGG_DEBUG_VIEW,
        // NGG_AO_RAYS, NGG_LIGHT_MODE, NGG_LEGACY_PROFILE; NGG_AO_SOURCE=screen (a ray-traced frame takes XeGTAO's AO),
        // NGG_GTAO_QUALITY=0..3 (over the tiers), NGG_GTAO_RADIUS=<m>, NGG_GTAO_NORMALS=prepass|depth,
        // NGG_GTAO_THIN=0..0.7, NGG_GTAO_DENOISE=1..3.
        if (const char* dv = std::getenv("NGG_DEBUG_VIEW")) r->scene.SetDebugMode(std::atoi(dv));
        if (const char* ar = std::getenv("NGG_AO_RAYS")) r->core.SetAoRays((uint32_t)std::atoi(ar));
        if (const char* as = std::getenv("NGG_AO_SOURCE"))
            r->core.SetAoSource(std::strcmp(as, "screen") == 0 ? ngg::core::AoSource::ScreenSpace
                                                               : ngg::core::AoSource::Traced);
        if (const char* gq = std::getenv("NGG_GTAO_QUALITY")) r->core.ScreenSpaceAo().ForceQuality(std::atoi(gq));
        if (const char* gr = std::getenv("NGG_GTAO_RADIUS")) r->core.ScreenSpaceAo().SetRadius((float)std::atof(gr));
        if (const char* gn = std::getenv("NGG_GTAO_NORMALS"))
            r->core.ScreenSpaceAo().SetDepthNormals(std::strcmp(gn, "prepass") != 0);
        if (const char* gt = std::getenv("NGG_GTAO_THIN")) r->core.ScreenSpaceAo().SetThinOccluders((float)std::atof(gt));
        if (const char* gd = std::getenv("NGG_GTAO_DENOISE")) r->core.ScreenSpaceAo().SetDenoisePasses(std::atoi(gd));
        if (const char* lm = std::getenv("NGG_LIGHT_MODE")) r->scene.SetLightMode(std::atoi(lm));
        if (const char* lp = std::getenv("NGG_LEGACY_PROFILE")) r->scene.SetLegacyProfile(std::atoi(lp));
        ngg::core::Log("wrenderer: %ux%u ready", input.width, input.height);

        output.info = {};
        std::snprintf(output.info.id, sizeof output.info.id, "wrenderer");
        std::snprintf(output.info.name, sizeof output.info.name, "WRenderer");
        output.info.moduleVersion = kModuleVersion;
        output.info.minGameVersion = MakeVersion(0, 1, 0);
        output.info.buildHash = WRENDERER_BUILD_HASH;
        output.info.caps = r->core.RayTracingAvailable() ? kRendererCapRayTracing : 0;
        output.api = &kApi;
        output.instance = r.release();
        return Status::Ok;
    } catch (...) {
        ngg::core::SetLogSink(nullptr);
        g_logHost = {};
        return Status::ErrInternal;
    }
}
