#include "sgc.h"

#include <xmmintrin.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace phys::data {

namespace {

struct Reader {
    const uint8_t* p;
    size_t at, end;
    void Need(size_t n) const
    {
        if (at + n > end) throw std::runtime_error(".sgc: a collision node is truncated");
    }
    uint32_t U32()
    {
        Need(4);
        uint32_t v;
        std::memcpy(&v, p + at, 4);
        at += 4;
        return v;
    }
    uint8_t U8()
    {
        Need(1);
        return p[at++];
    }
    float F32()
    {
        const uint32_t u = U32();
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    std::string Str()
    {
        const size_t from = at;
        while (at < end && p[at]) ++at;
        if (at == end) throw std::runtime_error(".sgc: a collision node's name runs past it");
        return std::string(reinterpret_cast<const char*>(p + from), at++ - from);
    }
};

} // namespace

std::vector<SgcCollisionNode> ReadSgcCollision(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> d;
    std::fseek(f, 0, SEEK_END);
    d.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const size_t got = std::fread(d.data(), 1, d.size(), f);
    std::fclose(f);
    if (got != d.size()) throw std::runtime_error("cannot read " + path);
    static constexpr uint32_t kHead[4] = {0xc, 0x10, 0, 1};
    std::vector<SgcCollisionNode> out;
    for (size_t at = 0; at + 20 <= d.size(); ++at) {
        if (std::memcmp(d.data() + at, kHead, 16)) continue;
        uint32_t size;
        std::memcpy(&size, d.data() + at + 16, 4);
        if (at + 20 + size > d.size()) continue;
        Reader r{d.data(), at + 20, at + 20 + size};
        SgcCollisionNode n;
        n.name = r.Str();
        n.id = r.U32();
        n.parent = r.U32();
        for (float& v : n.local) v = r.F32();
        for (float& v : n.world) v = r.F32();
        const uint32_t nv = r.U32();
        r.Need(size_t(nv) * 12);
        for (uint32_t k = 0; k < 3 * nv; ++k) n.verts.push_back(r.F32());
        const uint32_t nuv = r.U32();
        r.Need(size_t(nuv) * 8);
        for (uint32_t k = 0; k < 2 * nuv; ++k) n.uvs.push_back(r.F32());
        const uint32_t nf = r.U32();
        const uint32_t per = nuv ? 6 : 3;
        r.Need(size_t(nf) * 4 * per);
        for (uint32_t k = 0; k < nf; ++k) {
            uint32_t w[6] = {};
            for (uint32_t j = 0; j < per; ++j) w[j] = r.U32();
            n.faces.insert(n.faces.end(), w, w + 6);
        }
        if (r.U8()) {
            for (int g = 0; g < 2; ++g) {
                SgcGrid grid;
                grid.name = r.Str();
                grid.n = r.U32();
                r.U8();
                r.Need(size_t(grid.n) * grid.n);
                grid.cells.assign(d.begin() + r.at, d.begin() + r.at + size_t(grid.n) * grid.n);
                r.at += size_t(grid.n) * grid.n;
                n.grids.push_back(std::move(grid));
            }
        }
        if (r.at != r.end) throw std::runtime_error(path + ": collision node " + n.name + " does not fill its record");
        out.push_back(std::move(n));
        at += 19 + size;
    }
    return out;
}

namespace {

// HedgeHog3D's D3DX on an SSE2 CPU (its dispatch table, 0x100b1984 mode 2): the matrices as four row vectors.
struct M4 {
    __m128 r[4];
};

// D3DXMatrixMultiply (0x101552fa): out = a * b, each row's sums in its order.
M4 Mul(const M4& a, const M4& b)
{
    auto e = [&](int i, int j) { return _mm_set1_ps(reinterpret_cast<const float*>(&a.r[i])[j]); };
    M4 o;
    o.r[0] = _mm_add_ps(_mm_add_ps(_mm_mul_ps(e(0, 0), b.r[0]), _mm_mul_ps(e(0, 1), b.r[1])),
                        _mm_add_ps(_mm_mul_ps(e(0, 2), b.r[2]), _mm_mul_ps(e(0, 3), b.r[3])));
    for (int i = 1; i < 3; ++i)
        o.r[i] = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(e(i, 0), b.r[0]), _mm_mul_ps(e(i, 1), b.r[1])), _mm_mul_ps(e(i, 2), b.r[2])),
                            _mm_mul_ps(e(i, 3), b.r[3]));
    o.r[3] = _mm_add_ps(_mm_add_ps(_mm_mul_ps(b.r[0], e(3, 0)), _mm_mul_ps(b.r[1], e(3, 1))),
                        _mm_add_ps(_mm_mul_ps(b.r[2], e(3, 2)), _mm_mul_ps(b.r[3], e(3, 3))));
    return o;
}

#define SH(a, b, i) _mm_shuffle_ps(a, b, i)
// D3DXMatrixInverse (0x10155560: Intel's AP-928 cofactors, 1 / det by rcpss and one Newton step), transcribed
// instruction for instruction. A singular matrix (det 0) is returned as it came.
M4 Inverse(const M4& m)
{
    const float* f = reinterpret_cast<const float*>(m.r);
    auto pair = [&](int lo, int hi) { return _mm_setr_ps(f[lo], f[lo + 1], f[hi], f[hi + 1]); };
    __m128 x1 = pair(0, 4), x2 = pair(2, 6), x3 = pair(8, 12), x4 = pair(10, 14), x5, x6, x7;
    __m128 t30, t40, t50, t60, t70, t20, m80, m70, m60, m50, m40, m30, m20;
    x5 = SH(x2, x4, 0x88);
    x4 = SH(x4, x2, 0xDD);
    x2 = _mm_mul_ps(x4, x5);
    x2 = SH(x2, x2, 0xB1);
    x6 = SH(x2, x2, 0x4E);
    x7 = x3;
    x3 = SH(x3, x1, 0xDD);
    x1 = SH(x1, x7, 0x88);
    x7 = x3;
    x3 = _mm_mul_ps(x3, x6);
    x6 = _mm_mul_ps(x6, x1);
    t30 = x6;
    x6 = x7;
    x7 = _mm_mul_ps(x7, x2);
    x2 = _mm_mul_ps(x2, x1);
    x3 = _mm_sub_ps(x3, x7);
    x7 = _mm_mul_ps(x6, x5);
    x5 = SH(x5, x5, 0x4E);
    x7 = SH(x7, x7, 0xB1);
    t40 = x2;
    x2 = _mm_add_ps(_mm_mul_ps(x4, x7), x3);
    x3 = _mm_mul_ps(x7, x1);
    x7 = SH(x7, x7, 0x4E);
    t50 = x3;
    x3 = _mm_mul_ps(x4, x7);
    x7 = _mm_mul_ps(x7, x1);
    x2 = _mm_sub_ps(x2, x3);
    x3 = SH(x6, x6, 0x4E);
    x3 = _mm_mul_ps(x3, x4);
    x3 = SH(x3, x3, 0xB1);
    t60 = x7;
    x7 = x5;
    x5 = _mm_add_ps(_mm_mul_ps(x5, x3), x2);
    x2 = _mm_mul_ps(x3, x1);
    x3 = SH(x3, x3, 0x4E);
    t70 = x4;
    x4 = x7;
    x7 = _mm_mul_ps(x7, x3);
    x3 = _mm_mul_ps(x3, x1);
    x5 = _mm_sub_ps(x5, x7);
    x3 = _mm_sub_ps(x3, x2);
    x2 = x1;
    x1 = _mm_mul_ps(x1, x5);
    x3 = SH(x3, x3, 0x4E);
    x7 = x1;
    x1 = SH(x1, x1, 0x4E);
    t20 = x5;
    x1 = _mm_add_ps(x1, x7);
    x5 = x1;
    x1 = SH(x1, x1, 0xB1);
    x1 = _mm_add_ss(x1, x5);   // the determinant in lane 0
    x5 = _mm_mul_ps(x6, x2);
    x5 = SH(x5, x5, 0xB1);
    x7 = x5;
    x5 = SH(x5, x5, 0x4E);
    m80 = x4;
    x4 = t70;
    m70 = x6;
    x6 = _mm_add_ps(_mm_mul_ps(x4, x7), x3);
    x3 = _mm_sub_ps(_mm_mul_ps(x4, x5), x6);
    x6 = _mm_mul_ps(x4, x2);
    x6 = SH(x6, x6, 0xB1);
    m60 = x5;
    x5 = m70;
    m50 = x7;
    x7 = _mm_add_ps(_mm_mul_ps(x6, x5), x3);
    x3 = SH(x6, x6, 0x4E);
    m40 = x4;
    x4 = x5;
    x5 = _mm_mul_ps(x5, x3);
    m30 = x4;
    x4 = x6;
    x6 = _mm_sub_ps(x7, x5);
    x5 = SH(_mm_sub_ps(t30, t40), _mm_sub_ps(t30, t40), 0x4E);
    x7 = m80;
    x4 = _mm_mul_ps(x4, x7);
    x3 = _mm_mul_ps(x3, x7);
    x5 = _mm_sub_ps(x5, x4);
    x2 = _mm_mul_ps(x2, x7);
    x3 = _mm_add_ps(x3, x5);
    x2 = SH(x2, x2, 0xB1);
    x4 = SH(x2, x2, 0x4E);
    x5 = m40;
    m20 = x6;
    x6 = _mm_mul_ps(x5, x4);
    x5 = _mm_add_ps(_mm_mul_ps(x5, x2), x3);
    x3 = x4;
    x4 = _mm_sub_ps(x5, x6);
    x5 = SH(_mm_sub_ps(t60, t50), _mm_sub_ps(t60, t50), 0x4E);
    x6 = _mm_sub_ps(_mm_mul_ps(m50, x7), x5);
    x7 = _mm_mul_ps(x7, m60);
    x6 = _mm_sub_ps(x6, x7);
    x5 = m30;
    x2 = _mm_mul_ps(x2, x5);
    x5 = _mm_mul_ps(x5, x3);
    x6 = _mm_sub_ps(x6, x2);
    x2 = _mm_add_ps(x5, x6);
    if (_mm_cvtss_f32(x1) == 0.0f) return m;
    const __m128 det = x1;
    __m128 r = _mm_rcp_ss(det);
    x5 = r;
    r = _mm_mul_ss(r, r);
    __m128 d = _mm_mul_ss(det, r);
    x5 = _mm_sub_ss(_mm_add_ss(x5, x5), d);
    x5 = SH(x5, x5, 0);
    M4 o;
    o.r[0] = _mm_mul_ps(t20, x5);
    o.r[1] = _mm_mul_ps(x4, x5);
    o.r[2] = _mm_mul_ps(m20, x5);
    o.r[3] = _mm_mul_ps(x5, x2);
    return o;
}
#undef SH

M4 Identity()
{
    return {{_mm_setr_ps(1, 0, 0, 0), _mm_setr_ps(0, 1, 0, 0), _mm_setr_ps(0, 0, 1, 0), _mm_setr_ps(0, 0, 0, 1)}};
}

// A collision node's matrix, as the scene loader makes it (0x1000a3f0): its local transform (0x1000c7a0: translation,
// a rotation (angle, axis), the scale, a scaling rotation (angle, axis); 0x1008d420: inverse(Rs) * S * Rs * R * T)
// times its world transform (0x1000c6c0: the file's rows {r0 r1 r2 t} as the D3D matrix's columns). Only the zero
// angles every model here has are taken (their rotations the identity); another throws.
M4 NodeMatrix(const SgcCollisionNode& n)
{
    const float* l = n.local;
    if (l[3] != 0.0f || l[10] != 0.0f) throw std::runtime_error("collision node " + n.name + ": a rotated local transform (not supported)");
    const M4 rot = Identity();
    M4 scale = Identity(), trans = Identity();
    scale.r[0] = _mm_setr_ps(l[7], 0, 0, 0);
    scale.r[1] = _mm_setr_ps(0, l[8], 0, 0);
    scale.r[2] = _mm_setr_ps(0, 0, l[9], 0);
    trans.r[3] = _mm_setr_ps(l[0], l[1], l[2], 1);
    const M4 local = Mul(Mul(Mul(Mul(Inverse(rot), scale), rot), rot), trans);
    const float* w = n.world;
    M4 world;
    world.r[0] = _mm_setr_ps(w[0], w[4], w[8], 0);
    world.r[1] = _mm_setr_ps(w[1], w[5], w[9], 0);
    world.r[2] = _mm_setr_ps(w[2], w[6], w[10], 0);
    world.r[3] = _mm_setr_ps(w[3], w[7], w[11], 1);
    return Mul(local, world);   // 0x100a57fc(out, node + 0x60, node + 0x20)
}

// D3DXVec3TransformCoord (0x10156035): ((r0 x + r1 y) + (r2 z + r3)), divided by its w through rcpps and one Newton
// step (the CPU's reciprocal estimate).
void TransformCoord(const M4& m, const float v[3], float out[3])
{
    const __m128 zr = _mm_add_ps(_mm_mul_ps(m.r[2], _mm_set1_ps(v[2])), m.r[3]);
    const __m128 yr = _mm_mul_ps(m.r[1], _mm_set1_ps(v[1]));
    __m128 s = _mm_mul_ps(m.r[0], _mm_set1_ps(v[0]));
    s = _mm_add_ps(_mm_add_ps(s, yr), zr);
    const __m128 r = _mm_rcp_ps(s);
    const __m128 rwr = _mm_mul_ps(_mm_mul_ps(r, s), r);
    const __m128 inv = _mm_sub_ps(_mm_add_ps(r, r), rwr);
    s = _mm_mul_ps(s, _mm_shuffle_ps(inv, inv, 0xff));
    float o[4];
    _mm_storeu_ps(o, s);
    std::memcpy(out, o, 12);
}

RecMesh FromNode(const SgcCollisionNode& n, uint32_t slot, const std::string& name)
{
    RecMesh m;
    const uint32_t nv = static_cast<uint32_t>(n.verts.size() / 3), nf = static_cast<uint32_t>(n.faces.size() / 6);
    m.w[0] = slot;
    m.w[3] = (nf & 0xffff) << 16 | (nv & 0xffff);
    m.name = name;
    const M4 mat = NodeMatrix(n);
    for (uint32_t k = 0; k < nv; ++k) {
        float o[3];
        TransformCoord(mat, &n.verts[3 * k], o);
        const float v4[4] = {o[0], o[2], o[1], 0.0f};
        uint32_t w[4];
        std::memcpy(w, v4, 16);
        m.verts.insert(m.verts.end(), w, w + 4);
    }
    for (uint32_t k = 0; k < nf; ++k) {
        const uint32_t* f = &n.faces[6 * k];
        const uint8_t b[7] = {uint8_t(f[0]), uint8_t(f[1]), uint8_t(f[2]), 0, uint8_t(f[3]), uint8_t(f[4]), uint8_t(f[5])};
        m.faces.insert(m.faces.end(), b, b + 7);
    }
    if (!n.uvs.empty()) {
        m.w[6] = static_cast<uint32_t>(n.uvs.size() / 2);
        for (float u : n.uvs) m.tables[0].push_back(static_cast<uint8_t>(static_cast<int32_t>(static_cast<double>(u) * 255.0)));
    }
    if (n.grids.size() == 2) {   // the file's first grid is the record's second (+0x22 / +0x28)
        m.w[8] = (n.grids[0].n & 0xffff) << 16 | (n.grids[1].n & 0xffff);
        m.tables[1] = n.grids[1].cells;
        m.tables[2] = n.grids[0].cells;
    }
    return m;
}

} // namespace

std::vector<RecMesh> CarCollisionMeshes(const std::vector<SgcCollisionNode>& nodes)
{
    auto find = [&](const char* name) -> const SgcCollisionNode* {
        for (const SgcCollisionNode& n : nodes)
            if (n.name == name) return &n;
        return nullptr;
    };
    std::vector<RecMesh> out;
    if (const SgcCollisionNode* body = find("c_e_body")) out.push_back(FromNode(*body, 1, "MrCarCollision"));
    static constexpr struct {
        const char* name;
        uint32_t slot;
    } kParts[] = {{"c_e_door_l", 2}, {"c_e_door_r", 3}, {"c_e_bonnet", 4}, {"c_e_exhaust", 5},
                  {"c_e_bumper_f", 6}, {"c_e_bumper_b", 7}, {"c_e_wing", 8}};
    for (const auto& p : kParts)
        if (const SgcCollisionNode* n = find(p.name)) out.push_back(FromNode(*n, p.slot, p.name));
    return out;
}

} // namespace phys::data
