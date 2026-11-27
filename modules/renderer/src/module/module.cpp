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
#include <array>
#include <chrono>
#include <map>
#include <optional>
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
#include "pipeline_cache.h"
#include "shaders.h"
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
    float world[16] = {};            // Place's last InstanceUpdate: world, node poses (none: the rest pose)
    std::vector<float> nodes;
    bool placed = false;
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

// Everything bound to the D3D12 device. Made at InitWInterface and made again after a device removal (Recover, NGG
// design §10.6): nothing else in Renderer holds a device object, so dropping this drops the dead device whole. Members
// go in reverse: the UI and the frame's parts, then the swap chain, then the device.
struct Gpu {
    ngg::core::Device dev;
    SwapChain sc;
    ngg::core::Renderer core;
    ngg::core::Scene& scene = core.GetScene();
    ngg::core::PostFx& post = core.Post();
    wrally::render::UiPass ui;
    ComPtr<ID3D12Resource> captureBuf;
};

// A createModel's description, owned: what the call copied, kept to make the model again on a new device.
struct OwnedModel {
    std::vector<ModelNode> nodes;
    std::vector<MaterialDesc> materials;
    struct Mesh {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        uint32_t material, node;
    };
    std::vector<Mesh> meshes;
    std::vector<ModelMesh> views;   // Desc's, into meshes

    explicit OwnedModel(const ModelDesc& d)
        : nodes(d.nodes, d.nodes + d.nodeCount), materials(d.materials, d.materials + d.materialCount)
    {
        for (uint32_t i = 0; i < d.meshCount; ++i) {
            const ModelMesh& m = d.meshes[i];
            meshes.push_back({std::vector<Vertex>(m.vertices, m.vertices + m.vertexCount),
                              std::vector<uint32_t>(m.indices, m.indices + m.indexCount), m.material, m.node});
        }
    }
    ModelDesc Desc()
    {
        views.clear();
        for (const Mesh& m : meshes)
            views.push_back({m.vertices.data(), uint32_t(m.vertices.size()), m.indices.data(), uint32_t(m.indices.size()),
                             m.material, m.node});
        return {nodes.data(), uint32_t(nodes.size()), views.data(), uint32_t(views.size()), materials.data(),
                uint32_t(materials.size())};
    }
};

// What the client handed the renderer that lives on the GPU, kept so that a new device can be given all of it again
// (Recover). Two kinds:
//   - `ops`: the loaded scene's creations and the placements' edits, IN ORDER - replayed in that order, they hand out
//     the same texture, material, model and instance handles and the same object indices, so the client's handles and
//     the editor's indices stay valid. Started afresh by every loadScene. The big bytes (the .ngg, a texture's DDS) are
//     kept only under keepCopies; otherwise the client is asked for them again (RendererInput::reload). Models are
//     kept whole (small).
//   - the setters' last values, applied after the ops: a later call replaces an earlier one, so a drag in the editor
//     does not grow it.
// UI textures are the renderer's, not the scene's: kept by slot with their last pixels.
struct Journal {
    struct Op {
        enum Kind { Texture, TextureData, Model, ReleaseModel, Instance, DestroyInstance, AddPlacements, SetPlacements,
                    RemovePlacements } kind;
        uint32_t handle = 0;                     // ReleaseModel's, Instance's model, DestroyInstance's
        std::vector<uint8_t> bytes;              // a texture's DDS; empty: the client gives it again (kReloadTexture)
        std::shared_ptr<OwnedModel> model;
        std::vector<EditPlacement> placements;
        std::vector<uint32_t> handles;           // Set/RemovePlacements'
    };
    bool scene = false;                          // a scene is loaded (bytes + variant)
    std::vector<uint8_t> sceneBytes;             // empty: the client gives them again (kReloadScene)
    std::string variant;
    std::vector<Op> ops;

    std::map<uint32_t, std::array<float, 16>> objectTransform;   // EditSetObjectTransform, by object
    std::map<uint32_t, bool> objectVisible;                     // EditSetObjectVisible
    std::map<uint32_t, MaterialDesc> materials;                 // EditSetMaterial, by handle
    bool selectionSet = false;
    std::vector<uint32_t> selection;
    struct Lines { std::vector<LineVertex> v; uint32_t flags; };
    std::map<uint32_t, Lines> lines;                            // EditSetOverlayLines, by layer
    struct Font { std::vector<uint8_t> rgba; uint32_t width, height, columns, rows, firstChar; };
    std::unique_ptr<Font> font;
    struct Labels { std::vector<EditLabel> labels; std::string blob; float pixelHeight; uint32_t flags; };
    std::map<uint32_t, Labels> labels;                          // EditSetLabels, by layer
    std::optional<int32_t> debugView, legacyProfile, lightMode;
    std::optional<bool> cull;
    std::optional<std::pair<int32_t, int32_t>> probe;
    struct UiTexture { uint32_t width, height; std::vector<uint8_t> rgba; };
    std::map<uint32_t, UiTexture> uiTextures;                   // by the UI pass's slot

    void ForgetScene()
    {
        scene = false;
        sceneBytes.clear();
        sceneBytes.shrink_to_fit();
        variant.clear();
        ops.clear();
        objectTransform.clear();
        objectVisible.clear();
        materials.clear();
        selectionSet = false;
        selection.clear();
    }
};

} // namespace

namespace wrally::iface {

struct Renderer {
    HostServices host{};
    HWND hwnd = nullptr;
    std::unique_ptr<Gpu> g;                // never null once InitWInterface has succeeded
    ngg::core::SkyParams weatherSky;       // the weather's sky; a frame's is this plus its time (FrameDesc::skyTime)
    RenderSettings settings{};
    std::string error;

    // ---- device loss (NGG design §10.6)
    uint32_t initFlags = 0;                // RendererInput::flags, for the device made again
    LUID luid{};
    bool pickLuid = false;                 // the client named an adapter
    Journal journal;
    RendererReload reload{};               // the client's bytes again (RendererInput::reload)
    bool keepCopies = true;                // keepCopies: the big bytes kept here (default: no reload callback)
    bool replaying = false;                // Recover is calling the table: nothing journaled
    bool lost = false;                     // removed; the next call that needs the device makes a new one
    bool inFlight = false;                 // a frame submitted since the GPU was last waited for (Settle)
    bool broken = false;                   // a new device was made but the rest of the renderer was not: ErrFault
    uint32_t generation = 0;               // devices made after the first (RenderStats::deviceGeneration)
    std::chrono::steady_clock::time_point lastFailed{};   // a new device's last failed try
    uint64_t frames = 0;                   // renderFrame calls, for NGG_TEST_DEVICE_REMOVED
    std::vector<uint64_t> testRemovals;    // NGG_TEST_DEVICE_REMOVED's frames
    UINT width = 0, height = 0;            // the window's size the swap chain was last made or resized to

    bool sceneLoaded = false;
    int32_t appliedPreset = -2;            // the weather preset in weatherSky (-1 a WeatherDesc, -2 none yet)
    WeatherDesc appliedDesc{};
    SkyTimeDesc loggedSkyTime{};           // the last FrameDesc::skyTime the log reported
    float sunElevationDeg = 90.0f;         // the last frame's sun over the horizon (RenderStats)
    std::unordered_map<int32_t, std::vector<uint32_t>> byKey;   // NGGK key -> the scene's objects
    std::unordered_map<int32_t, bool> keyVisible;
    std::unordered_map<int32_t, StageObjectUpdate> stageMoved;   // each key's last move (FrameDesc::stageObjects)
    std::vector<Model> models;             // by handle - 1
    std::vector<Instance> instances;       // by handle - 1
    bool linesSet = false;

    std::string capturePath;
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
    r.g->core.SetQuality(static_cast<ngg::core::Quality>(s.quality > 2 ? 2 : s.quality));
    if (s.ambientOcclusionOff) r.g->core.SetAmbientOcclusion(false);   // after the tier, which sets its own
    r.g->core.SetPreferRayTracedShadows(s.rayTracing != 0);
    r.g->scene.SetShadowsOn(s.shadows != 0);
    r.g->scene.SetModelLodScale(s.vegetationDetail >= kVegetationHigh     ? 3.0f
                             : s.vegetationDetail == kVegetationMedium ? 2.0f
                                                                       : 1.0f);
    r.g->post.SetAutoExposure(s.autoExposure != 0);
    r.g->core.SetUpscaling(r.g->dev, s.upscaler == kUpscalerFsr      ? ngg::core::UpscalerKind::Fsr
                               : s.upscaler == kUpscalerDlss   ? ngg::core::UpscalerKind::Dlss
                               : s.upscaler == kUpscalerDlssRr ? ngg::core::UpscalerKind::DlssRr
                                                               : ngg::core::UpscalerKind::None,
                        s.renderScale);
    // NGG_DENOISER=nrd|ours over the settings, for A/B (NGG design 10.8).
    static const char* const envDenoiser = std::getenv("NGG_DENOISER");
    const bool nrd = envDenoiser ? std::strcmp(envDenoiser, "nrd") == 0 : s.denoiser == kDenoiserNrd;
    r.g->core.SetDenoiser(r.g->dev, nrd ? ngg::core::Denoiser::Nrd : ngg::core::Denoiser::Ours);
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
    d.windDirectionDeg = w.windDirectionDeg;
    d.windSpeed = w.windSpeed;
    d.gustiness = w.gustiness;
}

// The frame's weather into weatherSky, when it changed: a preset exactly as ngg-api's load applied the file's default
// (ApplyWeather onto fresh SkyParams); a WeatherDesc as ngg_set_weather applied it.
void ApplyFrameWeather(Renderer& r, const FrameDesc& f)
{
    const auto& presets = r.g->scene.WeatherPresets();
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
    r.stageMoved.clear();
    r.models.clear();
    r.instances.clear();
    r.appliedPreset = -2;
    r.linesSet = false;
}

// ---- device loss (NGG design §10.6)
//
// Detected where D3D12 says so: CheckRemoved after every submit and at the end of renderFrame, a failed Begin, a
// present's DXGI_ERROR_DEVICE_REMOVED / _RESET, and GetDeviceRemovedReason at the start of every call that needs the
// device (NeedDevice). DRED's report is logged once, by CheckRemoved, as always. Then the next call that needs the
// device makes a new one (Recover): everything device-bound (Gpu) dropped - the dead device has to be released
// before a new one can be made, D3D12 hands out one device per adapter - and made again on the same window, the
// adapter chosen again; then the journal replayed. Every handle the client holds stays valid.

bool Recover(Renderer& r);

// The dead device noticed (once: CheckRemoved logs DRED's record). True if it is gone.
bool Removed(Renderer& r, const char* where)
{
    if (!r.lost && r.g && r.g->dev.device && r.g->dev.CheckRemoved(where)) r.lost = true;
    return r.lost;
}

// Before a call that needs the device: Ok, or why not. After a removal, a new device first (a second or two: the
// scene loaded again); a renderer whose new device failed tries again at most once a second.
// With frames in flight (device.h) a call between frames that replaces what the GPU may still be drawing - a scene,
// a texture and its view, a model's materials, the editor's overlays, labels and font, the settings' passes - waits
// for it first: once per batch of such calls (loads and editor work, never the game's steady frame).
void Settle(Renderer& r)
{
    if (!r.inFlight || r.lost || !r.g || !r.g->dev.queue) return;
    r.inFlight = false;
    if (r.g->dev.frames > 1) r.g->dev.WaitForGpu();
}

Status NeedDevice(Renderer* r)
{
    if (!r) return Status::ErrInvalidArg;
    if (r->replaying) return Status::Ok;
    if (r->broken) return Status::ErrFault;
    if (!r->lost && r->g->dev.device && r->g->dev.device->GetDeviceRemovedReason() != S_OK)
        Removed(*r, "before a call");
    if (r->lost && !Recover(*r)) return Status::ErrDevice;
    return r->broken ? Status::ErrFault : Status::Ok;
}

// A call that failed: if the device went in it, a new device and the call once more.
template <class F> Status Retry(Renderer* r, F&& call)
{
    Status s = call();
    if (s != Status::Ok && !r->replaying && Removed(*r, "in a call") && NeedDevice(r) == Status::Ok) s = call();
    return s;
}

// NGG_TEST_DEVICE_REMOVED=<frame>[,<frame>...]: ID3D12Device5::RemoveDevice in renderFrame number <frame> (1 the
// first, counted by each renderer), after the frame is recorded and before it is submitted - a device removal on
// demand, to test the recovery. Read when the renderer is made.
std::vector<uint64_t> TestRemovals()
{
    // (The process's environment block, not the CRT's copy: a test sets it after the module's CRT read its own.)
    std::vector<uint64_t> v;
    char e[256] = {};
    if (GetEnvironmentVariableA("NGG_TEST_DEVICE_REMOVED", e, sizeof e))
        for (const char* p = e; *p;) {
            char* end = nullptr;
            const uint64_t n = std::strtoull(p, &end, 10);
            if (end == p) break;
            v.push_back(n);
            p = *end == ',' ? end + 1 : end;
        }
    return v;
}

// The environment's development switches over the settings (a quality tier sets the AO's rays) - ngg-api's:
// NGG_DEBUG_VIEW, NGG_AO_RAYS, NGG_LIGHT_MODE, NGG_LEGACY_PROFILE; NGG_AO_SOURCE=screen (a ray-traced frame takes
// XeGTAO's AO), NGG_GTAO_QUALITY=0..3 (over the tiers), NGG_GTAO_RADIUS=<m>, NGG_GTAO_NORMALS=prepass|depth,
// NGG_GTAO_THIN=0..0.7, NGG_GTAO_DENOISE=1..3.
void ApplyEnvironment(Renderer& r)
{
    ngg::core::Renderer& core = r.g->core;
    ngg::core::Scene& scene = r.g->scene;
    if (const char* dv = std::getenv("NGG_DEBUG_VIEW")) scene.SetDebugMode(std::atoi(dv));
    if (const char* ar = std::getenv("NGG_AO_RAYS")) core.SetAoRays((uint32_t)std::atoi(ar));
    if (const char* as = std::getenv("NGG_AO_SOURCE"))
        core.SetAoSource(std::strcmp(as, "screen") == 0 ? ngg::core::AoSource::ScreenSpace : ngg::core::AoSource::Traced);
    if (const char* gq = std::getenv("NGG_GTAO_QUALITY")) core.ScreenSpaceAo().ForceQuality(std::atoi(gq));
    if (const char* gr = std::getenv("NGG_GTAO_RADIUS")) core.ScreenSpaceAo().SetRadius((float)std::atof(gr));
    if (const char* gn = std::getenv("NGG_GTAO_NORMALS")) core.ScreenSpaceAo().SetDepthNormals(std::strcmp(gn, "prepass") != 0);
    if (const char* gt = std::getenv("NGG_GTAO_THIN")) core.ScreenSpaceAo().SetThinOccluders((float)std::atof(gt));
    if (const char* gd = std::getenv("NGG_GTAO_DENOISE")) core.ScreenSpaceAo().SetDenoisePasses(std::atoi(gd));
    if (const char* lm = std::getenv("NGG_LIGHT_MODE")) scene.SetLightMode(std::atoi(lm));
    if (const char* lp = std::getenv("NGG_LEGACY_PROFILE")) scene.SetLegacyProfile(std::atoi(lp));
}

// The D3D12 debug layer: the client's flag (kRendererDebugLayer), or NGG_DEBUG_LAYER=1 (DrainMessages logs it).
bool DebugLayer(const Renderer& r)
{
    const char* e = std::getenv("NGG_DEBUG_LAYER");
    return (r.initFlags & kRendererDebugLayer) != 0 || (e && e[0] == '1');
}

// The device-bound part, on r.g (fresh): the device on the client's adapter (any, on a new device after a loss, if
// that one is gone: a driver update gives it a new LUID), the pipeline cache, the swap chain on the window, the frame's
// parts, the UI pass, the settings. "" or what failed.
std::string MakeGpu(Renderer& r, UINT width, UINT height, bool again)
{
    Gpu& g = *r.g;
    if (!g.dev.Create(r.pickLuid ? &r.luid : nullptr, DebugLayer(r))) {
        if (!again || !r.pickLuid) return "D3D12 device creation failed";
        r.g = std::make_unique<Gpu>();
        if (!r.g->dev.Create(nullptr, DebugLayer(r))) return "D3D12 device creation failed (the adapter asked for, and any)";
        ngg::core::Log("wrenderer: the adapter the game asked for is gone - on the best one there is");
    }
    Gpu& n = *r.g;
    // The pipeline cache (pipeline_cache.h) in the game's cache folder for this module (cache/wrenderer).
    const char* cacheDir = r.host.files.cacheDir ? r.host.files.cacheDir(r.host.ctx) : nullptr;
    ngg::core::OpenPipelineCache(n.dev.device.Get(), cacheDir ? cacheDir : "");
    if (!n.sc.Create(n.dev, r.hwnd, width, height)) return "swap chain creation failed";
    r.width = n.sc.width, r.height = n.sc.height;
    if (!n.core.Create(n.dev, kBackBufferFormat, n.sc.width, n.sc.height, !(r.initFlags & kRendererNoRayTracing)))
        return "renderer creation failed";
    std::string e;
    if (!n.ui.Create(n.dev, kBackBufferFormat, e)) return e;
    Apply(r);
    ApplyEnvironment(r);
    return "";
}

// ---- capture, as ngg-api's (the back buffer copied out while the list is open, written once it is done)
bool RecordCapture(Renderer& r)
{
    ID3D12Resource* bb = r.g->sc.CurrentBuffer();
    const D3D12_RESOURCE_DESC bd = bb->GetDesc();
    if (!r.g->captureBuf || r.captureFoot.Footprint.Width != (UINT)bd.Width || r.captureFoot.Footprint.Height != bd.Height) {
        UINT64 bytes = 0;
        r.g->dev.device->GetCopyableFootprints(&bd, 0, 1, 0, &r.captureFoot, nullptr, nullptr, &bytes);
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
        r.g->captureBuf.Reset();
        if (FAILED(r.g->dev.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                                         nullptr, IID_PPV_ARGS(&r.g->captureBuf))))
            return r.Fail(Status::ErrDevice, "capture: readback buffer creation failed"), false;
    }
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    r.g->dev.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = r.g->captureBuf.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = r.captureFoot;
    src.pResource = bb;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    r.g->dev.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    return true;
}

void WriteCapture(Renderer& r)
{
    const UINT w = r.captureFoot.Footprint.Width, h = r.captureFoot.Footprint.Height, pitch = r.captureFoot.Footprint.RowPitch;
    const unsigned char* p = nullptr;
    if (FAILED(r.g->captureBuf->Map(0, nullptr, (void**)&p))) return void(r.Fail(Status::ErrDevice, "capture: readback would not map"));
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
    r.g->captureBuf->Unmap(0, &none);
}

// ---------------------------------------------------------------- the table

void WRALLY_CALL Destroy(Renderer* r)
{
    if (!r) return;
    if (r->g->dev.queue) r->g->dev.WaitForGpu();   // (none after a device loss no new device came for)
    ngg::core::LogPipelineStats("since");
    ngg::core::ClosePipelineCache();   // written if pipelines were added (XeGTAO's qualities, made when first used)
    ngg::core::SetLogSink(nullptr);
    g_logHost = {};
    delete r;
}

const char* WRALLY_CALL LastError(Renderer* r) { return r ? r->error.c_str() : "no renderer"; }

Status WRALLY_CALL ApplySettings(Renderer* r, const RenderSettings* s)
{
    if (!r || !s) return Status::ErrInvalidArg;
    if (s->mode != WindowMode::Windowed) return r->Fail(Status::ErrNotSupported, "only windowed so far");
    r->settings = *s;                  // (a new device takes these)
    if (const Status d = NeedDevice(r); d != Status::Ok) return d;
    Settle(*r);
    Apply(*r);
    return Status::Ok;
}

Status WRALLY_CALL Resize(Renderer* r, uint32_t width, uint32_t height)
{
    if (!r || !width || !height) return Status::ErrInvalidArg;
    if (const Status d = NeedDevice(r); d != Status::Ok) return d;
    if (r->g->sc.Resize(r->g->dev, width, height)) r->g->core.OnResize(r->g->dev, r->g->sc.width, r->g->sc.height);
    r->width = r->g->sc.width, r->height = r->g->sc.height;
    return Status::Ok;
}

Status LoadSceneNow(Renderer* r, const void* bytes, uint64_t count, SceneHandle* out)
{
    ForgetScene(*r);
    if (!r->g->core.LoadFromMemory(r->g->dev, bytes, static_cast<size_t>(count)))
        return r->Fail(Status::ErrFormat, "load: the buffer was rejected - see the log for which chunk");
    r->sceneLoaded = true;
    for (uint32_t i = 0, n = r->g->scene.ObjectCount(); i < n; ++i)
        if (const int32_t k = r->g->scene.ObjectKey(i); k >= 0) r->byKey[k].push_back(i);
    // The file's default weather until a frame says otherwise - as ngg-api's load.
    r->weatherSky = ngg::core::SkyParams{};
    if (const ngg::core::WeatherPreset* w = r->g->scene.FindWeather(nullptr)) {
        ngg::core::ApplyWeather(*w, r->weatherSky);
        r->appliedPreset = 0;
    }
    r->error.clear();
    *out = 1;
    return Status::Ok;
}

Status WRALLY_CALL LoadScene(Renderer* r, const void* bytes, uint64_t count, SceneHandle* out)
{
    if (out) *out = 0;
    if (!r || !out) return Status::ErrInvalidArg;
    if (!bytes || count < 16) return r->Fail(Status::ErrFormat, "load: empty buffer");
    if (const Status d = NeedDevice(r); d != Status::Ok) return d;
    Settle(*r);
    if (!r->replaying) r->journal.ForgetScene();
    const Status s = Retry(r, [&] { return LoadSceneNow(r, bytes, count, out); });
    if (s == Status::Ok && !r->replaying) {
        // For a new device: the bytes kept here (keepCopies), or asked for again (kReloadScene).
        r->journal.scene = true;
        if (r->keepCopies)
            r->journal.sceneBytes.assign(static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + count);
    }
    return s;
}

Status WRALLY_CALL LoadSceneVariant(Renderer* r, const void* bytes, uint64_t count, const char* variant, SceneHandle* out)
{
    if (!r) return Status::ErrInvalidArg;
    if (const Status d = NeedDevice(r); d != Status::Ok) return d;
    Settle(*r);
    r->g->scene.SetVariant(variant);
    const Status s = LoadScene(r, bytes, count, out);
    r->g->scene.SetVariant(nullptr);      // a later loadScene draws a file's main variant, as it always did
    if (s == Status::Ok && !r->replaying) r->journal.variant = variant ? variant : "";
    return s;
}

void WRALLY_CALL UnloadScene(Renderer* r, SceneHandle s)
{
    if (r && s == 1) {
        ForgetScene(*r);   // (the core keeps the geometry until the next load: nothing draws it)
        if (!r->replaying) r->journal.ForgetScene();
    }
}

bool SceneOk(Renderer* r, SceneHandle s) { return r && s == 1 && r->sceneLoaded; }

Status WRALLY_CALL SceneBounds(Renderer* r, SceneHandle s, float outMin[3], float outMax[3])
{
    if (!SceneOk(r, s) || !outMin || !outMax) return Status::ErrInvalidArg;
    r->g->scene.Bounds(outMin, outMax);
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
    return SceneOk(r, s) ? static_cast<uint32_t>(r->g->scene.WeatherPresets().size()) : 0;
}

Status WRALLY_CALL WeatherPresetAt(Renderer* r, SceneHandle s, uint32_t index, WeatherDesc* out, char* name, uint32_t nameBytes)
{
    if (!SceneOk(r, s) || index >= r->g->scene.WeatherPresets().size()) return Status::ErrInvalidArg;
    const ngg::core::WeatherPreset& w = r->g->scene.WeatherPresets()[index];
    if (out) ToDesc(w, *out);
    if (name && nameBytes) strncpy_s(name, nameBytes, w.name.c_str(), _TRUNCATE);
    return Status::Ok;
}

Status WRALLY_CALL FindWeatherPreset(Renderer* r, SceneHandle s, const char* name, uint32_t* outIndex)
{
    if (!SceneOk(r, s) || !name || !outIndex) return Status::ErrInvalidArg;
    const auto& presets = r->g->scene.WeatherPresets();
    for (size_t i = 0; i < presets.size(); ++i)
        if (!_stricmp(presets[i].name.c_str(), name)) return *outIndex = static_cast<uint32_t>(i), Status::Ok;
    return Status::ErrInvalidArg;
}

// createTexture (data false) and createTextureData (true).
Status AddSceneTexture(Renderer* r, const void* dds, uint64_t bytes, TextureHandle* out, bool data)
{
    const char* what = data ? "createTextureData" : "createTexture";
    if (out) *out = 0;
    if (!r || !out || !dds || !bytes) return Status::ErrInvalidArg;
    if (!r->sceneLoaded) return r->Fail(Status::ErrInvalidArg, std::string(what) + ": load a scene first (textures are its)");
    if (const Status d = NeedDevice(r); d != Status::Ok) return d;
    Settle(*r);
    const Status s = Retry(r, [&] {
        const uint32_t h = r->g->scene.AddTexture(r->g->dev.device.Get(), r->g->dev.queue.Get(), dds, static_cast<size_t>(bytes), data);
        if (!h) return r->Fail(Status::ErrFormat, std::string(what) + ": the DDS could not be decoded or uploaded");
        *out = h;
        return Status::Ok;
    });
    if (s == Status::Ok && !r->replaying) {
        Journal::Op op{data ? Journal::Op::TextureData : Journal::Op::Texture};
        op.handle = *out;
        if (r->keepCopies) op.bytes.assign(static_cast<const uint8_t*>(dds), static_cast<const uint8_t*>(dds) + bytes);
        r->journal.ops.push_back(std::move(op));
    }
    return s;
}

Status WRALLY_CALL CreateTexture(Renderer* r, const void* dds, uint64_t bytes, TextureHandle* out)
{
    return AddSceneTexture(r, dds, bytes, out, false);
}

Status WRALLY_CALL CreateTextureData(Renderer* r, const void* dds, uint64_t bytes, TextureHandle* out)
{
    return AddSceneTexture(r, dds, bytes, out, true);
}

Status WRALLY_CALL CreateTextureRgba(Renderer* r, const void* rgba, uint32_t width, uint32_t height, TextureHandle* out)
{
    if (out) *out = 0;
    if (!r || !out) return Status::ErrInvalidArg;
    if (const Status d = NeedDevice(r); d != Status::Ok) return d;
    Settle(*r);
    const Status s = Retry(r, [&] {
        std::string e;
        const uint32_t slot = r->g->ui.AddTexture(r->g->dev, rgba, width, height, e);
        if (!slot) return r->Fail(Status::ErrDevice, e);
        *out = kUiTextureBit | slot;
        return Status::Ok;
    });
    if (s == Status::Ok) {
        const uint8_t* p = static_cast<const uint8_t*>(rgba);
        r->journal.uiTextures[*out & ~kUiTextureBit] = {width, height, std::vector<uint8_t>(p, p + size_t(width) * height * 4)};
    }
    return s;
}

void WRALLY_CALL ReleaseTexture(Renderer* r, TextureHandle t)
{
    if (!r || !(t & kUiTextureBit)) return;   // (scene textures go with the scene)
    r->g->ui.ReleaseTexture(t & ~kUiTextureBit);
    r->journal.uiTextures.erase(t & ~kUiTextureBit);
}

Status WRALLY_CALL UpdateTextureRgba(Renderer* r, TextureHandle t, const void* rgba)
{
    if (!r || !rgba || !(t & kUiTextureBit)) return Status::ErrInvalidArg;
    // The pixels kept first: a new device made in the call below takes them.
    const auto it = r->journal.uiTextures.find(t & ~kUiTextureBit);
    if (it != r->journal.uiTextures.end()) std::memcpy(it->second.rgba.data(), rgba, it->second.rgba.size());
    if (const Status d = NeedDevice(r); d != Status::Ok) return d;
    return Retry(r, [&] {
        std::string e;
        if (!r->g->ui.UpdateTexture(r->g->dev, t & ~kUiTextureBit, rgba, e)) return r->Fail(Status::ErrInvalidArg, e);
        return Status::Ok;
    });
}

Status CreateModelNow(Renderer* r, const ModelDesc* d, ModelHandle* out)
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
    r->g->scene.BeginMaterialBatch();
    struct Batch {
        Renderer* r;
        ~Batch() { r->g->scene.EndMaterialBatch(r->g->dev.device.Get()); }
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
        const uint32_t h = r->g->scene.AddMaterial(r->g->dev.device.Get(), in.baseColour, in.detail, flags);
        if (!h) return r->Fail(Status::ErrDevice, "createModel: the descriptor heap could not be rebuilt");
        ngg::core::Scene::MaterialSlot s;
        if (!r->g->scene.GetMaterial(h - 1, &s)) return r->Fail(Status::ErrInternal, "createModel: a material vanished");
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
        if (!r->g->scene.SetMaterial(r->g->dev.device.Get(), h - 1, s)) return r->Fail(Status::ErrDevice, "createModel: material update failed");
        mats.push_back(h);
    }
    for (uint32_t i = 0; i < d->meshCount; ++i) {
        const ModelMesh& mm = d->meshes[i];
        static_assert(sizeof(Vertex) == sizeof(ngg::Vertex), "the vertices are uploaded raw");
        const uint32_t h = r->g->scene.AddMesh(r->g->dev.device.Get(), reinterpret_cast<const ngg::Vertex*>(mm.vertices),
                                            mm.vertexCount, mm.indices, mm.indexCount);
        if (!h) continue;   // (ngg-api's caller skipped a rejected mesh too)
        m.meshes.push_back({h, mats[mm.material], mm.node});
    }
    r->models.push_back(std::move(m));
    *out = static_cast<ModelHandle>(r->models.size());
    return Status::Ok;
}

Status WRALLY_CALL CreateModel(Renderer* r, const ModelDesc* d, ModelHandle* out)
{
    if (out) *out = 0;
    if (const Status s = NeedDevice(r); s != Status::Ok) return s;
    Settle(*r);
    const Status s = Retry(r, [&] { return CreateModelNow(r, d, out); });
    if (s == Status::Ok && !r->replaying) {
        Journal::Op op{Journal::Op::Model};
        op.handle = *out;
        op.model = std::make_shared<OwnedModel>(*d);
        r->journal.ops.push_back(std::move(op));
    }
    return s;
}

Model* FindModel(Renderer* r, ModelHandle h)
{
    return r && h && h <= r->models.size() && !r->models[h - 1].released ? &r->models[h - 1] : nullptr;
}

void WRALLY_CALL ReleaseModel(Renderer* r, ModelHandle h)
{
    Model* m = FindModel(r, h);
    if (!m) return;
    m->released = true;   // (its meshes stay in the pool until the scene goes)
    if (!r->replaying) {
        Journal::Op op{Journal::Op::ReleaseModel};
        op.handle = h;
        r->journal.ops.push_back(std::move(op));
    }
}

Status CreateInstanceNow(Renderer* r, ModelHandle h, InstanceHandle* out)
{
    if (out) *out = 0;
    Model* m = FindModel(r, h);
    if (!m || !out) return Status::ErrInvalidArg;
    Instance in;
    in.model = h;
    in.alive = true;
    for (const Model::Mesh& mesh : m->meshes) {
        const uint32_t o = r->g->scene.AddInstance(mesh.mesh, mesh.material, 0, nullptr);
        if (o == kUnknownInstance) return r->Fail(Status::ErrInternal, "createInstance: the scene refused a mesh");
        in.objects.push_back(o);
    }
    ++m->refs;
    r->instances.push_back(std::move(in));
    *out = static_cast<InstanceHandle>(r->instances.size());
    return Status::Ok;
}

Status WRALLY_CALL CreateInstance(Renderer* r, ModelHandle h, InstanceHandle* out)
{
    if (out) *out = 0;
    if (const Status s = NeedDevice(r); s != Status::Ok) return s;
    const Status s = CreateInstanceNow(r, h, out);   // (CPU only: nothing to retry)
    if (s == Status::Ok && !r->replaying) {
        Journal::Op op{Journal::Op::Instance};
        op.handle = h;
        r->journal.ops.push_back(std::move(op));
    }
    return s;
}

void WRALLY_CALL DestroyInstance(Renderer* r, InstanceHandle h)
{
    if (!r || !h || h > r->instances.size() || !r->instances[h - 1].alive) return;
    Instance& in = r->instances[h - 1];
    if (!r->lost)   // (no device and no scene: only the journal hears of it)
        for (uint32_t o : in.objects) r->g->scene.RemoveInstance(o);
    if (Model* m = FindModel(r, in.model)) --m->refs;
    in.alive = false;
    if (!r->replaying) {
        Journal::Op op{Journal::Op::DestroyInstance};
        op.handle = h;
        r->journal.ops.push_back(std::move(op));
    }
}

// One instance's placement this frame: each mesh at its node's world (local * parent's ... ) * the instance's world.
void Place(Renderer& r, const InstanceUpdate& u)
{
    if (!u.instance || u.instance > r.instances.size() || !r.instances[u.instance - 1].alive) return;
    Instance& in = r.instances[u.instance - 1];
    const Model& m = r.models[in.model - 1];
    for (int k = 0; k < 3; ++k) in.origin[k] = u.world[12 + k];
    // Kept: an instance the client does not place again keeps this, also on a new device (Recover).
    std::memcpy(in.world, u.world, sizeof in.world);
    in.nodes.assign(u.nodes ? u.nodes : nullptr, u.nodes ? u.nodes + 16 * size_t(u.nodeCount) : nullptr);
    in.placed = true;
    const bool posed = u.nodes && u.nodeCount == m.nodes.size();
    std::vector<float> nodeWorld(16 * m.nodes.size());
    for (size_t n = 0; n < m.nodes.size(); ++n) {
        const float* local = posed ? u.nodes + 16 * n : m.nodes[n].local;
        if (m.nodes[n].parent < 0) Mul44(local, u.world, &nodeWorld[16 * n]);
        else Mul44(local, &nodeWorld[16 * size_t(m.nodes[n].parent)], &nodeWorld[16 * n]);
    }
    for (size_t i = 0; i < m.meshes.size() && i < in.objects.size(); ++i)
        r.g->scene.SetObjectTransform(in.objects[i], &nodeWorld[16 * m.meshes[i].node]);
    const bool visible = u.visible != 0;
    if (visible != in.visible) {
        for (uint32_t o : in.objects) r.g->scene.SetObjectVisible(o, visible);
        in.visible = visible;
    }
}

void MoveStageObject(Renderer& r, const StageObjectUpdate& u)
{
    const auto it = r.byKey.find(u.key);
    if (it == r.byKey.end()) return;
    r.stageMoved[u.key] = u;   // (for a new device: Recover)
    for (uint32_t o : it->second) r.g->scene.SetObjectTransform(o, u.delta);
    bool& visible = r.keyVisible.try_emplace(u.key, true).first->second;
    if ((u.visible != 0) != visible) {
        visible = u.visible != 0;
        for (uint32_t o : it->second) r.g->scene.SetObjectVisible(o, visible);
    }
}

Status WRALLY_CALL RenderFrame(Renderer* r, const FrameDesc* f)
{
    if (!r || !f) return Status::ErrInvalidArg;
    // After a device removal: a new device, the scene and everything made in it again, before this frame (§10.6).
    if (const Status d = NeedDevice(r); d != Status::Ok) return d;
    if (!r->g->sc.swap) return r->Fail(Status::ErrDevice, "renderFrame: no swap chain");
    ++r->frames;
    const bool world = !(f->flags & kFrameNoWorld) && f->scene == 1 && r->sceneLoaded;
    if (world) {
        for (uint32_t i = 0; f->stageObjects && i < f->stageObjectCount; ++i) MoveStageObject(*r, f->stageObjects[i]);
        for (uint32_t i = 0; f->instances && i < f->instanceCount; ++i) Place(*r, f->instances[i]);
        // The car the cube probe is drawn around (core/probe.h).
        if (f->reflectionFocus && f->reflectionFocus <= r->instances.size() && r->instances[f->reflectionFocus - 1].alive) {
            const Instance& in = r->instances[f->reflectionFocus - 1];
            r->g->scene.SetReflectionFocus(in.objects.data(), static_cast<uint32_t>(in.objects.size()), in.origin);
        } else {
            r->g->scene.SetReflectionFocus(nullptr, 0, nullptr);
        }
        ApplyFrameWeather(*r, *f);
        r->g->scene.SetWetness(f->wetness);
        // The wind from the weather in force (a preset's, or the frame's WeatherDesc), timed by FrameDesc::time.
        {
            const auto& presets = r->g->scene.WeatherPresets();
            if (f->weatherPreset >= 0 && size_t(f->weatherPreset) < presets.size()) {
                const auto& w = presets[size_t(f->weatherPreset)];
                r->g->scene.SetWind(w.windDirectionDeg, w.windSpeed, w.gustiness);
            } else if (f->weatherPreset < 0) {
                r->g->scene.SetWind(f->weather.windDirectionDeg, f->weather.windSpeed, f->weather.gustiness);
            } else {
                r->g->scene.SetWind(0.0f, 0.0f, 0.0f);
            }
            // NGG_WIND=<speed>[,<gustiness>[,<degrees>]]: that wind over the weather's (measuring; NGG design §10).
            static const bool windSet = std::getenv("NGG_WIND") != nullptr;
            static float windEnv[3] = {0.0f, 0.5f, 90.0f};
            static const int windRead = windSet ? sscanf_s(std::getenv("NGG_WIND"), "%f,%f,%f", &windEnv[0], &windEnv[1], &windEnv[2]) : 0;
            if (windRead > 0) r->g->scene.SetWind(windEnv[2], windEnv[0], windEnv[1]);
            r->g->scene.SetWindTime(f->time);
        }
        // The frame's local lights, as the core takes them (NGG design §5.5).
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
            r->g->scene.SetLights(lights);
        }
        if (f->lineVertexCount && f->lines) {
            static_assert(sizeof(LineVertex) == sizeof(ngg::core::Scene::OverlayVertex), "uploaded raw");
            r->g->scene.SetOverlayLines(r->g->dev.device.Get(), 0, reinterpret_cast<const ngg::core::Scene::OverlayVertex*>(f->lines),
                                     f->lineVertexCount & ~1u, 0);
            r->linesSet = true;
        } else if (r->linesSet) {
            r->g->scene.SetOverlayLines(r->g->dev.device.Get(), 0, nullptr, 0, 0);
            r->linesSet = false;
        }
        if (f->camera.cut) r->g->core.ResetHistory();
    }

    if (!r->g->dev.BeginFrame()) {
        if (Removed(*r, "at Begin")) return Status::Ok;   // the frame is lost; the next one is on a new device
        return r->Fail(Status::ErrDevice, "renderFrame: the command list would not open");
    }
    D3D12_RESOURCE_BARRIER toRt = {};
    toRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRt.Transition.pResource = r->g->sc.CurrentBuffer();
    toRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    r->g->dev.list->ResourceBarrier(1, &toRt);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = r->g->sc.CurrentRtv();
    // The frame's sky: the weather's, and over its sun the time and place when the frame gives them (NGG design §5.4)
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
        r->g->core.SetSkyOn(true);
        r->g->core.SetTaa(r->settings.taa != 0);
        r->g->core.SetCurve(ngg::core::PostFx::kCurveAces);
        r->g->core.RecordFrame(r->g->dev, rtv, f->camera.view, f->camera.proj, r->g->sc.width, r->g->sc.height, sky);
        // (Depth-tested against the scene's depth, which is the render size: not drawn while upscaling.)
        if (r->g->core.RenderWidth() == r->g->sc.width && r->g->core.RenderHeight() == r->g->sc.height)
            r->g->scene.RenderEditorOverlay(r->g->dev.list.Get(), rtv, f->camera.view, f->camera.proj, r->g->sc.width, r->g->sc.height);
    } else {
        const float black[4] = {0, 0, 0, 1};
        r->g->dev.list->ClearRenderTargetView(rtv, black, 0, nullptr);
    }
    r->g->ui.Record(r->g->dev, rtv, r->g->sc.width, r->g->sc.height, f->ui);

    D3D12_RESOURCE_BARRIER toPresent = toRt;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    const bool capture = !r->capturePath.empty() && RecordCapture(*r);
    if (capture) toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    r->g->dev.list->ResourceBarrier(1, &toPresent);
    if (std::find(r->testRemovals.begin(), r->testRemovals.end(), r->frames) != r->testRemovals.end()) {
        ComPtr<ID3D12Device5> d5;
        if (SUCCEEDED(r->g->dev.device.As(&d5))) {
            ngg::core::Log("wrenderer: NGG_TEST_DEVICE_REMOVED: removing the device in frame %llu", (unsigned long long)r->frames);
            d5->RemoveDevice();
        }
    }
    r->g->dev.EndFrame();
    r->inFlight = true;
    // A capture is read at once (WriteCapture): with frames in flight its frame must have run first.
    if (capture && r->g->dev.frames > 1) r->g->dev.WaitForGpu();
    r->g->dev.DrainMessages();
    // A removal in this frame (EndFrame's CheckRemoved logged DRED's record): the frame is lost - not presented, its
    // readbacks not read, a capture asked for taken from the next one. Ok: the next call that needs the device makes
    // a new one, and the client goes on as if nothing happened.
    if (Removed(*r, "renderFrame")) return Status::Ok;
    if (capture) WriteCapture(*r);
    r->capturePath.clear();
    if (world) {
        if (r->g->dev.frames > 1 && r->g->scene.WantsFinishedFrame()) r->g->dev.WaitForGpu();
        r->g->scene.LogProbe();
        r->g->scene.ReadOverlayStats();
    }
    return Status::Ok;
}

Status WRALLY_CALL Present(Renderer* r)
{
    if (!r) return Status::ErrInvalidArg;
    if (r->lost) return Status::Ok;   // the frame was lost with the device: nothing to show (renderFrame makes a new one)
    if (r->broken) return Status::ErrFault;
    if (!r->g->sc.swap) return Status::ErrInvalidArg;
    const HRESULT hr = r->g->sc.swap->Present(r->settings.vsync ? 1 : 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        ngg::core::Log("wrenderer: present: 0x%08lX (%s)", (unsigned long)hr,
                       hr == DXGI_ERROR_DEVICE_RESET ? "DEVICE_RESET" : "DEVICE_REMOVED");
        Removed(*r, "at Present");
        r->lost = true;   // (a reset may leave GetDeviceRemovedReason S_OK: the swap chain is lost either way)
        return Status::Ok;
    }
    return FAILED(hr) ? Status::ErrDevice : Status::Ok;
}

Status WRALLY_CALL Stats(Renderer* r, RenderStats* out)
{
    if (!r || !out) return Status::ErrInvalidArg;
    *out = {};
    out->deviceGeneration = r->generation;
    out->sunElevationDeg = r->sunElevationDeg;     // (the game lights its lamps by it: kept through a loss)
    if (r->lost || r->broken) return Status::Ok;   // (no device: nothing to read)
    // (The ray tracer's and the denoiser's figures were read at the start of their own recording, where their frame
    // slot's last frame had run - device.h: never here, after a submit, where it may still be running.)
    ngg::core::Rt& rt = r->g->core.Raytracer();
    out->passMs = (float)r->g->scene.LastPassMs();
    out->frameMs = (float)r->g->core.LastFrameMs();
    out->sunElevationDeg = r->sunElevationDeg;
    out->envMs = (float)r->g->scene.LastEnvMs();
    out->aerialMs = (float)r->g->scene.LastAerialMs();
    out->rtActive = r->g->core.RayTracedShadowsActive() ? 1u : 0u;
    out->rtAvailable = r->g->core.RayTracingAvailable() ? 1u : 0u;
    if (out->rtActive) {
        out->rtMs = (float)rt.LastMs();
        out->aoMs = (float)rt.AoMs();
        out->denoiseMs = (float)r->g->core.DenoiseMs();
        out->lightsMs = (float)rt.LightsMs();
        out->lightCount = (uint32_t)r->g->scene.GetLights().Count();
        out->rtPixels = rt.TracedPixels();
        out->rtShadowed = rt.ShadowedPixels();
    } else if (const char* why = r->g->core.RtFallbackReason()) {
        strncpy_s(out->rtFallback, sizeof out->rtFallback, why, _TRUNCATE);
    }
    out->quality = (uint32_t)r->g->core.GetQuality();
    out->taa = r->g->post.Taa() ? 1u : 0u;
    out->shadows = r->g->scene.ShadowsOn() ? 1u : 0u;
    out->ambientOcclusion = r->g->core.AoApplied() ? 1u : 0u;
    strncpy_s(out->aoSource, sizeof out->aoSource, r->g->core.AoSourceName(), _TRUNCATE);
    if (!out->rtActive || r->g->core.GetAoSource() != ngg::core::AoSource::Traced) out->aoMs = (float)r->g->core.AoMs();
    out->autoExposure = r->g->post.AutoExposure() ? 1u : 0u;
    out->debugView = (uint32_t)r->g->scene.DebugMode();
    out->exposure = r->g->post.LastExposure();
    out->meteredEv = r->g->post.LastMeteredEv();
    out->adaptedEv = r->g->post.LastAdaptedEv();
    out->renderWidth = r->g->core.RenderWidth();
    out->renderHeight = r->g->core.RenderHeight();
    strncpy_s(out->upscaler, sizeof out->upscaler, r->g->core.UpscalerName(), _TRUNCATE);
    out->upscalersAvailable = 1u << kUpscalerNone;
    if (r->g->core.UpscalerAvailable(ngg::core::UpscalerKind::Fsr))  out->upscalersAvailable |= 1u << kUpscalerFsr;
    if (r->g->core.UpscalerAvailable(ngg::core::UpscalerKind::Dlss)) out->upscalersAvailable |= 1u << kUpscalerDlss;
    if (r->g->core.UpscalerAvailable(ngg::core::UpscalerKind::DlssRr)) out->upscalersAvailable |= 1u << kUpscalerDlssRr;
    strncpy_s(out->denoiser, sizeof out->denoiser, r->g->core.DenoiserName(), _TRUNCATE);
    out->denoisersAvailable = 1u << kDenoiserOurs;
    if (r->g->core.DenoiserAvailable(ngg::core::Denoiser::Nrd)) out->denoisersAvailable |= 1u << kDenoiserNrd;
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
    r->journal.debugView = mode;
    r->g->scene.SetDebugMode(mode);
    return Status::Ok;
}

const char* WRALLY_CALL DebugViewName(int32_t mode) { return ngg::core::Scene::DebugModeName(mode); }

Status WRALLY_CALL SetLegacyProfile(Renderer* r, int32_t mode)
{
    if (!r) return Status::ErrInvalidArg;
    r->journal.legacyProfile = mode;
    r->g->scene.SetLegacyProfile(mode);
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
bool ObjectOk(Renderer* r, uint32_t o) { return EditOk(r) && o < r->g->scene.ObjectCount(); }

uint32_t ToTextureHandle(uint32_t index) { return index == ngg::kNoTexture ? 0u : index + 1u; }
uint32_t ToTextureIndex(TextureHandle h) { return h ? h - 1u : ngg::kNoTexture; }

uint32_t WRALLY_CALL EditObjectCount(Renderer* r) { return EditOk(r) ? r->g->scene.ObjectCount() : 0; }
uint32_t WRALLY_CALL EditVisibleObjectCount(Renderer* r) { return EditOk(r) ? r->g->scene.VisibleObjectCount() : 0; }

Status WRALLY_CALL EditSetObjectVisible(Renderer* r, uint32_t o, uint32_t visible)
{
    if (!ObjectOk(r, o)) return Status::ErrInvalidArg;
    if (!r->g->scene.SetObjectVisible(o, visible != 0)) return r->Fail(Status::ErrInvalidArg, "setObjectVisible: refused");
    if (!r->replaying) r->journal.objectVisible[o] = visible != 0;
    return Status::Ok;
}

Status WRALLY_CALL EditSetObjectTransform(Renderer* r, uint32_t o, const float delta[16])
{
    if (!ObjectOk(r, o) || !delta) return Status::ErrInvalidArg;
    if (!r->g->scene.SetObjectTransform(o, delta)) return r->Fail(Status::ErrInvalidArg, "setObjectTransform: refused");
    if (!r->replaying) std::memcpy(r->journal.objectTransform[o].data(), delta, 16 * sizeof(float));
    return Status::Ok;
}

Status WRALLY_CALL EditObjectBounds(Renderer* r, uint32_t o, float outMin[3], float outMax[3])
{
    if (!ObjectOk(r, o) || !outMin || !outMax) return Status::ErrInvalidArg;
    return r->g->scene.ObjectBounds(o, outMin, outMax) ? Status::Ok : Status::ErrInvalidArg;
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
    return r->g->scene.FocusPoint(out) ? Status::Ok : Status::ErrInvalidArg;
}

Status WRALLY_CALL EditSetSelection(Renderer* r, const uint32_t* objects, uint32_t count)
{
    if (!r || (count && (!objects || !r->sceneLoaded))) return Status::ErrInvalidArg;
    if (const Status d = NeedDevice(r); d != Status::Ok) return d;
    if (!r->g->scene.SetSelection(objects, count)) return r->Fail(Status::ErrInvalidArg, "setSelection: refused");
    if (!r->replaying) {
        r->journal.selectionSet = true;
        r->journal.selection.assign(objects, objects + count);
    }
    return Status::Ok;
}

Status WRALLY_CALL EditSetOverlayLines(Renderer* r, uint32_t layer, const LineVertex* v, uint32_t count, uint32_t flags)
{
    if (!r || layer >= kEditOverlayLayers || (count && !v)) return Status::ErrInvalidArg;
    if (count & 1u) return r->Fail(Status::ErrInvalidArg, "setOverlayLines: an odd vertex count (a segment is a pair)");
    if (const Status d = NeedDevice(r); d != Status::Ok) return d;
    Settle(*r);
    const Status s = Retry(r, [&] {
        if (!r->g->scene.SetOverlayLines(r->g->dev.device.Get(), layer + 1,
                                         reinterpret_cast<const ngg::core::Scene::OverlayVertex*>(v), count, flags))
            return r->Fail(Status::ErrDevice, "setOverlayLines: the layer could not be uploaded");
        return Status::Ok;
    });
    if (s == Status::Ok && !r->replaying) r->journal.lines[layer] = {std::vector<LineVertex>(v, v + count), flags};
    return s;
}

void WRALLY_CALL EditClearOverlays(Renderer* r)
{
    if (!r) return;
    r->g->scene.ClearOverlays();
    r->linesSet = false;
    if (!r->replaying) r->journal.lines.clear();
}

Status WRALLY_CALL EditSetFont(Renderer* r, const void* rgba, uint32_t width, uint32_t height, uint32_t columns,
                               uint32_t rows, uint32_t firstChar)
{
    if (!r || !rgba || !width || !height || !columns || !rows) return Status::ErrInvalidArg;
    if (const Status d = NeedDevice(r); d != Status::Ok) return d;
    Settle(*r);
    const Status s = Retry(r, [&] {
        if (!r->g->scene.SetFont(r->g->dev.device.Get(), r->g->dev.queue.Get(), rgba, width, height, columns, rows, firstChar))
            return r->Fail(Status::ErrDevice, "setFont: the atlas could not be uploaded");
        return Status::Ok;
    });
    if (s == Status::Ok && !r->replaying) {
        const uint8_t* p = static_cast<const uint8_t*>(rgba);
        r->journal.font.reset(new Journal::Font{std::vector<uint8_t>(p, p + size_t(width) * height * 4), width, height,
                                                columns, rows, firstChar});
    }
    return s;
}

Status WRALLY_CALL EditSetLabels(Renderer* r, uint32_t layer, const EditLabel* labels, uint32_t count, const char* blob,
                                 uint32_t blobBytes, float pixelHeight, uint32_t flags)
{
    if (!r || layer >= kEditLabelLayers || (count && (!labels || !blob))) return Status::ErrInvalidArg;
    if (const Status d = NeedDevice(r); d != Status::Ok) return d;
    Settle(*r);
    if (count && !r->g->scene.HasFont()) return r->Fail(Status::ErrInvalidArg, "setLabels: no font yet (setFont)");
    if (!r->g->scene.SetLabels(r->g->dev.device.Get(), layer, reinterpret_cast<const ngg::core::Scene::Label*>(labels), count,
                            blob, blobBytes, pixelHeight, flags))
        return r->Fail(Status::ErrInvalidArg, "setLabels: refused - pixelHeight must be positive, every text inside the blob");
    if (!r->replaying)
        r->journal.labels[layer] = {std::vector<EditLabel>(labels, labels + count),
                                    std::string(blob ? blob : "", blob ? blobBytes : 0), pixelHeight, flags};
    return Status::Ok;
}

Status WRALLY_CALL EditOverlayStatsOf(Renderer* r, EditOverlayStats* out)
{
    if (!r || !out) return Status::ErrInvalidArg;
    out->selectionVisible = r->g->scene.SelectionPixelsVisible();
    out->selectionOccluded = r->g->scene.SelectionPixelsOccluded();
    out->linesVisible = r->g->scene.OverlayPixelsVisible();
    out->linesOccluded = r->g->scene.OverlayPixelsOccluded();
    out->labelsVisible = r->g->scene.LabelPixelsVisible();
    out->labelsOccluded = r->g->scene.LabelPixelsOccluded();
    return Status::Ok;
}

uint32_t WRALLY_CALL EditMaterialCount(Renderer* r) { return EditOk(r) ? r->g->scene.MaterialCount() : 0; }

Status WRALLY_CALL EditGetMaterial(Renderer* r, uint32_t h, MaterialDesc* out)
{
    ngg::core::Scene::MaterialSlot s;
    if (!EditOk(r) || !h || !out || !r->g->scene.GetMaterial(h - 1, &s)) return Status::ErrInvalidArg;
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
    if (!EditOk(r) || !h || !in || !r->g->scene.GetMaterial(h - 1, &s)) return Status::ErrInvalidArg;
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
    if (const Status d = NeedDevice(r); d != Status::Ok) return d;
    if (!r->g->scene.SetMaterial(r->g->dev.device.Get(), h - 1, s)) return r->Fail(Status::ErrDevice, "setMaterial: update failed");
    if (!r->replaying) r->journal.materials[h] = *in;
    return Status::Ok;
}

Status WRALLY_CALL EditMaterialName(Renderer* r, uint32_t h, char* buf, uint32_t bytes)
{
    if (!EditOk(r) || !h || h > r->g->scene.MaterialCount() || !buf || !bytes) return Status::ErrInvalidArg;
    const char* name = r->g->scene.MaterialName(h - 1);
    strncpy_s(buf, bytes, name ? name : "", _TRUNCATE);
    return Status::Ok;
}

Status WRALLY_CALL EditObjectMaterial(Renderer* r, uint32_t o, uint32_t* out)
{
    if (out) *out = 0;
    uint32_t m = 0;
    if (!ObjectOk(r, o) || !out || !r->g->scene.ObjectMaterial(o, &m)) return Status::ErrInvalidArg;
    *out = m + 1;
    return Status::Ok;
}

Status WRALLY_CALL EditSetLightMode(Renderer* r, int32_t mode)
{
    if (!r) return Status::ErrInvalidArg;
    r->journal.lightMode = mode;
    r->g->scene.SetLightMode(mode);
    return Status::Ok;
}

const char* WRALLY_CALL EditLightModeName(int32_t mode) { return ngg::core::Scene::LightModeName(mode); }

Status WRALLY_CALL EditSetCull(Renderer* r, uint32_t on)
{
    if (!r) return Status::ErrInvalidArg;
    r->journal.cull = on != 0;
    r->g->scene.SetCull(on != 0);
    return Status::Ok;
}

Status WRALLY_CALL EditProbePixel(Renderer* r, int32_t x, int32_t y)
{
    if (!r || x < 0 || y < 0) return Status::ErrInvalidArg;
    r->journal.probe = std::make_pair(x, y);
    r->g->scene.SetProbe(x, y);
    return Status::Ok;
}

uint32_t WRALLY_CALL EditModelCount(Renderer* r) { return r ? r->g->scene.ModelCount() : 0; }

Status WRALLY_CALL EditModelName(Renderer* r, uint32_t model, char* buf, uint32_t bytes)
{
    const char* name = r ? r->g->scene.ModelName(model) : nullptr;
    if (!name || !buf || !bytes) return Status::ErrInvalidArg;
    std::snprintf(buf, bytes, "%s", name);
    return Status::Ok;
}

uint32_t WRALLY_CALL EditPlacementCount(Renderer* r) { return r ? r->g->scene.PlacementCount() : 0; }

ngg::core::Scene::PlacementDesc ToDesc(const EditPlacement& e)
{
    ngg::core::Scene::PlacementDesc d;
    d.model = e.model;
    std::memcpy(d.position, e.position, sizeof(d.position));
    d.yaw = e.yaw;
    d.scaleXZ = e.scaleXZ;
    d.scaleY = e.scaleY;
    d.tint = e.tint;
    return d;
}

Status WRALLY_CALL EditGetPlacements(Renderer* r, uint32_t first, uint32_t count, EditPlacement* out)
{
    if (!r || (!out && count) || uint64_t(first) + count > r->g->scene.PlacementCount()) return Status::ErrInvalidArg;
    for (uint32_t i = 0; i < count; ++i) {
        ngg::core::Scene::PlacementDesc d;
        EditPlacement& e = out[i];
        e = EditPlacement{};
        if (!r->g->scene.GetPlacement(first + i, &d)) {
            e.flags = kEditPlacementRemoved;
            e.plant = kEditNoPlant;
            continue;
        }
        e.model = d.model;
        std::memcpy(e.position, d.position, sizeof(e.position));
        e.yaw = d.yaw, e.scaleXZ = d.scaleXZ, e.scaleY = d.scaleY, e.tint = d.tint, e.plant = d.plant;
    }
    return Status::Ok;
}

Status WRALLY_CALL EditAddPlacements(Renderer* r, const EditPlacement* in, uint32_t count, uint32_t* outHandles)
{
    if (!r || (!in && count)) return Status::ErrInvalidArg;
    std::vector<ngg::core::Scene::PlacementDesc> d(count);
    for (uint32_t i = 0; i < count; ++i) d[i] = ToDesc(in[i]);
    if (!r->g->scene.AddPlacements(d.data(), count, outHandles)) return Status::ErrInvalidArg;
    if (!r->replaying) {
        Journal::Op op{Journal::Op::AddPlacements};
        op.placements.assign(in, in + count);
        r->journal.ops.push_back(std::move(op));
    }
    return Status::Ok;
}

Status WRALLY_CALL EditSetPlacements(Renderer* r, const uint32_t* handles, const EditPlacement* in, uint32_t count)
{
    if (!r || ((!in || !handles) && count)) return Status::ErrInvalidArg;
    std::vector<ngg::core::Scene::PlacementDesc> d(count);
    for (uint32_t i = 0; i < count; ++i) d[i] = ToDesc(in[i]);
    if (!r->g->scene.SetPlacements(handles, d.data(), count)) return Status::ErrInvalidArg;
    if (!r->replaying) {
        // A drag moves the same placements every frame: the last move replaces the one before.
        auto& ops = r->journal.ops;
        if (ops.empty() || ops.back().kind != Journal::Op::SetPlacements ||
            ops.back().handles != std::vector<uint32_t>(handles, handles + count))
            ops.push_back({Journal::Op::SetPlacements});
        ops.back().handles.assign(handles, handles + count);
        ops.back().placements.assign(in, in + count);
    }
    return Status::Ok;
}

Status WRALLY_CALL EditRemovePlacements(Renderer* r, const uint32_t* handles, uint32_t count)
{
    if (!r) return Status::ErrInvalidArg;
    if (!r->g->scene.RemovePlacements(handles, count)) return Status::ErrInvalidArg;
    if (!r->replaying) {
        Journal::Op op{Journal::Op::RemovePlacements};
        if (handles) op.handles.assign(handles, handles + count);
        r->journal.ops.push_back(std::move(op));
    }
    return Status::Ok;
}

Status WRALLY_CALL EditModelStatsOf(Renderer* r, EditModelStats* out)
{
    if (!r || !out) return Status::ErrInvalidArg;
    const ngg::core::Scene::ModelStats& m = r->g->scene.LastModelStats();
    out->placements = m.placements;
    out->visible = m.visible;
    out->lod0 = m.perLod[0], out->lod1 = m.perLod[1], out->lod2 = m.perLod[2], out->billboard = m.perLod[3];
    out->fading = m.fading;
    out->shadowInstances = m.shadowInstances;
    out->draws = m.draws;
    out->cullMs = float(m.cullMs);
    return Status::Ok;
}

const RendererEditApi kEditApi = {
    &EditObjectCount,     &EditVisibleObjectCount, &EditSetObjectVisible, &EditSetObjectTransform, &EditObjectBounds,
    &EditInstanceObjects, &EditFocusPoint,         &EditSetSelection,     &EditSetOverlayLines,    &EditClearOverlays,
    &EditSetFont,         &EditSetLabels,          &EditOverlayStatsOf,   &EditMaterialCount,      &EditGetMaterial,
    &EditSetMaterial,     &EditMaterialName,       &EditObjectMaterial,   &EditSetLightMode,       &EditLightModeName,
    &EditSetCull,         &EditProbePixel,         &EditModelCount,       &EditModelName,          &EditPlacementCount,
    &EditGetPlacements,   &EditAddPlacements,      &EditSetPlacements,    &EditRemovePlacements,   &EditModelStatsOf};

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

Status WRALLY_CALL KeepCopies(Renderer* r, uint32_t on)
{
    if (!r) return Status::ErrInvalidArg;
    r->keepCopies = on != 0;
    return Status::Ok;
}

// ---- a new device after a removal (NGG design §10.6)

// The client's bytes again (RendererInput::reload), released when this goes. ok false: it could not.
struct Asked {
    Renderer& r;
    Blob blob{};
    bool ok = false;
    Asked(Renderer& r, uint32_t kind, uint32_t handle) : r(r)
    {
        ok = r.reload.read && r.reload.read(r.reload.ctx, kind, handle, &blob) == Status::Ok && blob.data && blob.size;
        if (!ok) ngg::core::Log("wrenderer: the client did not give %s %u again", kind == kReloadScene ? "scene" : "texture", handle);
    }
    ~Asked()
    {
        if (ok && r.reload.release) r.reload.release(r.reload.ctx, &blob);
    }
};

// What a texture the client could not give again is: a 4x4 white DXT1 - in its place, so the handles after it stay.
std::vector<uint8_t> WhiteDds()
{
    std::vector<uint8_t> d(128, 0);
    std::memcpy(d.data(), "DDS ", 4);
    auto w32 = [&](size_t off, uint32_t v) { std::memcpy(d.data() + off, &v, 4); };
    w32(4, 124), w32(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000), w32(12, 4), w32(16, 4), w32(20, 8), w32(28, 1);
    w32(76, 32), w32(80, 0x4), w32(84, 0x31545844u), w32(108, 0x1000);   // FOURCC 'DXT1'
    const uint8_t block[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
    d.insert(d.end(), block, block + 8);
    return d;
}

// The journal into a new Gpu, through the table itself (replaying: nothing journaled again). A handle that comes out
// otherwise than it first did is logged: the client's would then name something else.
void Replay(Renderer& r)
{
    Journal& j = r.journal;
    r.replaying = true;
    // What the frames put where (FrameDesc instances and stage objects are not handed again unless they move).
    std::vector<Instance> placed = r.instances;
    std::unordered_map<int32_t, StageObjectUpdate> moved = r.stageMoved;
    ForgetScene(r);
    uint32_t mismatches = 0, whites = 0;
    auto expect = [&](uint32_t got, uint32_t was, const char* what) {
        if (got != was && mismatches++ < 8) ngg::core::Log("wrenderer: after the new device a %s is %u, was %u", what, got, was);
    };
    for (auto& [slot, t] : j.uiTextures) {
        std::string e;
        if (!r.g->ui.Restore(r.g->dev, slot, t.rgba.data(), t.width, t.height, e))
            ngg::core::Log("wrenderer: UI texture %u not made again: %s", slot, e.c_str());
    }
    if (j.scene) {
        SceneHandle h = 0;
        const char* variant = j.variant.empty() ? nullptr : j.variant.c_str();
        Status s = Status::ErrIo;
        if (!j.sceneBytes.empty()) {
            s = LoadSceneVariant(&r, j.sceneBytes.data(), j.sceneBytes.size(), variant, &h);
        } else if (Asked a(r, kReloadScene, 1); a.ok) {
            s = LoadSceneVariant(&r, a.blob.data, a.blob.size, variant, &h);
        }
        if (s != Status::Ok) {
            r.Fail(Status::ErrIo, "device lost: the scene could not be made again (the client did not give it, or it "
                                  "would not load) - load it again");
            ngg::core::Log("wrenderer: the scene is gone with the device: %s", r.error.c_str());
        }
    }
    for (Journal::Op& op : j.ops) {
        uint32_t h = 0;
        switch (op.kind) {
        case Journal::Op::Texture:
        case Journal::Op::TextureData: {
            if (!r.sceneLoaded) break;
            const bool data = op.kind == Journal::Op::TextureData;
            if (!op.bytes.empty()) {
                AddSceneTexture(&r, op.bytes.data(), op.bytes.size(), &h, data);
            } else if (Asked a(r, kReloadTexture, op.handle); a.ok) {
                AddSceneTexture(&r, a.blob.data, a.blob.size, &h, data);
            }
            if (!h) {   // white in its place: the handles after it stay as they were
                const std::vector<uint8_t> white = WhiteDds();
                AddSceneTexture(&r, white.data(), white.size(), &h, data);
                ++whites;
            }
            expect(h, op.handle, "texture");
            break;
        }
        case Journal::Op::Model: {
            const ModelDesc d = op.model->Desc();
            CreateModel(&r, &d, &h);
            expect(h, op.handle, "model");
            break;
        }
        case Journal::Op::ReleaseModel: ReleaseModel(&r, op.handle); break;
        case Journal::Op::Instance:
            CreateInstance(&r, op.handle, &h);
            break;
        case Journal::Op::DestroyInstance: DestroyInstance(&r, op.handle); break;
        case Journal::Op::AddPlacements:
            EditAddPlacements(&r, op.placements.data(), uint32_t(op.placements.size()), nullptr);
            break;
        case Journal::Op::SetPlacements:
            EditSetPlacements(&r, op.handles.data(), op.placements.data(), uint32_t(op.placements.size()));
            break;
        case Journal::Op::RemovePlacements:
            EditRemovePlacements(&r, op.handles.empty() ? nullptr : op.handles.data(), uint32_t(op.handles.size()));
            break;
        }
    }
    if (r.instances.size() != placed.size()) {
        ngg::core::Log("wrenderer: after the new device %zu instances, were %zu", r.instances.size(), placed.size());
        ++mismatches;
    }
    // The setters' last values.
    for (const auto& [h, m] : j.materials) EditSetMaterial(&r, h, &m);
    for (const auto& [o, t] : j.objectTransform) EditSetObjectTransform(&r, o, t.data());
    for (const auto& [o, v] : j.objectVisible) EditSetObjectVisible(&r, o, v ? 1u : 0u);
    if (j.selectionSet) EditSetSelection(&r, j.selection.data(), uint32_t(j.selection.size()));
    for (const auto& [layer, l] : j.lines) EditSetOverlayLines(&r, layer, l.v.data(), uint32_t(l.v.size()), l.flags);
    if (j.font)
        EditSetFont(&r, j.font->rgba.data(), j.font->width, j.font->height, j.font->columns, j.font->rows, j.font->firstChar);
    for (const auto& [layer, l] : j.labels)
        EditSetLabels(&r, layer, l.labels.data(), uint32_t(l.labels.size()), l.blob.data(), uint32_t(l.blob.size()),
                      l.pixelHeight, l.flags);
    if (j.debugView) SetDebugView(&r, *j.debugView);
    if (j.legacyProfile) SetLegacyProfile(&r, *j.legacyProfile);
    if (j.lightMode) EditSetLightMode(&r, *j.lightMode);
    if (j.cull) EditSetCull(&r, *j.cull ? 1u : 0u);
    if (j.probe) EditProbePixel(&r, j.probe->first, j.probe->second);
    // The instances and stage objects where the frames last put them.
    if (r.sceneLoaded) {
        for (size_t i = 0; i < placed.size() && i < r.instances.size(); ++i) {
            const Instance& was = placed[i];
            if (!was.alive || !r.instances[i].alive || !was.placed) continue;
            InstanceUpdate u{};
            u.instance = InstanceHandle(i + 1);
            std::memcpy(u.world, was.world, sizeof u.world);
            u.nodes = was.nodes.empty() ? nullptr : was.nodes.data();
            u.nodeCount = uint32_t(was.nodes.size() / 16);
            u.visible = was.visible ? 1u : 0u;
            Place(r, u);
        }
        for (const auto& [key, u] : moved) MoveStageObject(r, u);
    }
    r.replaying = false;
    if (mismatches) ngg::core::Log("wrenderer: %u handles came out otherwise on the new device", mismatches);
    if (whites) ngg::core::Log("wrenderer: %u textures not given again - drawn white", whites);
}

bool Recover(Renderer& r)
{
    const auto now = std::chrono::steady_clock::now();
    if (r.lastFailed != std::chrono::steady_clock::time_point{} && now - r.lastFailed < std::chrono::seconds(1))
        return false;   // the last try failed a moment ago
    r.lastFailed = now;   // (until it succeeds)
    {
        size_t kept = r.journal.sceneBytes.size(), asked = r.journal.scene && r.journal.sceneBytes.empty() ? 1 : 0;
        for (const Journal::Op& op : r.journal.ops)
            if (op.kind == Journal::Op::Texture || op.kind == Journal::Op::TextureData)
                op.bytes.empty() ? void(++asked) : void(kept += op.bytes.size());
        ngg::core::Log("wrenderer: the device was lost - making a new one (%zu scene ops, %zu UI textures; %zu scene / "
                       "texture bytes asked for again, %.1f MB kept)",
                       r.journal.ops.size(), r.journal.uiTextures.size(), asked, kept / 1048576.0);
    }
    // All of the old device let go first: D3D12 has one device per adapter, and while any of the old one lives a new
    // D3D12CreateDevice hands it back, removed. That includes the swap chain (the window takes one flip chain).
    ngg::core::ClosePipelineCache();
    r.g.reset();
    r.g = std::make_unique<Gpu>();
    r.lost = false;
    r.broken = false;
    if (const std::string e = MakeGpu(r, r.width, r.height, true); !e.empty()) {
        r.Fail(Status::ErrDevice, "device lost, and a new one failed: " + e);
        if (!r.g->dev.device) {
            // No device (the adapter gone: unplugged, the driver mid-update): try again from the next call on.
            r.lost = true;
            r.g = std::make_unique<Gpu>();
        } else {
            r.broken = true;   // a device, but not the renderer on it: ErrFault, the instance must be destroyed
        }
        r.sceneLoaded = false;   // (the journal stays: a later new device still gets it all)
        return false;
    }
    Replay(r);
    r.lastFailed = {};
    ++r.generation;
    r.error.clear();
    ngg::core::SavePipelineCache();
    ngg::core::Log("wrenderer: device %u ready in %.0f ms - the scene, %zu models, %zu instances made again", r.generation,
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count(), r.models.size(),
                   r.instances.size());
    return true;
}

const RendererApi kApi = {&Destroy,       &LastError,         &ApplySettings,  &Resize,         &LoadScene,
                          &UnloadScene,   &SceneBounds,       &SceneKeyStats,  &WeatherPresetCount,
                          &WeatherPresetAt, &FindWeatherPreset, &CreateTexture, &CreateTextureRgba,
                          &ReleaseTexture, &CreateModel,      &ReleaseModel,   &CreateInstance, &DestroyInstance,
                          &RenderFrame,   &Present,           &Stats,          &CaptureFrame,   &SetDebugView,
                          &DebugViewName, &SetLegacyProfile, &UpdateTextureRgba, &CreateTextureData, &EditApi,
                          &LoadSceneVariant, &KeepCopies};

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
        const auto start = std::chrono::steady_clock::now();
        auto r = std::make_unique<Renderer>();
        r->host = input.host;
        r->hwnd = static_cast<HWND>(input.window);
        g_logHost = input.host;
        ngg::core::SetLogSink(&CoreLog);
        auto fail = [&](const char* what) {
            ngg::core::Log("wrenderer: %s", what);
            ngg::core::ClosePipelineCache();
            ngg::core::SetLogSink(nullptr);
            g_logHost = {};
            return Status::ErrDevice;
        };
        r->luid = LUID{input.adapterLuidLow, input.adapterLuidHigh};
        r->pickLuid = input.adapterLuidLow || input.adapterLuidHigh;
        r->initFlags = input.flags;
        r->reload = input.reload;
        r->keepCopies = input.reload.read == nullptr;   // without the client's bytes again: the renderer's own copies
        r->testRemovals = TestRemovals();
        r->settings = input.settings;
        if (r->settings.mode != WindowMode::Windowed) r->settings.mode = WindowMode::Windowed;
        r->g = std::make_unique<Gpu>();
        if (const std::string e = MakeGpu(*r, input.width, input.height, false); !e.empty()) return fail(e.c_str());
        ngg::core::LogShaderStats("start-up");
        ngg::core::LogPipelineStats("start-up");
        ngg::core::SavePipelineCache();
        ngg::core::Log("wrenderer: %ux%u ready in %.0f ms", input.width, input.height,
                       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());

        output.info = {};
        std::snprintf(output.info.id, sizeof output.info.id, "wrenderer");
        std::snprintf(output.info.name, sizeof output.info.name, "WRenderer");
        output.info.moduleVersion = kModuleVersion;
        output.info.minGameVersion = MakeVersion(0, 1, 0);
        output.info.buildHash = WRENDERER_BUILD_HASH;
        output.info.caps = r->g->core.RayTracingAvailable() ? kRendererCapRayTracing : 0;
        output.api = &kApi;
        output.instance = r.release();
        return Status::Ok;
    } catch (...) {
        ngg::core::SetLogSink(nullptr);
        g_logHost = {};
        return Status::ErrInternal;
    }
}
