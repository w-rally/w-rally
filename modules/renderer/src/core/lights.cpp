#include "lights.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

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
    // The shadow maps' candidates (ShadowCount): shadowed spots and beams on screen, by what they give near the camera.
    struct ShadowCand { uint32_t live, flags; bool beam; float score, tanX, tanY, range; float pos[3], dir[3], up[3]; };
    std::vector<ShadowCand> cands;
    m_shadowCount = 0;
    const uint32_t n = (uint32_t)(std::min<size_t>)(m_lights.size(), kMaxLights);
    uint32_t live = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const LightDesc& L = m_lights[i];
        if (L.intensity <= 0.0f) continue;
        GpuLight& g = m_lightMap[live];
        const float tintMax = (std::max)(L.colour[0], (std::max)(L.colour[1], L.colour[2]));
        // A beam's lobes, and what they decide: its peak (the reach) and how far off its axis it reaches (the
        // clusters' cone) - a lobe out to where it falls to 1% of its peak, (log2 100)^(1/sharpness) half-widths.
        const bool beam = L.type == LightDesc::kBeam && L.lobeCount > 0;
        float peak = L.intensity, extent = 0.0f, extentX = 0.0f, extentY = 0.0f;
        if (beam) {
            peak = 0.0f;
            for (uint32_t k = 0; k < (std::min)(L.lobeCount, LightDesc::kMaxLobes); ++k) {
                const LightLobe& o = L.lobes[k];
                peak = (std::max)(peak, o.candela * L.intensity);
                const float f = std::pow(6.644f, 1.0f / (std::max)(o.sharpness, 0.5f));
                const float dr = 3.14159265f / 180.0f;
                extentX = (std::max)(extentX, std::fabs(std::tan(o.yawDeg * dr)) + std::tan(o.halfWidthDeg * dr) * f);
                extentY = (std::max)(extentY, std::fabs(std::tan(o.pitchDeg * dr)) + std::tan(o.halfHeightDeg * dr) * f);
            }
            extent = (std::max)(extentX, extentY);
        }
        // Its reach: where it falls under ~0.1 lux, at most kMaxRange (a pencil beam's).
        float range = L.range;
        if (range <= 0.0f) range = (std::min)(std::sqrt(peak * tintMax / 0.1f), kMaxRange);
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
        // A beam's "outer" angle is its extent: the clusters' cone, not its shading.
        const float outer = beam ? (std::min)(std::atan(extent), 3.1f)
                                 : (std::min)((std::max)(L.outerDeg, 0.5f), 179.0f) * 3.14159265f / 180.0f;
        const float inner = (std::min)((std::max)(L.innerDeg, 0.0f) * 3.14159265f / 180.0f, outer * 0.999f);
        g.cosOuter = std::cos(outer);
        g.spotScale = 1.0f / (std::max)(std::cos(inner) - g.cosOuter, 1e-4f);
        // The beam's up, square to its axis (a lobe's yaw is towards up x axis: the lamp's right).
        float up[3] = { L.up[0], L.up[1], L.up[2] };
        const float ud = up[0] * g.dir[0] + up[1] * g.dir[1] + up[2] * g.dir[2];
        for (int k = 0; k < 3; ++k) up[k] -= ud * g.dir[k];
        float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        if (ul < 1e-6f) { up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f; ul = 1.0f; }   // an axis straight up or down
        for (int k = 0; k < 3; ++k) g.up[k] = up[k] / ul;
        g.type = beam ? LightDesc::kBeam : L.type == LightDesc::kSpot ? LightDesc::kSpot : LightDesc::kPoint;
        g.flags = L.shadows ? 1u : 0u;
        g.lobeCount = beam ? (std::min)(L.lobeCount, LightDesc::kMaxLobes) : 0u;
        g.pad = 0.0f;
        for (uint32_t k = 0; k < LightDesc::kMaxLobes; ++k) {
            const LightLobe& o = L.lobes[k];
            const float dr = 3.14159265f / 180.0f;
            const bool on = k < g.lobeCount;
            g.lobeA[k][0] = on ? o.candela : 0.0f;
            g.lobeA[k][1] = on ? 1.0f / std::tan((std::max)(o.halfWidthDeg, 0.1f) * dr) : 0.0f;
            g.lobeA[k][2] = on ? 1.0f / std::tan((std::max)(o.halfHeightDeg, 0.1f) * dr) : 0.0f;
            g.lobeA[k][3] = on ? 0.5f * (std::max)(o.sharpness, 0.5f) : 1.0f;
            g.lobeB[k][0] = on ? std::tan(o.yawDeg * dr) : 0.0f;
            g.lobeB[k][1] = on ? std::tan(o.pitchDeg * dr) : 0.0f;
            g.lobeB[k][2] = g.lobeB[k][3] = 0.0f;
        }

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
        sh.spot = (L.type == LightDesc::kSpot || beam) && outer < 3.14159265f * 0.5f;   // wider: the sphere will do
        sh.cosOuter = g.cosOuter;
        sh.sinOuter = std::sin(outer);
        shapes.push_back(sh);
        // A shadow map's candidate: a shadowed spot or beam (a point light would need a cube). Its frustum's half-angles
        // - a spot's cone, a beam's lobes across and up - at most kShadowMaxTan (what is outside it reads as lit). Its score: its peak over its distance from the camera squared - the
        // player's headlamps and the Garage's spots before a lamp down the road.
        if (L.shadows && (beam || L.type == LightDesc::kSpot)) {
            ShadowCand sc{};
            sc.live = live;
            sc.flags = g.flags;
            sc.beam = beam;
            sc.tanX = (std::min)(beam ? extentX : std::tan(outer), kShadowMaxTan);
            sc.tanY = (std::min)(beam ? extentY : std::tan(outer), kShadowMaxTan);
            sc.range = (std::min)(range, kShadowFar);
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sc.pos[k] = g.pos[k];
                sc.dir[k] = g.dir[k];
                sc.up[k] = g.up[k];
                d2 += (g.pos[k] - m_camPos[k]) * (g.pos[k] - m_camPos[k]);
            }
            sc.score = peak * tintMax / (std::max)(d2, 1.0f);
            if (sc.tanX > 0.01f && sc.tanY > 0.01f && sc.range > kShadowNear * 4.0f) cands.push_back(sc);
        }
        ++live;
    }
    if (!live) return false;

    // The shadow maps' lights: the best candidates, a map for each MOUNTING - lights side by side at one height share
    // one (a headlamp's beams, a ramp's pods fanned out across the bonnet: within kShareDist of the map's lamp,
    // within kShareRise of its height, their axes within ~45 degrees of its), seen from the map's own lamp, its frustum
    // widened to the others' - at most kShadowSlots maps; their slots in their records' flags. The Impreza's ramp of
    // four pods and its two headlamps: three maps, not six.
    std::sort(cands.begin(), cands.end(), [](const ShadowCand& a, const ShadowCand& b) { return a.score > b.score; });
    const float kShareDist = 0.8f, kShareRise = 0.12f, kShareCos = 0.7f;
    ShadowCand groups[kShadowSlots];
    for (const ShadowCand& c : cands) {
        uint32_t slot = m_shadowCount;
        for (uint32_t gi = 0; gi < m_shadowCount; ++gi) {
            const ShadowCand& g = groups[gi];
            const float dx = c.pos[0] - g.pos[0], dy = c.pos[1] - g.pos[1], dz = c.pos[2] - g.pos[2];
            const float cd = c.dir[0] * g.dir[0] + c.dir[1] * g.dir[1] + c.dir[2] * g.dir[2];
            if (dx * dx + dy * dy + dz * dz < kShareDist * kShareDist && std::fabs(dy) < kShareRise && cd > kShareCos) {
                slot = gi;
                break;
            }
        }
        if (slot == m_shadowCount) {
            if (m_shadowCount >= kShadowSlots) continue;   // unshadowed, as before
            groups[m_shadowCount++] = c;
        } else {
            // The joined light's cone about the map's axis: its own half-angles plus the angle between the axes.
            ShadowCand& g = groups[slot];
            const float cd = (std::min)(c.dir[0] * g.dir[0] + c.dir[1] * g.dir[1] + c.dir[2] * g.dir[2], 1.0f);
            const float off = std::sqrt((std::max)(1.0f - cd * cd, 0.0f)) / cd;
            g.tanX = (std::min)((std::max)(g.tanX, c.tanX + off), kShadowMaxTan);
            g.tanY = (std::min)((std::max)(g.tanY, c.tanY + off), kShadowMaxTan);
            g.range = (std::max)(g.range, c.range);
        }
        m_lightMap[c.live].flags = c.flags | ((slot + 1u) << 8);   // (written, not read: upload memory)
    }
    if (std::getenv("NGG_LOCAL_SHADOW_LOG")) {
        static uint32_t n = 0;
        if ((n++ % 300) == 0)
            for (const ShadowCand& c : cands)
                Log("lights: shadow candidate %u at %.2f %.2f %.2f dir %.2f %.2f %.2f tan %.2f x %.2f range %.0f score %.3g", c.live,
                    c.pos[0], c.pos[1], c.pos[2], c.dir[0], c.dir[1], c.dir[2], c.tanX, c.tanY, c.range, c.score);
    }
    for (uint32_t slot = 0; slot < m_shadowCount; ++slot) {
        const ShadowCand& sc = groups[slot];
        // The lamp's frame: forward its axis, up a beam's up (square to it already) or the world's, right = up x fwd.
        const float* f = sc.dir;
        float u[3] = { sc.up[0], sc.up[1], sc.up[2] };
        if (!sc.beam) {
            const bool vertical = std::fabs(f[1]) > 0.99f;
            u[0] = vertical ? 1.0f : 0.0f; u[1] = vertical ? 0.0f : 1.0f; u[2] = 0.0f;
            const float ud = u[0] * f[0] + u[1] * f[1] + u[2] * f[2];
            for (int k = 0; k < 3; ++k) u[k] -= ud * f[k];
            const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            for (int k = 0; k < 3; ++k) u[k] /= ul;
        }
        const float r[3] = { u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0] };
        float V[16] = {};
        for (int k = 0; k < 3; ++k) {
            V[k * 4 + 0] = r[k];
            V[k * 4 + 1] = u[k];
            V[k * 4 + 2] = f[k];
        }
        V[12] = -(sc.pos[0] * r[0] + sc.pos[1] * r[1] + sc.pos[2] * r[2]);
        V[13] = -(sc.pos[0] * u[0] + sc.pos[1] * u[1] + sc.pos[2] * u[2]);
        V[14] = -(sc.pos[0] * f[0] + sc.pos[1] * f[1] + sc.pos[2] * f[2]);
        V[15] = 1.0f;
        // Perspective, conventional depth (0 at the near plane, 1 at the far).
        const float nz = kShadowNear, fz = sc.range;
        float P[16] = {};
        P[0] = 1.0f / sc.tanX;
        P[5] = 1.0f / sc.tanY;
        P[10] = fz / (fz - nz);
        P[11] = 1.0f;
        P[14] = -nz * fz / (fz - nz);
        float* M = m_shadowVP[slot];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                float acc = 0.0f;
                for (int k = 0; k < 4; ++k) acc += V[i * 4 + k] * P[k * 4 + j];
                M[i * 4 + j] = acc;
            }
        m_shadowInfo[slot][0] = sc.tanX;
        m_shadowInfo[slot][1] = sc.tanY;
        m_shadowInfo[slot][2] = nz;
        m_shadowInfo[slot][3] = fz;
    }

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
            Log("lights: %u live, %u list entries, %u of %u clusters lit, the longest list %u; %u shadow map(s) for %zu "
                "candidate(s)", live, total, used, kClusters, longest, m_shadowCount, cands.size());
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
