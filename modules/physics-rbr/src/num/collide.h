// The collision primitives (the 0x4fd2xx-0x4fd4xx family of the original): boxes, triangles, segments.
#pragma once
#include "math.h"
#include "x87.h"

namespace phys::num {

// SAABBox (original CollisionFormat.h): origin, then the axis-aligned extension from it (half
// extents), 24 bytes.
struct AABBox {
    Vec3 pos;
    Vec3 ext;
};

static_assert(sizeof(AABBox) == 24, "game layout");

// 0x4fd230  box overlap: d = p - box.pos; for each axis |d| <= box.ext + e (x87, 24-bit precision,
// wide exponent; a NaN passes). Returns 1/0 like the original.
int BoxOverlap(const AABBox& box, const Vec3& p, const Vec3& e);

// 0x4fd2c0  segment (midpoint m, half vector h) vs box, separating axes: s = (m + h) - box.pos,
// the three box axes, then the three axes of h x s (x87; a NaN passes every test).
int SegmentBoxOverlap(const AABBox& box, const Vec3& m, const Vec3& h);

// Result record of RayTriangle (and of TreeSegmentQuery, which adds tri and bank).
struct RayHit {
    float unknown0[3];
    Vec3 normal;        // +0x0c  e2 x e1 (not normalised)
    uint32_t tri;       // +0x18  TreeSegmentQuery: address of the triangle hit
    uint32_t bank;      // +0x1c  TreeSegmentQuery: its vertex bank
    float t;            // +0x20
    float u, v;         // +0x24, +0x28  barycentrics
};

static_assert(sizeof(RayHit) == 0x2c, "game layout");

// 0x4fd480  __stdcall, Moeller-Trumbore ray/triangle, single-sided:
//   e1 = v1 - v0, e2 = v2 - v0, pvec = e2 x dir, det = pvec.e1; reject if 1e-6 > det (x87);
//   u = (o - v0).pvec, STORED before its tests (0 > u, u > det); v = (e1 x tvec).dir, stored
//   likewise (0 > v, u + v > det); then t = (e2.qvec) * (1/det) (x87 multiply), u, v *= 1/det
//   (SSE), normal = e2 x e1. Returns 1 on hit; a rejection can leave u/v written.
int RayTriangle(const Vec3& o, const Vec3& dir, const Vec3& v0, const Vec3& v1, const Vec3& v2, RayHit& hit);

// 0x4f56c0  cdecl(o, d, v0, v1, v2, t) -> int: one-sided segment/triangle (Moller-Trumbore, SSE). e1 = v1 - v0,
// e2 = v2 - v0, P = Cross3(d, e2), det = (P.z e1.z + P.y e1.y) + e1.x P.x; 1e-6 > det: 0. T = o - v0,
// u = (T.z P.z + T.y P.y) + T.x P.x; 0 > u or u > det: 0. Q = Cross3(T, e1), v = (d.y Q.y + d.z Q.z) + d.x Q.x;
// 0 > v or v + u > det: 0. *t = ((Q.z e2.z + Q.y e2.y) + e2.x Q.x) / det, 1.
int SegmentTriangle(const float* o, const float* d, const float* v0, const float* v1, const float* v2, float* t);

// 0x4fe2d0  stdcall(p, v0, v1, v2, out): is p just below triangle v0 v1 v2? e1 = v1 - v0,
// e2 = v2 - v0, w = p - v0; out.normal = n = e2 x e1, s = n.w (CrossDot3); fails if s > 0.
// inv = 1 / sqrt((nx*nx + ny*ny) + nz*nz) (x87, stored), depth h = 0 - inv * s: fails unless
// 1e-5 <= h <= 0.5. Then n *= inv in place, q = n * h, a = q x e2, inv2 = 1 / e1.a (x87);
// u = (w.a) * inv2 -> out.u, fails if 0 > u or u > 1; v = (q.(w x e1)) * inv2 -> out.v, fails if
// 0 > v or u + v > 1; out.t = 1, out position (+0) = p + q. Returns 1 (0 on failure, with
// whatever was stored so far).
int PointTriangle(const Vec3& p, const Vec3& v0, const Vec3& v1, const Vec3& v2, RayHit& out);

// 0x4f5570  thiscall(o, a, b, c) -> st0: the triple product ((a - o) x (b - o)) . (c - o), six times the signed volume of
// the tetrahedron. a - o and b - o in SSE, c - o on the x87 (stored as floats), Cross3, then (n.z c.z + n.y c.y) + n.x c.x
// on the x87 (not rounded to a float).
X87 TripleProduct(const float* o, const float* a, const float* b, const float* c);

// 0x4e0e40  cdecl(p0, p1, centre, ext) -> al: does the segment p0 .. p1 overlap the box? h = (p1 - p0) * 0.5 (mulps), then
// SegmentBoxOverlap's arithmetic (0x4fd2c0) with p0 as m and the box as centre / half extents. The original sets al only.
bool SegmentBoxTest(const float* p0, const float* p1, const float* centre, const float* ext);

// 0x4f5a90  cdecl(o, d, v0, v1, v2, hit) -> int: SegmentTriangle (0x4f56c0) with a record: u at hit +0x18 and v at
// +0x1c stored before their tests; on a hit t = (e2.Q) * (1 / det) at +0x14, u / v scaled by 1 / det, and
// hit.xyz = Cross3(e1, e2) (w not written).
int SegmentTriangleHit(const float* o, const float* d, const float* v0, const float* v1, const float* v2, uint8_t* hit);

} // namespace phys::num
