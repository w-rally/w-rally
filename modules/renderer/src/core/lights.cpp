#include "lights.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

const char* kLightsHlsl = R"(
// ---- local lights (lights.h) ------------------------------------------------
// The includer defines NGG_LIGHTS_T0/T1/T2, the registers of the three buffers (the scene and the ray tracer bind
// them in different places).
struct Light
{
    float3 pos;    float invRange2;  // 1 / range^2
    float3 colour; float radius;     // tint x intensity, irradiance units at 1 m
    float3 dir;    float cosOuter;
    float  spotScale; float invOuterRad; uint type; uint flags;   // 1 / (cos inner - cos outer), 1 / outer angle
    float  profile[16];
};
StructuredBuffer<Light> gLights     : register(NGG_LIGHTS_T0);
StructuredBuffer<uint2> gClusters   : register(NGG_LIGHTS_T1);   // per cluster: first index, count
StructuredBuffer<uint>  gLightIndex : register(NGG_LIGHTS_T2);
static const uint kLightShadows = 1u, kLightProfile = 2u;
static const uint kClustersX = 16u, kClustersY = 9u, kClustersZ = 24u;

// The cluster of a pixel at a view depth. params: tile width and height in pixels, the slices' near plane, their
// log scale (slices per natural-log unit of depth).
uint LightCluster(float2 px, float viewZ, float4 params)
{
    const uint cx = min((uint)(px.x / params.x), kClustersX - 1u);
    const uint cy = min((uint)(px.y / params.y), kClustersY - 1u);
    const float s = log(max(viewZ, params.z) / params.z) * params.w;
    const uint cz = min((uint)max(s, 0.0), kClustersZ - 1u);
    return (cz * kClustersY + cy) * kClustersX + cx;
}

// A light's record but its profile, READ AT ONCE: every field from the buffer by index - never through a copy of the
// struct, which indexed by a variable (its profile) is an indexable temporary in scratch memory, ~1.5 ms per light at
// 1080p on the development GPU - and every one of them used whatever the light's type (LightIrradiance selects, it
// does not branch), so the compiler issues them as one clause (one round trip, not four), and with a wave-uniform
// index as scalar loads (the scene pass's light loop, design 5.4 N2).
struct LightRec
{
    float3 pos;    float invRange2;
    float3 colour; float radius;
    float3 dir;    float cosOuter;
    float  spotScale; float invOuterRad; uint type; uint flags;
};
LightRec LightLoad(uint li)
{
    LightRec L;
    L.pos = gLights[li].pos;           L.invRange2 = gLights[li].invRange2;
    L.colour = gLights[li].colour;     L.radius = gLights[li].radius;
    L.dir = gLights[li].dir;           L.cosOuter = gLights[li].cosOuter;
    L.spotScale = gLights[li].spotScale; L.invOuterRad = gLights[li].invOuterRad;
    L.type = gLights[li].type;         L.flags = gLights[li].flags;
    return L;
}

// What light `li` (its record L) gives at p, as an irradiance on a surface facing it (rgb, before visibility and the
// cosine), and the direction towards it. Inverse square, softened inside the emitter's radius and faded to nothing at
// its range (a window, so the clusters' cut-off never shows); a spot's cone, and its beam profile over the cone.
float3 LightIrradiance(uint li, LightRec L, float3 p, out float3 l, out float dist)
{
    // Per light per pixel, so the divisions the light alone decides are made on the CPU (invRange2, spotScale,
    // invOuterRad) and the distance and direction come from one rsqrt.
    const float3 d = L.pos - p;
    const float d2 = dot(d, d);
    const float inv = rsqrt(max(d2, 1e-8));
    dist = d2 * inv;
    l = d * inv;
    const float x = d2 * L.invRange2;
    const float w = saturate(1.0 - x * x);
    float atten = w * w / max(d2, L.radius * L.radius);
    const float cd = dot(-l, L.dir);
    const float s = saturate((cd - L.cosOuter) * L.spotScale);   // smoothstep(cosOuter, cosInner, cd)
    atten *= L.type == 1u ? s * s * (3.0 - 2.0 * s) : 1.0;
    if (L.type == 1u && (L.flags & kLightProfile))   // the profile alone is read only where it is used
    {
        const float t = saturate(acos(clamp(cd, -1.0, 1.0)) * L.invOuterRad) * 15.0;
        const uint i = min((uint)t, 14u);
        atten *= lerp(gLights[li].profile[i], gLights[li].profile[i + 1u], t - (float)i);
    }
    return L.colour * atten;
}
float3 LightIrradiance(uint li, float3 p, out float3 l, out float dist)
{
    return LightIrradiance(li, LightLoad(li), p, l, dist);
}
)";

namespace {

ComPtr<ID3D12Resource> MakeUpload(ID3D12Device* dev, UINT64 bytes)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

// GPU-local (default heap): what the shaders read. Created in COMMON - a buffer is promoted from it implicitly by its
// first use in a list, and decays back to it at the list's end.
ComPtr<ID3D12Resource> MakeLocal(ID3D12Device* dev, UINT64 bytes)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

} // namespace

bool Lights::Create(ID3D12Device* dev)
{
    if (!dev) return false;
    // Persistently mapped upload buffers, rewritten each frame: the frame loop waits for the GPU at its end (as the
    // scene's own per-frame constants rely on), so last frame's contents are read before this frame's are written.
    m_lightBuf = MakeUpload(dev, sizeof(GpuLight) * kMaxLights);
    m_clusterBuf = MakeUpload(dev, sizeof(uint32_t) * 2 * kClusters);
    m_indexBuf = MakeUpload(dev, sizeof(uint32_t) * kMaxIndices);
    // ... and their GPU-local copies, which is what the shaders read: the upload heap is CPU-visible memory the GPU
    // reads slowly and barely caches (on the development GPU, an APU, it measured the same - design 5.4 N2).
    m_lightGpu = MakeLocal(dev, sizeof(GpuLight) * kMaxLights);
    m_clusterGpu = MakeLocal(dev, sizeof(uint32_t) * 2 * kClusters);
    m_indexGpu = MakeLocal(dev, sizeof(uint32_t) * kMaxIndices);
    if (!m_lightBuf || !m_clusterBuf || !m_indexBuf || !m_lightGpu || !m_clusterGpu || !m_indexGpu) return false;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(m_lightBuf->Map(0, &none, reinterpret_cast<void**>(&m_lightMap))) ||
        FAILED(m_clusterBuf->Map(0, &none, reinterpret_cast<void**>(&m_clusterMap))) ||
        FAILED(m_indexBuf->Map(0, &none, reinterpret_cast<void**>(&m_indexMap))))
        return false;
    std::memset(m_clusterMap, 0, sizeof(uint32_t) * 2 * kClusters);
    return true;
}

bool Lights::Build(ID3D12GraphicsCommandList* list, const float* view, const float* proj, uint32_t width,
                   uint32_t height)
{
    m_active = false;
    if (!m_lightMap || !view || !proj || !width || !height) return false;

    // The camera, from the view matrix (row vectors: v = p R + t): its forward axis is R's third column, and its
    // position -t R^T.
    for (int j = 0; j < 3; ++j) {
        m_camFwd[j] = view[j * 4 + 2];
        m_camPos[j] = -(view[12] * view[j * 4 + 0] + view[13] * view[j * 4 + 1] + view[14] * view[j * 4 + 2]);
    }
    m_tileW = (float)width / kClustersX;
    m_tileH = (float)height / kClustersY;
    if (m_lights.empty()) return false;

    const float sliceScale = SliceScale();
    auto slice = [&](float z) {
        const float s = std::log((std::max)(z, kNear) / kNear) * sliceScale;
        return (uint32_t)(std::min)((std::max)(s, 0.0f), (float)kClustersZ - 1.0f);
    };

    // Each light to the GPU's form, the box of clusters its range touches, and its shape in view space for the test of
    // each cluster in that box (Touches).
    struct Box { uint32_t x0, x1, y0, y1, z0, z1; };
    struct Shape { float c[3], range, dir[3], cosOuter, sinOuter; bool spot; };
    std::vector<Box> boxes;
    std::vector<Shape> shapes;
    const uint32_t n = (uint32_t)(std::min<size_t>)(m_lights.size(), kMaxLights);
    uint32_t live = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const LightDesc& L = m_lights[i];
        if (L.intensity <= 0.0f) continue;
        GpuLight& g = m_lightMap[live];
        const float tintMax = (std::max)(L.colour[0], (std::max)(L.colour[1], L.colour[2]));
        // Its reach: where it falls under ~0.1 lux, capped at 300 m (a beam that far is in the haze anyway).
        float range = L.range;
        if (range <= 0.0f) range = (std::min)(std::sqrt(L.intensity * tintMax / 0.1f), 300.0f);
        float len = std::sqrt(L.direction[0] * L.direction[0] + L.direction[1] * L.direction[1] +
                              L.direction[2] * L.direction[2]);
        if (len < 1e-6f) len = 1.0f;
        for (int k = 0; k < 3; ++k) {
            g.pos[k] = L.position[k];
            g.colour[k] = L.colour[k] * L.intensity * kCandelaToUnits;
            g.dir[k] = L.direction[k] / len;
        }
        g.invRange2 = 1.0f / (range * range);
        g.radius = (std::max)(L.radius, 0.005f);
        const float outer = (std::min)((std::max)(L.outerDeg, 0.5f), 179.0f) * 3.14159265f / 180.0f;
        const float inner = (std::min)((std::max)(L.innerDeg, 0.0f) * 3.14159265f / 180.0f, outer * 0.999f);
        g.cosOuter = std::cos(outer);
        g.spotScale = 1.0f / (std::max)(std::cos(inner) - g.cosOuter, 1e-4f);
        g.invOuterRad = 1.0f / outer;
        g.type = L.type;
        g.flags = (L.shadows ? 1u : 0u) | (L.hasProfile ? 2u : 0u);
        for (int k = 0; k < 16; ++k) g.profile[k] = L.hasProfile ? L.profile[k] : 1.0f;

        // In view space: its sphere of reach, and the slices and tiles that sphere covers (conservative).
        float c[3];
        for (int k = 0; k < 3; ++k)
            c[k] = L.position[0] * view[0 * 4 + k] + L.position[1] * view[1 * 4 + k] + L.position[2] * view[2 * 4 + k] + view[12 + k];
        const float zmin = c[2] - range, zmax = c[2] + range;
        if (zmax < kNear) continue;                       // wholly behind the camera
        Box b{ 0, kClustersX - 1, 0, kClustersY - 1, slice(zmin), slice(zmax) };
        if (zmin > kNear) {
            // The x/z of its extreme points, over the depths it spans: a conservative screen box.
            const float x0 = (std::min)((c[0] - range) / zmin, (c[0] - range) / zmax) * proj[0] + proj[8];
            const float x1 = (std::max)((c[0] + range) / zmin, (c[0] + range) / zmax) * proj[0] + proj[8];
            const float y0 = (std::min)((c[1] - range) / zmin, (c[1] - range) / zmax) * proj[5] + proj[9];
            const float y1 = (std::max)((c[1] + range) / zmin, (c[1] + range) / zmax) * proj[5] + proj[9];
            if (x1 < -1.0f || x0 > 1.0f || y1 < -1.0f || y0 > 1.0f) continue;   // off screen
            auto tile = [](float ndc, float tiles) {
                return (uint32_t)(std::min)((std::max)((ndc * 0.5f + 0.5f) * tiles, 0.0f), tiles - 1.0f);
            };
            b.x0 = tile(x0, kClustersX);
            b.x1 = tile(x1, kClustersX);
            // NDC y is up, pixel rows down.
            b.y0 = tile(-y1, kClustersY);
            b.y1 = tile(-y0, kClustersY);
        }
        boxes.push_back(b);
        Shape sh{};
        for (int k = 0; k < 3; ++k) {
            sh.c[k] = c[k];
            sh.dir[k] = g.dir[0] * view[0 * 4 + k] + g.dir[1] * view[1 * 4 + k] + g.dir[2] * view[2 * 4 + k];
        }
        sh.range = range;
        sh.spot = L.type == LightDesc::kSpot && outer < 3.14159265f * 0.5f;   // wider: a hemisphere, the sphere will do
        sh.cosOuter = g.cosOuter;
        sh.sinOuter = std::sin(outer);
        shapes.push_back(sh);
        ++live;
    }
    if (!live) return false;

    // Each cluster's bounding sphere in view space - its eight corners, from the tile's NDC rectangle at the slice's
    // near and far depth - and whether a light's sphere of reach, or a spot's CONE, touches it. The box above is the
    // sphere's; a spot's cone is much less (a 30-degree headlight beam reaches 300 m and every cluster ahead in its
    // box), and every light a cluster lists is shaded by every pixel in it (design 5.4 N2).
    m_clusterSpheres.resize(kClusters);
    for (uint32_t z = 0; z < kClustersZ; ++z) {
        const float zn = z == 0 ? 0.0f : kNear * std::exp((float)z / sliceScale);
        const float zf = kNear * std::exp((float)(z + 1) / sliceScale);
        for (uint32_t y = 0; y < kClustersY; ++y)
            for (uint32_t x = 0; x < kClustersX; ++x) {
                const float nx[2] = { (float)x / kClustersX * 2.0f - 1.0f, (float)(x + 1) / kClustersX * 2.0f - 1.0f };
                const float ny[2] = { 1.0f - (float)(y + 1) / kClustersY * 2.0f, 1.0f - (float)y / kClustersY * 2.0f };
                float pts[8][3], mid[3] = { 0.0f, 0.0f, 0.0f };
                int k = 0;
                for (float d : { zn, zf })
                    for (float ndcx : nx)
                        for (float ndcy : ny) {
                            pts[k][0] = (ndcx - proj[8]) * d / proj[0];
                            pts[k][1] = (ndcy - proj[9]) * d / proj[5];
                            pts[k][2] = d;
                            for (int j = 0; j < 3; ++j) mid[j] += pts[k][j] * 0.125f;
                            ++k;
                        }
                float r2 = 0.0f;
                for (auto& p : pts) {
                    const float dx = p[0] - mid[0], dy = p[1] - mid[1], dz = p[2] - mid[2];
                    r2 = (std::max)(r2, dx * dx + dy * dy + dz * dz);
                }
                ClusterSphere& cs = m_clusterSpheres[(z * kClustersY + y) * kClustersX + x];
                cs = { { mid[0], mid[1], mid[2] }, std::sqrt(r2) };
            }
    }
    auto touches = [&](const Shape& sh, uint32_t ci) {
        const ClusterSphere& cs = m_clusterSpheres[ci];
        const float v[3] = { cs.c[0] - sh.c[0], cs.c[1] - sh.c[1], cs.c[2] - sh.c[2] };
        const float v2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        const float reach = sh.range + cs.r;
        if (v2 > reach * reach) return false;
        if (!sh.spot) return true;
        // Cone against sphere: the sphere's distance from the cone's surface, along the axis and across it.
        const float along = v[0] * sh.dir[0] + v[1] * sh.dir[1] + v[2] * sh.dir[2];
        const float across = std::sqrt((std::max)(v2 - along * along, 0.0f));
        if (sh.cosOuter * across - along * sh.sinOuter > cs.r) return false;   // outside the cone's side
        return along > -cs.r;                                                  // not behind its apex
    };

    // Two passes: count per cluster, then fill at prefix-summed offsets.
    std::vector<uint32_t> count(kClusters, 0);
    for (uint32_t li = 0; li < live; ++li) {
        const Box& b = boxes[li];
        for (uint32_t z = b.z0; z <= b.z1; ++z)
            for (uint32_t y = b.y0; y <= b.y1; ++y)
                for (uint32_t x = b.x0; x <= b.x1; ++x) {
                    const uint32_t c = (z * kClustersY + y) * kClustersX + x;
                    if (touches(shapes[li], c)) ++count[c];
                }
    }
    uint32_t total = 0;
    for (uint32_t i = 0; i < kClusters; ++i) {
        const uint32_t take = (std::min)(count[i], kMaxIndices - total);
        m_clusterMap[2 * i] = total;
        m_clusterMap[2 * i + 1] = take;
        total += take;
        count[i] = 0;
    }
    for (uint32_t li = 0; li < live; ++li) {
        const Box& b = boxes[li];
        for (uint32_t z = b.z0; z <= b.z1; ++z)
            for (uint32_t y = b.y0; y <= b.y1; ++y)
                for (uint32_t x = b.x0; x <= b.x1; ++x) {
                    const uint32_t c = (z * kClustersY + y) * kClustersX + x;
                    if (touches(shapes[li], c) && count[c] < m_clusterMap[2 * c + 1])
                        m_indexMap[m_clusterMap[2 * c] + count[c]++] = li;
                }
    }

    {   // Now and then, what the clusters hold: a list longer than the light count is a culling bug.
        static uint32_t builds = 0;
        if ((builds++ % 300) == 0) {
            uint32_t longest = 0, used = 0;
            for (uint32_t i = 0; i < kClusters; ++i) {
                longest = (std::max)(longest, m_clusterMap[2 * i + 1]);
                used += m_clusterMap[2 * i + 1] != 0;
            }
            Log("lights: %u live, %u list entries, %u of %u clusters lit, the longest list %u", live, total, used,
                kClusters, longest);
        }
    }

    // To the GPU-local copies, on the frame's list, ahead of the ray tracer and the scene pass that read them.
    if (!list) return false;
    list->CopyBufferRegion(m_lightGpu.Get(), 0, m_lightBuf.Get(), 0, sizeof(GpuLight) * live);
    list->CopyBufferRegion(m_clusterGpu.Get(), 0, m_clusterBuf.Get(), 0, sizeof(uint32_t) * 2 * kClusters);
    if (total) list->CopyBufferRegion(m_indexGpu.Get(), 0, m_indexBuf.Get(), 0, sizeof(uint32_t) * total);
    D3D12_RESOURCE_BARRIER b[3] = {};
    ID3D12Resource* bufs[3] = { m_lightGpu.Get(), m_clusterGpu.Get(), m_indexGpu.Get() };
    for (int k = 0; k < 3; ++k) {
        b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[k].Transition.pResource = bufs[k];
        b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[k].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;   // promoted from COMMON by the copy
        b[k].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    // An index buffer no copy touched this frame is still in COMMON - readable as it is (promotion), so not barriered.
    list->ResourceBarrier(total ? 3 : 2, b);
    m_active = true;
    return true;
}

void Lights::WriteSrvs(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE at, UINT stride) const
{
    auto structured = [&](ID3D12Resource* r, UINT count, UINT bytes) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = count;
        sd.Buffer.StructureByteStride = bytes;
        dev->CreateShaderResourceView(r, &sd, at);
        at.ptr += stride;
    };
    structured(m_lightGpu.Get(), kMaxLights, sizeof(GpuLight));
    structured(m_clusterGpu.Get(), kClusters, 2 * sizeof(uint32_t));
    structured(m_indexGpu.Get(), kMaxIndices, sizeof(uint32_t));
}

} // namespace core
} // namespace ngg
