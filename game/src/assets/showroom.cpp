#include "assets/showroom.h"

#include <cmath>

#include "ngg_format.h"

namespace wrally::assets {

namespace {

enum { kMatFloor = 0, kMatDeck, kMatRim, kMatCount };

template <class T> void Put(std::vector<uint8_t>& out, const T& v)
{
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), b, b + sizeof(T));
}

void Chunk(std::vector<uint8_t>& out, uint32_t fourcc, const void* data, size_t bytes)
{
    Put(out, ngg::ChunkHeader{fourcc, static_cast<uint32_t>(bytes)});
    const uint8_t* b = reinterpret_cast<const uint8_t*>(data);
    out.insert(out.end(), b, b + bytes);
    while (out.size() % 4) out.push_back(0);
}

}  // namespace

std::vector<uint8_t> BuildShowroom()
{
    std::vector<ngg::Vertex> verts;
    std::vector<uint32_t> idx;
    std::vector<ngg::Object> objs;
    auto vertex = [&](float x, float y, float z, float nx, float ny, float nz) {
        ngg::Vertex v{};
        v.pos[0] = x, v.pos[1] = y, v.pos[2] = z;
        v.normal[0] = nx, v.normal[1] = ny, v.normal[2] = nz;
        v.uv0[0] = v.uv1[0] = x * 0.25f, v.uv0[1] = v.uv1[1] = z * 0.25f;
        v.colour = 0xFFFFFFFFu;
        verts.push_back(v);
        return static_cast<uint32_t>(verts.size()) - 1;
    };
    auto object = [&](uint32_t first, uint32_t material) {
        ngg::Object o{};
        o.materialId = material, o.firstIndex = first, o.indexCount = static_cast<uint32_t>(idx.size()) - first;
        objs.push_back(o);
    };
    // (A face is clockwise seen from its front - the lamp lab's builder's winding.)

    // The floor: 120 m square, tessellated (the lights' spots are not one triangle to the rasteriser), facing up.
    uint32_t first = static_cast<uint32_t>(idx.size());
    {
        constexpr int n = 60;
        constexpr float half = 60.0f, step = 2 * half / n;
        const uint32_t base = static_cast<uint32_t>(verts.size());
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i) vertex(-half + i * step, 0.0f, -half + j * step, 0, 1, 0);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const uint32_t p = base + j * (n + 1) + i;
                idx.insert(idx.end(), {p, p + n + 1, p + 1, p + 1, p + n + 1, p + n + 2});
            }
    }
    object(first, kMatFloor);

    // The turntable: a disc kShowroomDeck high, in rings (the car's shadow and the lights' fall-off), and its rim.
    constexpr int kSlices = 96, kRings = 8;
    constexpr float kPi = 3.14159265f;
    first = static_cast<uint32_t>(idx.size());
    {
        const uint32_t centre = vertex(0, kShowroomDeck, 0, 0, 1, 0);
        std::vector<uint32_t> prev(kSlices, centre);
        for (int r = 1; r <= kRings; ++r) {
            const float rad = kShowroomRadius * r / kRings;
            std::vector<uint32_t> ring(kSlices);
            for (int s = 0; s < kSlices; ++s) {
                const float a = 2 * kPi * s / kSlices;
                ring[s] = vertex(rad * std::cos(a), kShowroomDeck, rad * std::sin(a), 0, 1, 0);
            }
            for (int s = 0; s < kSlices; ++s) {
                const int t = (s + 1) % kSlices;
                if (r == 1) {
                    idx.insert(idx.end(), {centre, ring[t], ring[s]});
                } else {
                    idx.insert(idx.end(), {prev[s], ring[t], ring[s]});
                    idx.insert(idx.end(), {prev[s], prev[t], ring[t]});
                }
            }
            prev = ring;
        }
    }
    object(first, kMatDeck);
    first = static_cast<uint32_t>(idx.size());
    {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        for (int s = 0; s <= kSlices; ++s) {
            const float a = 2 * kPi * s / kSlices, nx = std::cos(a), nz = std::sin(a);
            vertex(kShowroomRadius * nx, 0, kShowroomRadius * nz, nx, 0, nz);
            vertex(kShowroomRadius * nx, kShowroomDeck, kShowroomRadius * nz, nx, 0, nz);
        }
        for (int s = 0; s < kSlices; ++s) {
            const uint32_t p = base + 2 * s;
            idx.insert(idx.end(), {p, p + 1, p + 2, p + 2, p + 1, p + 3});
        }
    }
    object(first, kMatRim);

    ngg::Material mats[kMatCount];
    const float albedo[kMatCount] = {0.035f, 0.09f, 0.5f};
    const float roughness[kMatCount] = {0.55f, 0.35f, 0.3f};
    for (int i = 0; i < kMatCount; ++i) {
        mats[i] = ngg::DefaultMaterial();
        mats[i].baseColourFactor[0] = mats[i].baseColourFactor[1] = mats[i].baseColourFactor[2] = albedo[i];
        mats[i].baseColourFactor[3] = 1.0f;
        mats[i].roughness = roughness[i];
    }

    ngg::SceneHeader sh{};
    for (int k = 0; k < 3; ++k) sh.bboxMin[k] = 1e30f, sh.bboxMax[k] = -1e30f;
    for (const ngg::Vertex& v : verts)
        for (int k = 0; k < 3; ++k) {
            sh.bboxMin[k] = std::fmin(sh.bboxMin[k], v.pos[k]);
            sh.bboxMax[k] = std::fmax(sh.bboxMax[k], v.pos[k]);
        }
    sh.vertexCount = static_cast<uint32_t>(verts.size());
    sh.indexCount = static_cast<uint32_t>(idx.size());
    sh.bakedScale = 1.0f;

    std::vector<uint8_t> out;
    Put(out, ngg::FileHeader{ngg::kMagic, ngg::kFormatV3, 5, 0});
    Chunk(out, ngg::ChunkSceneHeader(), &sh, sizeof sh);
    Chunk(out, ngg::ChunkVertices(), verts.data(), verts.size() * sizeof(ngg::Vertex));
    Chunk(out, ngg::ChunkIndices(), idx.data(), idx.size() * sizeof(uint32_t));
    Chunk(out, ngg::ChunkMaterials(), mats, sizeof mats);
    Chunk(out, ngg::ChunkObjects(), objs.data(), objs.size() * sizeof(ngg::Object));
    return out;
}

}  // namespace wrally::assets
