// ---------------------------------------------------------------------------
// Rt - the ray-tracing probe: an acceleration structure over the static world,
// and one inline ray per pixel.
//
// It exists to answer a cost question before a feature is built on top of it
// (design 1.8.3 makes RT the baseline and takes SSAO, CSM-as-primary and the
// probe grid off the critical path - so the first thing worth knowing is what
// RT actually costs on the floor hardware, not what it deletes).
//
// OUR CONTENT IS THE EASIEST RT CASE THIS PROJECT WILL EVER GET, and that is a
// property of the format rather than luck: the converter bakes every transform
// into world space (ngg_format.h), so the world is rigid. One BLAS, built once
// at load, never refitted, and a TLAS with a single identity instance. The day
// per-object transforms land (design 9b item 7) this stops being true and the
// TLAS has to be rebuilt per frame - cheap, but it has to exist as code.
//
// THE FIRST RAY IS A SHADOW RAY, not ambient occlusion, and deliberately: its
// answer can be checked against something we already have. If RT visibility and
// the cascaded shadow map disagree, one of them is wrong and the disagreement
// is visible. Ambient occlusion has nothing to be checked against, so it is the
// wrong thing to write first.
//
// Inline RayQuery in a compute shader, not a full DXR pipeline: no shader
// tables, no ray-gen/miss/hit shaders, one PSO. It needs tier 1.1 and SM 6.5,
// which is why this is the only thing here compiled by DXC (see dxc.h).
// ---------------------------------------------------------------------------
#pragma once

#include <map>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <stdint.h>

#include "device.h"
#include "dxc.h"
#include "lights.h"
#include "scene.h"

namespace ngg {
namespace core {

class Rt
{
public:
    // Checks the adapter, loads DXC, compiles the trace shader. False (with a
    // log line) on any machine that cannot do this; the caller carries on
    // without ray tracing rather than failing to start.
    bool Create(Device& dev);
    bool Ready() const { return m_pso != nullptr; }

    // The acceleration structures, per MESH (design 5.0o): one BLAS per key
    // in Scene::DescribeRt - the world, each moved object, each editor mesh -
    // cached, so only `newMeshes` (the keys HasMesh says are missing) are
    // built; BLASes whose keys are no longer live are dropped. Then the shared
    // any-hit buffers and the descriptor heap, then the TLAS. Blocking on
    // `dev`, which is why the Renderer hands it a secondary context.
    bool Rebuild(Device& dev, const Scene::RtShared& shared,
                 std::vector<Scene::RtMeshData>& newMeshes,
                 const std::vector<uint64_t>& liveKeys,
                 const std::vector<Scene::RtInstanceDesc>& instances);
    // When only instances changed (a drag, an added instance of a known mesh):
    // the TLAS and the instance table, nothing else.
    bool UpdateInstances(Device& dev, const std::vector<Scene::RtInstanceDesc>& instances);
    bool HasMesh(uint64_t key) const { return m_blas.count(key) != 0; }
    size_t MeshCount() const { return m_blas.size(); }
    uint32_t InstanceCount() const { return m_instanceCount; }
    bool HasScene() const { return m_tlas != nullptr; }

    // One shadow ray per pixel, from the surface at `depth` along `normal`
    // towards the sun. `viewProj` is the JITTERED matrix the prepass rendered
    // with (Scene::LastViewProjJittered) and is inverted here. Depth and
    // normal are transitioned to shader resources and back.
    // `sunAngle` is the sun's angular RADIUS in radians - 0.00465 for the real
    // one - and drives the penumbra: rays are scattered over that cone, one
    // per pixel per frame, and TAA accumulates them. 0 gives a point sun and
    // hard edges, which is what the probe traced.
    // `unjitteredViewProj` (Scene::LastViewProj) drives the denoiser's temporal
    // reprojection - the same rule as PostFx: accumulation into a fixed grid
    // must not see the jitter, while the ray origins must.
    // `ao` adds ambient occlusion on the same surfaces: `aoRays` cosine-
    // weighted hemisphere rays per pixel per frame, occluders counted out to
    // `aoRadius` metres. Both results are denoised before FilteredAo/Filtered.
    // `motion` (Scene::MotionBuffer, may be null) reprojects the pixels of
    // moved objects in that denoise; transitioned like the normal. `plant`
    // (Scene::PlantBuffer, may be null) is the prepass's plant per pixel.
    void Trace(Device& dev, ID3D12GraphicsCommandList* list, ID3D12Resource* depth,
               ID3D12Resource* normal, ID3D12Resource* motion, ID3D12Resource* plant,
               const float* viewProj, const float* unjitteredViewProj,
               const float* camPos, const float* sunTowards, float tMax, float sunAngle,
               bool ao, float aoRadius, uint32_t aoRays, UINT width, UINT height,
               const Lights* lights = nullptr);

    // Filled by ReadStats after the caller has waited for the GPU - the same
    // contract as Scene::LogProbe and ReadOverlayStats, and for the same reason.
    void ReadStats();
    double  LastMs() const     { return m_lastMs; }       // shadow + AO + local-light traces
    double  LightsMs() const   { return m_lightsMs; }     // the local lights' trace alone
    double  AoMs() const       { return m_aoMs; }         // the AO trace alone
    double  DenoiseMs() const  { return m_denoiseMs; }    // both denoises
    uint32_t ShadowedPixels() const { return m_shadowed; }
    uint32_t TracedPixels() const   { return m_traced; }
    // Any-hit candidates that passed / failed the alpha test. The ratio is the
    // health of the test: all misses means it is rejecting everything.
    uint32_t LeafHits() const   { return m_leafHits; }
    uint32_t LeafMisses() const { return m_leafMisses; }

    // The DENOISED visibility (RGBA16F: visibility in R), which is what the
    // lighting should read. Result() is the raw per-frame trace, for diagnostics.
    ID3D12Resource* Filtered() const { return m_filtered; }
    ID3D12Resource* FilteredAo() const { return m_filteredAo; }    // null unless traced this frame
    // The local lights' denoised shadowed fraction (lights.h; RGBA16F, R): null unless traced this frame.
    ID3D12Resource* FilteredLights() const { return m_filteredLights; }
    // This frame's AO denoise before the wide pass, the RGBA16F layout Filtered
    // has (z: the moved-object distrust); null unless traced this frame.
    ID3D12Resource* AoDenoised() const { return m_filteredAo ? m_aoHist[(m_frame - 1) & 1].Get() : nullptr; }
    // And last frame's, the same layout - what AoDenoised was a frame ago.
    ID3D12Resource* AoPrevDenoised() const { return m_filteredAo ? m_aoHist[m_frame & 1].Get() : nullptr; }
    bool AoAvailable() const { return m_psoAo != nullptr; }

    // The denoiser's constants; public only so a file-scope static_assert can
    // check its float4 alignment.
    struct DenoiseCB
    {
        float    reproj[16];
        uint32_t size[2];
        float    blend;
        float    valid;
        float    motionOn;     // the motion SRV holds this frame's prepass
        float    blendMoved;   // history weight where a ray hit a moved object
        float    period4;      // 1 = the AO's window, one 4-pixel period each way
        float    blendStill;   // history weight where the pixel stays put on the screen
    };
    ID3D12Resource* Result() const { return m_result.Get(); }

    // The result is written as a UAV after the scene pass and read by the NEXT
    // frame's scene pass as a texture - one frame of lag, which is nothing at a
    // static camera and is what makes an on-screen comparison possible without
    // reordering the frame. These move it between the two states; Trace calls
    // the second itself.
    void PrepareForRead(ID3D12GraphicsCommandList* list);
    bool ReadableResult() const { return m_result && m_resultReadable; }

private:
    bool CreateTargets(ID3D12Device* device, UINT width, UINT height);

    Dxc m_dxc;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pso;

    struct BlasEntry
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> as;
        std::vector<uint32_t> alpha, alphaMat, alphaPlant;   // kept for the shared any-hit buffers
        uint32_t vbSelect = 0, vertexBase = 0, triBase = 0;
    };
    std::map<uint64_t, BlasEntry>                m_blas;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_tlas, m_scratch, m_instances, m_instInfo;
    ID3D12Resource*                              m_staticVb = nullptr;
    ID3D12Resource*                              m_dynVb = nullptr;
    uint32_t                                     m_instanceCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_result;     // R16_FLOAT raw visibility (see EncodeRaw)
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoAo;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_aoResult, m_aoCounters;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_aoHist[2];
    D3D12_RESOURCE_STATES m_aoHistState[2] = {};
    ID3D12Resource*       m_filteredAo = nullptr;
    // The AO's wide pass (kAoWideSrc in rt.cpp) and its output.
    void Widen(ID3D12GraphicsCommandList* list, const DenoiseCB& dc,
               Microsoft::WRL::ComPtr<ID3D12Resource>* hists, D3D12_RESOURCE_STATES* histState,
               UINT pair0, UINT pair1, ID3D12Resource* wide, D3D12_RESOURCE_STATES& wideState, UINT uav,
               ID3D12Resource*& filtered);
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoAoWide;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_aoWide;
    D3D12_RESOURCE_STATES m_aoWideState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_lightWide;   // the local lights' wide pass output
    D3D12_RESOURCE_STATES m_lightWideState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool                  m_aoHistValid = false;
    // The local lights' (lights.h): its trace, output, counters, histories.
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoLights;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_lightResult, m_lightCounters;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_lightHist[2];
    D3D12_RESOURCE_STATES m_lightHistState[2] = {};
    ID3D12Resource*       m_filteredLights = nullptr;
    bool                  m_lightHistValid = false;
    void Denoise(ID3D12GraphicsCommandList* list, const DenoiseCB& base, ID3D12Resource* raw,
                 UINT pair0, UINT pair1, UINT uav0, UINT uav1,
                 Microsoft::WRL::ComPtr<ID3D12Resource>* hist, D3D12_RESOURCE_STATES* histState,
                 bool& histValid, ID3D12Resource*& filtered);
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_denoiseRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoDenoise;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_hist[2];    // RGBA16F filtered vis, depth, two flags
    D3D12_RESOURCE_STATES m_histState[2] = {};
    ID3D12Resource*       m_filtered = nullptr;
    bool                  m_histValid = false;
    float                 m_prevViewProj[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_counters;   // 2 uints, UAV
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_countersReadback;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_countersZero;
    // Kept alive with the BLAS: the acceleration structure references the
    // index buffers by address, and the shader reads the rest per hit.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_ibAlpha, m_triMaterial, m_materials;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_triPlant;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;       // see kSlot* in rt.cpp
    UINT   m_vertexStride = 0;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap>      m_timestamps;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_timestampReadback;
    UINT   m_stride = 0;
    UINT   m_width = 0, m_height = 0;
    UINT64 m_timestampHz = 0;
    double m_lastMs = 0.0, m_aoMs = 0.0, m_lightsMs = 0.0, m_denoiseMs = 0.0;
    uint32_t m_shadowed = 0, m_traced = 0, m_leafHits = 0, m_leafMisses = 0;
    bool   m_resultReadable = false;
    uint32_t m_frame = 0;        // the cone sample's decorrelation counter
};

} // namespace core
} // namespace ngg
