// ---------------------------------------------------------------------------
// Gtao - screen-space ambient occlusion: Intel's XeGTAO (third_party/xegtao,
// MIT, unchanged), on the prepass's depth and normals.
//
// The AO the cascades path never had. The ray-traced AO needs a BVH and the
// adapter to trace it; this needs only the prepass, so it runs wherever the
// renderer does - and it is the cheap AO a ray-tracing frame can take instead
// (Renderer::SetAoSource). It occludes the AMBIENT terms only, in the same slot
// the RT AO fills (Scene::SetRtAo), so the main pass cannot tell them apart.
//
// The passes at the render size, as XeGTAO ships them: the depth's view-space
// MIP chain (5 levels, FP32 - a stage's depth runs to kilometres, where FP16
// has half a metre of precision), the horizon search, three edge-aware
// spatial denoise passes (SetDenoisePasses). NO temporal accumulation here: its noise follows
// the frame index (R2 over a Hilbert curve, 64 frames) and the TAA, FSR or
// DLSS that runs anyway integrates it - with TAA off the noise stands still.
//
// The normals are the depth's own by default (SetDepthNormals says why); the
// prepass's (world space, R8G8B8A8_SNORM) are turned into XeGTAO's view space
// (x right, y up, z forward) by the view's rotation. The depth is reverse-Z
// and jittered, and the view-space reconstruction takes both (Record).
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <stdint.h>
#include <string>

#include "dxc.h"

namespace ngg {
namespace core {

class Gtao
{
public:
    // Loads DXC and compiles the passes. False (logged) where that cannot happen; the frame then has no
    // screen-space AO. `queue`: the one the frames run on, for the timestamps' frequency.
    bool Create(ID3D12Device* dev, ID3D12CommandQueue* queue);
    bool Ready() const { return m_psoPrefilter && m_psoDenoise; }

    // XeGTAO's quality: 0 low (1 slice x 2 steps), 1 medium (2 x 2), 2 high (3 x 3), 3 ultra (9 x 3).
    // The tier sets it (Renderer::SetQuality); ForceQuality (A/B work, NGG_GTAO_QUALITY) holds one over every tier, -1
    // lets go.
    void SetQuality(int q)   { m_quality = q < 0 ? 0 : q > 3 ? 3 : q; }
    void ForceQuality(int q) { m_forced = q < 0 ? -1 : q > 3 ? 3 : q; }
    int  Quality() const     { return m_forced >= 0 ? m_forced : m_quality; }
    // The occluding sphere's radius in metres, before XeGTAO's own 1.457 multiplier.
    void  SetRadius(float metres) { if (metres > 0.0f) m_radius = metres; }
    float Radius() const          { return m_radius; }
    // The normals the horizons are measured from: the depth's own (XeGTAO's in-place reconstruction - the triangles
    // as drawn; the default) or the prepass's. The prepass's are the SHADING normals, smooth over a terrain whose
    // triangles are not: every crease between two of them then rises above the normal's horizon, and the slopes
    // came out in dark bands along the triangle edges that the ray-traced AO does not have.
    void SetDepthNormals(bool on) { m_depthNormals = on; }
    // XeGTAO's "thin occluder compensation" (0..0.7): samples far behind the horizon count less, as if what is in
    // front had a thickness - a fence bar before a car door is not a wall.
    void  SetThinOccluders(float k) { m_thin = k < 0.0f ? 0.0f : k > 0.7f ? 0.7f : k; }
    float ThinOccluders() const     { return m_thin; }
    // Edge-aware spatial denoise passes: 1 sharp (XeGTAO's choice under a temporal filter), 2 medium, 3 soft.
    void SetDenoisePasses(int n)  { m_denoisePasses = n < 1 ? 1 : n > 3 ? 3 : n; }
    int  DenoisePasses() const    { return m_denoisePasses; }
    bool DepthNormals() const     { return m_depthNormals; }

    // The AO of this frame's prepass. `depth` (DEPTH_WRITE, reverse-Z, R32 typeless) and `normal` (RENDER_TARGET)
    // are borrowed and given back in those states. `view` is the client's view, `proj` its CONVENTIONAL projection
    // (the reverse-Z Scene applies is applied here too) and `jitter` the NDC offset the prepass drew with. `moving`:
    // the noise walks its 64 frames (a temporal filter integrates it); otherwise it stands still. Output() is then
    // in PIXEL_SHADER_RESOURCE, read with OutputSrvFormat.
    void Record(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* normal,
                const float* view, const float* proj, const float jitter[2], bool moving, UINT width, UINT height);
    ID3D12Resource* Output() const { return m_out.Get(); }
    static DXGI_FORMAT OutputSrvFormat() { return DXGI_FORMAT_R8_UNORM; }

    // A camera's cut: the noise sequence starts again. (No history to drop - it has none.)
    void ResetHistory() { m_frame = 0; }

    // The three passes' GPU time, last frame's (the frame is synchronous, so it has resolved by the next Record).
    double LastMs() const { return m_ms; }

private:
    bool CreateTargets(ID3D12Device* dev, UINT width, UINT height);

    Dxc m_dxc;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoPrefilter, m_psoDenoiseMid, m_psoDenoise;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoMain[2][4];   // [depth normals][quality], compiled when first used
    bool   m_mainFailed[2][4] = {};
    std::string m_source;                                       // the passes' HLSL, for those compiles
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_depthMips;   // R32F view-space depth, 5 MIPs
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_aoTerm;      // R8_UINT the horizon search's term
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_aoTermB;     // and the denoise's other one
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_edges;       // R8_UNORM its depth edges, for the denoise
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_out;         // R8 typeless: UAV R8_UINT, SRV R8_UNORM
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;        // tables of 7: t0 t1 u0..u4, one per pass (gtao.cpp)
    UINT   m_stride = 0;
    UINT   m_width = 0, m_height = 0;
    int    m_quality = 1;
    int    m_forced = -1;
    float  m_radius = 1.5f;   // x1.457: ~2.2 m. 2.5 put dark blotches on near leaf cards; 1.0 hardly reached the car
    bool   m_depthNormals = true;
    float  m_thin = 0.5f;         // fence before a door: grain 5.22 -> 4.25 alone, 2.47 with 3 passes (RT 2.99)
    int    m_denoisePasses = 3;   // 1 left the speckle a moving car keeps too little TAA history to average
    uint32_t m_frame = 0;

    Microsoft::WRL::ComPtr<ID3D12QueryHeap>      m_timer;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_timerReadback;
    UINT64 m_timerHz = 0;
    bool   m_timerPending = false;
    double m_ms = 0.0;
};

} // namespace core
} // namespace ngg
