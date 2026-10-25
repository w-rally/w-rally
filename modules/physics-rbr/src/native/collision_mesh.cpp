#include "native/collision_mesh.h"

#include <xmmintrin.h>

#include <cstring>

#include "num/collide.h"
#include "num/f32.h"
#include "num/x87.h"
#include "native/flags.h"

namespace phys::native {

using num::F32;

namespace {
// P[f0] - P[f1] and P[f2] - P[f1] of a face, xyz (w 0: the original's w lanes are stack garbage, never used).
void FaceEdges(const CollisionMesh& m, uint32_t face, Vec4& a, Vec4& b)
{
    const uint8_t* const f = m.faces + 7 * face;
    const float* const p0 = &m.vertices[f[0]].x;
    const float* const p1 = &m.vertices[f[1]].x;
    const float* const p2 = &m.vertices[f[2]].x;
    a = {(F32(p0[0]) - p1[0]).f(), (F32(p0[1]) - p1[1]).f(), (F32(p0[2]) - p1[2]).f(), 0.0f};
    b = {(F32(p2[0]) - p1[0]).f(), (F32(p2[1]) - p1[1]).f(), (F32(p2[2]) - p1[2]).f(), 0.0f};
}

// 0x4f7250: (P2 - P1) x (P0 - P1).
void FaceCross(const CollisionMesh& m, uint32_t face, Vec4& out)
{
    Vec4 a, b;
    FaceEdges(m, face, a, b);
    num::Cross3(b, a, out);
}

F32 Side(const float* plane, const float* p) { return ((F32(p[1]) * plane[1] + F32(p[2]) * plane[2]) + F32(plane[0]) * p[0]) + plane[3]; }

// A face's local copy for the adjacency walk.
struct Node {
    uint8_t v[7];
    uint32_t next[3];   // the neighbours over the edges v1 v2, v2 v0, v0 v1; -1 none
    uint32_t done;
};

void Walk(CollisionMesh& m, Node* nodes, uint32_t i)   // 0x4f9c00 / 0x4f9b80
{
    Node& n = nodes[i];
    if (n.done) return;
    n.done = 1;
    m.BspInsert(n.v, 0);
    for (uint32_t k = 0; k < 3; ++k)
        if (n.next[k] != 0xffffffffu) Walk(m, nodes, n.next[k]);
}

// One face's side of an edge of the adjacency: the face's edge (i % 3, (i + 1) % 3) is the edge in either direction.
void Link(CollisionMesh& m, Node& n, uint32_t i, const MeshEdge& e, uint32_t other, uint32_t faces)
{
    const uint8_t r = n.v[i % 3], s = n.v[(i + 1) % 3];
    if (!((r == e.a && s == e.b) || (s == e.a && r == e.b))) return;
    uint32_t& next = n.next[i - 1];
    if (next > faces) next = other;
    else m.problems |= 2;
}
} // namespace

int32_t CollisionMesh::AddEdge(uint32_t face, uint32_t a, uint32_t b, uint8_t x, uint8_t y)
{
    const uint32_t n = edgeCount;
    for (uint32_t i = 0; i < n; ++i) {
        MeshEdge& e = edges[i];
        if (e.a == b && e.b == a) {
            e.otherFace = static_cast<uint8_t>(face);
            return -1;
        }
        if (e.a == a && e.b == b) {
            e.otherFace = static_cast<uint8_t>(face);
            return 0;
        }
    }
    if (edgeCount >= 0x54) return 0;
    MeshEdge& e = edges[n];   // (the other face is not written)
    e.a = static_cast<uint8_t>(a);
    e.b = static_cast<uint8_t>(b);
    e.face = static_cast<uint8_t>(face);
    e.attributeA = x;
    e.attributeB = y;
    ++edgeCount;
    return 1;
}

void CollisionMesh::FacePlane(uint32_t face, float out[4]) const
{
    Vec4 a, b, n;
    FaceEdges(*this, face, a, b);
    num::CrossNormalize4Approx(b, a, n);
    out[0] = n.x;
    out[1] = n.y;
    out[2] = n.z;
    const float* const p1 = &vertices[faces[7 * face + 1]].x;
    out[3] = (F32(0.0f) - ((F32(p1[2]) * n.z + F32(p1[1]) * n.y) + F32(p1[0]) * n.x)).f();
}

uint32_t CollisionMesh::BspInsert(const uint8_t* v, uint32_t face)
{
    for (;;) {
        const uint32_t count = faceCount;
        if (face >= count) break;
        float plane[4];
        FacePlane(face, plane);
        uint32_t behind = 0, front = 0;
        for (int k = 0; k < 3; ++k) behind += -0.1f > Side(plane, &vertices[v[k]].x).f();
        for (int k = 0; k < 3; ++k) front += Side(plane, &vertices[v[k]].x).f() > 0.1f;
        uint32_t side = 0;
        if (behind) {
            if (front) problems |= 4;
        } else if (front) {
            side = 1;
        }
        uint8_t& child = bspChildren[2 * face + side];
        if (!child) child = static_cast<uint8_t>(count);
        face = child;
    }
    const uint32_t index = faceCount;
    Vec4 t;
    FaceCross(*this, index, t);   // the face at the BSP's index, not v's (the original's)
    for (int k = 0; k < 3; ++k) {
        float* const s = &normalSums[v[k]].x;
        s[0] = (F32(s[0]) + t.x).f();
        s[1] = (F32(s[1]) + t.y).f();
        s[2] = (F32(s[2]) + t.z).f();
    }
    faceCount = static_cast<uint8_t>(index + 1);
    return index;
}

bool CollisionMesh::BspInside(const float* p, uint32_t face) const
{
    for (;;) {
        float plane[4];
        FacePlane(face, plane);
        const F32 s = ((F32(p[0]) * plane[0] + F32(plane[1]) * p[1]) + F32(plane[2]) * p[2]) + plane[3];
        if ((F32(0.0f) - s).f() >= 0.0f) {
            const uint8_t c = bspChildren[2 * face];
            if (!c) return true;
            face = c;
        } else {
            const uint8_t c = bspChildren[2 * face + 1];
            if (!c) return false;
            face = c;
        }
    }
}

void CollisionMesh::Build(const MeshRecord& r)
{
    Node nodes[56];   // the faces' local copies (the BSP's children table holds as many)
    for (Node& n : nodes) {
        n.next[0] = n.next[1] = n.next[2] = 0xffffffffu;
        n.done = 0;
    }
    const uint32_t recordFaces = r.faceCount;
    faces = r.faces;
    flags = 0;
    material = r.material;
    name = r.name;
    vertexCount = static_cast<uint8_t>(r.vertexCount);
    gridWidthA = r.gridWidthA;
    gridWidthB = r.gridWidthB;
    gridA = gridB = nullptr;
    surfaceA = surfaceB = faceCount = edgeCount = 0;
    problems = 0;
    uvs = nullptr;
    uvCount = static_cast<uint8_t>(r.uvCount);
    shape = nullptr;
    std::memset(bspChildren, 0, sizeof bspChildren);
    if (r.flags & kRecordFlag4) flags |= kMeshRecordFlag4;
    if (r.flags & (kRecordSphere | kRecordBox)) {   // the record's cheap shape
        shape = r.shape;
        flags |= (r.flags & kRecordBox) ? kMeshBox : kMeshSphere;
    }
    if (!r.vertexCount) {
        problems = 0xffffffffu;
        return;
    }
    if (uvCount) uvs = r.uvs;
    if (gridWidthA) gridA = r.gridA;
    if (gridWidthB) gridB = r.gridB;
    if (!gridB || !gridA || !uvs) {
        gridA = gridB = nullptr;
        uvs = nullptr;
    }
    for (uint32_t i = 0; i < recordFaces; ++i) {
        const uint8_t* const f = faces + 7 * i;
        attributes[f[0]] = f[4];
        attributes[f[1]] = f[5];
        attributes[f[2]] = f[6];
    }

    float maxX = 0.0f;
    for (uint32_t i = 0; i < vertexCount; ++i) {
        num::Vec4Zero(normalSums[i]);
        const float x = r.vertices[i].x;
        if (x > maxX) maxX = x;
        std::memcpy(&vertices[i], &r.vertices[i], 12);   // xyz
    }
    for (uint32_t i = 0; i < recordFaces; ++i) {
        const uint8_t* const f = faces + 7 * i;
        if (!AddEdge(i, f[0], f[1], f[4], f[5]) || !AddEdge(i, f[1], f[2], f[5], f[6]) || !AddEdge(i, f[2], f[0], f[6], f[4])) problems |= 1;
    }

    // The face adjacency over the edges, then the BSP from face 0 along it.
    for (uint32_t i = 0; i < 56; ++i) {
        if (i < recordFaces) std::memcpy(nodes[i].v, faces + 7 * i, 7);
        else nodes[i].done = 1;
    }
    for (uint32_t i = 0; i < edgeCount; ++i) {
        const MeshEdge& e = edges[i];
        const uint32_t a = e.face, b = e.otherFace;
        if (a >= recordFaces || b >= recordFaces) {
            problems |= 2;
            continue;
        }
        for (uint32_t k = 1; k <= 3; ++k) {
            Link(*this, nodes[a], k, e, b, recordFaces);
            Link(*this, nodes[b], k, e, a, recordFaces);
        }
    }
    if (!nodes[0].done) {
        nodes[0].done = 1;
        BspInsert(nodes[0].v, 0);
        for (uint32_t k = 0; k < 3; ++k)
            if (nodes[0].next[k] != 0xffffffffu) Walk(*this, nodes, nodes[0].next[k]);
    }
    const float outside[3] = {(F32(maxX) * 2.0f).f(), 0.0f, 0.0f};
    if (BspInside(outside, 0)) problems |= 1;

    // The bounds (all four lanes), the centre and the half extent.
    Vec4& lo = boundsMin;
    Vec4& hi = boundsMax;
    num::Vec4Copy(lo, vertices[0]);
    num::Vec4Copy(hi, vertices[0]);
    for (uint32_t i = 1; i < vertexCount; ++i) {
        num::Min4(lo, vertices[i]);
        num::Max4(hi, vertices[i]);
    }
    float* const c = &centre.x;
    float* const h = &halfExtent.x;
    c[0] = (F32(hi.x) + lo.x).f();
    c[1] = (F32(hi.y) + lo.y).f();
    c[2] = (F32(hi.z) + lo.z).f();
    h[0] = (F32(hi.x) - lo.x).f();
    h[1] = (F32(hi.y) - lo.y).f();
    h[2] = (F32(hi.z) - lo.z).f();
    for (int k = 0; k < 4; ++k) c[k] = (F32(0.5f) * c[k]).f();   // all four lanes (mulps)
    for (int k = 0; k < 4; ++k) h[k] = (F32(0.5f) * h[k]).f();

    if (problems) return;
    // The edges again from the BSP's faces (results unchecked), then only the sharp ones (over 10 degrees) are kept.
    edgeCount = 0;
    for (uint32_t i = 0; i < faceCount; ++i) {
        const uint8_t* const f = faces + 7 * i;
        AddEdge(i, f[0], f[1], f[4], f[5]);
        AddEdge(i, f[1], f[2], f[5], f[6]);
        AddEdge(i, f[2], f[0], f[6], f[4]);
    }
    uint32_t cosBits = 0x3f7c1c5c;   // (float)fcos(0.17453293502330780): 10 degrees
    float cosLimit;
    std::memcpy(&cosLimit, &cosBits, 4);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < edgeCount; ++i) {
        const MeshEdge& e = edges[i];
        const uint32_t a = e.face, b = e.otherFace;
        if (a >= faceCount || b >= faceCount) continue;
        if (faces[7 * a + 3] == 0xff || faces[7 * b + 3] == 0xff) {
            flags |= kMeshNoEdges;
            continue;
        }
        float pa[4], pb[4];
        FacePlane(a, pa);
        FacePlane(b, pb);
        const float dot = ((F32(pb[2]) * pa[2] + F32(pb[1]) * pa[1]) + F32(pb[0]) * pa[0]).f();
        if (cosLimit >= dot) {
            std::memmove(&edges[kept], &e, sizeof e);
            ++kept;
        }
    }
    edgeCount = static_cast<uint8_t>(kept);
}

CollisionMesh* MeshMove(const Frame& t, const Frame& a, CollisionMesh* m, uint32_t n)
{
    struct Ops {   // one level (0x4f5e40 / 0x4f5eb0): each mesh's vertices as points, its normal sums as directions
        CollisionMesh* m;
        void Step(const Frame& f, uint32_t k, bool up) const
        {
            const uint32_t count = m[0].vertexCount;   // the first mesh's for all of them (kept)
            for (uint32_t i = 0; i < k; ++i) {
                if (up) {
                    f.Up(m[i].vertices, count, Move::Points);
                    f.Up(m[i].normalSums, count, Move::Dirs);
                } else {
                    f.Down(m[i].vertices, count, Move::Points);
                    f.Down(m[i].normalSums, count, Move::Dirs);
                }
            }
        }
        void Up(const Frame& f, uint32_t k) const { Step(f, k, true); }
        void Down(const Frame& f, uint32_t k) const { Step(f, k, false); }
    };
    walk::Move(t, a, n, Ops{m});
    return m;
}

namespace {
float FromBits(uint32_t b)
{
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
F32 Byte(uint8_t x) { return F32(static_cast<float>(x)); }
Vec4 Sub3(const Vec4& a, const Vec4& b) { return {(F32(a.x) - F32(b.x)).f(), (F32(a.y) - F32(b.y)).f(), (F32(a.z) - F32(b.z)).f(), 0.0f}; }
} // namespace

void CollisionMesh::SurfaceCells(F32 s, F32 t, uint32_t* a, uint32_t* b) const
{
    auto cell = [&](uint32_t n, const uint8_t* grid) {   // a grid n wide, row-major
        const F32 fn = F32(static_cast<float>(static_cast<int32_t>(n)));
        const int32_t i = _mm_cvtt_ss2si(_mm_set_ss((fn * t).f()));
        const int32_t j = _mm_cvtt_ss2si(_mm_set_ss((fn * s).f()));
        // The original's 32-bit address arithmetic: a point past the edge's end gives a negative cell, which reads the
        // bytes before the grid (the record data there is the surface). Kept as a signed offset into the record's data -
        // TODO(M6 step 2, the load): a decision once the native load owns the records.
        const int32_t at = static_cast<int32_t>(static_cast<uint32_t>(i) * n + static_cast<uint32_t>(j));
        return static_cast<uint32_t>(grid[at]);
    };
    *a = cell(gridWidthA, gridA);
    *b = cell(gridWidthB, gridB);
}

void CollisionMesh::SurfaceAtUv(uint32_t idx, uint32_t* b, uint32_t* a) const
{
    if (!uvs) {
        *b = *a = 0;
        return;
    }
    const uint8_t* const uv = uvs + 2 * attributes[idx];
    const F32 k = F32(FromBits(0x3b800000));   // 1/256
    const F32 t = Byte(uv[1]) * k, s = Byte(uv[0]) * k;
    SurfaceCells(s, t, a, b);
}

void CollisionMesh::SurfaceOnEdge(uint32_t k, float t, uint32_t* b, uint32_t* a) const
{
    if (!uvs) {
        *a = *b = 0;
        return;
    }
    const uint8_t* const A = uvs + 2 * edges[k].attributeB;
    const uint8_t* const B = uvs + 2 * edges[k].attributeA;
    const F32 tt = F32(t), r = F32(1.0f) - tt, c = F32(FromBits(0x3b800000));
    const F32 s = (Byte(A[0]) * r + Byte(B[0]) * tt) * c;
    const F32 v = (Byte(B[1]) * tt + Byte(A[1]) * r) * c;
    SurfaceCells(s, v, a, b);
}

float CollisionMesh::EdgePlane(uint32_t k, const Vec4& a, const Vec4& b, Vec4& out) const
{
    const float* const v0 = &vertices[edges[k].a].x;
    const float* const v1 = &vertices[edges[k].b].x;
    const Vec4 e{(F32(v1[0]) - F32(v0[0])).f(), (F32(v1[1]) - F32(v0[1])).f(), (F32(v1[2]) - F32(v0[2])).f(), 0.0f};   // w: stack
    const Vec4 d{(F32(b.x) - F32(a.x)).f(), (F32(b.y) - F32(a.y)).f(), (F32(b.z) - F32(a.z)).f(), 0.0f};
    Vec4 n{};
    num::Cross3(e, d, n);
    const F32 s = (F32(v1[1]) * F32(n.y) + F32(v1[2]) * F32(n.z)) + F32(v1[0]) * F32(n.x);
    if (num::jmp::A(0.0f, s.f())) (num::F32x4::Splat(F32(-1.0f)) * num::F32x4::Load(&n.x)).Store(&n.x);   // facing away from v1
    num::Normalize3Approx4(n);
    out.x = n.x;
    out.y = n.y;
    out.z = n.z;
    out.w = (F32(0.0f) - ((F32(v0[1]) * F32(n.y) + F32(v0[2]) * F32(n.z)) + F32(v0[0]) * F32(n.x))).f();
    return 1.0f;
}

num::X87 CollisionMesh::EdgeSweep(uint32_t k, const Vec4& A, const Vec4& B, const Vec4& C, const Vec4& D, Vec4* P, Vec4* Q, Vec4* R) const
{
    using num::X87;
    namespace jmp = num::jmp;
    const MeshEdge& e = edges[k];
    const Vec4& v0 = vertices[e.a];
    const Vec4& v1 = vertices[e.b];
    const X87 miss = X87(2.0f);
    const float t1 = num::TripleProduct(&v0.x, &v1.x, &A.x, &B.x).Store();
    const float t2 = num::TripleProduct(&v0.x, &v1.x, &C.x, &D.x).Store();
    if (jmp::A(t1, 0.0f) && jmp::A(t2, 0.0f)) return miss;   // the segment stays on one side of the edge's line
    if (jmp::A(0.0f, t1) && jmp::A(0.0f, t2)) return miss;
    const float s = (X87(t1) / (X87(t1) - X87(t2))).Store();   // fsub; fdivr
    num::Lerp4(A, C, s, *Q);
    num::Lerp4(B, D, s, *R);
    Vec4 n1, n2, n3, n4, ev;   // the quad v0 Q R v1: each corner's normal
    num::Cross3(Sub3(v0, *Q), Sub3(*R, *Q), n1);
    num::Cross3(Sub3(*R, v0), Sub3(v1, v0), n2);
    num::Cross3(Sub3(v1, *R), Sub3(*Q, *R), n3);
    num::Cross3(Sub3(*Q, v1), Sub3(v0, v1), n4);
    num::Cross3(v0, v1, ev);
    const F32 d1 = (F32(C.y) * F32(ev.y) + F32(C.z) * F32(ev.z)) + F32(C.x) * F32(ev.x);
    num::Cross3(v0, v1, ev);   // computed again by the original
    const F32 d2 = (F32(D.y) * F32(ev.y) + F32(D.z) * F32(ev.z)) + F32(D.x) * F32(ev.x);
    if (jmp::A(d1.f(), 0.0f) && jmp::A(d2.f(), 0.0f)) return miss;
    if (jmp::A(0.0f, d1.f()) && jmp::A(0.0f, d2.f())) return miss;
    auto dot = [](const Vec4& a, const Vec4& b) { return ((F32(a.z) * F32(b.z) + F32(a.y) * F32(b.y)) + F32(a.x) * F32(b.x)).f(); };
    if (jmp::A(0.0f, dot(n2, n1)) || jmp::A(0.0f, dot(n3, n2)) || jmp::A(0.0f, dot(n4, n3)) || jmp::A(0.0f, dot(n4, n1))) return miss;
    num::Lerp4(*Q, *R, (d1 / (d1 - d2)).f(), *P);
    return X87(s);
}

bool CollisionMesh::EdgesSweep(const Vec4& A, const Vec4& B, const Vec4& C, const Vec4& D, Vec4* P, float* best, Vec4* plane)
{
    *best = 1.01f;
    uint32_t hit = 0x54;
    Vec4 q, r;
    for (uint32_t k = 0; k < edgeCount; ++k) {
        Vec4 p, qk, rk;
        const num::X87 s = EdgeSweep(k, A, B, C, D, &p, &qk, &rk);
        if (!num::jmp::A(*best, s.d)) continue;
        *best = s.Store();
        num::Vec4Copy(*P, p);
        hit = k;
        num::Vec4Copy(q, qk);
        num::Vec4Copy(r, rk);
    }
    if (hit >= 0x54) return false;
    EdgePlane(hit, q, r, *plane);
    if (!uvs) return true;
    // The surface bytes at the hit: t = |P - v1| / |v1 - v0| along the edge, the uvs of v1 and v0 blended by it.
    const uint8_t i0 = edges[hit].a, i1 = edges[hit].b;
    const float le = num::Length3(Sub3(vertices[i1], vertices[i0]));
    const F32 t = F32((num::X87(num::Length3(Sub3(*P, vertices[i1]))) / num::X87(le)).Store());
    const uint8_t* const u1 = uvs + 2 * attributes[i1];
    const uint8_t* const u0 = uvs + 2 * attributes[i0];
    const F32 rt = F32(1.0f) - t, c = F32(FromBits(0x3b800000));
    const F32 su = (Byte(u1[0]) * rt + Byte(u0[0]) * t) * c;
    const F32 sv = (Byte(u0[1]) * t + Byte(u1[1]) * rt) * c;
    uint32_t a, b;
    SurfaceCells(su, sv, &a, &b);
    surfaceA = static_cast<uint8_t>(a);
    surfaceB = static_cast<uint8_t>(b);
    return true;
}

bool CollisionMesh::BoxSegmentOverlap(const float* p, const float* h) const
{
    num::AABBox box;
    std::memcpy(&box.pos, &centre, 12);
    std::memcpy(&box.ext, &halfExtent, 12);
    num::Vec3 m, hh;
    std::memcpy(&m, p, 12);
    std::memcpy(&hh, h, 12);
    return num::SegmentBoxOverlap(box, m, hh) != 0;
}

bool CollisionMesh::SegmentHit(const float* p0, const float* p1, Vec4* point, float* t, Vec4* plane, uint32_t* materialOut)
{
    namespace jmp = num::jmp;
    const Vec4 d{(F32(p1[0]) - F32(p0[0])).f(), (F32(p1[1]) - F32(p0[1])).f(), (F32(p1[2]) - F32(p0[2])).f(), 0.0f};   // w: stack
    Vec4 h;
    num::Scale4(d, 0.5f, h);
    if (!BoxSegmentOverlap(p0, &h.x)) return false;
    struct alignas(16) Hit {   // SegmentTriangleHit's record: {the normal (xyz written), -, t, u, v}
        Vec4 normal;
        float unknown10, t, u, v;
    } hit{};
    const uint8_t* tri = nullptr;
    for (uint32_t i = 0; i < faceCount; ++i) {   // the enabled faces (material 0xff: off)
        const uint8_t* const c = faces + 7 * i;
        if (c[3] == 0xff) continue;
        if (!num::SegmentTriangleHit(p0, &d.x, &vertices[c[0]].x, &vertices[c[1]].x, &vertices[c[2]].x, reinterpret_cast<uint8_t*>(&hit))) continue;
        if (jmp::A(hit.t, 0.0f) && jmp::A(FromBits(0x3f8147ae), hit.t)) {   // 0 < t < 1.01
            tri = c;
            break;
        }
    }
    if (!tri) return false;
    *t = hit.t;
    Vec4 n = hit.normal;
    num::Normalize3Approx4(n);
    const float* const v0 = &vertices[tri[0]].x;
    plane->x = n.x;
    plane->y = n.y;
    plane->z = n.z;
    plane->w = (F32(0.0f) - ((F32(v0[2]) * F32(n.z) + F32(v0[1]) * F32(n.y)) + F32(v0[0]) * F32(n.x))).f();
    const F32 s = F32(*t);
    point->x = (F32(d.x) * s + F32(p0[0])).f();
    point->y = (F32(d.y) * s + F32(p0[1])).f();
    point->z = (F32(d.z) * s + F32(p0[2])).f();
    point->w = (F32(d.w) * s + F32(p0[3])).f();
    *materialOut = material;
    if (uvs) {   // the surface maps: the hit's texture coordinates (uv bytes / 256) into the two byte grids
        const uint8_t *a = uvs + 2 * tri[4], *b = uvs + 2 * tri[5], *c = uvs + 2 * tri[6];
        const F32 u = F32(hit.u), v = F32(hit.v);
        const F32 w = F32(1.0f) - (v + u);
        const F32 k = F32(FromBits(0x3b800000));   // 1/256
        const F32 sc = ((Byte(b[0]) * u + Byte(c[0]) * v) + Byte(a[0]) * w) * k;
        const F32 tc = ((Byte(a[1]) * w + Byte(b[1]) * u) + Byte(c[1]) * v) * k;
        auto cell = [&](uint32_t n2, const uint8_t* grid) {   // a grid n2 wide (no wrap here: the hit is on the face)
            const F32 fn = F32(static_cast<float>(static_cast<int32_t>(n2)));
            const int32_t i = _mm_cvtt_ss2si(_mm_set_ss((fn * tc).f()));
            const int32_t j = _mm_cvtt_ss2si(_mm_set_ss((fn * sc).f()));
            return grid[static_cast<uint32_t>(i) * n2 + static_cast<uint32_t>(j)];
        };
        surfaceA = cell(gridWidthA, gridA);
        surfaceB = cell(gridWidthB, gridB);
    }
    return true;
}

CollisionMesh* MeshBuild(CollisionMesh* mesh, const CollisionMesh* src, const Frame& from, const Frame& to)
{
    if (!src) {   // 0x4f6550: the empty mesh
        if (!mesh) return mesh;
        mesh->surfaceA = mesh->surfaceB = 0;
        mesh->shape = nullptr;
        mesh->uvs = mesh->gridA = mesh->gridB = mesh->faces = nullptr;
        mesh->faceCount = mesh->edgeCount = mesh->vertexCount = mesh->uvCount = 0;
        num::Vec4Zero(mesh->boundsMin);
        num::Vec4Zero(mesh->boundsMax);
        return mesh;
    }
    if (mesh != src) *mesh = *src;
    const uint32_t n = mesh->vertexCount;
    if (!n) return mesh;
    Vec4* const v = mesh->vertices;
    FrameMoveCopyN(from, v, to, v, n, Move::Points);
    FrameMoveCopyN(from, mesh->normalSums, to, mesh->normalSums, n, Move::Dirs);
    __m128 lo = _mm_loadu_ps(&v[0].x), hi = lo;   // n - 1 steps (n == 1 would wrap the original's counter)
    uint32_t count = n - 1;
    const Vec4* p = v;
    do {
        const __m128 x = _mm_loadu_ps(&(++p)->x);
        lo = _mm_min_ps(lo, x);
        hi = _mm_max_ps(hi, x);
    } while (--count);
    _mm_storeu_ps(&mesh->boundsMin.x, lo);
    _mm_storeu_ps(&mesh->boundsMax.x, hi);
    num::Lerp4(mesh->boundsMin, mesh->boundsMax, 0.5f, mesh->centre);
    for (uint32_t k = 0; k < 3; ++k) (&mesh->halfExtent.x)[k] = (F32((&mesh->boundsMax.x)[k]) - F32((&mesh->boundsMin.x)[k])).f();
    (num::F32x4::Splat(F32(0.5f)) * num::F32x4::Load(&mesh->halfExtent.x)).Store(&mesh->halfExtent.x);
    return mesh;
}

bool MeshWorldBounds(const CollisionMesh& mesh, const Frame& entity, const Frame& world, Vec4* lo, Vec4* hi)
{
    const uint32_t n = mesh.vertexCount;
    if (!n) return false;
    Vec4 v[256];   // the original's 0x1000-byte stack buffer
    FrameMoveCopyN(entity, mesh.vertices, world, v, n, Move::Points);
    num::Vec4Copy(*lo, v[0]);
    num::Vec4Copy(*hi, v[0]);
    for (uint32_t i = 1; i < mesh.vertexCount; ++i) {
        num::Min4(*lo, v[i]);
        num::Max4(*hi, v[i]);
    }
    return true;
}

} // namespace phys::native
