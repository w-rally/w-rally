// ---------------------------------------------------------------------------
// Rt - the ray tracer: acceleration structures over the scene, and the inline
// rays that give the sun's shadow, AO and the local lights' shadows, with their
// denoisers (design §4).
//
// ONE BLAS PER MESH, A TLAS OF INSTANCES (design §4.2). Moving an object or a
// placement changes only its instance; a new or dropped mesh builds or drops
// its BLAS (Renderer::KeepRtCurrent). Opaque and alpha-tested triangles are two
// geometries, so foliage is cut out by an any-hit test with the rasteriser's
// own alpha rule.
//
// The trace runs after the prepass and before the main pass of the SAME frame:
// the main pass reads this frame's visibility, not last frame's.
//
// Inline RayQuery in a compute shader, not a full DXR pipeline: no shader
// tables, no ray-gen/miss/hit shaders, one PSO. It needs tier 1.1 and SM 6.5,
// which is why it is compiled by DXC (shader_compiler.h).
// ---------------------------------------------------------------------------
#pragma once

#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <stdint.h>

#include "device.h"
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

    // The acceleration structures, per MESH (design §4.2): one BLAS per key
    // in Scene::DescribeRt - the world, each moved object, each car part or
    // model submesh - cached by key. A new mesh never stalls a frame:
    //   - SubmitBlases queues the missing ones (the frame copies their indices
    //     out of the scene, ~1-5 ms); StartBatches, once the frame is recorded,
    //     gives the batch a thread that allocates from pooled memory (one index
    //     upload, one 8 MB scratch, 4 MB result arenas - a committed buffer per
    //     BLAS cost ~1.2 ms each to create and as much to free: 1.6 s for a
    //     stage's 922), records the builds and submits them on the ray tracer's
    //     OWN compute queue with a fence. Nothing waits for it;
    //   - Land, at the start of a later frame, takes in the batches the GPU
    //     has finished. Until then their meshes are simply not in the TLAS:
    //     no RT shadow, AO or reflection rays from them.
    // `Building` is true while a batch is out.
    bool SubmitBlases(Device& dev, uint32_t stride, std::vector<Scene::RtMeshData>& newMeshes);
    bool Land();
    bool Building() const { return !m_batches.empty(); }
    bool IsBuilding(uint64_t key) const;     // (or failed to: not queued again until the next scene)
    // The BLASes whose keys are no longer live dropped (a superseded world
    // kept while its successor builds: see rt.cpp), then the shared any-hit
    // buffers and the descriptor heap, then the TLAS over the built meshes.
    // Blocking on `dev` - the TLAS, which is why the Renderer hands it a
    // secondary context.
    bool Rebuild(Device& dev, const Scene::RtShared& shared,
                 const std::vector<uint64_t>& liveKeys,
                 const std::vector<Scene::RtInstanceDesc>& instances);
    // When only instances changed (a drag, an added instance of a known mesh):
    // the TLAS and the instance table, nothing else.
    bool UpdateInstances(Device& dev, const std::vector<Scene::RtInstanceDesc>& instances);
    // The dynamic pool was uploaded again (a new buffer, the old one freed) and nothing else changed for the TLAS:
    // the any-hit test's view of it follows, without a Rebuild.
    void FollowDynamicPool(ID3D12Device* device, ID3D12Resource* dynVb, uint32_t dynVertexCount);
    // The instance table still flags meshes that landed last frame as new
    // (UpdateInstances): the next frame must rewrite it even if nothing moved.
    bool InstancesStale() const { return m_appearedFlagged; }
    // The live keys the last Rebuild published (sorted): what the TLAS can
    // hold without another Rebuild.
    const std::vector<uint64_t>& PublishedKeys() const { return m_publishedKeys; }
    // A new scene: every BLAS and the TLAS let go (the frames are synchronous,
    // so nothing on the GPU holds them). Batches still out keep their own
    // memory and are dropped as dead when they land.
    void ForgetScene();
    // The batches SubmitBlases queued get their threads. The renderer calls it once the frame is recorded, so their
    // allocations overlap the frame's wait for the GPU rather than its recording (see SubmitBlases).
    void StartBatches();
    // The outputs and histories made at this size now, not in the first frame that traces: they are 11 textures at
    // the render size, 95 ms to create at 1080p - most of the frame the first BVH of a stage landed in. They survive
    // scene loads (ForgetScene does not touch them).
    bool PrepareTargets(ID3D12Device* device, UINT width, UINT height);
    ~Rt();
    bool HasMesh(uint64_t key) const { return m_blas.count(key) != 0; }
    size_t MeshCount() const { return m_blas.size(); }
    uint32_t InstanceCount() const { return m_instanceCount; }
    bool HasScene() const { return m_tlas != nullptr; }
    D3D12_GPU_VIRTUAL_ADDRESS TlasVa() const { return m_tlas ? m_tlas->GetGPUVirtualAddress() : 0; }

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

    // The denoisers' histories dropped on the next Trace - a camera's cut, as
    // PostFx::ResetHistory: reprojected, the old view lands on the new one's
    // surfaces wherever the depths happen to agree, and a still camera keeps
    // that for its long still history (rt_denoise.hlsl's STILL).
    void ResetHistory() { m_histValid = m_aoHistValid = m_lightHistValid = false; }

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
    // The local lights' denoise before the wide pass, the layout AoDenoised has (z: the distrust); null unless traced.
    ID3D12Resource* LightsDenoised() const { return m_filteredLights ? m_lightHist[(m_frame - 1) & 1].Get() : nullptr; }
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

    // THE NRD PATH (nrd_visibility.h): the traces undenoised, for NRD to denoise. EnableNrdInputs makes the trace variants that
    // also write NRD's inputs (rt_common.hlsli NRD_INPUTS) and their targets - once, the first time NRD is chosen;
    // false (logged) if they cannot be made. With SetNrdInputs(true) Trace runs those variants and none of our
    // denoisers but - with `ao` false - the AO's (NRD then takes the shadows only, and the AO is FilteredAo as on our
    // path): Filtered/FilteredLights (and FilteredAo with `ao`) are null, and the outputs below are left as
    // UNORDERED_ACCESS:
    //   - the raw 0/1 per pixel (Result, RawAo, RawLights; R16F, EncodeRaw: negative where a ray hit a moved object,
    //     carrying the history weight it allows);
    //   - NrdIn: SIGMA's penumbra for the sun and the lamps (metres; 65504 unoccluded), REBLUR's normalized hit
    //     distance for the AO (R16F).
    // AoTraced/LightsTraced: whether this frame traced them.
    bool EnableNrdInputs(Device& dev);
    void SetNrdInputs(bool on, bool ao = true) { m_nrdMode = on; m_nrdAo = ao; }
    enum NrdInput { kNrdSunPenumbra = 0, kNrdAoHitDist = 1, kNrdLightsPenumbra = 2 };
    ID3D12Resource* NrdIn(NrdInput i) const { return m_nrdIn[i].Get(); }
    ID3D12Resource* RawAo() const     { return m_aoResult.Get(); }
    ID3D12Resource* RawLights() const { return m_lightResult.Get(); }
    bool AoTraced() const     { return m_tracedAo; }
    bool LightsTraced() const { return m_tracedLights; }

    // The result is written as a UAV after the scene pass and read by the NEXT
    // frame's scene pass as a texture - one frame of lag, which is nothing at a
    // static camera and is what makes an on-screen comparison possible without
    // reordering the frame. These move it between the two states; Trace calls
    // the second itself.
    void PrepareForRead(ID3D12GraphicsCommandList* list);
    bool ReadableResult() const { return m_result && m_resultReadable; }

private:
    // The outputs and the denoisers' histories at this size, and their views in the heap. A new heap (Rebuild) only
    // rewrites the views (WriteTargetViews): recreating the targets dropped every history whenever a mesh appeared.
    bool CreateTargets(ID3D12Device* device, UINT width, UINT height);
    bool CreateNrdTargets(ID3D12Device* device);     // NRD's inputs at m_width x m_height (EnableNrdInputs)
    void DenoiseAll(ID3D12GraphicsCommandList* list, const DenoiseCB& dc, bool doSun, bool doAo, bool doLights);
    // The NRD path (EnableNrdInputs, SetNrdInputs).
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoNrd, m_psoAoNrd, m_psoLightsNrd;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_nrdIn[3];
    bool m_nrdMode = false, m_nrdAo = true;
    bool m_tracedAo = false, m_tracedLights = false;

    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pso;

    struct BlasEntry
    {
        // Its memory: a buffer of its own (a big BLAS - the world) or a share of a batch's arena, which lives as long
        // as any BLAS in it.
        Microsoft::WRL::ComPtr<ID3D12Resource> as;
        UINT64   offset = 0;
        D3D12_GPU_VIRTUAL_ADDRESS Va() const { return as ? as->GetGPUVirtualAddress() + offset : 0; }
        std::vector<uint32_t> alpha, alphaMat, alphaPlant;   // kept for the shared any-hit buffers
        uint32_t vbSelect = 0, vertexBase = 0, triBase = 0;
    };
    std::map<uint64_t, BlasEntry>                m_blas;
    // A batch of BLAS builds (SubmitBlases -> Land): recorded and submitted on a thread of its own, built on the
    // build queue, taken in by the first Land after both.
    struct BlasBatch
    {
        std::vector<uint64_t>             keys;      // its meshes' (IsBuilding), set before the thread starts
        std::vector<Scene::RtMeshData>    meshes;    // the thread's input
        std::future<bool>                 work;
        bool          started = false;     // its thread (StartBatches, at the end of the frame that queued it)
        ID3D12Device* device = nullptr;
        uint32_t      stride = 0;
        bool        ok = false;
        const char* error = nullptr;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>     alloc;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> list;
        UINT64 fence = 0;
        std::vector<std::pair<uint64_t, BlasEntry>> entries;
        // The index upload, the scratch and the source vertex buffers: the GPU reads them until the fence.
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> keep;
        uint32_t triangles = 0;
        UINT64   bytes = 0, scratchBytes = 0;
        double   recordMs = 0.0;
        LARGE_INTEGER submitted = {};
        uint32_t frames = 0;               // Land calls it has been out for
    };
    bool RecordBatch(BlasBatch& b, ID3D12Device* device, uint32_t stride);
    std::vector<std::unique_ptr<BlasBatch>>      m_batches;
    std::mutex                                   m_submitLock;   // the batches' threads submit one at a time
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>   m_buildQueue;   // COMPUTE: overlaps the frames
    Microsoft::WRL::ComPtr<ID3D12Fence>          m_buildFence;
    UINT64                                       m_buildFenceValue = 0;
    std::vector<uint64_t>                        m_appeared;     // landed, not yet flagged in an instance table
    bool                                         m_appearedFlagged = false;
    std::vector<uint64_t>                        m_publishedKeys;
    std::vector<uint64_t>                        m_failedKeys;   // a batch that failed: counted as building (IsBuilding)
    bool WriteTargetViews(ID3D12Device* device);
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
    // The AO's wide pass (shaders/rt_ao_wide.hlsl) and its output.
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
