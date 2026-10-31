#include "assets/lamp_lab.h"

#include <cmath>
#include <cstring>

#include "ngg_format.h"

namespace wrally::assets {

namespace {

constexpr float kPi = 3.14159265f;

enum { kMatFloor = 0, kMatWall, kMatWhite, kMatGrey, kMatCount };

// The old lab's builder (ngg/host/testscene.cpp): render axes, y up; a face is clockwise seen from its front.
struct Builder {
    std::vector<ngg::Vertex> verts;
    std::vector<uint32_t> idx;
    std::vector<ngg::Object> objs;
    uint32_t objStart = 0;

    void Begin() { objStart = static_cast<uint32_t>(idx.size()); }
    void End(uint32_t material)
    {
        ngg::Object o{};
        o.materialId = material;
        o.firstIndex = objStart;
        o.indexCount = static_cast<uint32_t>(idx.size()) - objStart;
        if (o.indexCount) objs.push_back(o);
    }
    uint32_t V(float x, float y, float z, float nx, float ny, float nz, float u, float v)
    {
        ngg::Vertex vx{};
        vx.pos[0] = x, vx.pos[1] = y, vx.pos[2] = z;
        vx.normal[0] = nx, vx.normal[1] = ny, vx.normal[2] = nz;
        vx.uv0[0] = vx.uv1[0] = u;
        vx.uv0[1] = vx.uv1[1] = v;
        vx.colour = 0xFFFFFFFFu;
        verts.push_back(vx);
        return static_cast<uint32_t>(verts.size()) - 1;
    }
    void Tri(uint32_t a, uint32_t b, uint32_t c) { idx.push_back(a), idx.push_back(b), idx.push_back(c); }
    // A flat quad: corner o, edges eu and ev; its normal ev x eu.
    void Quad(const float o[3], const float eu[3], const float ev[3], int su = 1, int sv = 1)
    {
        float n[3] = {ev[1] * eu[2] - ev[2] * eu[1], ev[2] * eu[0] - ev[0] * eu[2], ev[0] * eu[1] - ev[1] * eu[0]};
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (float& c : n) c /= l;
        const uint32_t base = static_cast<uint32_t>(verts.size());
        for (int j = 0; j <= sv; ++j)
            for (int i = 0; i <= su; ++i) {
                const float a = static_cast<float>(i) / su, b = static_cast<float>(j) / sv;
                V(o[0] + eu[0] * a + ev[0] * b, o[1] + eu[1] * a + ev[1] * b, o[2] + eu[2] * a + ev[2] * b, n[0], n[1], n[2], a, b);
            }
        for (int j = 0; j < sv; ++j)
            for (int i = 0; i < su; ++i) {
                const uint32_t p = base + j * (su + 1) + i;
                Tri(p, p + su + 1, p + 1);
                Tri(p + 1, p + su + 1, p + su + 2);
            }
    }
    // An axis box at a centre, sizes sx, sy, sz.
    void Box(float cx, float cy, float cz, float sx, float sy, float sz, int su = 1, int sv = 1)
    {
        const float hx = sx * 0.5f, hy = sy * 0.5f, hz = sz * 0.5f;
        struct F { float o[3], u[3], v[3]; };
        const F f[6] = {
            {{-hx, -hy, -hz}, {sx, 0, 0}, {0, sy, 0}},    // -z
            {{hx, -hy, hz}, {-sx, 0, 0}, {0, sy, 0}},     // +z
            {{hx, -hy, -hz}, {0, 0, sz}, {0, sy, 0}},     // +x
            {{-hx, -hy, hz}, {0, 0, -sz}, {0, sy, 0}},    // -x
            {{-hx, hy, -hz}, {sx, 0, 0}, {0, 0, sz}},     // +y
            {{-hx, -hy, hz}, {sx, 0, 0}, {0, 0, -sz}},    // -y
        };
        for (const F& q : f) {
            const float o[3] = {q.o[0] + cx, q.o[1] + cy, q.o[2] + cz};
            Quad(o, q.u, q.v, su, sv);
        }
    }
    // An upright cylinder's side, from the floor up.
    void Cylinder(float cx, float cz, float r, float h, int slices = 16)
    {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        for (int j = 0; j <= 1; ++j)
            for (int i = 0; i <= slices; ++i) {
                const float ph = 2.0f * kPi * i / slices;
                const float nx = std::cos(ph), nz = std::sin(ph);
                V(cx + r * nx, h * j, cz + r * nz, nx, 0.0f, nz, static_cast<float>(i) / slices, static_cast<float>(j));
            }
        for (int i = 0; i < slices; ++i) {
            const uint32_t p = base + i, q = p + slices + 1;
            Tri(p, q, p + 1);
            Tri(p + 1, q, q + 1);
        }
    }
};

template <class T>
void Put(std::vector<uint8_t>& out, const T& v)
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

std::vector<uint8_t> BuildLampLab()
{
    Builder b;
    // The floor: 400 m square around the car, tessellated, facing up.
    b.Begin();
    {
        const float o[3] = {-200.0f, 0.0f, -200.0f}, u[3] = {400.0f, 0.0f, 0.0f}, v[3] = {0.0f, 0.0f, 400.0f};
        b.Quad(o, u, v, 40, 40);
    }
    b.End(kMatFloor);
    // The wall: 30 m wide, 6 m tall, its face kLabWallAhead in front of the car (-z), tessellated so a lamp's spot is
    // not one triangle to the rasteriser.
    b.Begin();
    b.Box(0.0f, 3.0f, -(kLabWallAhead + 0.2f), 30.0f, 6.0f, 0.4f, 30, 6);
    b.End(kMatWall);
    // Thin white poles, 4 cm, 1.6 m tall, 5 m ahead across the beams; another row at 8 m, offset.
    b.Begin();
    for (float x : {-3.0f, -1.5f, 0.0f, 1.5f, 3.0f}) b.Cylinder(x, -5.0f, 0.04f, 1.6f);
    for (float x : {-4.5f, -2.25f, 2.25f, 4.5f}) b.Cylinder(x, -8.0f, 0.04f, 1.6f);
    b.End(kMatWhite);
    // A person-sized block, 0.5 x 1.8 x 0.3 m, 7 m ahead and to the right of the car (-x: its right).
    b.Begin();
    b.Box(-2.6f, 0.9f, -7.0f, 0.5f, 1.8f, 0.3f);
    b.End(kMatGrey);

    ngg::Material mats[kMatCount];
    const float albedo[kMatCount] = {0.18f, 0.5f, 0.9f, 0.18f};
    for (int i = 0; i < kMatCount; ++i) {
        mats[i] = ngg::DefaultMaterial();
        mats[i].baseColourFactor[0] = mats[i].baseColourFactor[1] = mats[i].baseColourFactor[2] = albedo[i];
        mats[i].baseColourFactor[3] = 1.0f;
    }

    ngg::SceneHeader sh{};
    for (int k = 0; k < 3; ++k) sh.bboxMin[k] = 1e30f, sh.bboxMax[k] = -1e30f;
    for (const ngg::Vertex& v : b.verts)
        for (int k = 0; k < 3; ++k) {
            sh.bboxMin[k] = std::fmin(sh.bboxMin[k], v.pos[k]);
            sh.bboxMax[k] = std::fmax(sh.bboxMax[k], v.pos[k]);
        }
    sh.vertexCount = static_cast<uint32_t>(b.verts.size());
    sh.indexCount = static_cast<uint32_t>(b.idx.size());
    sh.bakedScale = 1.0f;

    std::vector<uint8_t> out;
    Put(out, ngg::FileHeader{ngg::kMagic, ngg::kFormatV3, 5, 0});
    Chunk(out, ngg::ChunkSceneHeader(), &sh, sizeof sh);
    Chunk(out, ngg::ChunkVertices(), b.verts.data(), b.verts.size() * sizeof(ngg::Vertex));
    Chunk(out, ngg::ChunkIndices(), b.idx.data(), b.idx.size() * sizeof(uint32_t));
    Chunk(out, ngg::ChunkMaterials(), mats, sizeof mats);
    Chunk(out, ngg::ChunkObjects(), b.objs.data(), b.objs.size() * sizeof(ngg::Object));
    return out;
}

}  // namespace wrally::assets
