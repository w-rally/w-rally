#include "native/world.h"

#include <xmmintrin.h>

#include <algorithm>
#include <cstring>
#include <cstring>

#include "num/f32.h"
#include "num/bits.h"
#include "num/collide.h"
#include "num/x87.h"
#include "native/flags.h"
#include "native/records.h"

namespace phys::native {

using num::F32;
using num::X87;
using num::F32x4;
namespace jmp = num::jmp;

namespace {
float FromBitsF(uint32_t b)
{
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
int32_t Cvtt(float f) { return _mm_cvtt_ss2si(_mm_set_ss(f)); }
float Sub(float a, float b) { return _mm_cvtss_f32(_mm_sub_ss(_mm_set_ss(a), _mm_set_ss(b))); }
int32_t Clamp(int32_t i, int32_t count)
{
    if (i < 0) return 0;
    const int32_t n = count * 100;
    return i >= n ? n - 1 : i;
}
bool BadMesh(const Entity& e) { return e.Mesh() && e.Mesh()->problems; }   // its build found problems (+0x67c)

// The damping box's contacts (CL2 / damping objects; the classic core/collide.cpp's, the game's 0x4f3030 and 0x4f2ce0):
// the oriented box against a shape in the world, the contact `idx` filled when they overlap.
// BoxBoxContact: against a box (centre, half extents, unit axes) - the separating-axis test, all x87 with the original's
// stored temporaries: B's three axes, the box's three, then (unless some |a_i . b_j| > 0.999999) the nine edge axes.
// The contact: the depth 1 - min(|B - A| / |A ext + B ext|, 1) (0.5 for no extents), the point A - d * depth, the
// volume ex ez ey 4 depth, the normal from B's centre to the point (from A's when its length is off).
// BoxSphereContact: against a sphere (centre, radius) - separated unless A ext_i + r > |d . a_i| on every axis; wholly
// inside: depth 1 at the sphere's centre; else the largest of 1 - p_i / (ext_i + r), the point on the axis of the
// largest p; the volume 0, the normal from the sphere's centre (or (0, 0, 1)). The point's w lane is the original's
// stack: 0 here.
int32_t BoxBoxContact(ContactBox& Bx, const ShapeBox& S, int32_t idx)
{
    auto put = [](float& to, float v) { to = v; };
    auto x = [](float v) { return X87(v); };
    auto sep = [](X87 lhs, X87 rhs) { return jmp::A(lhs.d, rhs.d); };   // fcompi lhs, rhs; ja: separated
    const float eps = num::FromBits(0x3f7fffef);                             // 0.999999
    bool parallel = false;
    auto near1 = [&](float r) {
        if (jmp::A(r, eps)) parallel = true;
    };
    // d = A's centre - B's centre (SSE); R = A's axes . B's axes (x87, stored), stack slots by name.
    const float d0 = (F32(Bx.centre.x) - F32(S.centre.x)).f(), d1 = (F32(Bx.centre.y) - F32(S.centre.y)).f(),
                d2 = (F32(Bx.centre.z) - F32(S.centre.z)).f();
    const float r00 = ((X87(S.axes[0].z) * X87(Bx.axes[0].z) + X87(S.axes[0].y) * X87(Bx.axes[0].y)) + X87(S.axes[0].x) * X87(Bx.axes[0].x)).Store();
    const float r10 = ((X87(Bx.axes[1].z) * X87(S.axes[0].z) + X87(Bx.axes[1].y) * X87(S.axes[0].y)) + X87(S.axes[0].x) * X87(Bx.axes[1].x)).Store();
    const float r20 = ((X87(Bx.axes[2].z) * X87(S.axes[0].z) + X87(Bx.axes[2].y) * X87(S.axes[0].y)) + X87(Bx.axes[2].x) * X87(S.axes[0].x)).Store();
    const float q00 = Abs(x(r00)).Store(), q10 = Abs(x(r10)).Store(), q20 = Abs(x(r20)).Store();
    near1(q00), near1(q10), near1(q20);
    const float t0 = ((x(d2) * X87(S.axes[0].z) + x(d1) * X87(S.axes[0].y)) + x(d0) * X87(S.axes[0].x)).Store();
    float s58 = (x(q00) * X87(Bx.halfExtent.x)).Store();
    if (sep(Abs(x(t0)), ((x(q20) * X87(Bx.halfExtent.z) + x(q10) * X87(Bx.halfExtent.y)) + x(s58)) + X87(S.halfExtent.x))) return 0;

    const float r01 = ((X87(Bx.axes[0].z) * X87(S.axes[1].z) + X87(Bx.axes[0].y) * X87(S.axes[1].y)) + X87(Bx.axes[0].x) * X87(S.axes[1].x)).Store();
    const float r11 = ((X87(Bx.axes[1].z) * X87(S.axes[1].z) + X87(Bx.axes[1].y) * X87(S.axes[1].y)) + X87(Bx.axes[1].x) * X87(S.axes[1].x)).Store();
    const float r21 = ((X87(Bx.axes[2].z) * X87(S.axes[1].z) + X87(Bx.axes[2].y) * X87(S.axes[1].y)) + X87(Bx.axes[2].x) * X87(S.axes[1].x)).Store();
    const float q01 = Abs(x(r01)).Store(), q11 = Abs(x(r11)).Store(), q21 = Abs(x(r21)).Store();
    near1(q01), near1(q11), near1(q21);
    const float t1 = ((x(d2) * X87(S.axes[1].z) + x(d1) * X87(S.axes[1].y)) + x(d0) * X87(S.axes[1].x)).Store();
    float s5c = (x(q11) * X87(Bx.halfExtent.y)).Store();
    if (sep(Abs(x(t1)), ((x(q01) * X87(Bx.halfExtent.x) + x(q21) * X87(Bx.halfExtent.z)) + x(s5c)) + X87(S.halfExtent.y))) return 0;

    const float r02 = ((X87(Bx.axes[0].z) * X87(S.axes[2].z) + X87(Bx.axes[0].y) * X87(S.axes[2].y)) + X87(Bx.axes[0].x) * X87(S.axes[2].x)).Store();
    const float r12 = ((X87(Bx.axes[1].z) * X87(S.axes[2].z) + X87(Bx.axes[1].y) * X87(S.axes[2].y)) + X87(Bx.axes[1].x) * X87(S.axes[2].x)).Store();
    const float r22 = ((X87(Bx.axes[2].z) * X87(S.axes[2].z) + X87(Bx.axes[2].y) * X87(S.axes[2].y)) + X87(S.axes[2].x) * X87(Bx.axes[2].x)).Store();
    const float q02 = Abs(x(r02)).Store(), q12 = Abs(x(r12)).Store(), q22 = Abs(x(r22)).Store();
    near1(q02), near1(q12), near1(q22);
    const float t2 = ((x(d2) * X87(S.axes[2].z) + x(d1) * X87(S.axes[2].y)) + x(d0) * X87(S.axes[2].x)).Store();
    const float sa4 = (x(q22) * X87(Bx.halfExtent.z)).Store();
    if (sep(Abs(x(t2)), ((x(q02) * X87(Bx.halfExtent.x) + x(q12) * X87(Bx.halfExtent.y)) + x(sa4)) + X87(S.halfExtent.z))) return 0;

    const float s18 = (x(q02) * X87(Bx.halfExtent.z)).Store(), sa8 = (x(q01) * X87(Bx.halfExtent.y)).Store();
    if (sep(Abs((x(d2) * X87(Bx.axes[0].z) + x(d1) * X87(Bx.axes[0].y)) + x(d0) * X87(Bx.axes[0].x)), ((x(sa8) + x(s18)) + x(s58)) + X87(S.halfExtent.x))) return 0;
    const float s7c = (x(q12) * X87(Bx.halfExtent.z)).Store();
    s58 = (x(q10) * X87(Bx.halfExtent.x)).Store();
    if (sep(Abs((x(d2) * X87(Bx.axes[1].z) + x(d1) * X87(Bx.axes[1].y)) + x(d0) * X87(Bx.axes[1].x)), ((x(s58) + x(s7c)) + x(s5c)) + X87(S.halfExtent.y))) return 0;
    const float sac = (x(q21) * X87(Bx.halfExtent.y)).Store();
    s5c = (x(q20) * X87(Bx.halfExtent.x)).Store();
    if (sep(Abs((x(d2) * X87(Bx.axes[2].z) + x(d1) * X87(Bx.axes[2].y)) + x(d0) * X87(Bx.axes[2].x)), ((x(s5c) + x(sac)) + x(sa4)) + X87(S.halfExtent.z))) return 0;

    if (!parallel) {   // the nine edge-edge axes
        if (sep(Abs(x(t2) * x(r01) - x(r02) * x(t1)), (x(q01) * X87(S.halfExtent.z) + x(q02) * X87(S.halfExtent.y)) + (x(q20) * X87(Bx.halfExtent.y) + x(q10) * X87(Bx.halfExtent.z)))) return 0;
        if (sep(Abs(x(t2) * x(r11) - x(r12) * x(t1)), (x(q11) * X87(S.halfExtent.z) + x(q12) * X87(S.halfExtent.y)) + (x(q00) * X87(Bx.halfExtent.z) + x(s5c)))) return 0;
        if (sep(Abs(x(t2) * x(r21) - x(r22) * x(t1)), (x(q21) * X87(S.halfExtent.z) + x(q22) * X87(S.halfExtent.y)) + (x(q00) * X87(Bx.halfExtent.y) + x(s58)))) return 0;
        if (sep(Abs(x(r02) * x(t0) - x(t2) * x(r00)), (x(q02) * X87(S.halfExtent.x) + x(q00) * X87(S.halfExtent.z)) + (x(q11) * X87(Bx.halfExtent.z) + x(sac)))) return 0;
        if (sep(Abs(x(r12) * x(t0) - x(t2) * x(r10)), (x(q12) * X87(S.halfExtent.x) + x(q10) * X87(S.halfExtent.z)) + (x(q21) * X87(Bx.halfExtent.x) + x(q01) * X87(Bx.halfExtent.z)))) return 0;
        if (sep(Abs(x(r22) * x(t0) - x(t2) * x(r20)), (x(q20) * X87(S.halfExtent.z) + x(q22) * X87(S.halfExtent.x)) + (x(q11) * X87(Bx.halfExtent.x) + x(sa8)))) return 0;
        if (sep(Abs(x(t1) * x(r00) - x(r01) * x(t0)), (x(q01) * X87(S.halfExtent.x) + x(q00) * X87(S.halfExtent.y)) + (x(q22) * X87(Bx.halfExtent.y) + x(s7c)))) return 0;
        if (sep(Abs(x(t1) * x(r10) - x(r11) * x(t0)), (x(q11) * X87(S.halfExtent.x) + x(q10) * X87(S.halfExtent.y)) + (x(q22) * X87(Bx.halfExtent.x) + x(s18)))) return 0;
        if (sep(Abs(x(t1) * x(r20) - x(r21) * x(t0)), (x(q21) * X87(S.halfExtent.x) + x(q20) * X87(S.halfExtent.y)) + (x(q12) * X87(Bx.halfExtent.x) + x(q02) * X87(Bx.halfExtent.y)))) return 0;
    }

    // Overlap: the box's contact *idx (the point, the normal, B's centre, the depth and the volume).
    Vec4 sum = {};   // w: stack garbage in the original, unused
    sum.x = (F32(Bx.halfExtent.x) + F32(S.halfExtent.x)).f();
    sum.y = (F32(Bx.halfExtent.y) + F32(S.halfExtent.y)).f();
    sum.z = (F32(Bx.halfExtent.z) + F32(S.halfExtent.z)).f();
    const float ls = num::Length3(sum);
    Vec4 dv;
    num::Sub3W(S.centre, Bx.centre, dv);
    const float lv = num::Length3(dv);
    BoxContact& C = Bx.contacts[idx];
    if (jmp::A(ls, 0.0f)) {   // fcompi ls, 0; jbe
        F32 v = F32(lv) / F32(ls);
        if (jmp::A(v.f(), 1.0f)) v = F32(1.0f);
        put(C.depth, (F32(1.0f) - v).f());
    } else {
        put(C.depth, 0.5f);
    }
    num::Vec4Copy(C.otherCentre, S.centre);
    const Vec4 dvec = {d0, d1, d2, 0.0f};   // w: stack garbage in the original, unused
    Vec4 sd;
    num::Scale4To(dvec, sd, C.depth);
    put(C.point.x, (F32(Bx.centre.x) - F32(sd.x)).f());
    put(C.point.y, (F32(Bx.centre.y) - F32(sd.y)).f());
    put(C.point.z, (F32(Bx.centre.z) - F32(sd.z)).f());
    put(C.volume, (((F32(Bx.halfExtent.x) * F32(Bx.halfExtent.z)) * F32(Bx.halfExtent.y)) * F32(4.0f) * F32(C.depth)).f());
    C.normal.x = (F32(C.point.x) - F32(S.centre.x)).f();   // from B's centre
    C.normal.y = (F32(C.point.y) - F32(S.centre.y)).f();
    C.normal.z = (F32(C.point.z) - F32(S.centre.z)).f();
    num::Normalize3Approx4(C.normal);
    const F32 n0 = C.normal.x, n1 = C.normal.y, n2 = C.normal.z;
    const F32 m2 = (n2 * n2 + n1 * n1) + n0 * n0;
    if (jmp::A(num::FromBits(0x3f733333), m2.f()) || jmp::A(m2.f(), num::FromBits(0x3f8ccccd))) {   // 0.95, 1.1
        C.normal.x = (F32(Bx.centre.x) - F32(S.centre.x)).f();   // from A's centre instead
        C.normal.y = (F32(Bx.centre.y) - F32(S.centre.y)).f();
        C.normal.z = (F32(Bx.centre.z) - F32(S.centre.z)).f();
        num::Normalize3Approx4(C.normal);
    }
    return 1;
}

int32_t BoxSphereContact(ContactBox& Bx, const ShapeSphere& S, int32_t idx)
{
    auto put = [](float& to, float v) { to = v; };
    // d = B - A's centre (x87, stored); its |projections| on A's axes.
    alignas(16) float d[4] = {(X87(S.centre.x) - X87(Bx.centre.x)).Store(), (X87(S.centre.y) - X87(Bx.centre.y)).Store(), 0.0f, 0.0f};   // d.w: stack garbage
    const X87 dz = X87(S.centre.z) - X87(Bx.centre.z);
    d[2] = dz.Store();   // fst: the chain continues with the unstored value
    const float p0 = Abs((dz * X87(Bx.axes[0].z) + X87(d[1]) * X87(Bx.axes[0].y)) + X87(d[0]) * X87(Bx.axes[0].x)).Store();
    const float p1 = Abs((X87(d[2]) * X87(Bx.axes[1].z) + X87(d[1]) * X87(Bx.axes[1].y)) + X87(d[0]) * X87(Bx.axes[1].x)).Store();
    const float p2 = Abs((X87(d[2]) * X87(Bx.axes[2].z) + X87(d[1]) * X87(Bx.axes[2].y)) + X87(d[0]) * X87(Bx.axes[2].x)).Store();
    const F32 r = S.radius;
    if (jmp::BE((X87(Bx.halfExtent.x) + X87(r.f())).d, X87(p0).d)) return 0;   // fcompi; jbe
    if (jmp::BE((F32(Bx.halfExtent.y) + r).f(), p1)) return 0;
    if (jmp::BE((r + F32(Bx.halfExtent.z)).f(), p2)) return 0;
    BoxContact& C = Bx.contacts[idx];
    num::Vec4Copy(C.otherCentre, S.centre);
    const float p[3] = {p0, p1, p2};
    if (jmp::A((F32(Bx.halfExtent.x) - F32(p0)).f(), r.f()) && jmp::A((F32(Bx.halfExtent.y) - F32(p1)).f(), r.f()) &&
        jmp::A((F32(Bx.halfExtent.z) - F32(p2)).f(), r.f())) {   // the sphere wholly inside
        put(C.depth, 1.0f);
        num::Vec4Copy(C.point, S.centre);
    } else {
        uint32_t k = jmp::A(p0, p1) ? 0 : 1;
        if (jmp::A(p2, p[k])) k = 2;
        const F32 one = 1.0f;
        const F32 f0 = one - F32(p0) / (F32(Bx.halfExtent.x) + r);
        const F32 f1 = one - F32(p1) / (F32(Bx.halfExtent.y) + r);
        const F32 f2 = one - F32(p2) / (r + F32(Bx.halfExtent.z));
        put(C.depth, (jmp::A(f0.f(), f1.f()) ? f0 : f1).f());
        put(C.depth, jmp::A(f2.f(), C.depth) ? f2.f() : C.depth);
        num::Normalize3Approx4(*reinterpret_cast<Vec4*>(d));
        (F32x4::Load(d) * F32x4::Splat(F32((&Bx.halfExtent.x)[k])) + F32x4::Load(&Bx.centre.x)).Store(&C.point.x);
    }
    put(C.volume, 0.0f);
    C.normal.x = (F32(C.point.x) - F32(S.centre.x)).f();   // from the sphere's centre
    C.normal.y = (F32(C.point.y) - F32(S.centre.y)).f();
    C.normal.z = (F32(C.point.z) - F32(S.centre.z)).f();
    const F32 nx = C.normal.x, ny = C.normal.y, nz = C.normal.z;
    const F32 m2 = (nz * nz + ny * ny) + nx * nx;
    if (jmp::A(m2.f(), num::FromBits(0x3f666666))) {   // 0.9
        const float inv = (X87(1.0f) / Sqrt(X87(m2.f()))).Store();   // fsqrt; fdivr
        (F32x4::Splat(F32(inv)) * F32x4::Load(&C.normal.x)).Store(&C.normal.x);
    } else {
        put(C.normal.x, 0.0f);
        put(C.normal.y, 0.0f);
        put(C.normal.z, 1.0f);
    }
    return 1;
}

} // namespace

void ObjectGrid::Create(float x0, float y0, float w, float h)
{
    originX = x0, originY = y0;
    columns = (Cvtt(w) + 99) / 100;
    rows = (Cvtt(h) + 99) / 100;
    blocks.clear();
    blocks.resize(static_cast<size_t>(rows) * static_cast<size_t>(columns));
}

ObjectGrid::Cell* ObjectGrid::CellAt(float x, float y)
{
    const int32_t ix = Clamp(Cvtt(Sub(x, originX)), columns);
    const int32_t iy = Clamp(Cvtt(Sub(y, originY)), rows);
    const int32_t bx = ix / 100, by = iy / 100;
    auto& block = blocks[static_cast<size_t>(columns * by + bx)];
    if (!block) block = std::make_unique<std::array<Cell, 100>>();
    return &(*block)[static_cast<size_t>((ix - bx * 100) / 10 + ((iy - by * 100) / 10) * 10)];
}

void ObjectGrid::CellsAround(bool spread, const float* a, const float* b, Cell* out[4])
{
    const float mx = ((F32(a[0]) + F32(b[0])) * F32(0.5f)).f();
    const float my = ((F32(a[1]) + F32(b[1])) * F32(0.5f)).f();
    if (spread) {
        const float y0 = (X87(my) - X87(4.7f)).Store();   // fld; fsub dword [0x73f7a8]; fstp
        const float x0 = (X87(mx) - X87(4.7f)).Store();
        out[0] = CellAt(x0, y0);
        const float y1 = (X87(my) + X87(4.7f)).Store();
        out[1] = CellAt(x0, y1);
        const float x1 = (X87(mx) + X87(4.7f)).Store();
        out[2] = CellAt(x1, y0);
        out[3] = CellAt(x1, y1);
    } else {
        out[0] = CellAt(a[0], a[1]);
        out[1] = CellAt(a[0], b[1]);
        out[2] = CellAt(b[0], a[1]);
        out[3] = CellAt(b[0], b[1]);
    }
    if (out[3] == out[0] || out[3] == out[1] || out[3] == out[2]) out[3] = nullptr;
    if (out[2] == out[0] || out[2] == out[1]) out[2] = nullptr;
    if (out[1] == out[0]) out[1] = nullptr;
}

void FenceDummy::WorldBox(Vec4& lo, Vec4& hi) const
{
    // DummyBounds: the box's x, y, z from its (x, z, y) centre and half size (w not written).
    lo.x = (F32(centre[0]) - F32(halfSize[0])).f(), lo.y = (F32(centre[2]) - F32(halfSize[2])).f(), lo.z = (F32(centre[1]) - F32(halfSize[1])).f();
    hi.x = (F32(halfSize[0]) + F32(centre[0])).f(), hi.y = (F32(halfSize[2]) + F32(centre[2])).f(), hi.z = (F32(halfSize[1]) + F32(centre[1])).f();
}

bool FenceDummy::BoxTest(const FramePoint& fa, const FramePoint& fb)
{
    const Frame& world = WorldOf(frame);
    Vec4 p, q;
    FramePointMove(fa, p, world);
    FramePointMove(fb, q, world);
    Vec4 d{(F32(q.x) - F32(p.x)).f(), (F32(q.y) - F32(p.y)).f(), (F32(q.z) - F32(p.z)).f(), q.w};
    (num::F32x4::Splat(F32(0.5f)) * num::F32x4::Load(&d.x)).Store(&d.x);   // the half gap
    const F32 cx = F32(p.x) + F32(d.x), cy = F32(p.y) + F32(d.y), cz = F32(p.z) + F32(d.z);   // the midpoint
    const float ex = (F32(centre[0]) - cx).f(), ey = (F32(centre[2]) - cy).f(), ez = (F32(centre[1]) - cz).f();
    auto inside = [](float half, float ext, float e) { return jmp::A((X87(half) + X87(ext)).d, e < 0.0f ? -e : e); };   // fabs on the x87
    if (inside(d.x, halfSize[0], ex) && inside(d.y, halfSize[2], ey) && inside(d.z, halfSize[1], ez) && react) react(*this);
    return false;   // the spectators only react
}

bool CanCollide(const Entity& a, const Entity& b)
{
    if (a.parent && !CanCollide(*a.parent, b)) return false;
    if (!a.hasExclusions || !b.hasExclusions) return true;
    if (&a == &b) return false;
    const std::vector<Entity*>& list = a.exclusions.size() < b.exclusions.size() ? a.exclusions : b.exclusions;   // the shorter (b's on a tie)
    const Entity* const other = &list == &a.exclusions ? &b : &a;
    for (const Entity* e : list)
        if (e == other) return false;
    return true;
}

bool PairCollide(Entity& a, const MeshInFrame& recA, const MeshInFrame& recB, Entity& b, EventList& list, uint32_t physicsFlags)
{
    if (physicsFlags & kFaceColl)
        if (a.Sweep(recA, recB, b, list)) return true;
    if (!(physicsFlags & kEdgeColl) || !b.EdgeCapable()) return false;
    return a.EdgeSweep(recA, recB, b, list);
}

namespace {
// An awake child and its awake children, depth first (ChildVisit, 0x51a1f0).
template <class Visit> void ChildVisit(Entity& e, Visit visit)
{
    for (Entity* ch : e.children) {
        if (!ch->Awake()) continue;
        visit(*ch);
        ChildVisit(*ch, visit);
    }
}
// A grid cell's entities but the wheels (CellVisit, 0x4e0080).
template <class Visit> void CellVisit(const ObjectGrid::Cell& cell, Visit visit)
{
    for (Entity* e : cell)
        if (e && !(e->flags & kWheel)) visit(*e);
}
bool AsleepUnderAwakeParent(const Entity& e) { return e.parent && !e.Awake() && e.parent->Awake(); }
} // namespace

void World::CollisionDetect(Entity& owner, EventList& list)
{
    // CL1CollisionDetector vslot 1: the owner's mesh where it was and is, in the world; its world box as two frame points.
    if (BadMesh(owner) || !owner.Detectable()) return;
    CollisionMesh was, is;
    MeshBuild(&was, owner.Mesh(), owner.olderFrame, *world);
    MeshBuild(&is, owner.Mesh(), owner.frame, *world);
    Vec4 lo, hi;
    owner.WorldBox(lo, hi);
    const FramePoint fpLo{&lo, world}, fpHi{&hi, world};
    // CL2CollisionDetector vslot 1 (0x5257e0) on each candidate.
    auto l2 = [&](Entity& e) {
        if (&e == &owner || !e.Collidable() || !CanCollide(e, owner) || AsleepUnderAwakeParent(e) || BadMesh(e)) return;
        if (!e.BoxTest(fpLo, fpHi)) return;
        CollisionMesh a, b;   // the owner's meshes into the entity's frames, to the pair (owner, entity)
        MeshBuild(&a, &was, *world, e.olderFrame);
        MeshBuild(&b, &is, *world, e.frame);
        PairCollide(owner, {&a, &e.olderFrame}, {&b, &e.frame}, e, list, physicsFlags);
        if (!e.Mesh()) return;
        CollisionMesh m13, m12;   // the entity's own mesh into the owner's frames, the other way round
        MeshBuild(&m13, e.Mesh(), e.olderFrame, owner.olderFrame);
        MeshBuild(&m12, e.Mesh(), e.frame, owner.frame);
        PairCollide(e, {&m13, &owner.olderFrame}, {&m12, &owner.frame}, owner, list, physicsFlags);
    };
    ObjectGrid::Cell* cells[4];
    grid.CellsAround((physicsFlags & 0x2000) != 0, &lo.x, &hi.x, cells);
    for (ObjectGrid::Cell* c : cells)
        if (c) CellVisit(*c, l2);
    for (Entity* o : objects) {   // the world's static objects but the brake wall (WorldObjectsVisit, 0x526390)
        if (o->flags & kBrakeWall) continue;
        l2(*o);
        ChildVisit(*o, l2);
    }
}

void World::PenetrationDetect(Entity& owner, EventList& list)
{
    // CL1PenetrationDetector vslot 1.
    if (BadMesh(owner) || !owner.Detectable()) return;
    Vec4 lo, hi;
    owner.WorldBox(lo, hi);
    // CL2PenetrationDetector vslot 1 (0x524480) on each grid candidate: the boxes strictly overlapping, the owner's mesh in
    // the entity's frame offered to the owner; declined, the entity's in the owner's offered to the entity.
    auto l2 = [&](Entity& e) {
        if (&owner == &e || BadMesh(e) || !CanCollide(owner, e) || AsleepUnderAwakeParent(e) || !e.Collidable()) return;
        Vec4 elo, ehi, d;
        e.WorldBox(elo, ehi);
        num::Sub3W(lo, ehi, d);   // detector min - entity max
        if (!jmp::A(0.0f, d.x) || !jmp::A(0.0f, d.y) || !jmp::A(0.0f, d.z)) return;
        num::Sub3W(elo, hi, d);   // entity min - detector max
        if (!num::AllNegative3(&d.x)) return;
        CollisionMesh m;
        MeshBuild(&m, owner.Mesh(), owner.frame, e.frame);
        if (owner.Penetrate({&m, &e.frame}, e, list, physicsFlags)) return;
        CollisionMesh m2;
        MeshBuild(&m2, e.Mesh(), e.frame, owner.frame);
        e.Penetrate({&m2, &owner.frame}, owner, list, physicsFlags);
    };
    ObjectGrid::Cell* cells[4];
    grid.CellsAround((physicsFlags & 0x2000) != 0, &lo.x, &hi.x, cells);
    for (ObjectGrid::Cell* c : cells)
        if (c) CellVisit(*c, l2);
    // CL2GiganticPenetrationDetector vslot 1 (0x5242c0) on each of the world's static objects - and the owner's children,
    // once per object (the original's loop).
    auto gigantic = [&](Entity& e) {
        if (&owner == &e || !owner.Mesh() || BadMesh(e) || !CanCollide(owner, e) || !e.Collidable()) return;
        CollisionMesh was, is;
        MeshBuild(&was, owner.Mesh(), owner.olderFrame, e.olderFrame);
        MeshBuild(&is, owner.Mesh(), owner.frame, e.frame);
        Vec4 ulo, uhi;
        num::Bounds4Union(&was.boundsMin, &is.boundsMin, ulo, uhi);
        if (!e.BoxTest({&ulo, &e.frame}, {&uhi, &e.frame})) return;
        owner.Penetrate({&is, &e.frame}, e, list, physicsFlags);
    };
    for (Entity* o : objects) {
        gigantic(*o);
        ChildVisit(owner, gigantic);
    }
}
// ---- the entities' pose updates and resets

namespace {
constexpr float kBig = 999999.875f;
} // namespace

void Solid::Notify()
{
    body.UpdateRotation();   // (twice in the original: the same result)
    Body::Notify();
}

void Solid::ResetStep()
{
    // SolidScratchReset (0x529af0): the step's saved state and accelerations, the direction and the push group (the
    // accumulated force and torque stay).
    num::Vec4Zero(body.savedPosition);
    num::Vec4Zero(body.savedVelocity);
    num::Vec4Zero(body.stepAcceleration);
    num::Vec4Zero(body.savedOrientation);
    num::Vec4Zero(body.savedAngularVelocity);
    num::Vec4Zero(body.stepAngularAcceleration);
    body.backwards = false;
    pushGroup = nullptr;
}

// ---- the wheels' queries

bool Water::QuadHit(const float* o, const float* d, Vec4* out, float* t) const
{
    const float* const q = &quad[0].x;   // corners 0, 2, 1 and 0, 3, 2
    if (!num::SegmentTriangle(o, d, q, q + 8, q + 4, t) && !num::SegmentTriangle(o, d, q, q + 12, q + 8, t)) return false;
    if (jmp::BE(*t, 0.0f)) *t = 0.0f;
    *out = plane;
    return true;
}

bool World::MeshListBoxQuery(const float* centre, const float* ext, uint32_t excludeFlags, const Entity* exclude, MeshList& list)
{
    list.count = 0;
    Vec4 lo{}, hi{};
    lo.x = (F32(centre[0]) - F32(ext[0])).f(), lo.y = (F32(centre[1]) - F32(ext[1])).f(), lo.z = (F32(centre[2]) - F32(ext[2])).f();
    hi.x = (F32(centre[0]) + F32(ext[0])).f(), hi.y = (F32(centre[1]) + F32(ext[1])).f(), hi.z = (F32(ext[2]) + F32(centre[2])).f();
    ObjectGrid::Cell* cells[4];
    grid.CellsAround((physicsFlags & 0x2000) != 0, &lo.x, &hi.x, cells);
    const FramePoint fpLo{&lo, world}, fpHi{&hi, world};
    for (ObjectGrid::Cell* c : cells) {
        if (!c) continue;
        for (Entity* e : *c) {   // (every item: the wheels too)
            if (list.count >= 8) break;
            if ((e->flags & excludeFlags) || e == exclude) continue;
            const CollisionMesh* const mesh = e->Mesh();
            if (!mesh || !mesh->vertexCount) continue;
            if (!e->BoxTest(fpLo, fpHi)) continue;
            list.entities[list.count] = e;
            MeshBuild(&list.meshes[list.count], mesh, e->frame, *world);   // the mesh moved into the world
            ++list.count;
        }
    }
    return list.count != 0;
}

Entity* World::MeshListSegmentQuery(const float* p0, const float* p1, const MeshList& list, GroundHit& o, float& pointW)
{
    o.t = FromBitsF(0x3f8147ae);   // 1.01: beyond the segment
    o.blendFlag = 0;
    o.lightFlag = 0;
    Entity* id = nullptr;
    for (uint32_t i = 0; i < list.count; ++i) {
        Vec4 point, plane;
        float t;
        uint32_t material;
        if (!const_cast<CollisionMesh&>(list.meshes[i]).SegmentHit(p0, p1, &point, &t, &plane, &material)) continue;
        if (!jmp::A(o.t, t)) continue;
        id = list.entities[i];
        o.point = {point.x, point.y, point.z};
        pointW = point.w;   // (the classic record's word after the point)
        o.normal = {plane.x, plane.y, plane.z};
        o.plane = plane.w;
        o.light = 1.0f;
        o.blend = 0.0f;
        o.material[0] = material;
        o.material[1] = material;
        o.t = t;
    }
    return id;
}

bool World::GridQuadSegmentQuery(const float* a, const float* b, Vec4* out, float* t)
{
    const F32 r = 0.1f;
    const float lo[3] = {(F32(a[0]) - r).f(), (F32(a[1]) - r).f(), (F32(a[2]) - r).f()};
    const float hi[3] = {(F32(a[0]) + r).f(), (F32(a[1]) + r).f(), (F32(a[2]) + r).f()};
    const float d[3] = {(F32(b[0]) - F32(a[0])).f(), (F32(b[1]) - F32(a[1])).f(), (F32(b[2]) - F32(a[2])).f()};
    ObjectGrid::Cell* cells[4];
    grid.CellsAround((physicsFlags & 0x2000) != 0, lo, hi, cells);
    for (ObjectGrid::Cell* c : cells) {
        if (!c) continue;
        for (Entity* e : *c) {
            if (!e || !(e->flags & kOpenMesh)) continue;
            const Water* const w = dynamic_cast<const Water*>(e);
            if (!w) {
                Unreached("an open mesh without a quad in the wheels' quad query");
                return false;
            }
            if (w->QuadHit(a, d, out, t)) return true;
        }
    }
    return false;
}

// ---- a tick's slots

void Solid::PrepareForces(World& w)
{
    Entity::PrepareForces(w);
    // SolidGravity (vslot 24): the weight into the body's axes (the gravity over the inverse mass).
    const float m = body.def->inverseMass;
    if (m == 0.0f) return;
    Vec4 f;
    FrameMoveCopy(*w.world, w.gravity, frame, f, Move::Dirs);
    (F32x4::Splat(F32(1.0f) / F32(m)) * F32x4::Load(&f.x)).Store(&f.x);
    body.AccumulateForce(f);
}

void Solid::Forces(World& w)
{
    Entity::Forces(w);
    Vec4 F, T;
    if (ContactSum(*this, 1.0f, w.frictionScale, F, T)) {
        body.AccumulateForce(F);
        body.AccumulateTorque(T);
    }
    if (flags & kCar) DampingContacts(w);
    Vec4 a, b;   // the force over the mass and the torque through the inverse inertia, into the world
    num::Scale4(body.force, body.def->inverseMass, a);
    FrameMoveCopy(frame, a, *w.world, body.stepAcceleration, Move::Dirs);
    num::Mul4(body.torque, body.def->localInverseInertia, b);
    FrameMoveCopy(frame, b, *w.world, body.stepAngularAcceleration, Move::Dirs);
    num::Vec4Zero(body.force);
    num::Vec4Zero(body.torque);
}

bool World::BoxObjectContacts(const Entity& e, int32_t& count)
{
    count = 0;
    if (!e.Mesh()) return false;
    ContactBox& Bx = dampingBox;
    FrameMoveCopy(e.frame, Bx.localCentre, *world, Bx.centre, Move::Points);   // the box into the world: centre, then its axes
    for (uint32_t k = 0; k < 3; ++k) FrameMoveCopy(e.frame, Bx.localAxes[k], *world, Bx.axes[k], Move::Dirs);
    const F32 d = F32(0.1f);
    float lo[4], hi[4];
    for (uint32_t k = 0; k < 3; ++k) {
        const float x = (&Bx.centre.x)[k];
        lo[k] = (F32(x) - d).f();
        hi[k] = (F32(x) + d).f();
    }
    lo[3] = hi[3] = 0.0f;
    ObjectGrid::Cell* cells[4];
    grid.CellsAround((physicsFlags & 0x2000) != 0, lo, hi, cells);
    for (uint32_t i = 0; i < 4 && count < 8; ++i) {
        if (!cells[i]) continue;
        for (const Entity* o : *cells[i]) {
            if (count >= 8) break;
            const CollisionMesh* const mesh = o->Mesh();
            if (!mesh || !mesh->shape) continue;
            int32_t hit;
            if (mesh->flags & kMeshSphere) {   // a sphere
                const ShapeSphere& shape = *static_cast<const ShapeSphere*>(mesh->shape);
                alignas(16) ShapeSphere sph{};
                FrameMoveCopy(o->frame, shape.centre, *world, sph.centre, Move::Points);
                sph.radius = shape.radius;
                hit = BoxSphereContact(Bx, sph, count);
            } else {   // a box: {centre, half extents, unit axes}
                const ShapeBox& shape = *static_cast<const ShapeBox*>(mesh->shape);
                alignas(16) ShapeBox B{};   // in the world: the half extents the axes' lengths, the axes unit
                FrameMoveCopy(o->frame, shape.centre, *world, B.centre, Move::Points);
                for (uint32_t k = 0; k < 3; ++k) {
                    Vec4 a;
                    num::Scale4To(shape.axes[k], a, (&shape.halfExtent.x)[k]);   // the axes' scales
                    FrameMoveCopy(o->frame, a, *world, B.axes[k], Move::Dirs);
                }
                const float len[3] = {num::Length3(B.axes[0]), num::Length3(B.axes[1]), num::Length3(B.axes[2])};
                B.halfExtent.x = len[0], B.halfExtent.y = len[1], B.halfExtent.z = len[2];
                for (uint32_t k = 0; k < 3; ++k) {
                    const float inv = (X87(1.0f) / X87(len[k])).Store();
                    (F32x4::Splat(F32(inv)) * F32x4::Load(&B.axes[k].x)).Store(&B.axes[k].x);
                }
                hit = BoxBoxContact(Bx, B, count);
            }
            if (!hit) continue;
            Bx.contacts[count].material = mesh->material;
            ++count;
        }
    }
    return count != 0;
}

void Solid::DampingContacts(World& w)
{
    int32_t count = 0;
    if (!w.BoxObjectContacts(*this, count)) return;
    const BoxContact* const contacts = w.dampingBox.contacts;
    uint32_t sel = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i) {
        if (jmp::A(contacts[i].depth, 0.0f)) sel = i;   // the last one with depth > 0
        if (contacts[i].material == 200 && (flags & kCar) && w.retireCar) w.retireCar(*this);
    }
    World::DampingCollision ev;
    for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i) {
        const BoxContact& rec = contacts[i];
        const Vec4& p = rec.point;
        const Vec4& o = body.position;
        const Vec4 d{(F32(p.x) - F32(o.x)).f(), (F32(p.y) - F32(o.y)).f(), (F32(p.z) - F32(o.z)).f(), 0.0f};   // w: stack
        Vec4 v;
        VelocityAt(p, v);
        float len = num::Length3(v);
        const F32 x = F32(rec.volume) * F32(0.5f);
        F32 k = F32(0.0f);   // clamp(x, 0, 1); NaN passes
        if (!jmp::A(0.0f, x.f())) k = jmp::A(x.f(), 1.0f) ? F32(1.0f) : x;
        Vec4 F;
        num::Scale4(rec.normal, (k * F32(5000.0f)).f(), F);   // the push out along the normal
        if (jmp::A(len, 1e-4f)) {   // damping against the velocity: u = v / |v|, F += u * (min(5 depth, 1) min(|v|, 10) -50)
            Vec4 u;
            num::Scale4(v, (F32(1.0f) / F32(len)).f(), u);
            if (jmp::A(len, 10.0f)) len = 10.0f;
            F32 f = F32(rec.depth) * F32(5.0f);
            if (jmp::A(f.f(), 1.0f)) f = F32(1.0f);
            const F32 s = (f * F32(len)) * F32(-50.0f);
            (F32x4::Load(&u.x) * F32x4::Splat(s) + F32x4::Load(&F.x)).Store(&F.x);
        }
        Vec4 T{};
        num::Cross3(d, F, T);
        Vec4 Fl, Tl;
        FrameMoveCopy(*w.world, F, frame, Fl, Move::Dirs);
        FrameMoveCopy(*w.world, T, frame, Tl, Move::Dirs);
        body.AccumulateForce(Fl);
        body.AccumulateTorque(Tl);
        if (i == sel) ev = {rec.depth, rec.material, rec.otherCentre, p, v};
    }
    if (w.dampingCollision) w.dampingCollision(*this, ev);
}

void Solid::Step(World& w, float dt)
{
    body.Step(dt, (flags & kCar) != 0);
    if (!parent && (jmp::A(dt, 0.0f) || jmp::A(0.0f, dt))) w.GridRefile(*this);   // (SolidIntegrate: moved, re-filed)
    if ((flags & kCar) && !parent && (jmp::A(dt, 0.0f) || jmp::A(0.0f, dt)))   // (SolidIntegrate: the car's parameter)
        DriveLineStep(w);
    Entity::Step(w, dt);   // BodyVslot27: the pose's dependants, the children stepped
    if (body.SleepCheck(dt)) w.Sleep(*this);
    if (w.stepped) w.stepped(*this);
}

void World::Phases(float dt)
{
    for (size_t i = 0; i < awake.size(); ++i) awake[i]->PrepareForces(*this);
    for (size_t i = 0; i < awake.size(); ++i) {
        Entity* const e = awake[i];
        e->Forces(*this);
        if (i >= awake.size() || awake[i] != e) break;   // filed elsewhere (a retired car held): the walk ends there too
    }
    for (size_t i = 0; i < awake.size(); ++i) {
        Entity* const e = awake[i];
        e->Step(*this, dt);
        e->StepChildren(*this, dt);
        if (i >= awake.size() || awake[i] != e) break;   // filed elsewhere (asleep): the original's walk ends there
    }
}

void World::Sleep(Solid& s)
{
    s.awake = s.held = false;
    s.contactGroup = nullptr;
    s.body.inContactGroup = false;
    if (!s.parent) Refile(s);   // (BodyRefile: a child stays where its parent is)
    if (slept) slept(s);
}

void World::Hold(Solid& s)
{
    if (!s.parent) s.awake = false, s.held = true;
    s.contactGroup = nullptr;
    s.body.inContactGroup = false;
    Refile(s);
    if (slept) slept(s);
}

// ---- the world's lists and the grid

void World::File(Entity& e)
{
    if (const Solid* s = e.AsSolid(); s && s->held) held.push_back(&e);
    else if (e.Awake()) awake.push_back(&e);
    else (e.Static() ? objects : resting).push_back(&e);
}

void World::Refile(Entity& e)
{
    for (std::vector<Entity*>* l : {&awake, &held, &objects, &resting}) {
        const auto it = std::find(l->begin(), l->end(), &e);
        if (it == l->end()) continue;
        l->erase(it);
        File(e);
        return;
    }
}

void World::GridRefile(Entity& e)
{
    const Solid* const s = e.AsSolid();
    if (!s) return;
    if (e.cell)
        if (const auto it = std::find(e.cell->begin(), e.cell->end(), &e); it != e.cell->end()) e.cell->erase(it);
    e.cell = grid.CellAt(s->body.position.x, s->body.position.y);
    e.cell->insert(e.cell->begin(), &e);
}

void World::BlendState(Solid& s, float f)
{
    if (!s.body.BlendState(f)) return;
    if (!s.parent) GridRefile(s);
    s.Notify();
}

void World::AdvanceRest(Solid& s, float dt)
{
    if (!s.body.AdvanceRest(dt)) return;
    if (!s.parent) GridRefile(s);
    s.Notify();
}

void World::Wake(Solid& s)
{
    if (!s.parent) Refile(s);   // (BodyRefile: a child stays where its parent is)
    if (woken) woken(s);
}

// ---- the car's parts

bool World::ReleasePart(Solid& h, Solid*& carSlot, bool renormalise)
{
    if (!h.parent || std::find(h.parent->children.begin(), h.parent->children.end(), &h) == h.parent->children.end()) return false;
    RigidBody& b = h.body;
    const Vec4 p0 = b.position, q0 = b.orientation;
    Solid& car = *carSlot;
    // The pose from the car's frame into the world's (0x51da60 / 0x4e6950; a stick's orientation renormalised).
    Vec4 p, q;
    FrameMoveCopy(car.frame, b.position, *world, p, Move::Points);
    b.position = p;
    num::QuatMul(car.body.orientation, b.orientation, q);
    if (renormalise) num::Normalize4ApproxTo(q, b.orientation);
    else b.orientation = q;
    // PartFree: the step's start where it is, its velocities into the world's with the car's added.
    std::memcpy(&b.savedPosition, &b.position, 16);
    std::memcpy(&b.savedOrientation, &b.orientation, 16);
    Vec4 v, w;
    FrameMoveCopy(car.frame, b.velocity, *world, v, Move::Dirs);
    FrameMoveCopy(car.frame, b.angularVelocity, *world, w, Move::Dirs);
    b.velocity.x = (F32(car.body.velocity.x) + F32(v.x)).f();   // (the car's lane first; w kept)
    b.velocity.y = (F32(car.body.velocity.y) + F32(v.y)).f();
    b.velocity.z = (F32(car.body.velocity.z) + F32(v.z)).f();
    b.angularVelocity.x = (F32(car.body.angularVelocity.x) + F32(w.x)).f();
    b.angularVelocity.y = (F32(car.body.angularVelocity.y) + F32(w.y)).f();
    b.angularVelocity.z = (F32(car.body.angularVelocity.z) + F32(w.z)).f();
    h.ResetStep();   // vslot 21
    num::Vec4Zero(b.force);
    num::Vec4Zero(b.torque);
    auto& siblings = h.parent->children;   // out of the car's children
    siblings.erase(std::find(siblings.begin(), siblings.end(), &h));
    h.parent = nullptr;
    carSlot = nullptr;
    h.Wake();   // vslot 36: woken (not in a list yet: the re-filing finds none)
    Wake(h);
    h.frame.parent = h.olderFrame.parent = world;   // both frames under the world now
    h.frame.depth = h.olderFrame.depth = world->depth + 1;
    GridRefile(h);
    File(h);
    h.SaveState();   // vslot 18
    if (released) released(h);
    return true;
}

// ---- the stages

void World::CollisionBroadPhase()
{
    for (size_t i = 0; i < awake.size(); ++i) {
        CollisionDetect(*awake[i], events);
        ChildVisit(*awake[i], [&](Entity& e) { CollisionDetect(e, events); });
    }
}

void World::PenetrationBroadPhase()
{
    for (size_t i = 0; i < awake.size(); ++i) {
        PenetrationDetect(*awake[i], events);
        ChildVisit(*awake[i], [&](Entity& e) { PenetrationDetect(e, events); });
    }
}

float World::PruneEvents()
{
    float best = 2.0f;
    for (size_t i = 0; i < events.Size();) {
        const CollisionEvent& ev = *events[i];
        if (ev.flags & (kEventBodyPassThrough | kEventOtherPassThrough)) {
            events.Remove(i);
            continue;
        }
        if (jmp::A(best, ev.hit.time)) best = ev.hit.time;
        ++i;
    }
    return best;
}

void World::CollisionStage(float dt)
{
    ImpulseSolver solver;
    solver.materials = materials;
    solver.woken = [this](Solid& s) { Wake(s); };
    auto each = [&](auto&& f) {   // the awake bodies and both sides of each event that step
        for (size_t i = 0; i < events.Size(); ++i)
            for (Entity* e : {static_cast<Entity*>(events[i]->body), events[i]->hit.other})
                if (e->Awake() && (e->flags & kSolid)) f(*e->AsSolid(), *events[i]);
    };
    const float z = (F32(dt) * F32(0.0f)).f(), eps = (F32(dt) * F32(0.05f)).f();
    float rest = dt;
    uint32_t passes = 1;   // (a single pass: the original's counter starts at 1)
    if (jmp::A(rest, eps)) {
        while (passes) {
            --passes;
            CollisionBroadPhase();
            EventsFlag(events, *materials);
            const float m = PruneEvents();   // the earliest event's fraction of the step
            float next = 0.0f;
            if (!jmp::B(1.100000023841858f, m)) {   // 1.1 < m: no contact
                const F32 f = F32(m) - F32(z) / F32(rest);
                float s;
                if (jmp::A(0.0f, f.f())) s = 0.0f;
                else s = jmp::A(f.f(), 1.0f) ? 1.0f : f.f();
                each([&](Solid& b, const CollisionEvent& ev) { BlendState(b, ev.hit.time); });   // ContactsBlendStates
                solver.SolveAll(events);
                for (size_t i = 0; i < awake.size(); ++i) awake[i]->SaveState();
                each([&](Solid& b, const CollisionEvent& ev) {   // ContactsAdvanceRest: the partial steps
                    if (jmp::A(1.0f, ev.hit.time)) AdvanceRest(b, rest);
                });
                next = ((F32(1.0f) - F32(s)) * F32(rest)).f();
            }
            rest = next;
            events.Clear();
            if (!jmp::A(rest, eps)) break;
        }
    }
    for (size_t i = 0; i < awake.size(); ++i) awake[i]->ResetStep();
    for (size_t i = 0; i < awake.size(); ++i) awake[i]->SaveState();
}

void World::PenetrationStage()
{
    PenetrationBroadPhase();
    groups.Clear();
    if (events.Size()) {
        EventsFlag(events, *materials);
        groups.Depenetrate(events, *materials);
        ImpulseSolver solver;
        solver.materials = materials;
        solver.woken = [this](Solid& s) { Wake(s); };
        solver.SolveAll(events);
    }
    events.Clear();
}

void World::Step(float dt)
{
    if (!tick)
        for (Entity* e : awake) e->SaveState();   // (tick 0: the bodies' vslot 18)
    ++tick;
    for (Entity* e : awake) e->TickBegin(*this, dt);
    Phases(dt);
    // (vslot 9: nothing for every class)
    if (!((physicsFlags & kSubStepping) && (tick & 3))) Collide((F32(dt) * F32(4.0f)).f());
    for (size_t i = 0; i < held.size();) {   // (the next one taken first: a held body may leave the list)
        Entity* const e = held[i];
        e->HeldTick(*this, dt);
        if (i < held.size() && held[i] == e) ++i;
    }
    time = (X87(dt) + X87::Raw(time)).d;   // fadd qword under 24-bit precision control (X87's)
}

void World::Collide(float dt)
{
    if (physicsFlags & kCheckColl) CollisionStage(dt);
    if (physicsFlags & kCheckPenetr) PenetrationStage();
    events.Clear();
    for (size_t i = 0; i < awake.size(); ++i) awake[i]->ResetStep();
    for (size_t i = 0; i < awake.size(); ++i) awake[i]->SaveState();
}

} // namespace phys::native
