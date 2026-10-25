#include "collide.h"

#include "x87.h"

#include <cstring>
#include <initializer_list>

#include "bits.h"
#include "f32.h"

// x87 sequences are mirrored instruction by instruction: fld/fadd/fmul/faddp operand order,
// fstp dword where the original stores, and the jump the original takes after fcomi.

namespace phys::num {

int BoxOverlap(const AABBox& box, const Vec3& p, const Vec3& e)
{
    Vec3 d;
    Sub3(p, box.pos, d);
    // fld |d.x|; fld h.x; fadd e.x; fcomi st(1); jb fail   (st0 = h + e, st1 = |d|)
    if (jmp::B((X87(box.ext.x) + X87(e.x)).d, Abs(X87(d.x)).d)) return 0;
    if (jmp::B((X87(e.y) + X87(box.ext.y)).d, Abs(X87(d.y)).d)) return 0;
    if (jmp::B((X87(e.z) + X87(box.ext.z)).d, Abs(X87(d.z)).d)) return 0;
    return 1;
}

int SegmentBoxOverlap(const AABBox& box, const Vec3& m, const Vec3& h)
{
    Vec3 s;
    AddSub3(m, h, box.pos, s);
    // |h| is stored to float temporaries (fabs; fstp dword), then reloaded.
    const float ax = Abs(X87(h.x)).Store();
    if (jmp::A(Abs(X87(s.x)).d, (X87(ax) + X87(box.ext.x)).d)) return 0;
    const float ay = Abs(X87(h.y)).Store();
    if (jmp::A(Abs(X87(s.y)).d, (X87(ay) + X87(box.ext.y)).d)) return 0;
    const float az = Abs(X87(h.z)).Store();
    if (jmp::A(Abs(X87(s.z)).d, (X87(az) + X87(box.ext.z)).d)) return 0;

    Vec3 c;
    Cross3(h, s, c);
    // faddp st(1): the first product loaded is the left operand.
    if (jmp::A(Abs(X87(c.x)).d, (X87(az) * X87(box.ext.y) + X87(ay) * X87(box.ext.z)).d)) return 0;
    if (jmp::A(Abs(X87(c.y)).d, (X87(ax) * X87(box.ext.z) + X87(az) * X87(box.ext.x)).d)) return 0;
    if (jmp::BE(Abs(X87(c.z)).d, (X87(ax) * X87(box.ext.y) + X87(ay) * X87(box.ext.x)).d)) return 1;
    return 0;
}

int RayTriangle(const Vec3& o, const Vec3& dir, const Vec3& v0, const Vec3& v1, const Vec3& v2, RayHit& hit)
{
    const float kEps = 9.99999997e-7f;   // [0x795e70] 0x358637bd
    Vec3 e1, e2, pvec, tvec, qvec;
    Sub3(v1, v0, e1);
    Sub3(v2, v0, e2);
    const float det = CrossDot3(e2, dir, pvec, e1);
    if (jmp::A(X87(kEps).d, X87(det).d)) return 0;              // fld eps; fcomi st(1); ja

    const float u = SubDot3(o, v0, tvec, pvec);
    hit.u = u;                                                    // stored before the tests
    if (jmp::A(0.0, X87(u).d)) return 0;                          // fld 0; fcomi; ja
    if (jmp::A(u, det)) return 0;                                 // comiss u, det; ja

    const float v = CrossDot3(e1, tvec, qvec, dir);
    hit.v = v;
    if (jmp::A(0.0, X87(v).d)) return 0;
    if (jmp::A((F32(hit.u) + F32(v)).f(), det)) return 0;         // addss; comiss; ja

    const float inv = (F32(1.0f) / F32(det)).f();
    const float t = (X87(Dot3(e2, qvec)) * X87(inv)).Store();     // fmul dword; fstp dword
    const float su = (F32(hit.u) * F32(inv)).f();
    hit.t = t;
    hit.u = su;
    hit.v = (F32(hit.v) * F32(inv)).f();
    Cross3(e2, e1, hit.normal);
    return 1;
}

int PointTriangle(const Vec3& p, const Vec3& v0, const Vec3& v1, const Vec3& v2, RayHit& out)
{
    Vec3 e1, e2, w;
    Sub3(v1, v0, e1);
    Sub3(v2, v0, e2);
    Sub3(p, v0, w);
    const float s = CrossDot3(e2, e1, out.normal, w);
    if (jmp::A(s, 0.0f)) return 0;                      // fld 0; fld s; fcomip; ja
    const X87 nx = X87(out.normal.x), ny = X87(out.normal.y), nz = X87(out.normal.z);
    const float inv = (X87(1.0f) / Sqrt((nx * nx + ny * ny) + nz * nz)).Store();
    const float h = (F32(0.0f) - F32(inv) * F32(s)).f();
    if (jmp::A(1e-5f, h)) return 0;   // comiss 1e-5, h; ja
    if (jmp::A(h, 0.5f)) return 0;
    Scale3InPlace(out.normal, inv);
    Vec3 q, a, b;
    Scale3(out.normal, h, q);
    Cross3(q, e2, a);
    const float inv2 = (X87(1.0f) / X87(Dot3(e1, a))).Store();
    const float u = (X87(Dot3(w, a)) * X87(inv2)).Store();
    out.u = u;
    if (jmp::A(0.0f, u)) return 0;
    if (jmp::A(u, 1.0f)) return 0;
    Cross3(w, e1, b);
    const float v = (X87(Dot3(q, b)) * X87(inv2)).Store();
    out.v = v;
    if (jmp::A(0.0f, v)) return 0;
    if (jmp::A((F32(v) + F32(out.u)).f(), 1.0f)) return 0;
    out.t = 1.0f;
    Add3(p, q, *reinterpret_cast<Vec3*>(&out));
    return 1;
}

int SegmentTriangle(const float* o, const float* d, const float* v0, const float* v1, const float* v2, float* t)
{
    auto sub = [](const float* a, const float* b) {
        return Vec4{(F32(a[0]) - F32(b[0])).f(), (F32(a[1]) - F32(b[1])).f(), (F32(a[2]) - F32(b[2])).f(), 0.0f};
    };
    const Vec4 e1 = sub(v1, v0), e2 = sub(v2, v0);
    const Vec4 dv = {d[0], d[1], d[2], 0.0f};
    Vec4 p;
    Cross3(dv, e2, p);
    const F32 det = (F32(p.z) * F32(e1.z) + F32(p.y) * F32(e1.y)) + F32(e1.x) * F32(p.x);
    if (jmp::A(1e-6f, det.f())) return 0;   // 0x358637bd
    const Vec4 tv = sub(o, v0);
    const F32 u = (F32(tv.z) * F32(p.z) + F32(tv.y) * F32(p.y)) + F32(tv.x) * F32(p.x);
    if (jmp::A(0.0f, u.f()) || jmp::A(u.f(), det.f())) return 0;
    Vec4 q;
    Cross3(tv, e1, q);
    const F32 v = (F32(d[1]) * F32(q.y) + F32(d[2]) * F32(q.z)) + F32(d[0]) * F32(q.x);
    if (jmp::A(0.0f, v.f()) || jmp::A((v + u).f(), det.f())) return 0;
    *t = (((F32(q.z) * F32(e2.z) + F32(q.y) * F32(e2.y)) + F32(e2.x) * F32(q.x)) / det).f();
    return 1;
}


int SegmentTriangleHit(const float* o, const float* d, const float* v0, const float* v1, const float* v2, uint8_t* rec)
{
    struct Hit {   // {the normal (unnormalised; xyz written), -, t, u, v}
        Vec4 normal;
        float unknown10, t, u, v;
    };
    Hit& hit = *reinterpret_cast<Hit*>(rec);
    const F32 e1x = F32(v1[0]) - F32(v0[0]), e1y = F32(v1[1]) - F32(v0[1]), e1z = F32(v1[2]) - F32(v0[2]);
    const F32 e2x = F32(v2[0]) - F32(v0[0]), e2y = F32(v2[1]) - F32(v0[1]), e2z = F32(v2[2]) - F32(v0[2]);
    Vec4 dv{d[0], d[1], d[2], 0.0f}, e1{e1x.f(), e1y.f(), e1z.f(), 0.0f}, e2{e2x.f(), e2y.f(), e2z.f(), 0.0f}, P;
    Cross3(dv, e2, P);
    const F32 det = (F32(P.z) * e1z + F32(P.y) * e1y) + e1x * F32(P.x);
    if (jmp::A(FromBits(0x358637bd), det.f())) return 0;   // 1e-6 > det
    const Vec4 T{(F32(o[0]) - F32(v0[0])).f(), (F32(o[1]) - F32(v0[1])).f(), (F32(o[2]) - F32(v0[2])).f(), 0.0f};
    const float u = ((F32(T.z) * F32(P.z) + F32(T.y) * F32(P.y)) + F32(T.x) * F32(P.x)).f();
    hit.u = u;
    if (jmp::A(0.0f, u) || jmp::A(u, det.f())) return 0;
    Vec4 Q;
    Cross3(T, e1, Q);
    const float v = ((F32(d[1]) * F32(Q.y) + F32(d[2]) * F32(Q.z)) + F32(d[0]) * F32(Q.x)).f();
    hit.v = v;
    if (jmp::A(0.0f, v) || jmp::A((F32(v) + F32(hit.u)).f(), det.f())) return 0;
    const F32 inv = F32(1.0f) / det;
    hit.t = (((F32(Q.z) * e2z + F32(Q.y) * e2y) + e2x * F32(Q.x)) * inv).f();
    hit.u = (inv * F32(hit.u)).f();
    hit.v = (F32(hit.v) * inv).f();
    Vec4 n = hit.normal;
    Cross3(e1, e2, n);
    std::memcpy(&hit.normal, &n, 12);
    return 1;
}

X87 TripleProduct(const float* o, const float* a, const float* b, const float* c)
{
    const Vec4 A{(F32(a[0]) - F32(o[0])).f(), (F32(a[1]) - F32(o[1])).f(), (F32(a[2]) - F32(o[2])).f(), 0.0f};
    const Vec4 B{(F32(b[0]) - F32(o[0])).f(), (F32(b[1]) - F32(o[1])).f(), (F32(b[2]) - F32(o[2])).f(), 0.0f};
    const float cx = (X87(c[0]) - X87(o[0])).Store(), cy = (X87(c[1]) - X87(o[1])).Store(), cz = (X87(c[2]) - X87(o[2])).Store();
    Vec4 n;
    Cross3(A, B, n);
    return (X87(n.z) * X87(cz) + X87(n.y) * X87(cy)) + X87(n.x) * X87(cx);
}

bool SegmentBoxTest(const float* p0, const float* p1, const float* centre, const float* ext)
{
    Vec4 d{(F32(p1[0]) - F32(p0[0])).f(), (F32(p1[1]) - F32(p0[1])).f(), (F32(p1[2]) - F32(p0[2])).f(), 0.0f};
    (F32x4::Splat(F32(0.5f)) * F32x4::Load(&d.x)).Store(&d.x);   // mulps, the scalar first
    AABBox box;
    std::memcpy(&box.pos, centre, 12);
    std::memcpy(&box.ext, ext, 12);
    Vec3 m, h;
    std::memcpy(&m, p0, 12);
    std::memcpy(&h, &d, 12);
    return SegmentBoxOverlap(box, m, h) != 0;
}

} // namespace phys::num
