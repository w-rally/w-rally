// ---------------------------------------------------------------------------
// ngg-api - implementation of the flat C surface (design 8.2).
//
// This composes exactly what host/viewer.cpp composes - Device, swap chain,
// Scene, ShadowMaps, PostFx, Sky - and drives it from the host's calls instead
// of from key presses. That symmetry is the point: the editor preview and the
// standalone viewer must not become two renderers, which is the whole reason
// ngg-core exists as a module (design 8).
//
// Nothing in here knows about RBR, and nothing knows about .lbs or glTF. It
// takes buffers. The content pipeline stays in Wallaby's C#, so the preview
// cannot disagree with the shipped asset (design 8.2, "why the renderer must
// NOT load glTF").
// ---------------------------------------------------------------------------
#define NGG_API_EXPORTS
#include "ngg_api.h"

#include "device.h"
#include "log.h"
#include "post.h"
#include "renderer.h"
#include "weather.h"
#include "scene.h"
#include "shadow.h"
#include "sky.h"
#include "../format/ngg_format.h"

#include <dxgi1_4.h>
#include <stdlib.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static_assert(sizeof(ngg_vertex) == sizeof(ngg::Vertex),
              "ngg_vertex must match ngg::Vertex - the editor uploads straight into the "
              "vertex buffer, so a size difference would shear the whole scene");
static_assert(sizeof(ngg_vertex) == 44, "ngg_vertex layout");

namespace {

const DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
const UINT        kBackBufferCount = 2;
const wchar_t*    kWindowClass = L"NggApiViewport";

// A near-copy of the viewer's swap chain. Duplicated rather than shared for now
// because the viewer's lives in its own translation unit and extracting it is a
// change to a working binary; if a third client appears it belongs in core.
struct SwapChain
{
    ComPtr<IDXGISwapChain3>      swap;
    ComPtr<ID3D12Resource>       buffers[kBackBufferCount];
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT                         rtvStride = 0;
    UINT                         width = 0, height = 0;

    bool Create(ngg::core::Device& dev, HWND hwnd, UINT w, UINT h)
    {
        width = w; height = h;

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
        if (FAILED(factory->CreateSwapChainForHwnd(dev.queue.Get(), hwnd, &sd,
                                                   nullptr, nullptr, &sc1)))
            return false;

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
        for (UINT i = 0; i < kBackBufferCount; ++i)
        {
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

        if (FAILED(swap->ResizeBuffers(kBackBufferCount, w, h, kBackBufferFormat, 0)))
            return false;

        width = w; height = h;
        return CreateViews(dev);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CurrentRtv() const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)swap->GetCurrentBackBufferIndex() * rtvStride;
        return h;
    }

    ID3D12Resource* CurrentBuffer() const
    {
        return buffers[swap->GetCurrentBackBufferIndex()].Get();
    }
};

LRESULT CALLBACK ViewportProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    // Input belongs to the host: it owns the tools, the modifiers and the undo
    // stack. This window exists to be drawn into.
    return DefWindowProcW(h, m, w, l);
}

void RegisterViewportClass()
{
    static bool done = false;
    if (done) return;
    done = true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ViewportProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
}

void Identity(float m[16])
{
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

} // namespace

struct ngg_scene_t
{
    ngg::core::Device     dev;
    SwapChain             sc;
    // The frame is ngg-core's (renderer.h) - the same one the viewer and the
    // game record, ray-traced shadows and AO included. The references keep
    // the many scene/post call sites below as they were.
    ngg::core::Renderer   core;
    ngg::core::Scene&     scene = core.GetScene();
    ngg::core::PostFx&    post  = core.Post();
    ngg::core::SkyParams  skyParams;
    float                 rain = 0.0f, snow = 0.0f;   // carried for the editor; not rendered yet

    HWND  hwnd = nullptr;
    float view[16];
    float proj[16];

    bool skyOn = true;
    bool aerial = true;
    bool taa = true;
    int  curve = ngg::core::PostFx::kCurveAces;

    std::string error;
    // ngg_capture_frame: where the next frame goes, and the readback it is
    // copied to (made on first use, remade if the size changes).
    std::string capturePath;
    Microsoft::WRL::ComPtr<ID3D12Resource> captureBuf;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT captureFoot = {};

    void Fail(const char* what) { error = what; ngg::core::Log("ngg-api: %s", what); }
};

// ------------------------------------------------------------------------ life

extern "C" NGG_API ngg_scene* ngg_create(HWND parent, int width, int height)
{
    if (width <= 0 || height <= 0) return nullptr;

    RegisterViewportClass();

    auto* s = new ngg_scene_t();
    Identity(s->view);
    Identity(s->proj);

    // A child window, because the host owns the layout. WS_CLIPSIBLINGS keeps
    // it from fighting neighbouring controls for the same pixels.
    s->hwnd = CreateWindowExW(
        0, kWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, width, height, parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!s->hwnd) { s->Fail("CreateWindowEx failed"); delete s; return nullptr; }

    // No LUID: nothing here is shared with another process, so the
    // highest-performance adapter is the right choice (see Device::Create).
    if (!s->dev.Create(nullptr, false))
    {
        s->Fail("D3D12 device creation failed");
        DestroyWindow(s->hwnd);
        delete s;
        return nullptr;
    }

    if (!s->sc.Create(s->dev, s->hwnd, (UINT)width, (UINT)height))
    {
        s->Fail("swap chain creation failed");
        DestroyWindow(s->hwnd);
        delete s;
        return nullptr;
    }

    // Sky, shadow maps, RT, post and the selection overlay (which depends on
    // the BACK BUFFER format and must survive a track change) - all built by
    // the renderer, once.
    if (!s->core.Create(s->dev, kBackBufferFormat, s->sc.width, s->sc.height, true))
    {
        s->Fail("renderer creation failed");
        DestroyWindow(s->hwnd);
        delete s;
        return nullptr;
    }

    // NGG_DEBUG_VIEW=<mode>: a debug view from the start (ngg_debug_view_name),
    // for a client without a key for it - 13, the RT AO, to measure its noise.
    if (const char* dv = getenv("NGG_DEBUG_VIEW")) s->scene.SetDebugMode(atoi(dv));
    // NGG_AO_RAYS=<n>: the AO's rays per pixel, past the tiers - a many-ray
    // run is the reference the tiers' noise is measured against.
    if (const char* ar = getenv("NGG_AO_RAYS")) s->core.SetAoRays((uint32_t)atoi(ar));
    // NGG_LIGHT_MODE=<mode>: the lighting from the start (ngg_light_mode_name) -
    // 0, RBR's own baked look, is the reference a dynamic-light change is
    // judged against.
    if (const char* lm = getenv("NGG_LIGHT_MODE")) s->scene.SetLightMode(atoi(lm));

    ngg::core::Log("ngg-api: viewport %dx%d ready", width, height);
    return s;
}

extern "C" NGG_API void ngg_destroy(ngg_scene* s)
{
    if (!s) return;
    s->dev.WaitForGpu();
    if (s->hwnd) DestroyWindow(s->hwnd);
    delete s;
}

extern "C" NGG_API void ngg_resize(ngg_scene* s, int width, int height)
{
    if (!s || width <= 0 || height <= 0) return;

    MoveWindow(s->hwnd, 0, 0, width, height, TRUE);
    if (s->sc.Resize(s->dev, (UINT)width, (UINT)height))
    {
        s->core.OnResize(s->dev, s->sc.width, s->sc.height);
    }
}

extern "C" NGG_API HWND ngg_hwnd(ngg_scene* s) { return s ? s->hwnd : nullptr; }

extern "C" NGG_API void ngg_set_log_sink(ngg_log_fn sink)
{
    ngg::core::SetLogSink(sink);
}

extern "C" NGG_API const char* ngg_last_error(ngg_scene* s)
{
    return s ? s->error.c_str() : "no scene";
}

// --------------------------------------------------------------------- content

extern "C" NGG_API int ngg_load_memory(ngg_scene* s, const void* bytes, size_t count)
{
    if (!s) return 0;
    if (!bytes || count < 16) { s->Fail("load: empty buffer"); return 0; }

    if (!s->core.LoadFromMemory(s->dev, bytes, count))
    {
        s->Fail("load: the buffer was rejected - see the log for which chunk");
        return 0;
    }

    // The file's default weather, as the game and the viewer use it - so the
    // editor opens a track under the same sky (NGGW, design 5.0q).
    if (const ngg::core::WeatherPreset* w = s->scene.FindWeather(nullptr))
    {
        ngg::core::ApplyWeather(*w, s->skyParams);
        s->rain = w->rain;
        s->snow = w->snow;
    }

    s->error.clear();
    return 1;
}

// The dynamic pool is not built yet. Failing loudly beats returning a handle
// that quietly draws nothing: a silent no-op here would look exactly like a
// transform bug in the editor, which is the expensive kind of confusion.
extern "C" NGG_API ngg_mesh ngg_add_mesh(ngg_scene* s, const ngg_vertex* verts,
                                         uint32_t vertexCount,
                                         const uint32_t* indices, uint32_t indexCount)
{
    if (!s) return NGG_INVALID;
    static_assert(sizeof(ngg_vertex) == sizeof(ngg::Vertex),
                  "ngg_vertex must match ngg::Vertex - the array is uploaded raw");
    const ngg_mesh h = s->scene.AddMesh(s->dev.device.Get(),
                                        reinterpret_cast<const ngg::Vertex*>(verts),
                                        vertexCount, indices, indexCount);
    if (!h)
        s->Fail("add_mesh: rejected - empty, an index count that is not a multiple "
                "of 3, or an index past the end of the vertex array");
    return h;
}

extern "C" NGG_API ngg_texture ngg_add_texture(ngg_scene* s, const void* dds, size_t bytes)
{
    if (!s) return NGG_INVALID;
    const ngg_texture h = s->scene.AddTexture(s->dev.device.Get(), s->dev.queue.Get(),
                                              dds, bytes);
    if (!h) s->Fail("add_texture: the DDS could not be decoded or uploaded");
    return h;
}

extern "C" NGG_API ngg_material ngg_add_material(ngg_scene* s, ngg_texture stage0,
                                                 ngg_texture stage1, uint32_t flags)
{
    if (!s) return NGG_INVALID;
    const ngg_material h = s->scene.AddMaterial(s->dev.device.Get(), stage0, stage1, flags);
    if (!h) s->Fail("add_material: the descriptor heap could not be rebuilt");
    return h;
}

extern "C" NGG_API ngg_instance ngg_add_instance(ngg_scene* s, ngg_mesh mesh,
                                                 ngg_material material, uint32_t layer,
                                                 const float xform[16])
{
    if (!s) return NGG_INVALID_INSTANCE;
    const ngg_instance i = s->scene.AddInstance(mesh, material, layer, xform);
    if (i == NGG_INVALID_INSTANCE)
        s->Fail("add_instance: no such mesh or material handle");
    return i;
}

extern "C" NGG_API int ngg_set_transform(ngg_scene* s, ngg_instance instance,
                                        const float xform[16])
{
    if (!s) return 0;
    if (!s->scene.SetObjectTransform(instance, xform))
    {
        s->Fail("set_transform: no such instance, or a null matrix");
        return 0;
    }
    return 1;
}

extern "C" NGG_API int32_t ngg_object_key(ngg_scene* s, ngg_instance instance)
{
    return s ? s->scene.ObjectKey(instance) : -1;
}

extern "C" NGG_API int ngg_object_bounds(ngg_scene* s, ngg_instance instance,
                                         float outMin[3], float outMax[3])
{
    if (!s) return 0;
    if (!s->scene.ObjectBounds(instance, outMin, outMax))
    {
        s->Fail("object_bounds: no such instance");
        return 0;
    }
    return 1;
}

extern "C" NGG_API int ngg_set_visible(ngg_scene* s, ngg_instance instance, int visible)
{
    if (!s) return 0;
    if (!s->scene.SetObjectVisible(instance, visible != 0))
    {
        s->Fail("set_visible: no such instance");
        return 0;
    }
    return 1;
}

extern "C" NGG_API int ngg_remove_instance(ngg_scene* s, ngg_instance instance)
{
    // Still a draw-list edit -- the geometry never leaves the buffer, which is
    // what makes undo cost the same nothing as the delete. The slot becomes a
    // tombstone rather than being reused, so every handle the editor holds
    // (including the ones in its undo stack) stays valid.
    if (!s) return 0;
    if (!s->scene.RemoveInstance(instance))
    {
        s->Fail("remove_instance: no such instance");
        return 0;
    }
    return 1;
}

extern "C" NGG_API int ngg_remove_mesh(ngg_scene* s, ngg_mesh mesh)
{
    if (!s) return 0;
    if (!s->scene.RemoveMesh(mesh))
    {
        s->Fail("remove_mesh: no such mesh, or instances still reference it");
        return 0;
    }
    return 1;
}

extern "C" NGG_API int ngg_compact(ngg_scene* s)
{
    if (!s) return 0;
    if (!s->scene.Compact(s->dev.device.Get()))
    {
        s->Fail("compact: the pool could not be re-uploaded");
        return 0;
    }
    return 1;
}

extern "C" NGG_API uint32_t ngg_instance_count(ngg_scene* s)
{
    return s ? s->scene.ObjectCount() : 0;
}

extern "C" NGG_API uint32_t ngg_visible_count(ngg_scene* s)
{
    return s ? s->scene.VisibleObjectCount() : 0;
}

// ------------------------------------------------------------------- materials
//
// The API's texture handles are 1-based and the renderer's indices are 0-based
// with kNoTexture for "none", so the two conversions live here and nowhere
// else. Doing it at the boundary is what keeps 0 meaning "empty" consistently
// on the managed side.
namespace {

uint32_t ToHandle(uint32_t index)
{
    return (index == ngg::kNoTexture) ? 0u : index + 1u;
}
uint32_t ToIndex(uint32_t handle)
{
    return (handle == 0u) ? ngg::kNoTexture : handle - 1u;
}

} // namespace

extern "C" NGG_API uint32_t ngg_material_count(ngg_scene* s)
{
    return s ? s->scene.MaterialCount() : 0;
}

extern "C" NGG_API int ngg_get_material(ngg_scene* s, ngg_material h, ngg_material_desc* out)
{
    if (!s || !out || h == 0) return 0;
    ngg::core::Scene::MaterialSlot m;
    if (!s->scene.GetMaterial(h - 1, &m)) { s->Fail("get_material: no such material"); return 0; }

    out->base_colour = ToHandle(m.tex[0]);
    out->detail      = ToHandle(m.tex[1]);
    out->normal      = ToHandle(m.tex[2]);
    out->orm         = ToHandle(m.tex[3]);
    out->emissive    = ToHandle(m.tex[4]);
    // The runtime-derived bits stay inside the renderer: they are a restatement
    // of which slots are filled, and handing them out would invite a caller to
    // set them and disagree with its own texture handles.
    out->flags = m.flags & (NGG_MATERIAL_ALPHA_TEST | NGG_MATERIAL_TWO_SIDED);
    for (int k = 0; k < 4; ++k) out->base_colour_factor[k] = m.baseColourFactor[k];
    out->roughness          = m.roughness;
    out->metallic           = m.metallic;
    out->normal_scale       = m.normalScale;
    out->occlusion_strength = m.occlusionStrength;
    out->emissive_strength  = m.emissiveStrength;
    out->alpha_cutoff       = m.alphaCutoff;
    return 1;
}

extern "C" NGG_API int ngg_set_material(ngg_scene* s, ngg_material h, const ngg_material_desc* in)
{
    if (!s || !in || h == 0) return 0;
    ngg::core::Scene::MaterialSlot m;
    if (!s->scene.GetMaterial(h - 1, &m)) { s->Fail("set_material: no such material"); return 0; }

    m.tex[0] = ToIndex(in->base_colour);
    m.tex[1] = ToIndex(in->detail);
    m.tex[2] = ToIndex(in->normal);
    m.tex[3] = ToIndex(in->orm);
    m.tex[4] = ToIndex(in->emissive);
    m.flags  = in->flags & (NGG_MATERIAL_ALPHA_TEST | NGG_MATERIAL_TWO_SIDED);
    for (int k = 0; k < 4; ++k) m.baseColourFactor[k] = in->base_colour_factor[k];
    m.roughness         = in->roughness;
    m.metallic          = in->metallic;
    m.normalScale       = in->normal_scale;
    m.occlusionStrength = in->occlusion_strength;
    m.emissiveStrength  = in->emissive_strength;
    m.alphaCutoff       = in->alpha_cutoff;

    if (!s->scene.SetMaterial(s->dev.device.Get(), h - 1, m))
    {
        s->Fail("set_material: update failed");
        return 0;
    }
    return 1;
}

extern "C" NGG_API uint32_t ngg_material_name(ngg_scene* s, ngg_material h,
                                              char* buf, uint32_t bytes)
{
    if (!s || !buf || bytes == 0 || h == 0) return 0;
    const char* name = s->scene.MaterialName(h - 1);
    if (!name) { buf[0] = 0; return 0; }

    uint32_t n = 0;
    while (name[n] && n + 1 < bytes) { buf[n] = name[n]; ++n; }
    buf[n] = 0;
    return n;
}

// Instance handles are NOT 1-based - they are object indices, passed straight
// through, as ngg_object_bounds and ngg_set_visible already do. Materials are,
// hence the asymmetry in one function.
extern "C" NGG_API ngg_material ngg_object_material(ngg_scene* s, ngg_instance instance)
{
    if (!s) return 0;
    uint32_t mat = 0;
    if (!s->scene.ObjectMaterial(instance, &mat)) return 0;
    return mat + 1;
}

// ------------------------------------------------------------------ view state

extern "C" NGG_API void ngg_set_camera(ngg_scene* s, const float view[16], const float proj[16])
{
    if (!s) return;
    if (view) for (int i = 0; i < 16; ++i) s->view[i] = view[i];
    if (proj) for (int i = 0; i < 16; ++i) s->proj[i] = proj[i];
}

extern "C" NGG_API void ngg_set_sun(ngg_scene* s, const float dir[3], float exposure)
{
    if (!s) return;
    if (dir)
    {
        s->skyParams.sunDir[0] = dir[0];
        s->skyParams.sunDir[1] = dir[1];
        s->skyParams.sunDir[2] = dir[2];
    }
    if (exposure > 0.0f) s->skyParams.exposure = exposure;
}

extern "C" NGG_API void ngg_set_debug_view(ngg_scene* s, int mode)
{
    if (s) s->scene.SetDebugMode(mode);
}

extern "C" NGG_API void ngg_set_light_mode(ngg_scene* s, int mode)
{
    if (s) s->scene.SetLightMode(mode);
}

extern "C" NGG_API void ngg_set_shadows(ngg_scene* s, int on)
{
    if (s) s->scene.SetShadowsOn(on != 0);
}

extern "C" NGG_API void ngg_set_sky(ngg_scene* s, int on, int aerial)
{
    if (!s) return;
    s->skyOn = on != 0;
    s->aerial = aerial != 0;
    s->scene.SetAtmosphere(s->skyParams, s->aerial);
}

extern "C" NGG_API void ngg_set_taa(ngg_scene* s, int on) { if (s) s->taa = on != 0; }

// ---- weather ---------------------------------------------------------------

namespace {

void ToDesc(const ngg::core::WeatherPreset& w, ngg_weather_desc* d)
{
    for (int k = 0; k < 3; ++k) { d->sun_direction[k] = w.sky.sunDir[k]; d->ground_colour[k] = w.sky.groundColour[k]; }
    d->sun_intensity  = w.sky.sunIntensity;
    d->turbidity      = w.sky.turbidity;
    d->ground_albedo  = w.sky.groundAlbedo;
    d->cloud_cover    = w.cloudCover;
    d->fog_visibility = w.fogVisibility;
    d->fog_height     = w.fogHeight;
    d->rain           = w.rain;
    d->snow           = w.snow;
    d->exposure       = w.sky.exposure;
}

void FromDesc(const ngg_weather_desc* d, ngg::core::WeatherPreset& w)
{
    for (int k = 0; k < 3; ++k) { w.sky.sunDir[k] = d->sun_direction[k]; w.sky.groundColour[k] = d->ground_colour[k]; }
    w.sky.sunIntensity = d->sun_intensity;
    w.sky.turbidity    = d->turbidity;
    w.sky.groundAlbedo = d->ground_albedo;
    w.cloudCover       = d->cloud_cover;
    w.fogVisibility    = d->fog_visibility;
    w.fogHeight        = d->fog_height;
    w.rain             = d->rain;
    w.snow             = d->snow;
    w.sky.exposure     = d->exposure;
}

} // namespace

extern "C" NGG_API int ngg_get_weather(ngg_scene* s, ngg_weather_desc* out)
{
    if (!s || !out) return 0;
    ngg::core::WeatherPreset w;
    w.sky = s->skyParams;
    w.cloudCover = s->skyParams.cloudCover;
    w.fogVisibility = s->skyParams.fogVisibility;
    w.fogHeight = s->skyParams.fogHeight;
    w.rain = s->rain;
    w.snow = s->snow;
    ToDesc(w, out);
    return 1;
}

extern "C" NGG_API int ngg_set_weather(ngg_scene* s, const ngg_weather_desc* in)
{
    if (!s || !in) return 0;
    ngg::core::WeatherPreset w;
    FromDesc(in, w);
    // The exposure is the preset's, explicitly: an editor setting it means it.
    ngg::core::ApplyWeather(w, s->skyParams);
    s->skyParams.exposure = in->exposure;
    s->rain = in->rain;
    s->snow = in->snow;
    return 1;       // ngg_render syncs the atmosphere every frame - visible next frame
}

extern "C" NGG_API uint32_t ngg_weather_preset_count(ngg_scene* s)
{
    return s ? (uint32_t)s->scene.WeatherPresets().size() : 0;
}

extern "C" NGG_API uint32_t ngg_weather_preset_name(ngg_scene* s, uint32_t index, char* buf, uint32_t bytes)
{
    if (!s || !buf || !bytes || index >= s->scene.WeatherPresets().size()) return 0;
    const std::string& n = s->scene.WeatherPresets()[index].name;
    strncpy_s(buf, bytes, n.c_str(), _TRUNCATE);
    return (uint32_t)strlen(buf);
}

extern "C" NGG_API int ngg_get_weather_preset(ngg_scene* s, uint32_t index, ngg_weather_desc* out)
{
    if (!s || !out || index >= s->scene.WeatherPresets().size()) return 0;
    ToDesc(s->scene.WeatherPresets()[index], out);
    return 1;
}

extern "C" NGG_API int ngg_select_weather_preset(ngg_scene* s, uint32_t index)
{
    ngg_weather_desc d;
    return ngg_get_weather_preset(s, index, &d) && ngg_set_weather(s, &d);
}

extern "C" NGG_API uint32_t ngg_weather_to_json(const ngg_weather_desc* d, const char* name, const char* source,
                                                char* buf, uint32_t bytes)
{
    if (!d || !buf || !bytes) return 0;
    ngg::core::WeatherPreset w;
    FromDesc(d, w);
    w.name = name ? name : "";
    w.source = source ? source : "Wallaby";
    const std::string j = ngg::core::WeatherToJson(w);
    if (j.size() + 1 > bytes) return 0;
    memcpy(buf, j.c_str(), j.size() + 1);
    return (uint32_t)j.size();
}

extern "C" NGG_API int ngg_weather_from_json(const char* json, ngg_weather_desc* out,
                                             char* name, uint32_t nameBytes)
{
    if (!json || !out) return 0;
    ngg::core::WeatherPreset w;
    if (!ngg::core::WeatherFromJson(json, w)) return 0;
    ToDesc(w, out);
    if (name && nameBytes) strncpy_s(name, nameBytes, w.name.c_str(), _TRUNCATE);
    return 1;
}

extern "C" NGG_API void ngg_set_quality(ngg_scene* s, int tier)
{
    if (s) s->core.SetQuality((ngg::core::Quality)(tier < 0 ? 0 : tier > 2 ? 2 : tier));
}

extern "C" NGG_API void ngg_set_ray_tracing(ngg_scene* s, int on)
{
    if (s) s->core.SetPreferRayTracedShadows(on != 0);
}

extern "C" NGG_API int ngg_get_stats(ngg_scene* s, ngg_stats* out)
{
    if (!s || !out) return 0;
    *out = {};
    // The frame is synchronous (ngg_render waits for it), so last frame's
    // counters and timestamps have landed.
    ngg::core::Rt& rt = s->core.Raytracer();
    rt.ReadStats();
    out->pass_ms    = (float)s->scene.LastPassMs();
    out->env_ms     = (float)s->scene.LastEnvMs();
    out->aerial_ms  = (float)s->scene.LastAerialMs();
    out->rt_active  = s->core.RayTracedShadowsActive() ? 1u : 0u;
    out->rt_available = s->core.RayTracingAvailable() ? 1u : 0u;
    if (out->rt_active)
    {
        out->rt_ms      = (float)rt.LastMs();
        out->ao_ms      = (float)rt.AoMs();
        out->denoise_ms = (float)rt.DenoiseMs();
        out->rt_pixels  = rt.TracedPixels();
        out->rt_shadowed = rt.ShadowedPixels();
    }
    else if (const char* why = s->core.RtFallbackReason())
        strncpy_s(out->rt_fallback, sizeof out->rt_fallback, why, _TRUNCATE);
    out->quality    = (uint32_t)s->core.GetQuality();
    out->taa        = s->post.Taa() ? 1u : 0u;
    out->shadows    = s->scene.ShadowsOn() ? 1u : 0u;
    out->ambient_occlusion = (out->rt_active && s->core.AmbientOcclusion()) ? 1u : 0u;
    out->auto_exposure = s->post.AutoExposure() ? 1u : 0u;
    out->debug_view = (uint32_t)s->scene.DebugMode();
    out->exposure   = s->post.LastExposure();
    out->metered_ev = s->post.LastMeteredEv();
    out->adapted_ev = s->post.LastAdaptedEv();
    return 1;
}

extern "C" NGG_API void ngg_set_auto_exposure(ngg_scene* s, int on)
{
    if (s) s->post.SetAutoExposure(on != 0);
}

extern "C" NGG_API void ngg_set_tonemap(ngg_scene* s, int curve) { if (s) s->curve = curve; }

extern "C" NGG_API const char* ngg_debug_view_name(int mode)
{
    return ngg::core::Scene::DebugModeName(mode);
}

extern "C" NGG_API const char* ngg_light_mode_name(int mode)
{
    return ngg::core::Scene::LightModeName(mode);
}

extern "C" NGG_API void ngg_set_weld_normals(ngg_scene* s, int on)
{
    if (s) s->scene.SetWeldNormals(on != 0);
}

extern "C" NGG_API void ngg_set_foliage_normals(ngg_scene* s, int on)
{
    if (s) s->scene.SetFoliageNormals(on != 0);
}

extern "C" NGG_API void ngg_set_cull(ngg_scene* s, int on)
{
    if (s) s->scene.SetCull(on != 0);
}

// --------------------------------------------------------------- editor extras

extern "C" NGG_API void ngg_set_selection(ngg_scene* s, const ngg_instance* instances,
                                         uint32_t count)
{
    if (!s) return;
    if (!s->scene.SetSelection(instances, count))
        s->Fail("set_selection: an instance handle was out of range - selection cleared");
}

extern "C" NGG_API int ngg_selection_stats(ngg_scene* s, uint32_t* visiblePixels,
                                           uint32_t* occludedPixels)
{
    if (!s) return 0;
    if (visiblePixels)  *visiblePixels  = s->scene.SelectionPixelsVisible();
    if (occludedPixels) *occludedPixels = s->scene.SelectionPixelsOccluded();
    return 1;
}

extern "C" NGG_API int ngg_set_overlay_lines(ngg_scene* s, uint32_t layer,
                                             const ngg_overlay_vertex* verts,
                                             uint32_t count, uint32_t flags)
{
    if (!s) return 0;
    static_assert(sizeof(ngg_overlay_vertex) == sizeof(ngg::core::Scene::OverlayVertex),
                  "the C and C++ overlay vertex must agree - the buffer is uploaded raw");
    const auto* v = reinterpret_cast<const ngg::core::Scene::OverlayVertex*>(verts);
    if (!s->scene.SetOverlayLines(s->dev.device.Get(), layer, v, count, flags))
    {
        s->Fail("set_overlay_lines: rejected - layer must be under 64 and the "
                "vertex count even, since a segment is a pair");
        return 0;
    }
    return 1;
}

extern "C" NGG_API int ngg_set_font(ngg_scene* s, const void* rgba,
                                   uint32_t width, uint32_t height,
                                   uint32_t columns, uint32_t rows, uint32_t firstChar)
{
    if (!s) return 0;
    if (!s->scene.SetFont(s->dev.device.Get(), s->dev.queue.Get(), rgba,
                          width, height, columns, rows, firstChar))
    {
        s->Fail("set_font: the atlas could not be uploaded");
        return 0;
    }
    return 1;
}

extern "C" NGG_API int ngg_set_labels(ngg_scene* s, uint32_t layer,
                                      const ngg_label* labels, uint32_t count,
                                      const char* textBlob, uint32_t blobBytes,
                                      float pixelHeight, uint32_t flags)
{
    if (!s) return 0;
    static_assert(sizeof(ngg_label) == sizeof(ngg::core::Scene::Label),
                  "the C and C++ label structs must agree");
    const auto* l = reinterpret_cast<const ngg::core::Scene::Label*>(labels);
    if (!s->scene.SetLabels(s->dev.device.Get(), layer, l, count,
                            textBlob, blobBytes, pixelHeight, flags))
    {
        s->Fail("set_labels: rejected - layer must be under 64, a font must be set "
                "with ngg_set_font first, and pixel_height must be positive");
        return 0;
    }
    return 1;
}

extern "C" NGG_API int ngg_label_stats(ngg_scene* s, uint32_t* visiblePixels,
                                       uint32_t* occludedPixels)
{
    if (!s) return 0;
    if (visiblePixels)  *visiblePixels  = s->scene.LabelPixelsVisible();
    if (occludedPixels) *occludedPixels = s->scene.LabelPixelsOccluded();
    return 1;
}

extern "C" NGG_API void ngg_clear_overlays(ngg_scene* s)
{
    if (s) s->scene.ClearOverlays();
}

extern "C" NGG_API int ngg_overlay_stats(ngg_scene* s, uint32_t* visiblePixels,
                                         uint32_t* occludedPixels)
{
    if (!s) return 0;
    if (visiblePixels)  *visiblePixels  = s->scene.OverlayPixelsVisible();
    if (occludedPixels) *occludedPixels = s->scene.OverlayPixelsOccluded();
    return 1;
}

extern "C" NGG_API void ngg_probe_pixel(ngg_scene* s, int x, int y)
{
    if (s) s->scene.SetProbe(x, y);
}

extern "C" NGG_API int ngg_bounds(ngg_scene* s, float outMin[3], float outMax[3])
{
    if (!s || !s->scene.Ready() || !outMin || !outMax) return 0;
    s->scene.Bounds(outMin, outMax);
    return 1;
}

extern "C" NGG_API int ngg_focus_point(ngg_scene* s, float outPoint[3])
{
    if (!s || !s->scene.Ready() || !outPoint) return 0;
    return s->scene.FocusPoint(outPoint) ? 1 : 0;
}

// ngg_capture_frame's two halves: the back buffer copied out while the list is
// open (left in COPY_SOURCE), and the copy written as a .bmp once it is done.
static bool RecordCapture(ngg_scene* s)
{
    ID3D12Resource* bb = s->sc.CurrentBuffer();
    const D3D12_RESOURCE_DESC bd = bb->GetDesc();
    if (!s->captureBuf || s->captureFoot.Footprint.Width != (UINT)bd.Width ||
        s->captureFoot.Footprint.Height != bd.Height)
    {
        UINT64 bytes = 0;
        s->dev.device->GetCopyableFootprints(&bd, 0, 1, 0, &s->captureFoot, nullptr, nullptr, &bytes);
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
        s->captureBuf.Reset();
        if (FAILED(s->dev.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s->captureBuf))))
        {
            s->Fail("capture: readback buffer creation failed");
            return false;
        }
    }
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    s->dev.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = s->captureBuf.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = s->captureFoot;
    src.pResource = bb;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s->dev.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    return true;
}

static void WriteCapture(ngg_scene* s)
{
    const UINT w = s->captureFoot.Footprint.Width, h = s->captureFoot.Footprint.Height;
    const UINT pitch = s->captureFoot.Footprint.RowPitch;
    const unsigned char* p = nullptr;
    // The whole buffer: its last row is not padded to the pitch.
    if (FAILED(s->captureBuf->Map(0, nullptr, (void**)&p))) { s->Fail("capture: readback would not map"); return; }
    FILE* f = fopen(s->capturePath.c_str(), "wb");
    if (!f) ngg::core::Log("ngg-api: capture: cannot write %s", s->capturePath.c_str());
    if (f)
    {
        // 24-bit, bottom-up, BGR - from the R8G8B8A8 back buffer.
        const UINT stride = (w * 3 + 3) & ~3u;
        BITMAPFILEHEADER fh = {};
        BITMAPINFOHEADER bi = {};
        fh.bfType = 0x4d42;
        fh.bfOffBits = sizeof fh + sizeof bi;
        fh.bfSize = fh.bfOffBits + stride * h;
        bi.biSize = sizeof bi;
        bi.biWidth = (LONG)w;
        bi.biHeight = (LONG)h;
        bi.biPlanes = 1;
        bi.biBitCount = 24;
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi, sizeof bi, 1, f);
        std::vector<unsigned char> row(stride, 0);
        for (UINT y = 0; y < h; ++y)
        {
            const unsigned char* r = p + (size_t)(h - 1 - y) * pitch;
            for (UINT x = 0; x < w; ++x)
            {
                row[x * 3 + 0] = r[x * 4 + 2];
                row[x * 3 + 1] = r[x * 4 + 1];
                row[x * 3 + 2] = r[x * 4 + 0];
            }
            fwrite(row.data(), 1, stride, f);
        }
        fclose(f);
    }
    D3D12_RANGE none = { 0, 0 };
    s->captureBuf->Unmap(0, &none);
}

extern "C" NGG_API void ngg_capture_frame(ngg_scene* s, const char* path)
{
    if (s) s->capturePath = path ? path : "";
}

extern "C" NGG_API void ngg_render(ngg_scene* s)
{
    if (!s || !s->sc.swap) return;
    if (!s->dev.Begin()) { s->Fail("render: command list would not open"); return; }


    D3D12_RESOURCE_BARRIER toRt = {};
    toRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRt.Transition.pResource = s->sc.CurrentBuffer();
    toRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s->dev.list->ResourceBarrier(1, &toRt);

    // The frame, exactly as the viewer and the game record it. The editor's
    // switches are settings on it; the order and the curve rules are not.
    // While the editor has added or moved objects, the renderer uses the
    // cascades instead of RT shadows - the static BVH would cast shadows from
    // where things used to be (renderer.h).
    s->core.SetSkyOn(s->skyOn);
    s->core.SetTaa(s->taa);
    s->core.SetCurve(s->curve);
    s->core.RecordFrame(s->dev, s->sc.CurrentRtv(), s->view, s->proj,
                        s->sc.width, s->sc.height, s->skyParams);

    // AFTER the tonemap and after TAA, straight onto the back buffer, but still
    // depth-tested against the scene's own buffer - which is the whole reason
    // the overlay lives in here rather than in the host's paint handler.
    s->scene.RenderEditorOverlay(s->dev.list.Get(), s->sc.CurrentRtv(), s->view, s->proj,
                                 s->sc.width, s->sc.height);

    D3D12_RESOURCE_BARRIER toPresent = toRt;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    const bool capture = !s->capturePath.empty() && RecordCapture(s);
    if (capture) toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    s->dev.list->ResourceBarrier(1, &toPresent);

    s->dev.EndAndWait();
    if (capture) WriteCapture(s);
    s->capturePath.clear();
    s->scene.LogProbe();               // safe here: EndAndWait already blocked
    s->scene.ReadOverlayStats();       // same reason
    // NGG_RT_TIMING: the ray tracing's GPU time, averaged over 240 frames, to
    // the log - the passes' cost for a client with no HUD of its own.
    static const bool rtTiming = getenv("NGG_RT_TIMING") != nullptr;
    if (rtTiming)
    {
        static double sum[3] = {};
        static int n = 0;
        ngg::core::Rt& rt = s->core.Raytracer();
        rt.ReadStats();
        sum[0] += rt.LastMs() - rt.AoMs(); sum[1] += rt.AoMs(); sum[2] += rt.DenoiseMs();
        if (++n == 240)
        {
            ngg::core::Log("rt: shadow %.3f ms, ao %.3f ms, denoise %.3f ms (240-frame mean)",
                           sum[0] / n, sum[1] / n, sum[2] / n);
            sum[0] = sum[1] = sum[2] = 0.0; n = 0;
        }
    }
    s->dev.DrainMessages();
    s->sc.swap->Present(1, 0);
}
