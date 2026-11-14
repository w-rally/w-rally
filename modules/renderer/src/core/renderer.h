// ---------------------------------------------------------------------------
// Renderer - ONE frame, recorded the same way for every client (design §1.4).
//
// Until this existed, each client wired the frame up itself: the viewer had a
// depth prepass, ray-traced shadows and AO, a denoiser and a carefully ordered
// jitter; the game host had sky, cascaded shadows, scene and post; Wallaby's
// API had the game host's frame. So "what the viewer shows" and "what the game
// shows" were two code paths, and every feature built and measured in the
// viewer silently stopped at the viewer's edge - the environment map was the
// first case found (5.0i), the whole RT path the second (5.0m).
//
// The rule now: a client owns its window, its input, its overlays and its
// presentation; the Renderer owns everything between "here is a camera" and
// "the tonemapped image is in your target". Settings are the client's to
// choose; ORDER and CORRECTNESS are not.
//
// It also owns the one policy no client should have to know: whether the
// ray-traced shadows are valid for the scene as it stands. KeepRtCurrent keeps
// one BLAS per mesh key (the world, each moved object, each added mesh) and a
// TLAS of their instances: a new mesh builds its BLAS, and when only
// transforms changed (a car driving, a cone knocked over) just the instances
// and the TLAS are rebuilt - so moving objects keep their ray-traced shadows.
// Only a failed build falls back to the cascaded maps.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <memory>

#include "device.h"
#include "scene.h"
#include "sky.h"
#include "shadow.h"
#include "post.h"
#include "rt.h"
#include "gtao.h"
#include "visibility.h"
#include "nrd_visibility.h"
#include "upscaler.h"

namespace ngg {
namespace core {

// QUALITY TIERS (design §1.5): one technique-agnostic lighting path, scaled
// by tier rather than forked into a second renderer. VR at 90 Hz in stereo is
// the reason they exist from P1; the floor hardware is the reason they matter
// now. What each tier changes is only what has been MEASURED to matter
// (5.0m): AO is 75-80 % of the ray-tracing cost, the RT sun shadow is cheap
// and cheaper than the cascades on real geometry - so every tier keeps RT
// shadows (falling back to cascades only where RT is unavailable) and the
// tiers differ in AO. New techniques join by adding a field here, not a mode.
// Where the frame has no RT AO (the cascades, or AoSource::ScreenSpace) the AO
// is XeGTAO's at the tier's gtaoQuality.
enum class Quality { Low = 0, Medium = 1, High = 2 };

// TEMPORAL UPSCALING (upscaler.h): which upscaler replaces the TAA, and at what
// render scale. None is our own TAA at the output size - the default, and the
// frame exactly as it was. DLSS is the second backend (RTX only).
enum class UpscalerKind { None = 0, Fsr = 1, Dlss = 2 };

// WHAT DENOISES THE RAY-TRACED VISIBILITY (visibility.h): our own denoisers (RtVisibility, NGG design 4.5) or NVIDIA
// NRD's SIGMA and REBLUR over the same traces (NrdVisibility).
enum class Denoiser { Ours = 0, Nrd = 1 };

struct QualityTier
{
    const char* name;
    bool        ao;           // RT ambient occlusion
    uint32_t    aoRays;       // per pixel per frame; the denoiser accumulates
    int         gtaoQuality;  // the screen-space AO's (Gtao::SetQuality)
    float       sharpen;      // TAA post-sharpen
};

class Renderer
{
public:
    // The tier's settings, applied as a set. The individual setters below
    // stay for A/B work; a client that changes one is no longer "on" a tier.
    static const QualityTier& Tier(Quality q);
    void    SetQuality(Quality q);
    Quality GetQuality() const { return m_quality; }

    // outputFormat: the client's final target (swap chain, shared texture).
    // rayTracing: create the RT path at all (it still needs an adapter that
    // supports it; Rt::Create logs and declines otherwise).
    bool Create(Device& dev, DXGI_FORMAT outputFormat, UINT width, UINT height,
                bool rayTracing = true);
    void OnResize(Device& dev, UINT width, UINT height);

    // The upscaler and the render scale per axis (1 native; FSR's and DLSS's
    // Quality is 0.67, Balanced 0.58, Performance 0.5). Resizes the render-size
    // targets when it changes; call between frames. An upscaler that cannot run
    // here (DLSS without an RTX GPU) leaves None (logged).
    void SetUpscaling(Device& dev, UpscalerKind kind, float scale);
    UpscalerKind Upscaling() const      { return m_upKind; }
    // Whether a backend can run on this adapter (probed once at Create: its DLL, the GPU, the driver) - what a
    // settings menu may offer. None is always available.
    bool UpscalerAvailable(UpscalerKind kind) const
    { return kind == UpscalerKind::None || (kind == UpscalerKind::Fsr ? m_fsrAvailable : m_dlssAvailable); }
    float RenderScale() const           { return m_upKind == UpscalerKind::None ? 1.0f : m_renderScale; }

    bool Load(Device& dev, const wchar_t* path);
    bool LoadFromMemory(Device& dev, const void* data, size_t bytes);

    // Records the whole frame into dev.list (already open): environment and
    // aerial tables, depth+normal prepass, sun visibility (ray traced or
    // cascades), AO, sky, scene, TAA, exposure and tonemap - into `outRtv`,
    // which the caller has in RENDER_TARGET. The caller draws its overlays on
    // top afterwards, and handles its own target's barriers and presentation.
    void RecordFrame(Device& dev, D3D12_CPU_DESCRIPTOR_HANDLE outRtv,
                     const float* view, const float* proj, UINT width, UINT height,
                     const SkyParams& sky);

    // ---- settings: the client's to choose
    void SetPreferRayTracedShadows(bool on) { m_preferRt = on; }
    bool PreferRayTracedShadows() const     { return m_preferRt; }
    void SetAmbientOcclusion(bool on)       { m_ao = on; m_scene.SetAmbientOcclusion(on); }
    bool AmbientOcclusion() const           { return m_ao; }
    void SetAoRays(uint32_t n)              { m_aoRays = n ? n : 1; }
    // The AO a ray-traced frame takes: its own (Traced, the default) or XeGTAO's (ScreenSpace), the cheap mode. The
    // cascades always take XeGTAO's.
    void SetAoSource(AoSource s)            { m_rtVis.SetAoSource(s); m_nrdVis.SetAoSource(s); }
    AoSource GetAoSource() const            { return m_rtVis.GetAoSource(); }
    // What denoises the ray-traced visibility: ours (RtVisibility, the default) or NVIDIA NRD's (NrdVisibility, made
    // the first time it is chosen). Where NRD cannot run, ours (logged). The cascades are not denoised.
    void SetDenoiser(Device& dev, Denoiser d);
    Denoiser GetDenoiser() const            { return m_denoiser; }
    // Whether NRD may be offered: in this build, and the adapter ray traces.
    bool DenoiserAvailable(Denoiser d) const { return d == Denoiser::Ours || (NrdVisibility::InBuild() && m_rt.Ready()); }
    // This frame's: "ours", "NRD"; "" without ray tracing. And its GPU time, last frame's.
    const char* DenoiserName() const;
    double DenoiseMs() const;
    void ReadDenoiserStats()                { m_nrdVis.ReadStats(); }    // after the GPU wait, as Rt::ReadStats
    // The screen-space AO's knobs (the tier sets the quality).
    Gtao& ScreenSpaceAo()                   { return m_gtao; }
    uint32_t AoRays() const                 { return m_aoRays; }
    // What is traced: twice the rays when the render size has at most half the output's pixels (an upscaler at
    // Quality and below), so the AO keeps about as many rays per OUTPUT pixel as native - its one-ray grain is
    // otherwise denoised at the render size and blown up by the upscale, in blotches 1.5-2x the size. Paid out of the
    // ray tracing the smaller frame saved.
    uint32_t AoRaysTraced() const
    {
        return (uint64_t)m_renderWidth * m_renderHeight * 2 <= (uint64_t)m_width * m_height ? m_aoRays * 2 : m_aoRays;
    }
    void SetSkyOn(bool on)                  { m_skyOn = on; }
    bool SkyOn() const                      { return m_skyOn; }
    void SetTaa(bool on)                    { m_taa = on; }
    bool Taa() const                        { return m_taa; }
    // The curve for normal viewing. Debug views always get raw and the baked
    // reference always gets the sRGB encode - those are rules, not choices.
    void SetCurve(int curve)                { m_curve = curve; }
    int  Curve() const                      { return m_curve; }

    // ---- state, for readouts
    bool RayTracedShadowsActive() const { return m_rtActive; }   // this frame
    // This frame's AO: applied at all, where it came from ("ray traced, 1 ray", "XeGTAO medium", "off") and its GPU
    // time, last frame's.
    bool AoApplied() const              { return m_ao && m_vis.ao != nullptr; }
    const char* AoSourceName() const    { return AoApplied() ? m_vis.aoSource : "off"; }
    double AoMs() const                 { return AoApplied() ? m_vis.aoMs : 0.0; }
    // A camera's cut: TAA's history (and the upscaler's), the shadow and AO sources' - whatever each keeps.
    void ResetHistory();
    // The whole frame's GPU time (RecordFrame, first command to last), last frame's. The one number a change of
    // what the GPU does is judged by: a pass's own timestamps are not (design §5.5).
    double LastFrameMs() const          { return m_frameMs; }
    // The size the scene is rendered at, and what upscales it ("" none: our own TAA, at the output size).
    UINT RenderWidth() const            { return m_renderWidth; }
    UINT RenderHeight() const           { return m_renderHeight; }
    const char* UpscalerName() const    { return m_post.Upscaling() ? m_upscaler->Name() : ""; }
    bool RayTracingAvailable() const    { return m_rt.Ready(); }
    // Why RT shadows are not active this frame; null when they are.
    const char* RtFallbackReason() const;

    // ---- the parts, for client-specific work (overlays, editing, dumps)
    Scene&      GetScene()   { return m_scene; }
    Sky&        GetSky()     { return m_sky; }
    PostFx&     Post()       { return m_post; }
    Rt&         Raytracer()  { return m_rt; }
    ShadowMaps& Shadows()    { return m_shadows; }

private:
    void KeepRtCurrent(Device& dev);

    Scene      m_scene;
    Sky        m_sky;
    ShadowMaps m_shadows;
    ShadowMaps m_localShadows;   // the local lights' maps without the ray tracer (Scene::RenderLocalShadows)
    PostFx     m_post;
    Rt         m_rt;
    Gtao       m_gtao;
    // The sources of the sun's visibility and the AO (visibility.h), one of them per frame.
    RtVisibility      m_rtVis{ m_rt, m_gtao };
    CascadeVisibility m_cascadeVis{ m_scene, m_gtao };
    NrdVisibility     m_nrdVis{ m_rt, m_gtao };
    Denoiser          m_denoiser = Denoiser::Ours;
    VisibilityProvider* m_lastSource = nullptr;    // last frame's: another one starts from no history
    Visibility        m_vis;                       // this frame's
    // Out-of-band work while the client's list is open: the BVH rebuild after
    // an edit. Rt::Build is synchronous on whatever context it is given; on the
    // client's own one it reset an allocator mid-recording and failed.
    Device     m_buildDev;

    DXGI_FORMAT m_outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT  m_width = 0, m_height = 0;                 // the output
    UINT  m_renderWidth = 0, m_renderHeight = 0;     // the scene, the prepass, the ray tracing
    UpscalerKind m_upKind = UpscalerKind::None;
    float m_renderScale = 1.0f;
    std::unique_ptr<Upscaler> m_upscaler;
    bool m_fsrAvailable = false, m_dlssAvailable = false;
    bool  m_preferRt = true;
    bool  m_ao = true;
    uint32_t m_aoRays = 1;          // 5.0m: one ray, accumulated by the denoiser
    Quality  m_quality = Quality::Medium;
    bool  m_skyOn = true;
    bool  m_taa = true;
    int   m_curve = PostFx::kCurveAces;

    uint32_t    m_rtRevision = 0xFFFFFFFFu;   // scene geometry the BVH was built from
    bool        m_rtValid = false;            // the TLAS holds the scene's meshes whose BLASes are built (design §4.2)
    bool        m_rtActive = false;
    bool        m_rtTraced = false;           // m_rtActive as of the last frame that drew the world
    const char* m_rtReason = "not built yet";

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_frameTimer;       // [0] the frame's first command, [1] its last
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_frameReadback;
    UINT64      m_frameHz = 0;
    bool        m_framePending = false;
    double      m_frameMs = 0.0;
};

} // namespace core
} // namespace ngg
