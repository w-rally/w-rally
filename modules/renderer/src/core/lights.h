#pragma once
// ---------------------------------------------------------------------------
// Local lights (NGG design 5.4, phase N2): point and spot lights, many of them, every one of them shadowed.
//
// HOW THEY ARE SHADED - the ratio estimator (Heitz, Hill and McGuire 2018, "Combining Analytic Direct Illumination
// and Stochastic Shadows"). The scene pass sums the lights' UNSHADOWED light exactly, every light that reaches the
// pixel; the ray tracer picks ONE of them per pixel, with a probability in proportion to what it gives there, and
// traces one shadow ray to a random point on it (a soft shadow). With that choice, the ray's 0/1 is an unbiased
// estimate of the SHADOWED FRACTION of the sum - a smooth scalar, as the sun's visibility is, so it goes through the
// same denoiser - and the light is the exact sum times it. Every light is ray traced (the user's decision, 5.4), at
// one ray per pixel however many lights there are.
//
// WHICH LIGHTS A PIXEL SEES - clustered: the view frustum cut into 16 x 9 screen tiles and 24 depth slices
// (exponential in view depth), each with the list of lights whose range touches it, built on the CPU each frame
// (Build) and read by the scene pass and the ray tracer alike - so both see exactly the same lights.
//
// UNITS. A light's intensity is in CANDELA, its colour a linear tint; the renderer's irradiance unit is the sun's
// (22 for a clear day's ~110,000 lux), so a candela at a metre - a lux - is 2e-4 of it.
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstdint>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

namespace ngg {
namespace core {

// A BEAM's lobe (design 5.4 N3a): an intensity peak in the lamp's own frame - its axis, its up - that falls to half at
// the half-width across and the half-height up and down, centred `yawDeg` right and `pitchDeg` up of the axis.
// `sharpness` 2 is a gaussian; higher, a flatter top with a harder edge (a headlamp's cut-off). A lamp's beam is the
// sum of a few: a pod's pencil plus its driving spread, an LED bar's one wide flood.
struct LightLobe
{
    float candela = 0.0f;
    float halfWidthDeg = 10.0f, halfHeightDeg = 10.0f;
    float yawDeg = 0.0f, pitchDeg = 0.0f;
    float sharpness = 2.0f;
};

struct LightDesc
{
    static constexpr uint32_t kMaxLobes = 4;
    enum Type : uint32_t { kPoint = 0, kSpot = 1, kBeam = 2 };
    uint32_t type = kPoint;
    float    position[3] = {};     // world (render axes: y up)
    float    direction[3] = { 0.0f, 0.0f, 1.0f };   // a spot's or a beam's axis, unit
    float    up[3] = { 0.0f, 1.0f, 0.0f };          // a beam's up (made square to the axis)
    float    colour[3] = { 1.0f, 1.0f, 1.0f };      // linear tint
    float    intensity = 0.0f;     // candela on the axis; a beam: its lobes' scale (1: as given)
    float    range = 0.0f;         // metres; 0: from the intensity
    float    radius = 0.05f;       // the emitter's size, metres: how soft its shadows are
    float    innerDeg = 0.0f, outerDeg = 45.0f;     // a spot's full-intensity and cut-off half-angles
    bool     shadows = true;       // false: never occluded (a dashboard lamp)
    uint32_t lobeCount = 0;        // a beam's
    LightLobe lobes[kMaxLobes];
};

// One light as the GPU reads it (kLightsHlsl's Light), 208 bytes.
struct GpuLight
{
    float    pos[3];    float invRange2;   // 1 / range^2
    float    colour[3]; float radius;      // colour: tint x intensity, in irradiance units at 1 m
    float    dir[3];    float cosOuter;    // a spot's cut-off; a beam's extent (the clusters' culling only)
    float    up[3];     float spotScale;   // 1 / (cos inner - cos outer)
    uint32_t type; uint32_t flags; uint32_t lobeCount; float pad;
    float    lobeA[LightDesc::kMaxLobes][4];   // candela, 1/tan(half-width), 1/tan(half-height), sharpness / 2
    float    lobeB[LightDesc::kMaxLobes][4];   // tan(yaw), tan(pitch)
};
static_assert(sizeof(GpuLight) == 208, "GpuLight - keep kLightsHlsl in step");

class Lights
{
public:
    static constexpr uint32_t kClustersX = 16, kClustersY = 9, kClustersZ = 24;
    static constexpr uint32_t kClusters = kClustersX * kClustersY * kClustersZ;
    static constexpr uint32_t kMaxLights = 512;
    static constexpr uint32_t kMaxIndices = 65536;
    static constexpr float    kNear = 0.25f, kFar = 1000.0f;   // the slices' depth range, metres
    static constexpr float    kMaxRange = 1000.0f;              // a light's reach at most: a pencil beam's
    static constexpr float    kCandelaToUnits = 2e-4f;          // lux -> the sun's irradiance unit

    bool Create(ID3D12Device* dev);

    // This frame's lights (replacing the last frame's).
    void Set(const std::vector<LightDesc>& lights) { m_lights = lights; }
    size_t Count() const { return m_lights.size(); }

    // The clusters for this view (row-major, p' = p * M), uploaded. Before the ray tracer and the scene pass read
    // them; false (and nothing to read) without lights.
    // Recorded on `list`: the copies to the GPU-local buffers the shaders read.
    bool Build(ID3D12GraphicsCommandList* list, const float* view, const float* proj, uint32_t width, uint32_t height);
    bool Active() const { return m_active; }

    // What the shaders need besides the buffers: the cluster tile in pixels, the slices' near plane and log scale,
    // the light count; and the camera the depth slices are measured along.
    float TileW() const { return m_tileW; }
    float TileH() const { return m_tileH; }
    float SliceScale() const { return (float)kClustersZ / logf(kFar / kNear); }
    const float* CamPos() const { return m_camPos; }
    const float* CamFwd() const { return m_camFwd; }

    D3D12_GPU_VIRTUAL_ADDRESS LightsVa() const   { return m_lightGpu ? m_lightGpu->GetGPUVirtualAddress() : 0; }
    D3D12_GPU_VIRTUAL_ADDRESS ClustersVa() const { return m_clusterGpu ? m_clusterGpu->GetGPUVirtualAddress() : 0; }
    D3D12_GPU_VIRTUAL_ADDRESS IndicesVa() const  { return m_indexGpu ? m_indexGpu->GetGPUVirtualAddress() : 0; }
    // Three SRVs (lights, clusters, indices) at `at`, `at + stride`, `at + 2 stride`.
    void WriteSrvs(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE at, UINT stride) const;

private:
    std::vector<LightDesc> m_lights;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightBuf, m_clusterBuf, m_indexBuf;   // upload heap, persistently mapped
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightGpu, m_clusterGpu, m_indexGpu;   // GPU-local: what is read
    GpuLight* m_lightMap = nullptr;
    uint32_t* m_clusterMap = nullptr;
    uint32_t* m_indexMap = nullptr;
    float     m_tileW = 1.0f, m_tileH = 1.0f;
    float     m_camPos[3] = {}, m_camFwd[3] = { 0.0f, 0.0f, 1.0f };
    struct ClusterSphere { float c[3], r; };          // view space
    std::vector<ClusterSphere> m_clusterSpheres;
    bool      m_active = false;
};

// HLSL shared by the scene pass and the ray tracer: the Light struct, the three buffers (gLights, gClusters,
// gLightIndex) at the registers the includer #defines as NGG_LIGHTS_T0/T1/T2, the cluster lookup and a light's
// irradiance, read by index.
extern const char* kLightsHlsl;

} // namespace core
} // namespace ngg
