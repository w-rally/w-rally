#include "native/rigid_body.h"

#include <cstddef>
#include <cstring>

#include "num/f32.h"
#include "num/x87.h"   // jmp:: branch helpers

namespace phys::native {

using num::F32;
using num::F32x4;
using num::X87;
namespace jmp = num::jmp;

// RotateRows4 reads the rotation's rows 4 wide, its last row's 4th lane the float after the matrix.
static_assert(offsetof(RigidBody, rotationTail) == offsetof(RigidBody, rotation) + sizeof(Mat33), "the rotation's tail follows it");

namespace {
// One point through a matrix's rows as the original loads them: 16 bytes from M, M + 3 and M + 6 (the three floats past
// the matrix 0), row * lane, (r0 x + r1 y) + r2 z per lane.
void RotateByRows(const float* m12, Vec4& v)
{
    const F32x4 r0 = F32x4::Load(m12), r1 = F32x4::Load(m12 + 3), r2 = F32x4::Load(m12 + 6);
    const F32x4 p = F32x4::Load(&v.x);
    ((r0 * F32x4::Splat(p.v[0]) + r1 * F32x4::Splat(p.v[1])) + r2 * F32x4::Splat(p.v[2])).Store(&v.x);
}
struct Mat33Padded {
    Mat33 m;
    float pad[3];
};
// v.xyz = M v: x = (m2 z + m1 y) + m0 x, ... (vectorFirst: the x row's products take the vector operand first).
void MulRows3(const float* m, F32 x, F32 y, F32 z, Vec4& v, bool vectorFirst = false)
{
    v.x = vectorFirst ? ((z * F32(m[2]) + y * F32(m[1])) + x * F32(m[0])).f() : ((F32(m[2]) * z + F32(m[1]) * y) + F32(m[0]) * x).f();
    v.y = ((F32(m[5]) * z + F32(m[4]) * y) + F32(m[3]) * x).f();
    v.z = ((F32(m[8]) * z + F32(m[7]) * y) + F32(m[6]) * x).f();
}
F32 Square3(const Vec4& v) { return (F32(v.z) * F32(v.z) + F32(v.y) * F32(v.y)) + F32(v.x) * F32(v.x); }
} // namespace

Vec4* RigidBody::Rotate3(Vec4* v, uint32_t n) const
{
    const float* const m = rotation.m;
    auto R = [m](int k) { return F32(m[k]); };
    for (uint32_t i = 0; i < n; ++i) {
        Vec4 t;
        num::Vec4Copy(t, v[i]);
        const F32 x = t.x, y = t.y, z = t.z;
        v[i].x = ((R(0) * x + R(1) * y) + R(2) * z).f();
        v[i].y = ((R(3) * x + R(4) * y) + z * R(5)).f();
        v[i].z = ((R(8) * z + R(6) * x) + R(7) * y).f();
    }
    return v;
}

Vec4* RigidBody::RotateT4(Vec4* v, uint32_t n) const
{
    float r[10];
    std::memcpy(r, rotation.m, sizeof rotation.m);
    r[9] = rotationTail;
    for (uint32_t i = 0; i < n; ++i) {
        Vec4 t;
        num::Vec4Copy(t, v[i]);
        const F32 x = t.x, y = t.y, z = t.z;
        float out[4];
        for (int lane = 0; lane < 4; ++lane)   // mulps row*broadcast, then addps, addps
            out[lane] = ((F32(r[lane]) * x + F32(r[3 + lane]) * y) + F32(r[6 + lane]) * z).f();
        std::memcpy(&v[i], out, 16);
    }
    return v;
}

Vec4* RigidBody::TransformPoints4(Vec4* v, uint32_t n) const
{
    num::RotateRows4(rotation.m, v, n);
    num::AddVec4Array(position, v, n);
    return v;
}

Vec4* RigidBody::TransformPlanes4(Vec4* v, uint32_t n) const
{
    const float* const m = rotation.m;
    const Vec4& p = position;
    for (uint32_t k = 0; k < n; ++k) {
        Vec4 t;
        num::Vec4Copy(t, v[k]);
        Vec4 r;
        ((F32x4::Load(m) * F32x4::Splat(F32(t.x)) + F32x4::Load(m + 3) * F32x4::Splat(F32(t.y))) + F32x4::Load(m + 6) * F32x4::Splat(F32(t.z))).Store(&r.x);
        const F32 dd = ((F32(p.y) * F32(r.y) + F32(p.z) * F32(r.z)) + F32(p.x) * F32(r.x)) - F32(v[k].w);
        v[k].x = r.x;
        v[k].y = r.y;
        v[k].z = r.z;
        v[k].w = (F32(0.0f) - dd).f();
    }
    return v;
}

Vec4* RigidBody::WorldToLocal4(Vec4* v, uint32_t n) const
{
    num::SubVec4Array(position, v, n);
    num::Mat33RotateArray(rotation.m, v, n);
    return v;
}

Vec4* RigidBody::RotateDirs4(Vec4* v, uint32_t n) const
{
    for (uint32_t k = 0; k < n; ++k) {
        Mat33Padded mp = {};
        num::Mat33FromQuat(mp.m, savedOrientation);
        RotateByRows(mp.m.m, v[k]);
    }
    return v;
}

Vec4* RigidBody::LocalToWorld4(Vec4* v, uint32_t n) const
{
    for (uint32_t k = 0; k < n; ++k) {
        Mat33Padded mp = {};
        num::Mat33FromQuat(mp.m, savedOrientation);
        RotateByRows(mp.m.m, v[k]);
        v[k].x = (F32(savedPosition.x) + F32(v[k].x)).f();
        v[k].y = (F32(v[k].y) + F32(savedPosition.y)).f();
        v[k].z = (F32(savedPosition.z) + F32(v[k].z)).f();
    }
    return v;
}

Vec4* RigidBody::WorldToBody4(Vec4* v, uint32_t n) const
{
    for (uint32_t k = 0; k < n; ++k) {
        Mat33 m;
        num::Mat33FromQuat(m, savedOrientation);
        const F32 dy = F32(v[k].y) - F32(savedPosition.y), dz = F32(v[k].z) - F32(savedPosition.z), dx = F32(v[k].x) - F32(savedPosition.x);
        MulRows3(m.m, dx, dy, dz, v[k]);
    }
    return v;
}

Vec4* RigidBody::WorldToBodyDirs4(Vec4* v, uint32_t n) const
{
    for (uint32_t k = 0; k < n; ++k) {
        Mat33 m;
        num::Mat33FromQuat(m, savedOrientation);
        MulRows3(m.m, F32(v[k].x), F32(v[k].y), F32(v[k].z), v[k], true);
    }
    return v;
}

void RigidBody::PointVelocity(const Vec4& p, Vec4& out) const
{
    Vec4 d{};
    d.x = (F32(p.x) - F32(position.x)).f();
    d.y = (F32(p.y) - F32(position.y)).f();
    d.z = (F32(p.z) - F32(position.z)).f();
    num::Cross3(d, angularVelocity, out);
    out.x = (F32(out.x) + F32(velocity.x)).f();
    out.y = (F32(velocity.y) + F32(out.y)).f();
    out.z = (F32(velocity.z) + F32(out.z)).f();
}

void RigidBody::PointAccel(const Vec4& p, Vec4& out) const
{
    Vec4 d{};
    d.x = (F32(p.x) - F32(position.x)).f();
    d.y = (F32(p.y) - F32(position.y)).f();
    d.z = (F32(p.z) - F32(position.z)).f();
    Vec4 ww, s20;
    num::Cross3(angularVelocity, angularVelocity, ww);   // (the original's: w x w, so 0)
    num::Cross3(ww, d, out);
    num::Cross3(angularAcceleration, d, s20);
    const float x = (F32(out.x) + F32(s20.x)).f();
    const float y = (F32(s20.y) + F32(out.y)).f();
    const float z = (F32(s20.z) + F32(out.z)).f();
    out.x = (F32(x) + F32(acceleration.x)).f();
    out.y = (F32(acceleration.y) + F32(y)).f();
    out.z = (F32(acceleration.z) + F32(z)).f();
}

bool RigidBody::SlowTest() const
{
    if (jmp::A(def->angularThresholdVelocity, num::Length3(angularVelocity))) return true;
    return jmp::A(def->linearThresholdVelocity, num::Length3(velocity));
}

X87 RigidBody::AngularResponse(const Vec4& r, const Vec4& n, const float* d) const
{
    Vec4 rn, w, t;
    num::Cross3(r, n, rn);
    num::Mul4(def->localInverseInertia, rn, w);
    num::Cross3(w, r, t);
    return ((X87(t.y) * X87(d[1]) + X87(t.z) * X87(d[2])) + X87(t.x) * X87(d[0])) * X87(0.6f);
}

void RigidBody::ImpulseResponse(const Vec4& r, const Vec4& n, const Vec4& d, bool cross, float* self, float* other) const
{
    if (cross) *other = (AngularResponse(r, d, &n.x) + X87(*other)).Store();
    *self = ((AngularResponse(r, n, &n.x) + X87(*self)) + X87(def->inverseMass)).Store();
}

void RigidBody::AccumulateTorque(const Vec4& v) { (F32x4::Load(&v.x) * F32x4::Splat(F32(accumulateScale)) + F32x4::Load(&torque.x)).Store(&torque.x); }
void RigidBody::AccumulateForce(const Vec4& v) { (F32x4::Load(&v.x) * F32x4::Splat(F32(accumulateScale)) + F32x4::Load(&force.x)).Store(&force.x); }

void RigidBody::SaveState()
{
    num::Vec4Copy(savedPosition, position);
    num::Vec4Copy(savedVelocity, velocity);
    std::memcpy(&savedOrientation, &orientation, 16);   // (movups)
    num::Vec4Copy(savedAngularVelocity, angularVelocity);
}

void RigidBody::UpdateRotation()
{
    Mat33 r;
    num::Mat33FromQuat(r, orientation);
    rotation = r;
    if (!jmp::A(scaleOn, 0.0f)) return;   // comiss on, 0; jbe skip
    Mat33 d;
    num::Mat33Zero(d);
    d.m[0] = scale[0];
    d.m[4] = scale[1];
    d.m[8] = scale[2];
    Mat33 out;
    num::Mat33Mul(r, out, d);
    rotation = out;
}

bool RigidBody::Integrate(float dt, bool translate, bool rotate)
{
    if (jmp::A(dt, 0.0f)) backwards = false;
    else if (jmp::A(0.0f, dt)) backwards = true;
    else return false;   // dt == 0 (or NaN): nothing
    if (rotate) {   // orientation += (w dt / 2, 1) * q, renormalised
        const F32 h = F32(dt) * F32(0.5f);
        const Vec4 dq{(F32(angularVelocity.x) * h).f(), (F32(angularVelocity.y) * h).f(), (F32(angularVelocity.z) * h).f(), 1.0f};
        num::QuatMul(dq, orientation, orientation);
        num::Normalize4Approx(orientation);
    }
    if (translate) (F32x4::Load(&velocity.x) * F32x4::Splat(F32(dt)) + F32x4::Load(&position.x)).Store(&position.x);
    return true;
}

void RigidBody::Step(float dt, bool freezable)
{
    float* const w = &angularVelocity.x;
    float* const v = &velocity.x;
    const F32x4 D = F32x4::Splat(F32(dt));
    (F32x4::Load(&stepAngularAcceleration.x) * D + F32x4::Load(w)).Store(w);   // from the torque / the force
    (F32x4::Load(&stepAcceleration.x) * D + F32x4::Load(v)).Store(v);
    bool translate = true, rotate = true;
    if (freezable && jmp::A(1e-5f, Square3(velocity).f())) {   // all but still: at rest
        num::Vec4Zero(velocity);
        translate = false;
        if (jmp::A(1e-5f, Square3(angularVelocity).f())) {
            num::Vec4Zero(angularVelocity);
            rotate = false;
        }
    }
    Integrate(dt, translate, rotate);
    // The accelerations from the velocity change, smoothed (0.2 old + 0.8 new).
    float* const ma = &measuredAcceleration.x;
    float* const mw = &measuredAngularAcceleration.x;
    for (uint32_t k = 0; k < 3; ++k) {
        ma[k] = (F32(v[k]) - F32((&previousVelocity.x)[k])).f();
        mw[k] = (F32(w[k]) - F32((&previousAngularVelocity.x)[k])).f();
    }
    const F32x4 inv = F32x4::Splat(F32(1.0f) / F32(dt));
    (inv * F32x4::Load(mw)).Store(mw);
    (inv * F32x4::Load(ma)).Store(ma);
    const F32x4 k2 = F32x4::Splat(F32(0.2f)), k8 = F32x4::Splat(F32(0.8f));
    float* const a = &acceleration.x;
    float* const aw = &angularAcceleration.x;
    (k2 * F32x4::Load(a)).Store(a);
    (F32x4::Load(ma) * k8 + F32x4::Load(a)).Store(a);
    (k2 * F32x4::Load(aw)).Store(aw);
    (F32x4::Load(mw) * k8 + F32x4::Load(aw)).Store(aw);
    num::Vec4Copy(previousAngularVelocity, angularVelocity);
    num::Vec4Copy(previousVelocity, velocity);
}

bool RigidBody::SleepCheck(float dt)
{
    if (!def->sleepable) return false;
    const F32 tl = F32(def->linearSleepVelocity);
    if (jmp::BE((tl * tl).f(), Square3(velocity).f())) {   // comiss lsv^2, |v|^2; jbe
        sleepTimer = 0.0f;
        return false;
    }
    const F32 ta = F32(def->angularSleepVelocity);
    if (jmp::BE((ta * ta).f(), Square3(angularVelocity).f())) {
        sleepTimer = 0.0f;
        return false;
    }
    sleepTimer = (F32(sleepTimer) + F32(dt)).f();
    if (!inContactGroup || jmp::BE(sleepTimer, def->sleepDelay)) return false;   // comiss timer, SleepDelay; jbe
    num::Vec4Zero(velocity);
    num::Vec4Zero(angularVelocity);
    sleepTimer = 0.0f;
    return true;
}

bool RigidBody::BlendState(float f)
{
    if (!jmp::B(f, blend)) return false;   // jae: f >= blend (NaN goes on)
    const F32x4 F = F32x4::Splat(F32(f)), S = F32x4::Splat(F32(1.0f) - F32(f));
    auto mix = [&](Vec4& to, const Vec4& from) {   // to = f to + (1 - f) from
        (F * F32x4::Load(&to.x)).Store(&to.x);
        (F32x4::Load(&from.x) * S + F32x4::Load(&to.x)).Store(&to.x);
    };
    mix(orientation, savedOrientation);
    num::Normalize4Approx(orientation);
    mix(position, savedPosition);
    blend = f;
    return true;
}

bool RigidBody::AdvanceRest(float dt)
{
    if (!jmp::B(blend, 1.0f)) return false;   // jae: done (blend >= 1)
    const F32 r = (F32(1.0f) - F32(blend)) * F32(dt);
    Vec4 w{angularVelocity.x, angularVelocity.y, angularVelocity.z, 0.0f};
    (F32x4::Splat(r * F32(0.5f)) * F32x4::Load(&w.x)).Store(&w.x);
    w.w = 1.0f;   // (set after the scaling; its scaled lane was stack garbage)
    Vec4 q;
    num::QuatMul(w, orientation, q);
    num::Normalize4ApproxTo(q, orientation);
    (F32x4::Load(&velocity.x) * F32x4::Splat(r) + F32x4::Load(&position.x)).Store(&position.x);
    blend = 1.0f;
    return true;
}

void RigidBody::CancelNormalVelocity(const Vec4& n, float a, float cap, float offset, float k)
{
    Vec4& v = velocity;
    const F32 a2 = F32(a) - F32(offset);
    const F32 d = (F32(v.z) * F32(n.z) + F32(v.y) * F32(n.y)) + F32(n.x) * F32(v.x);
    if (!jmp::A(0.0f, d.f())) return;
    F32 m = F32(0.0f) - d;
    if (jmp::A(a2.f(), 0.01f)) {
        const F32 q = (d * d) / (a2 * F32(2.0f));
        if (jmp::A((m / q).f(), cap)) m = (q * F32(k)) * F32(cap);
    }
    (F32x4::Load(&n.x) * F32x4::Splat(m) + F32x4::Load(&v.x)).Store(&v.x);
}

} // namespace phys::native
