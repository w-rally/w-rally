#include "native/entity.h"

#include <cstring>

#include "num/f32.h"
#include "num/bits.h"
#include "num/math.h"
#include "num/trig.h"
#include "num/x87.h"
#include "native/solver.h"
#include "native/world.h"

namespace phys::native {

using num::F32;
using num::F32x4;
using num::X87;
namespace jmp = num::jmp;

const Body* HingedSolid::carBody() const
{
    const Body* b = static_cast<const Car*>(car)->carBody;
    if (!b) Unreached("a hinged part on a car without its body");
    return b;
}

void HingedSolid::RotationRows()   // HingeRotationRows (0x51c960)
{
    const float s = X87::Raw(num::SinOfFloat(angle)).Store();
    const float c = X87::Raw(num::CosOfFloat(angle)).Store();
    const float row[9] = {1.0f, 0.0f, 0.0f, 0.0f, c, s, 0.0f, (F32(0.0f) - F32(s)).f(), c};
    std::memcpy(hingeRows, row, sizeof row);
}

X87 HingedSolid::SpringTorque(uint32_t i)   // HingeSpringTorque (0x51cf10)
{
    const HingeSpringDef& s = hingeDef->springs[i];
    float& pos = springPos[i];
    const F32 a = F32(angle);
    const F32 span = F32(s.span), maxDisp = F32(s.maxDisp);
    const F32 lo = F32(pos) - span;
    F32 d;
    if (!jmp::BE(lo.f(), a.f())) {
        d = a - lo;
        const F32 m = F32(0.0f) - maxDisp;
        if (!jmp::BE(m.f(), d.f())) {   // past MaxDisp: the rest angle follows
            d = m;
            pos = ((span + a) - d).f();
        }
    } else {
        const F32 hi = span + F32(pos);
        if (jmp::BE(a.f(), hi.f())) return X87(0.0f);
        d = a - hi;
        if (!jmp::BE(d.f(), maxDisp.f())) {
            d = maxDisp;
            pos = ((a - span) - d).f();
        }
    }
    if (d.f() == 0.0f) return X87(0.0f);
    return Neg(X87(d) * X87(s.stiff)) - X87(spin) * X87(s.damp);
}

X87 HingedSolid::SpringTorqueSum()   // HingeSpringTorqueSum (0x51ea80)
{
    float acc = 0.0f;
    for (uint32_t i = 0; i < 2; ++i)
        if (hingeDef->springs[i].type == 2) acc = (SpringTorque(i) + X87(acc)).Store();
    return X87(acc);
}

void HingedSolid::Limits()   // HingeLimits (0x51f280)
{
    const HingeDef& D = *hingeDef;
    const F32 a = F32(angle);
    bool bounce;
    if (!jmp::BE(a.f(), D.maxAngle)) {   // above HingeMaxAngle: held there
        angle = D.maxAngle;
        bounce = !jmp::BE(spin, 0.0f);
    } else if (!jmp::BE(D.minAngle, a.f())) {
        angle = D.minAngle;
        bounce = !jmp::BE(0.0f, spin);
    } else {
        return;
    }
    if (bounce) spin = (F32(0.0f) - F32(D.restitution) * F32(spin)).f();
}

void HingedSolid::WorldFrame(World& w)   // HingeWorldFrame (0x51e1d0)
{
    const HingeDef& D = *hingeDef;
    const RigidBody& cb = car->body;
    const Vec4& acc = cb.acceleration;
    const Vec4 p{acc.x, acc.y, (F32(acc.z) + F32(9.81f)).f(), 0.0f};   // + 9.81 in z
    Vec4 anchor, av, r, ww;
    FrameMoveCopy(*w.world, p, car->frame, anchor, Move::Dirs);
    FrameMoveCopy(*w.world, cb.angularAcceleration, car->frame, axisCar, Move::Dirs);
    FrameMoveCopy(*w.world, cb.angularVelocity, car->frame, av, Move::Dirs);
    FrameMoveCopy(carBody()->frame, D.hingePointRF, car->frame, r, Move::Points);   // (0x4e6a30: the car body into the car)
    num::Cross3(av, av, ww);   // (sic: w x w)
    num::Cross3(ww, r, pivotAcceleration);
    float* const pa = &pivotAcceleration.x;
    for (uint32_t k = 0; k < 3; ++k) pa[k] = (F32(pa[k]) + F32((&anchor.x)[k])).f();
    const Vec4& e = D.hingeAxisRF;
    axisAcceleration = ((F32(e.z) * F32(axisCar.z) + F32(e.y) * F32(axisCar.y)) + F32(e.x) * F32(axisCar.x)).f();
}

X87 HingedSolid::InertialTorque(World& w)   // HingeInertialTorque: the air on the part's face
{
    const HingeDef& d = *hingeDef;
    const Car& c = static_cast<const Car&>(*car);
    Vec4 a, sp = c.carState.travelDirection, v;
    FrameMoveCopy(frame, d.faceNormalBF, car->frame, a, Move::Dirs);
    const F32 g = F32(c.carState.travelSpeed);
    FrameMoveCopy(*w.world, sp, car->frame, v, Move::Dirs);
    const Vec4& hp = d.hingePoint;
    const Vec4 r = {(F32(body.position.x) - F32(hp.x)).f(), (F32(body.position.y) - F32(hp.y)).f(), (F32(body.position.z) - F32(hp.z)).f(), 0.0f};
    F32 dot = ((F32(v.z) * F32(a.z) + F32(v.y) * F32(a.y)) + F32(v.x) * F32(a.x)) * F32(d.faceArea);
    if (!jmp::A(dot.f(), 0.0f)) dot = F32(0.0f) - dot;
    Vec4 force, cr;
    num::Scale4(v, (((dot * g) * g) * F32(0.5f)).f(), force);
    num::Cross3(r, force, cr);
    const Vec4& ax = d.hingeAxisRF;
    return (X87(cr.y) * X87(ax.y) + X87(cr.z) * X87(ax.z)) + X87(cr.x) * X87(ax.x);
}

void HingedSolid::Forces(World& w)   // HingeSolve (0x51ead0)
{
    if (damage->lock) return;   // locked shut
    if (!car) {
        Solid::Forces(w);
        return;
    }
    const HingeDef& D = *hingeDef;
    const float I = D.inertia == 0.0f ? 0.1f : D.inertia;   // ucomiss: NaN keeps it
    const float m = body.def->inverseMass;
    const float invMass = m == 0.0f ? 1.0f : (F32(1.0f) / F32(m)).f();
    WorldFrame(w);
    // M = A B (A = the hinge rows, B = the def's basis) in the original's operand order.
    auto a = [&](uint32_t i, uint32_t j) { return F32(hingeRows[3 * i + j]); };
    auto b = [&](uint32_t i, uint32_t j) { return F32(D.basisRF[3 * i + j]); };
    float M[12] = {};
    M[0] = ((a(0, 2) * b(2, 0) + b(1, 0) * a(0, 1)) + a(0, 0) * b(0, 0)).f();
    M[1] = ((b(1, 1) * a(0, 1) + a(0, 2) * b(2, 1)) + a(0, 0) * b(0, 1)).f();
    M[2] = ((a(0, 2) * b(2, 2) + b(1, 2) * a(0, 1)) + b(0, 2) * a(0, 0)).f();
    M[3] = ((a(1, 2) * b(2, 0) + a(1, 0) * b(0, 0)) + a(1, 1) * b(1, 0)).f();
    M[4] = ((a(1, 1) * b(1, 1) + a(1, 0) * b(0, 1)) + a(1, 2) * b(2, 1)).f();
    M[5] = ((b(0, 2) * a(1, 0) + a(1, 2) * b(2, 2)) + a(1, 1) * b(1, 2)).f();
    M[6] = ((a(2, 2) * b(2, 0) + b(1, 0) * a(2, 1)) + a(2, 0) * b(0, 0)).f();
    M[7] = ((b(1, 1) * a(2, 1) + a(2, 0) * b(0, 1)) + a(2, 2) * b(2, 1)).f();
    M[8] = ((a(2, 2) * b(2, 2) + b(0, 2) * a(2, 0)) + b(1, 2) * a(2, 1)).f();
    const F32x4 v = F32x4::Load(&D.anchorBF.x);
    Vec4 r;
    ((F32x4::Load(M) * F32x4::Splat(v.v[0]) + F32x4::Load(M + 3) * F32x4::Splat(v.v[1])) + F32x4::Load(M + 6) * F32x4::Splat(v.v[2])).Store(&r.x);
    Vec4 q;
    num::Cross3(r, pivotAcceleration, q);
    const Vec4& ax = D.hingeAxisRF;
    const X87 t = (X87(q.y) * X87(ax.y) + X87(q.z) * X87(ax.z)) + X87(q.x) * X87(ax.x);
    const X87 drive = X87::Raw(-((t * X87(invMass)).d)) + (X87(D.defaultAngle) - X87(angle)) * X87(D.springRate);
    float acc = (drive - X87(D.damping) * X87(spin)).Store();   // the spring to the rest angle, the damping
    acc = (InertialTorque(w) + X87(acc)).Store();
    acc = (SpringTorqueSum() + X87(acc)).Store();
    // The hinge's friction -0.3 sign(w); at rest (w == 0) it holds the hinge when it outweighs the torque.
    const float sw = spin;
    const int32_t sgn = jmp::A(sw, 0.0f) ? 1 : (jmp::A(0.0f, sw) ? -1 : 0);
    const F32 fr = F32(static_cast<float>(sgn)) * F32(-0.3f);
    auto absf = [](F32 x) { return jmp::A(x.f(), 0.0f) ? x : F32(0.0f) - x; };
    if (sw == 0.0f && jmp::A(absf(fr).f(), absf(F32(acc)).f())) acc = 0.0f;
    else acc = (fr + F32(acc)).f();
    Vec4 F, T;
    F32 res = F32(acc);
    if (ContactSum(*this, 3.0f, w.frictionScale, F, T)) {   // the contacts' torque about the hinge
        Vec4 arm, u;
        num::Scale4(D.hingePointBF, -1.0f, arm);
        num::Cross3(arm, F, u);
        const F32 ux = F32(T.x) + F32(u.x), uy = F32(T.y) + F32(u.y), uz = F32(T.z) + F32(u.z);
        const Vec4& a2 = D.hingeAxisBF;
        res = ((F32(a2.z) * uz + F32(a2.y) * uy) + F32(a2.x) * ux) + F32(acc);
    }
    angularAcceleration = ((res / F32(I)) - F32(axisAcceleration)).f();
}

void HingedSolid::FrameSetup(World& w)   // HingeFrameSetup (0x51d160)
{
    const HingeDef& D = *hingeDef;
    FrameMoveCopy(carBody()->frame, D.hingePointRF, *w.world, pivot, Move::Points);   // (0x4e8130: the car body into the world)
    FrameMoveCopy(car->frame, D.hingeAxisRF, *w.world, axisWorld, Move::Dirs);
    RotationRows();
    // The rotation products (generated from the original's scalar SSE, instruction by instruction: re/py/sse2cpp_block.py).
    F32 x0(0.0f), x1(0.0f), x2(0.0f), x3(0.0f), x4(0.0f), x5(0.0f), x6(0.0f), x7(0.0f);
    F32 s50(0.0f), s54(0.0f), s5c(0.0f);
    x3 = F32(D.basisBFT[2]);
    x3 = x3 * F32(hingeRows[6]);
    x0 = F32(D.basisBFT[0]);
    x0 = x0 * F32(hingeRows[0]);
    x1 = F32(hingeRows[4]);
    x1 = x1 * F32(D.basisBFT[1]);
    x4 = F32(D.basisBFT[2]);
    x4 = x4 * F32(hingeRows[8]);
    x5 = F32(hingeRows[6]);
    x5 = x5 * F32(D.basisBFT[5]);
    x2 = F32(D.basisBFT[5]);
    x2 = x2 * F32(hingeRows[7]);
    x3 = x3 + x0;
    x0 = F32(D.basisBFT[1]);
    x0 = x0 * F32(hingeRows[3]);
    x6 = F32(D.basisBFT[4]);
    x6 = x6 * F32(hingeRows[5]);
    x3 = x3 + x0;
    x0 = F32(D.basisBFT[2]);
    x0 = x0 * F32(hingeRows[7]);
    x0 = x0 + x1;
    x1 = F32(hingeRows[1]);
    x1 = x1 * F32(D.basisBFT[0]);
    x0 = x0 + x1;
    x1 = F32(D.basisBFT[1]);
    x1 = x1 * F32(hingeRows[5]);
    x4 = x4 + x1;
    x1 = F32(hingeRows[2]);
    x1 = x1 * F32(D.basisBFT[0]);
    x4 = x4 + x1;
    x1 = F32(D.basisBFT[4]);
    x1 = x1 * F32(hingeRows[3]);
    x5 = x5 + x1;
    x1 = F32(D.basisBFT[3]);
    x1 = x1 * F32(hingeRows[0]);
    x7 = F32(hingeRows[2]);
    x7 = x7 * F32(D.basisBFT[6]);
    x5 = x5 + x1;
    x1 = F32(hingeRows[1]);
    x1 = x1 * F32(D.basisBFT[3]);
    x1 = x1 + x2;
    x2 = F32(D.basisBFT[4]);
    x2 = x2 * F32(hingeRows[4]);
    x1 = x1 + x2;
    x2 = F32(hingeRows[8]);
    x2 = x2 * F32(D.basisBFT[5]);
    x2 = x2 + x6;
    x6 = F32(D.basisBFT[3]);
    x6 = x6 * F32(hingeRows[2]);
    x2 = x2 + x6;
    x6 = F32(D.basisBFT[6]);
    x6 = x6 * F32(hingeRows[0]);
    s50 = x2;
    x2 = F32(D.basisBFT[8]);
    x2 = x2 * F32(hingeRows[6]);
    x2 = x2 + x6;
    x6 = F32(D.basisBFT[7]);
    x6 = x6 * F32(hingeRows[3]);
    x2 = x2 + x6;
    x6 = F32(hingeRows[1]);
    x6 = x6 * F32(D.basisBFT[6]);
    s54 = x2;
    x2 = F32(D.basisBFT[8]);
    x2 = x2 * F32(hingeRows[7]);
    x2 = x2 + x6;
    x6 = F32(hingeRows[4]);
    x6 = x6 * F32(D.basisBFT[7]);
    x2 = x2 + x6;
    x6 = F32(D.basisBFT[8]);
    x6 = x6 * F32(hingeRows[8]);
    x6 = x6 + x7;
    x7 = F32(D.basisBFT[7]);
    x7 = x7 * F32(hingeRows[5]);
    x6 = x6 + x7;
    x7 = F32(D.basisRF[3]);
    x7 = x7 * x0;
    s5c = x6;
    x6 = F32(D.basisRF[0]);
    x6 = x6 * x3;
    x6 = x6 + x7;
    x7 = F32(D.basisRF[6]);
    x7 = x7 * x4;
    x6 = x6 + x7;
    rotation[0] = x6.f();
    x7 = F32(D.basisRF[1]);
    x7 = x7 * x3;
    x6 = x0;
    x6 = x6 * F32(D.basisRF[4]);
    x6 = x6 + x7;
    x7 = x4;
    x7 = x7 * F32(D.basisRF[7]);
    x6 = x6 + x7;
    rotation[1] = x6.f();
    x0 = x0 * F32(D.basisRF[5]);
    x6 = F32(D.basisRF[8]);
    x6 = x6 * x4;
    x4 = F32(D.basisRF[2]);
    x4 = x4 * x3;
    x0 = x0 + x6;
    x0 = x0 + x4;
    rotation[2] = x0.f();
    x3 = F32(D.basisRF[0]);
    x0 = F32(D.basisRF[3]);
    x4 = F32(D.basisRF[6]);
    x3 = x3 * x5;
    x0 = x0 * x1;
    x3 = x3 + x0;
    x0 = s50;
    x4 = x4 * x0;
    x3 = x3 + x4;
    rotation[3] = x3.f();
    x3 = x1;
    x3 = x3 * F32(D.basisRF[4]);
    x4 = x0;
    x4 = x4 * F32(D.basisRF[7]);
    x3 = x3 + x4;
    x4 = F32(D.basisRF[1]);
    x4 = x4 * x5;
    x3 = x3 + x4;
    rotation[4] = x3.f();
    x1 = x1 * F32(D.basisRF[5]);
    x3 = F32(D.basisRF[8]);
    x3 = x3 * x0;
    x0 = F32(D.basisRF[2]);
    x3 = x3 + x1;
    x0 = x0 * x5;
    x3 = x3 + x0;
    x0 = s54;
    rotation[5] = x3.f();
    x3 = F32(D.basisRF[0]);
    x1 = F32(D.basisRF[3]);
    x4 = F32(D.basisRF[6]);
    x3 = x3 * x0;
    x1 = x1 * x2;
    x3 = x3 + x1;
    x1 = s5c;
    x4 = x4 * x1;
    x3 = x3 + x4;
    rotation[6] = x3.f();
    x4 = F32(D.basisRF[1]);
    x3 = x2;
    x3 = x3 * F32(D.basisRF[4]);
    x4 = x4 * x0;
    x3 = x3 + x4;
    x4 = x1;
    x4 = x4 * F32(D.basisRF[7]);
    x3 = x3 + x4;
    rotation[7] = x3.f();
    x3 = F32(D.basisRF[8]);
    x2 = x2 * F32(D.basisRF[5]);
    x3 = x3 * x1;
    x1 = F32(D.basisRF[2]);
    x1 = x1 * x0;
    x3 = x3 + x2;
    x3 = x3 + x1;
    rotation[8] = x3.f();
    x0 = F32(D.basisRF[0]);
    x0 = x0 * F32(hingeRows[0]);
    x1 = F32(D.basisRF[6]);
    x1 = x1 * F32(hingeRows[2]);
    x0 = x0 + x1;
    x1 = F32(D.basisRF[3]);
    x1 = x1 * F32(hingeRows[1]);
    x0 = x0 + x1;
    bodyRows[0] = x0.f();
    x0 = F32(D.basisRF[7]);
    x0 = x0 * F32(hingeRows[2]);
    x1 = F32(D.basisRF[1]);
    x1 = x1 * F32(hingeRows[0]);
    x0 = x0 + x1;
    x1 = F32(hingeRows[1]);
    x1 = x1 * F32(D.basisRF[4]);
    x0 = x0 + x1;
    bodyRows[1] = x0.f();
    x0 = F32(D.basisRF[8]);
    x0 = x0 * F32(hingeRows[2]);
    x1 = F32(D.basisRF[5]);
    x1 = x1 * F32(hingeRows[1]);
    x0 = x0 + x1;
    x1 = F32(D.basisRF[2]);
    x1 = x1 * F32(hingeRows[0]);
    x0 = x0 + x1;
    bodyRows[2] = x0.f();
    x0 = F32(D.basisRF[0]);
    x0 = x0 * F32(hingeRows[3]);
    x1 = F32(D.basisRF[6]);
    x1 = x1 * F32(hingeRows[5]);
    x0 = x0 + x1;
    x1 = F32(D.basisRF[3]);
    x1 = x1 * F32(hingeRows[4]);
    x0 = x0 + x1;
    bodyRows[3] = x0.f();
    x0 = F32(D.basisRF[7]);
    x0 = x0 * F32(hingeRows[5]);
    x1 = F32(D.basisRF[1]);
    x1 = x1 * F32(hingeRows[3]);
    x0 = x0 + x1;
    x1 = F32(hingeRows[4]);
    x1 = x1 * F32(D.basisRF[4]);
    x0 = x0 + x1;
    bodyRows[4] = x0.f();
    x0 = F32(D.basisRF[8]);
    x0 = x0 * F32(hingeRows[5]);
    x1 = F32(D.basisRF[5]);
    x1 = x1 * F32(hingeRows[4]);
    x0 = x0 + x1;
    x1 = F32(D.basisRF[2]);
    x1 = x1 * F32(hingeRows[3]);
    x0 = x0 + x1;
    bodyRows[5] = x0.f();
    x0 = F32(hingeRows[8]);
    x0 = x0 * F32(D.basisRF[6]);
    x1 = F32(D.basisRF[3]);
    x1 = x1 * F32(hingeRows[7]);
    x0 = x0 + x1;
    x1 = F32(D.basisRF[0]);
    x1 = x1 * F32(hingeRows[6]);
    x0 = x0 + x1;
    bodyRows[6] = x0.f();
    x0 = F32(hingeRows[8]);
    x0 = x0 * F32(D.basisRF[7]);
    x1 = F32(D.basisRF[1]);
    x1 = x1 * F32(hingeRows[6]);
    x0 = x0 + x1;
    x1 = F32(hingeRows[7]);
    x1 = x1 * F32(D.basisRF[4]);
    x0 = x0 + x1;
    bodyRows[7] = x0.f();
    x0 = F32(hingeRows[6]);
    x0 = x0 * F32(D.basisRF[2]);
    x1 = F32(hingeRows[8]);
    x1 = x1 * F32(D.basisRF[8]);
    x0 = x0 + x1;
    x1 = F32(D.basisRF[5]);
    x1 = x1 * F32(hingeRows[7]);
    x0 = x0 + x1;   // (scheduled after the lea / test that set up QuatFromMat33's argument)
    bodyRows[8] = x0.f();
    num::QuatFromMat33(rotation, body.orientation);
    // p = the rotation's rows (16-byte loads: the 4th lanes are the next row's first, then hingeRows[0]) . the anchor, + the
    // hinge point (xyz).
    const float* const m = rotation;
    const F32x4 v = F32x4::Load(&D.anchorBF.x);
    Vec4 p;
    ((F32x4::Load(m) * F32x4::Splat(v.v[0]) + F32x4::Load(m + 3) * F32x4::Splat(v.v[1])) + F32x4::Load(m + 6) * F32x4::Splat(v.v[2])).Store(&p.x);
    p.x = (F32(p.x) + F32(D.hingePoint.x)).f();
    p.y = (F32(D.hingePoint.y) + F32(p.y)).f();
    p.z = (F32(D.hingePoint.z) + F32(p.z)).f();
    body.position = p;
}

void HingedSolid::Step(World& w, float dt)   // HingeIntegrate (0x51f950)
{
    if (damage->lock) return;   // locked shut
    if (!car) {
        Solid::Step(w, dt);
        return;
    }
    auto sign = [](float x) { return jmp::A(x, 0.0f) ? 1 : (jmp::A(0.0f, x) ? -1 : 0); };
    const F32 sw = F32(angularAcceleration) * F32(dt) + F32(spin);   // the hinge's spin, clamped to +-10
    float nw;
    int s;
    if (jmp::A(-10.0f, sw.f())) {
        nw = -10.0f;
        s = -1;
    } else if (jmp::A(sw.f(), 10.0f)) {
        nw = 10.0f;
        s = 1;
    } else {
        nw = sw.f();
        s = sign(nw);
    }
    const float old = spin;
    spin = (s != sign(old) && !(old == 0.0f)) ? 0.0f : nw;   // a sign change stops it (unless it was at rest)
    num::Scale4(hingeDef->hingeAxisRF, spin, body.angularVelocity);   // w = axis * spin
    num::Vec4Zero(body.velocity);
    angle = (F32(spin) * F32(dt) + F32(angle)).f();
    Limits();
    FrameSetup(w);
    if (w.stepped) w.stepped(*this);   // CSigMrRBodyState
}

} // namespace phys::native
