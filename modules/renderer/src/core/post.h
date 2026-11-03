// ---------------------------------------------------------------------------
// PostFx - the HDR intermediate, temporal anti-aliasing, and the tonemap.
//
// Everything before this wrote LINEAR HDR radiance straight into an 8-bit UNORM
// target, which meant two separate compromises: the image hard-clipped at 1.0
// (the sun and anything near it went flat white), and the only "tonemap" was
// 1-exp(-x), chosen because it was one line rather than because it looked like
// anything. Both are display problems, not lighting problems, and they belong
// in one pass at the end rather than smeared across every shader that writes
// colour - which is how the exposure control ended up working on the sky and
// not on the surfaces (design 5.0).
//
// TAA IS FIRST BECAUSE IT IS LOAD-BEARING, not because aliasing is the worst
// artefact. Soft shadows (PCSS), SSAO, stochastic alpha and every RT effect are
// noisy techniques that are only affordable when a temporal filter is already
// there to accumulate them. Building them first means building them twice.
//
// REPROJECTION NEEDS NO MOTION VECTORS, and that is a property of our content
// rather than a shortcut: the converter bakes every transform into world space
// (ngg_format.h; movable meshes are design 9b item 5), so the world is rigid
// and the only thing that moves is the camera. Camera reprojection from depth
// is then EXACT, not an approximation. The day per-object transforms land
// (format v2) this stops being true and a velocity buffer has to appear with
// them - the assumption is written here so it cannot be forgotten there.
//
// The matrices follow ngg-core's camera contract (see scene.h): row-major, row
// vector. PostFx does not build them - it takes `Scene::LastViewProj`, which is
// reverse-Z included and JITTER EXCLUDED. That asymmetry is deliberate:
// reverse-Z is part of what the depth buffer means, while the jitter is not.
// TAA accumulates into a fixed pixel grid, and the jitter only varies where
// inside a pixel the scene is sampled. Reproject through jittered matrices and
// the history follows the jitter rather than staying put, so the resolved image
// oscillates over the whole Halton pattern - a one-pixel shake that reads as a
// tuning problem and is not one.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <stdint.h>

#include "upscaler.h"

namespace ngg {
namespace core {

class PostFx
{
public:
    // Tonemap curves. 0 and 1 exist to keep A/B comparison possible, which is
    // how every lighting question in this project has actually been settled.
    enum Curve
    {
        kCurveRaw = 0,   // straight copy: debug views carry false colour, not radiance
        kCurveSrgb,      // saturate + encode: RBR's baked look, byte-identical to before
        kCurveExp,       // 1-exp(-x*E): what dynamic lighting shipped with, kept as reference
        kCurveAces,      // Narkowicz fitted ACES - the default
        kCurveAgx,       // AgX - holds hue through a blown-out sun better than ACES
        kCurveCount
    };
    static const char* CurveName(int c);

    // The intermediate everything draws into. Callers build their sky and
    // scene PSOs against THIS, not against their own back-buffer format -
    // an 8-bit intermediate would clip the highlights before the tonemap
    // ever sees them, which is the whole problem this pass exists to fix.
    static constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // The one colour the HDR target may be cleared to. It is baked in as the
    // resource's optimised clear value, so clearing to anything else is both
    // slower and a validation warning every frame - and clearing to nothing is
    // the same warning in the other direction. Callers that clear (only the
    // sky-off path does) must use this.
    static constexpr float kHdrClear[4] = { 0.06f, 0.08f, 0.12f, 1.0f };

    // outputFormat is the caller's final target (a swap chain or a shared
    // texture). The HDR intermediate is always RGBA16F.
    bool Create(ID3D12Device* device, DXGI_FORMAT outputFormat);
    bool Ready() const { return m_psoTonemap != nullptr; }

    // Allocates the HDR target and the two history buffers. Call before the
    // first frame and on every resize; history is invalidated, because a
    // resized history is a different image.
    bool OnResize(ID3D12Device* device, UINT width, UINT height)
    {
        return OnResize(device, width, height, width, height);
    }
    // With an upscaler (SetUpscaler): the HDR target, the TAA's histories and
    // the upscaler's inputs at the RENDER size, its output and the tonemap at
    // the OUTPUT size. Without one the two are the same.
    bool OnResize(ID3D12Device* device, UINT renderWidth, UINT renderHeight,
                  UINT outputWidth, UINT outputHeight);

    // The temporal upscaler that replaces the TAA (upscaler.h), or null for
    // our own TAA at the output size - the default, and exactly as before. Not
    // owned; set before OnResize, which sizes it. It runs where the TAA would:
    // not in debug views and not with TAA off, which then show the render-size
    // image stretched.
    void SetUpscaler(Upscaler* u)          { m_upscaler = u; m_historyValid = false; }
    Upscaler* GetUpscaler() const          { return m_upscaler; }
    bool Upscaling() const                 { return m_upscaler && m_upReady; }
    // The camera, for the upscaler's depth: near and far planes, vertical
    // field of view in radians.
    void SetCamera(float zNear, float zFar, float fovY) { m_near = zNear; m_far = zFar; m_fovY = fovY; }

    // Where the sky and the scene draw. Transitions the HDR target and clears
    // nothing - the sky pass covers every pixel.
    D3D12_CPU_DESCRIPTOR_HANDLE Begin(ID3D12GraphicsCommandList* list);

    // TAA then tonemap, into the caller's target. `sceneViewProj` is the exact
    // matrix the frame was rendered with; `depth` is the scene's depth buffer,
    // which is transitioned to a shader resource and back to DEPTH_WRITE.
    // `motion` is Scene::MotionBuffer when this frame ran the prepass, else
    // null (every pixel then reprojects by the camera); RENDER_TARGET on entry
    // and on return. `rtShadow` is Rt::Filtered when the ray-traced shadow ran
    // this frame, else null: its z says where a moved object's shadow was
    // (PSTaa); `rtAo` likewise Rt::AoDenoised, for its occlusion, and
    // `rtAoPrev` Rt::AoPrevDenoised, last frame's, and `rtLights`
    // Rt::LightsDenoised, for the local lights' shadows.
    // PIXEL_SHADER_RESOURCE, and left so. `glassMotion` is
    // Scene::GlassMotionBuffer with `motion` (null without), in its state.
    void Resolve(ID3D12GraphicsCommandList* list,
                 D3D12_CPU_DESCRIPTOR_HANDLE outRtv,
                 ID3D12Resource* depth,
                 ID3D12Resource* motion,
                 ID3D12Resource* glassMotion,
                 ID3D12Resource* rtShadow,
                 ID3D12Resource* rtAo,
                 ID3D12Resource* rtAoPrev,
                 ID3D12Resource* rtLights,
                 const float* sceneViewProj,
                 UINT width, UINT height);

    // The sub-pixel offset for THIS frame, in NDC. Both the scene and the sky
    // must apply it - a sky that does not jitter with the geometry anti-aliases
    // the world against a static background and the horizon crawls.
    // With an upscaler its own sequence for the render-to-output ratio.
    void Jitter(UINT width, UINT height, float* outNdcX, float* outNdcY) const;

    void SetTaa(bool on)        { m_taa = on; }
    bool Taa() const            { return m_taa; }

    // How the history is fetched at its reprojected position (design 5.0h).
    // Catmull-Rom by default; bilinear is kept as the A/B reference, because
    // the softness it causes only exists under motion and is easy to doubt.
    void SetHistoryCubic(bool on) { m_historyCubic = on; }
    bool HistoryCubic() const     { return m_historyCubic; }

    // Post-tonemap sharpen, 0 = off. Applied only to a TAA-resolved image:
    // it exists to give back what accumulating jittered samples integrates
    // away, and an unresolved image has had nothing taken.
    void  SetSharpen(float k)   { m_sharpen = k < 0.0f ? 0.0f : k; }
    float Sharpen() const       { return m_sharpen; }
    void SetCurve(int c)        { m_curve = c % kCurveCount; }
    int  CurveIndex() const     { return m_curve; }
    // The exposure dial. With auto exposure OFF it is the exposure, exactly as
    // before. With it ON it is the exposure at REFERENCE brightness (see
    // kReferenceLog2 in post.cpp): a scene metered as bright as the reference
    // gets exactly this value, a darker one more, a brighter one less. So the
    // dial keeps one meaning across both modes, and switching auto on does not
    // change the look of the scene the look was tuned on.
    void SetExposure(float e)   { m_exposure = e; }

    // Auto exposure (design 9b item 17): metered from the image being
    // tonemapped, adapted over time, entirely on the GPU. On by default.
    void SetAutoExposure(bool on) { if (on != m_autoExposure) m_snapExposure = true; m_autoExposure = on; }
    bool AutoExposure() const     { return m_autoExposure; }
    // What the last frame actually used, read back one frame late for the HUD
    // and the log - never fed back into rendering, which reads it on the GPU.
    float LastExposure() const    { return m_lastExposure; }
    float LastMeteredEv() const   { return m_lastMeteredLog2; }   // log2 luminance, this frame
    float LastAdaptedEv() const   { return m_lastAdaptedLog2; }   // log2 luminance, adapted
    // History is dropped on the next frame: use after a teleport or a reload,
    // where the previous image describes a different world.
    void ResetHistory()         { m_historyValid = false; m_snapExposure = true; }

    // Diagnostics: the intermediates, so a frame dump can show what TAA
    // consumed and produced rather than only what reached the screen. All are
    // in PIXEL_SHADER_RESOURCE between frames.
    ID3D12Resource* HdrTexture() const      { return m_hdr.Get(); }
    ID3D12Resource* HistoryTexture(int i) const { return m_history[i & 1].Get(); }
    int  HistoryWritten() const             { return (int)((m_frame + 1) & 1); }   // written by the LAST Resolve

private:
    bool CreatePasses(ID3D12Device* device, DXGI_FORMAT outputFormat);
    bool CreateMetering(ID3D12Device* device);
    void ExposureStateTo(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES s);
    void Meter(ID3D12GraphicsCommandList* list, ID3D12Resource* image,
               D3D12_GPU_DESCRIPTOR_HANDLE imageTable, UINT width, UINT height);

    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoTaa;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoTonemap;

    Microsoft::WRL::ComPtr<ID3D12Resource>       m_hdr;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_history[2];
    // Beside each history, how long each of its pixels has stood still on the screen (PSTaa's `settled`; R8).
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_settled[2];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;   // hdr, history0, history1, the upscaler's two, settled0, 1
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;   // five groups of four
    UINT m_rtvStride = 0;
    UINT m_srvStride = 0;

    DXGI_FORMAT m_outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT  m_width = 0, m_height = 0;              // the render size
    UINT  m_outWidth = 0, m_outHeight = 0;        // the output size
    UINT  m_frame = 0;          // drives the jitter sequence and the ping-pong
    bool  m_historyValid = false;
    bool  m_taa = true;
    bool  m_historyCubic = true;
    float m_sharpen = 0.5f;
    int   m_curve = kCurveAces;
    float m_exposure = 1.0f;
    float m_prevViewProj[16] = {};
    bool  m_hdrIsTarget = false;

    // The upscaler, when there is one: its inputs (render size: the motion,
    // the reactive mask; the 1x1 exposure) and its output (output size), all
    // PIXEL|NON_PIXEL_SHADER_RESOURCE between frames.
    void Upscale(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* glassMotion,
                 const float* consts, D3D12_GPU_DESCRIPTOR_HANDLE taaGroup, bool autoExposure);
    Upscaler* m_upscaler = nullptr;
    bool  m_upReady = false;
    int   m_upPhases = 8;
    float m_near = 0.25f, m_far = 8000.0f, m_fovY = 1.0f;
    LARGE_INTEGER m_upTick = {};
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoUpIn;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_upMotion;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_upReactive;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_upExposure;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_upOutput;

    // Auto exposure. The histogram is cleared by the pass that consumes it, so
    // it is all zeros between frames; the state buffer holds adapted log2
    // luminance, metered log2 luminance, a valid flag, and the exposure.
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_meterRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoHistogram;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoAdapt;
    // BOTH ARE BUFFERS, and buffers decay to COMMON at the end of every
    // ExecuteCommandLists - so neither has a state "between frames". Each frame
    // starts them from COMMON, and m_exposureStateNow tracks the state buffer
    // within the frame, because three passes touch it in different states.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_histogram;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_exposureState;
    D3D12_RESOURCE_STATES                        m_exposureStateNow = D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_exposureReadback;
    bool   m_autoExposure = true;
    bool   m_snapExposure = true;    // jump straight to the metered value next frame
    bool   m_readbackPending = false;
    LARGE_INTEGER m_lastTick = {};
    float  m_lastExposure = 0.0f;
    float  m_lastMeteredLog2 = 0.0f;
    float  m_lastAdaptedLog2 = 0.0f;
};

} // namespace core
} // namespace ngg
