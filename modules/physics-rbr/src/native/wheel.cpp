#include "native/wheel.h"

#include <cstring>

#include "num/f32.h"
#include "num/bits.h"
#include "num/trig.h"
#include "num/x87.h"
#include "native/flags.h"
#include "native/records.h"
#include "native/world.h"

namespace phys::native {

using num::F32;
using num::F32x4;
using num::X87;
using num::FromBits;
namespace jmp = num::jmp;

void Wheel::TopMountDeform()
{
    const WheelDef& D = *wheelDef;
    const F32 su = F32(D.topMountMaxDeformUp) * F32(damage->topMountUp);
    (F32x4::Load(&D.topMountDeformUpAxis.x) * F32x4::Splat(su) + F32x4::Load(&D.topMount.x)).Store(&suspension.topMount.x);
    const F32 ss = F32(D.topMountMaxDeformSide) * F32(damage->topMountSide);
    (F32x4::Load(&D.topMountDeformSideAxis.x) * F32x4::Splat(ss) + F32x4::Load(&suspension.topMount.x)).Store(&suspension.topMount.x);
}

void Wheel::AxleWobble()
{
    WheelSuspension& s = suspension;
    const float a = damage->inclination;
    if (a == 0.0f) {   // equal and ordered
        s.axle = s.axisRow0;
        return;
    }
    const float a2 = (F32(a) * F32(2.0f)).f();
    float c;
    if (!jmp::BE(-1.0f, a2)) c = -1.0f;
    else c = jmp::A(a2, 1.0f) ? 1.0f : a2;
    const float ang = (X87(c) * X87(0.07f)).Store();
    const float y1 = Neg(X87::Raw(num::SinOfFloat(s.spinAngle)) * X87(ang)).Store();
    const float c2 = (X87::Raw(num::CosOfFloat(s.spinAngle)) * X87(ang)).Store();
    Vec4 v;
    num::Scale4(s.axisRow1, c2, v);   // the bent rim's wobble, turning with the wheel
    s.axle.x = (F32(s.axisRow0.x) + F32(v.x)).f();
    s.axle.y = (F32(s.axisRow0.y) + F32(v.y)).f();
    s.axle.z = (F32(s.axisRow0.z) + F32(v.z)).f();
    s.axle.y = y1;
    num::Normalize3Approx4(s.axle);
}

void Wheel::SteerRows()
{
    const WheelDef& D = *wheelDef;
    WheelSuspension& s = suspension;
    const float a = (X87(damage->inclination) * X87(D.inclinationPerDamage) + X87(D.axisInclination)).Store();   // the axis inclination
    const float sn = X87::Raw(num::SinOfFloat(a)).Store();
    s.axisRow0.x = X87::Raw(num::CosOfFloat(a)).Store();
    s.axisRow0.y = 0.0f;
    s.axisRow0.z = sn;
    const float cs = X87::Raw(num::CosOfFloat(a)).Store();
    s.axisRow1.x = X87::Raw(-num::SinOfFloat(a)).Store();   // fchs
    s.axisRow1.y = 0.0f;
    s.axisRow1.z = cs;
}

void Wheel::SteerReduce(float x)
{
    const WheelDef& D = *wheelDef;
    if (jmp::BE(x, D.steerReduceStart)) return;
    WheelDamage& dm = *damage;
    const F32 t = (F32(x) - F32(D.steerReduceStart)) / (F32(D.steerReduceFull) - F32(D.steerReduceStart));
    F32 v = (F32(0.0f) - t) + F32(dm.inclination);
    if (!jmp::BE(-1.0f, v.f())) v = -1.0f;
    else if (!jmp::BE(v.f(), 1.0f)) v = 1.0f;
    dm.inclination = v.f();
    SteerRows();
    damageChanged = 1;
}

void Wheel::SuspensionKinematics(float a1, float steeringAngle, bool renderGeometry)
{
    // The original's frame (model/suspension.h), slot for slot: its unaligned row loads read the next floats as w lanes,
    // which reach the hub's position.
    alignas(16) SuspensionFrame f{};
    auto B = [](auto& field) { return reinterpret_cast<uint8_t*>(&field); };
    const WheelDef& D = *wheelDef;
    WheelSuspension& s = suspension;
    RigidBody& b = body;
    const F32 one = 1.0f, zero = 0.0f, two = 2.0f;
    auto put = [](float& to, F32 v) { to = v.f(); };

    TopMountDeform();
    AxleWobble();
    num::Vec4Copy(f.topMount, s.topMount);
    s.steeringRodLength = (F32(D.steeringRodLength) - F32(damage->rodBend) * F32(FromBits(0x3c75c28f))).f();   // 0.015
    s.wishboneLength = D.wishboneLength;

    // The wishbone: t along the joint axis, the top mount's foot on it, its distance from the joint's mount.
    const F32 t = (F32(D.jointAxis.z) * (F32(D.jointMount.z) - F32(f.topMount.z)) + F32(D.jointAxis.y) * (F32(D.jointMount.y) - F32(f.topMount.y))) +
                  F32(D.jointAxis.x) * (F32(D.jointMount.x) - F32(f.topMount.x));
    put(f.jointT, t);
    (F32x4::Load(&D.jointAxis.x) * F32x4::Splat(t) + F32x4::Load(B(f.topMount.x))).Store(B(f.jointFoot.x));
    put(f.jointToFoot.x, F32(f.jointFoot.x) - F32(D.jointMount.x));
    put(f.jointToFoot.y, F32(f.jointFoot.y) - F32(D.jointMount.y));
    put(f.jointToFoot.z, F32(f.jointFoot.z) - F32(D.jointMount.z));
    const F32 L1 = num::Length3(f.jointToFoot);
    num::Scale4(f.jointToFoot, (X87(1.0f) / X87(L1.f())).Store(), f.jointDir);
    num::Cross3(D.jointAxis, f.jointDir, f.jointNormal);
    put(f.up.x, zero);
    put(f.up.y, zero);
    put(f.up.z, one);
    num::Cross3(f.up, s.axle, f.side);
    num::Cross3(s.axle, f.side, f.up);
    const X87 A = X87(a1) + X87(D.strutDimY);
    const X87 c = X87(D.strutDimX);
    const F32 R = Sqrt(c * c + A * A).Store();
    std::memcpy(B(f.axleRows[0]), &s.axle, 12);   // M, rows of 3
    std::memcpy(B(f.axleRows[3]), B(f.side.x), 12);
    std::memcpy(B(f.axleRows[6]), B(f.up.x), 12);

    // The law of cosines, clamped to [-1, 1].
    auto cosSin = [&](F32 v, float& cs, float& sn) {
        if (!jmp::B(v.f(), 1.0f)) {
            cs = 1.0f;
            sn = 0.0f;
        } else if (jmp::AE(-1.0f, v.f())) {
            cs = -1.0f;
            sn = 0.0f;
        } else {
            cs = v.f();
            sn = Sqrt(X87(1.0f) - X87(v.f()) * X87(v.f())).Store();
        }
    };
    const F32 e = F32(s.wishboneLength);
    const F32 c0 = ((e * e + L1 * L1) - (R * R - t * t)) / ((e * L1) * two);
    float cos0, sin0;
    cosSin(c0, cos0, sin0);
    put(f.wishboneSign, F32(D.wishboneSign));
    num::Scale4(f.jointDir, cos0, f.wishbone);
    (F32x4::Load(B(f.jointNormal.x)) * F32x4::Splat(F32(sin0) * F32(f.wishboneSign)) + F32x4::Load(B(f.wishbone.x))).Store(B(f.wishbone.x));
    num::Cross3(D.jointAxis, f.wishbone, f.wishboneNormal);
    const F32 e2 = F32(s.wishboneLength);
    put(f.wishboneLength, e2);
    (F32x4::Splat(e2) * F32x4::Load(B(f.wishbone.x))).Store(B(f.wishbone.x));
    const F32 x0 = F32(D.jointMount.x) + F32(f.wishbone.x), y0 = F32(D.jointMount.y) + F32(f.wishbone.y), z0 = F32(D.jointMount.z) + F32(f.wishbone.z);
    put(f.wishboneEnd.x, x0);
    put(f.wishboneEnd.y, y0);
    put(f.wishboneEnd.z, z0);
    put(f.strutVector.x, F32(f.topMount.x) - x0);
    put(f.strutVector.y, F32(f.topMount.y) - y0);
    put(f.strutVector.z, F32(f.topMount.z) - z0);
    const F32 invR = one / R;
    num::Scale4(f.strutVector, invR.f(), f.strutAxis);

    // The lower link.
    put(f.strutDim.x, F32(D.strutDimX));
    put(f.strutDim.y, zero);
    put(f.strutDim.z, F32(D.strutDimY) + F32(a1));
    (F32x4::Splat(invR) * F32x4::Load(B(f.strutDim.x))).Store(B(f.strutDim.x));
    const F32 u = (F32(D.strutRodMount.z) * F32(f.strutDim.z) + F32(D.strutRodMount.y) * F32(f.strutDim.y)) + F32(D.strutRodMount.x) * F32(f.strutDim.x);
    num::Scale4(f.strutDim, (zero - u).f(), f.rodOffset);
    put(f.rodOffset.x, F32(f.rodOffset.x) + F32(D.strutRodMount.x));
    put(f.rodOffset.y, F32(D.strutRodMount.y) + F32(f.rodOffset.y));
    put(f.rodOffset.z, F32(D.strutRodMount.z) + F32(f.rodOffset.z));
    const F32 L2 = num::Length3(f.rodOffset);
    num::Scale4(f.rodOffset, (X87(1.0f) / X87(L2.f())).Store(), f.rodDir);
    num::Cross3(f.strutDim, f.rodDir, f.dimCrossRod);
    const F32 k80 = F32(f.strutDim.x);
    num::Scale4(f.strutDim, k80.f(), f.dimScaled);
    put(f.cross.x, one - F32(f.dimScaled.x));
    put(f.cross.y, zero);
    put(f.cross.z, zero - F32(f.dimScaled.z));
    const F32 L3 = num::Length3(f.cross);
    num::Scale4(f.cross, (X87(1.0f) / X87(L3.f())).Store(), f.crossDir);
    const F32 bx = F32(f.crossDir.x), by = F32(f.crossDir.y), bz = F32(f.crossDir.z);
    const F32 p2c = (bz * F32(f.rodDir.z) + by * F32(f.rodDir.y)) + bx * F32(f.rodDir.x);
    const F32 p24 = (F32(f.dimCrossRod.z) * bz + F32(f.dimCrossRod.y) * by) + F32(f.dimCrossRod.x) * bx;
    const F32 g = D.isSteered ? F32(steeringAngle) : zero;
    (F32x4::Load(&D.steeringAxis.x) * F32x4::Splat(g) + F32x4::Load(&D.steeringMount.x)).Store(B(f.steeringPoint.x));
    (F32x4::Load(B(f.strutAxis.x)) * F32x4::Splat(u) + F32x4::Load(B(f.wishboneEnd.x))).Store(B(f.rodFoot.x));
    const F32 v = (F32(f.strutAxis.z) * (F32(f.rodFoot.z) - F32(f.steeringPoint.z)) + F32(f.strutAxis.y) * (F32(f.rodFoot.y) - F32(f.steeringPoint.y))) +
                  (F32(f.rodFoot.x) - F32(f.steeringPoint.x)) * F32(f.strutAxis.x);
    (F32x4::Load(B(f.strutAxis.x)) * F32x4::Splat(v) + F32x4::Load(B(f.steeringPoint.x))).Store(B(f.rodReach.x));
    const F32 h = F32(s.steeringRodLength);
    put(f.rodSpan.x, F32(f.rodReach.x) - F32(f.rodFoot.x));
    put(f.rodSpan.y, F32(f.rodReach.y) - F32(f.rodFoot.y));
    put(f.rodSpan.z, F32(f.rodReach.z) - F32(f.rodFoot.z));
    const F32 L4 = num::Length3(f.rodSpan);
    const F32 c1 = ((L4 * L4 + L2 * L2) - (h * h - v * v)) / ((L4 * L2) * two);
    float cos1, sin1;
    cosSin(c1, cos1, sin1);
    const F32 q = F32(sin1) * F32(D.steeringSign);
    num::Scale4(f.rodSpan, (one / L4).f(), f.rodSpanDir);
    num::Cross3(f.strutAxis, f.rodSpanDir, f.strutCrossSpan);
    num::Scale4(f.rodSpanDir, (F32(cos1) * p2c - q * p24).f(), f.knuckleX);
    (F32x4::Load(B(f.strutCrossSpan.x)) * F32x4::Splat(F32(cos1) * p24 + q * p2c) + F32x4::Load(B(f.knuckleX.x))).Store(B(f.knuckleX.x));
    (F32x4::Splat(L3) * F32x4::Load(B(f.knuckleX.x))).Store(B(f.knuckleX.x));
    (F32x4::Load(B(f.strutAxis.x)) * F32x4::Splat(k80) + F32x4::Load(B(f.knuckleX.x))).Store(B(f.knuckleX.x));
    num::Cross3(f.strutAxis, f.knuckleX, f.knuckleY);
    num::Normalize3Approx4(f.knuckleY);
    num::Cross3(f.knuckleX, f.knuckleY, f.knuckleZ);

    // N = the knuckle's rows; the hub's matrix = M N.
    std::memcpy(B(f.knuckleRows[0]), B(f.knuckleX.x), 12);
    std::memcpy(B(f.knuckleRows[3]), B(f.knuckleY.x), 12);
    std::memcpy(B(f.knuckleRows[6]), B(f.knuckleZ.x), 12);
    for (uint32_t r = 0; r < 3; ++r)
        for (uint32_t k = 0; k < 3; ++k) {
            auto n = [&](uint32_t i) { return F32(f.knuckleRows[3 * i + k]); };
            auto m = [&](uint32_t i) { return F32(f.axleRows[3 * r + i]); };
            put(f.hubMatrix[3 * r + k], (n(2) * m(2) + n(1) * m(1)) + n(0) * m(0));
        }
    // v.x row0 + v.y row1 + v.z row2, the rows read as unaligned vec4s (w lanes: the next floats).
    auto rows = [&f, &B](const Vec4& vec, float* out) {
        const F32x4 r0 = F32x4::Load(B(f.knuckleRows[0])) * F32x4::Splat(F32(vec.x));
        const F32x4 r1 = F32x4::Load(B(f.knuckleRows[3])) * F32x4::Splat(F32(vec.y));
        const F32x4 r2 = F32x4::Load(B(f.knuckleRows[6])) * F32x4::Splat(F32(vec.z));
        ((r0 + r1) + r2).Store(out);
    };
    rows(D.strutHubPoint, &b.position.x);
    b.position.x = (F32(b.position.x) + F32(f.wishboneEnd.x)).f();
    b.position.y = (F32(b.position.y) + F32(f.wishboneEnd.y)).f();
    b.position.z = (F32(b.position.z) + F32(f.wishboneEnd.z)).f();
    std::memcpy(&contact.hubAxis, B(f.hubMatrix[0]), 12);
    const F32 drop = zero - F32(tyre.def->hubOffset);   // the hub below the point
    (F32x4::Load(&contact.hubAxis.x) * F32x4::Splat(drop) + F32x4::Load(&b.position.x)).Store(&b.position.x);
    num::Scale4(f.knuckleZ, -1.0f, contact.springAxis);
    num::Vec4Copy(contact.linkAxis, f.strutAxis);
    num::Vec4Copy(contact.linkPivot, f.wishboneEnd);
    rows(D.strutRodMount, &f.rodMount.x);
    put(f.strutTop.x, F32(f.rodMount.x) + F32(f.wishboneEnd.x));
    put(f.strutTop.y, F32(f.rodMount.y) + F32(f.wishboneEnd.y));
    put(f.strutTop.z, F32(f.rodMount.z) + F32(f.wishboneEnd.z));
    num::Vec4Copy(contact.strutTop, f.strutTop);
    put(f.steeringRod.x, (F32(f.steeringPoint.x) - F32(f.wishboneEnd.x)) - F32(f.rodMount.x));
    put(f.steeringRod.y, (F32(f.steeringPoint.y) - F32(f.wishboneEnd.y)) - F32(f.rodMount.y));
    put(f.steeringRod.z, (F32(f.steeringPoint.z) - F32(f.wishboneEnd.z)) - F32(f.rodMount.z));
    num::Scale4(f.steeringRod, (one / F32(s.steeringRodLength)).f(), f.steeringRodDir);
    num::Vec4Copy(contact.strutVector, f.steeringRod);
    num::Cross3(f.rodMount, f.steeringRodDir, f.armCross);
    const F32 ra = (F32(f.armCross.x) * F32(f.strutAxis.x) + F32(f.armCross.z) * F32(f.strutAxis.z)) + F32(f.armCross.y) * F32(f.strutAxis.y);
    const F32 rb = (F32(D.steeringAxis.y) * F32(f.steeringRodDir.y) + F32(D.steeringAxis.z) * F32(f.steeringRodDir.z)) + F32(f.steeringRodDir.x) * F32(D.steeringAxis.x);
    const F32 aa = !jmp::BE(ra.f(), 0.0f) ? ra : zero - ra;
    const F32 ab = !jmp::BE(rb.f(), 0.0f) ? rb : zero - rb;
    const float eps = FromBits(0x3a83126f);   // 0.001
    if (jmp::A(aa.f(), eps) && jmp::A(ab.f(), eps)) {
        contact.inverseArmA = (one / ra).f();
        contact.inverseArmB = (one / rb).f();
    } else {
        contact.inverseArmA = 0.0f;
        contact.inverseArmB = 0.0f;
    }
    (F32x4::Load(&contact.hubAxis.x) * F32x4::Splat(F32(D.driveAxleMountOffset)) + F32x4::Load(&b.position.x)).Store(B(f.driveAxleMount.x));
    put(f.driveAxle.x, F32(f.driveAxleMount.x) - F32(D.driveAxleJointPos.x));
    put(f.driveAxle.y, F32(f.driveAxleMount.y) - F32(D.driveAxleJointPos.y));
    put(f.driveAxle.z, F32(f.driveAxleMount.z) - F32(D.driveAxleJointPos.z));
    num::Normalize3Approx4To(f.driveAxle, contact.driveAxleDir);
    if (renderGeometry) {
        num::Vec4Copy(contact.debugAxes[0], f.knuckleX);
        num::Vec4Copy(contact.debugAxes[1], f.knuckleY);
        num::Vec4Copy(contact.debugAxes[2], f.knuckleZ);
    }
    num::QuatFromMat33(reinterpret_cast<const float*>(B(f.hubMatrix[0])), b.orientation);
}


// ---- the ground probe

namespace {
Vec4 AsVec4(const num::Vec3& v, float w) { return {v.x, v.y, v.z, w}; }
} // namespace

uint32_t Wheel::RingSector(uint32_t* out, int32_t n) const
{
    const X87 a = X87(suspension.spinAngle) + X87(62.83185577392578f);   // + 20pi
    const X87 nn = num::FildUnsigned(n);
    const float step = (X87(6.2831854820251465f) / nn).Store();
    const int32_t i = num::Ftol((nn * a) * X87(0.15915493667125702f));
    const X87 f = (a - num::FildUnsigned(i) * X87(step)) / X87(step);
    const uint32_t un = static_cast<uint32_t>(n), ui = static_cast<uint32_t>(i);
    out[0] = ui % un;
    const float frac = (X87(1.0f) - f).Store();
    std::memcpy(&out[2], &frac, 4);
    out[1] = (ui + 1u) % un;
    return (ui + 1u) / un;
}

void Wheel::RingTableBlend()
{
    uint32_t s[3] = {};
    RingSector(s, 4);
    const WheelDamage& dm = *damage;
    float f;
    std::memcpy(&f, &s[2], 4);
    const F32 one = F32(1.0f), wt = one - F32(f);
    const F32 v = F32(dm.sectorsA[s[1]]) * wt + F32(dm.sectorsA[s[0]]) * F32(f);
    contact.sectorBlendA = ((one - v) * F32(0.2f) + v * F32(0.15f)).f();
    const F32 v2 = F32(dm.sectorsB[s[1]]) * wt + F32(dm.sectorsB[s[0]]) * F32(f);
    contact.sectorBlendB = ((one - v2) * F32(0.2f) + v2 * F32(0.0f)).f();
}

void Wheel::MirrorOffsets(const Vec4& dir, const Vec4& base, Vec4& o1, Vec4& o2)
{
    const F32 h = F32(tyre.def->carcassWidth) * F32(0.5f);   // half the carcass's width (the def's +0xcc)
    (F32x4::Load(&dir.x) * F32x4::Splat(h) + F32x4::Load(&base.x)).Store(&o1.x);
    (F32x4::Load(&dir.x) * F32x4::Splat(F32(0.0f) - h) + F32x4::Load(&base.x)).Store(&o2.x);
    contact.rimA = o2;
    contact.rimB = o1;
    contact.rimBShown = o1;
    contact.rimAShown = o2;
}

void Wheel::RaySurface(const World& w, WheelRay& r, float lo, float hi) const
{
    if (w.forceMaterial) {
        r.ground.material[0] = w.forceMaterialId;
        r.ground.material[1] = w.forceMaterialId;
        r.ground.blend = 0.0f;
    }
    // SurfaceBlend: the ground's offset and the soil's bottom under the ray, the bumps' waves added.
    const SurfaceMaterial& a = (*w.materials)[r.ground.material[0]];
    const SurfaceMaterial& b = (*w.materials)[r.ground.material[1]];
    const float bw = r.ground.blend;
    const float twoPi = 6.2831854820251465f;
    auto waves = [&](float k) {   // sin(x k) + sin(y k): the sin(y k) term through a float
        const float sy = X87::Raw(num::SinOfFloat((F32(r.ground.point.y) * F32(k)).f())).Store();
        return X87::Raw(num::SinOfFloat((F32(r.ground.point.x) * F32(k)).f())) + X87(sy);
    };
    float acc = 0.0f;
    if (!(a.bumpFrequency == 0.0f)) {
        const float k = (F32(twoPi) / F32(a.bumpFrequency)).f();
        acc = ((waves(k) * X87(a.bumpAmplitude)) * X87(bw)).Store();
    }
    if (!(b.bumpFrequency == 0.0f)) {
        const float k = (F32(twoPi) / F32(b.bumpFrequency)).f();
        acc = (((waves(k) * (X87(1.0f) - X87(bw))) * X87(b.bumpAmplitude)) + X87(acc)).Store();
    }
    auto lerp = [bw](float x, float y) { return ((F32(y) - F32(x)) * F32(bw) + F32(x)).f(); };   // TweakRealLerp
    const float r1 = (F32(lerp(a.solidGroundOffset, b.solidGroundOffset)) + F32(acc)).f();
    const float r2 = lerp(a.soilThickness, b.soilThickness);
    r.surfaceOffset = r1;
    r.soilOffset = (F32(r2) + F32(r1)).f();
    // SurfaceContact: how far down the soil's bottom is.
    const float depth = (F32(r.ground.t) - F32(r.soilOffset)).f();
    r.depth = depth;
    if (jmp::B(lo, depth)) {
        r.valid = 0;
        return;
    }
    r.valid = 1;
    if (jmp::BE(hi, depth)) {
        r.gain = 0.0f;
        r.unknown060 = 0.0f;
        return;
    }
    const float x = (F32(r.ground.t) - F32(r.surfaceOffset)).f();
    r.depth = hi;
    r.gain = 0.0f;
    r.unknown060 = jmp::BE(x, x) ? 0.0f : (F32(hi) - F32(x)).f();   // (the original compares x with itself: always 0)
}

void Wheel::ContactAxis(const WheelRay& ra, const WheelRay& rb, Vec4& out) const
{
    auto along = [this](const WheelRay& r) {   // the ray's normal along the roll direction
        if (!r.hit) return F32(0.0f);
        const Vec4 n = AsVec4(r.ground.normal, r.ground.plane);
        return (F32(contact.rollDir.y) * F32(n.y) + F32(contact.rollDir.z) * F32(n.z)) + F32(contact.rollDir.x) * F32(n.x);
    };
    const F32 zero = 0.0f;
    const F32 a = along(ra), b = along(rb);
    const F32 aa = !jmp::BE(a.f(), 0.0f) ? a : zero - a;
    const F32 ab = !jmp::BE(b.f(), 0.0f) ? b : zero - b;
    const F32 m = !jmp::BE(aa.f(), ab.f()) ? a : b;
    const F32 q = F32(1.0f) - m * m;
    const F32 s = !jmp::BE(q.f(), 0.0f) ? F32(Sqrt(X87(q)).Store()) : zero;
    const float dx = (F32(rb.lateral) - F32(ra.lateral)).f();
    const float dz = (F32(rb.depth) - F32(ra.depth)).f();
    const F32 inv = (X87(1.0f) / Sqrt(X87(dx) * X87(dx) + X87(dz) * X87(dz))).Store();
    F32 c1 = (inv * F32(dx)) * s;
    F32 c2 = zero - (inv * F32(dz)) * s;
    if (!jmp::BE(c1.f(), 0.0f)) {
        c1 = zero - c1;
        c2 = zero - c2;
    }
    num::Scale4(contact.rollDir, m.f(), out);
    (F32x4::Load(&contact.hubAxisWorld.x) * F32x4::Splat(c2) + F32x4::Load(&out.x)).Store(&out.x);
    (F32x4::Load(&contact.down.x) * F32x4::Splat(c1) + F32x4::Load(&out.x)).Store(&out.x);
}

void Wheel::SurfaceBlend(const World& w, uint32_t a, uint32_t b, float u)
{
    const SurfaceMaterial& A = (*w.materials)[a];
    const SurfaceMaterial& B = (*w.materials)[b];
    float s = 0.0f, v = 0.0f;
    tyre.PairTableScale(static_cast<int32_t>(A.type), static_cast<int32_t>(B.type), A.waterFactor, B.waterFactor, &s, &v);
    // ParamBlend: the frictions weighted by the pair table too, the rest by u alone.
    TyreSurface& o = contact.surface;
    const F32 w0 = F32(1.0f) - F32(u);
    const F32 wa = w0 * F32(s);
    const F32 wb = F32(v) * F32(u);
    auto ba = [&](float SurfaceMaterial::*f) { return (F32(B.*f) * wb + F32(A.*f) * wa).f(); };
    auto ab = [&](float SurfaceMaterial::*f) { return (F32(A.*f) * w0 + F32(B.*f) * F32(u)).f(); };
    o.kineticFriction = ba(&SurfaceMaterial::kineticFriction);
    o.staticFriction = ba(&SurfaceMaterial::staticFriction);
    o.unknown02c = ab(&SurfaceMaterial::unknown02c);
    o.solidGroundOffset = ab(&SurfaceMaterial::solidGroundOffset);
    o.unknown01c = ab(&SurfaceMaterial::unknown01c);
    o.soilDensity = ab(&SurfaceMaterial::soilDensity);
    o.sinkFactor = ab(&SurfaceMaterial::sinkFactor);
    o.soilThickness = ab(&SurfaceMaterial::soilThickness);
    o.soilParticleMass = ab(&SurfaceMaterial::soilParticleMass);
    o.staticSoilFriction = ba(&SurfaceMaterial::staticSoilFriction);
    o.kineticSoilFriction = ba(&SurfaceMaterial::kineticSoilFriction);
    o.rollingResistance = ab(&SurfaceMaterial::rollingResistance);
    o.waterFactor = ab(&SurfaceMaterial::waterFactor);
    contact.surfaceA = a;
    contact.surfaceB = b;
    contact.surfaceBlend = u;
}

bool Wheel::GroundProbe(World& w, Vec4& point, Vec4& normal)
{
    WheelContact& c = contact;
    const float R = tyre.state->rollingRadius;   // the tyre's radius: the rays' reach
    c.rayA.lateral = FromBits(0xbdcccccd);   // -0.1
    c.rayCentre.lateral = 0.0f;
    c.rayB.lateral = FromBits(0x3dcccccd);
    float best = 2.0f;
    c.movingGround = nullptr;
    RingTableBlend();
    const float ext[4] = {0.4f, 0.4f, 0.4f, 0.0f};
    std::vector<const StageCollision::Node*> leaves;
    w.collision->BoxQuery(num::Vec3{c.hub.x, c.hub.y, c.hub.z}, num::Vec3{ext[0], ext[1], ext[2]}, leaves);
    static thread_local World::MeshList list;
    w.MeshListBoxQuery(&c.hub.x, ext, 8, nullptr, list);
    const Vec4& dir = c.down;
    // One ray from `origin` along dir: the ground and the objects' meshes; a nearer mesh hit wins unless its object is very
    // soft (vslot 85); a solid nearer than `limit` is remembered (movingGround).
    auto ray = [&](WheelRay& rec, const Vec4& origin, Vec4& end, float limit, bool keepBest) {
        (F32x4::Load(&dir.x) * F32x4::Splat(F32(1.0f)) + F32x4::Load(&origin.x)).Store(&end.x);
        rec.hit = w.collision->LeavesRaycast(*w.maps, num::Vec3{origin.x, origin.y, origin.z}, num::Vec3{end.x, end.y, end.z}, leaves, rec.ground) ? 1 : 0;
        GroundHit hit{};
        float hitW = 0.0f;
        Entity* const id = w.MeshListSegmentQuery(&origin.x, &end.x, list, hit, hitW);
        if (id && !id->VerySoft()) {
            const float tm = hit.t;
            if (!rec.hit || jmp::A(rec.ground.t, tm)) {
                rec.hit = 1;
                rec.ground = hit;   // (the contact and the word after it: 0)
                rec.pointW = hitW;
                rec.after = 0;
                if ((id->flags & kSolid) && jmp::A(limit, tm)) {
                    c.movingGround = id;
                    if (keepBest) best = tm;
                }
            }
        }
    };
    WheelRay& recA = c.rayA;
    WheelRay& recB = c.rayB;
    WheelRay& recC = c.rayCentre;
    Vec4 endA, endB, endC;
    ray(recA, c.rimA, endA, 2.0f, true);
    if (recA.hit) RaySurface(w, recA, R, c.sectorBlendB);
    else recA.valid = 0;
    ray(recB, c.rimB, endB, best, true);
    if (recB.hit) RaySurface(w, recB, R, c.sectorBlendB);
    else recB.valid = 0;
    {   // the centre ray: its ends kept for display
        Vec4 e;
        (F32x4::Load(&dir.x) * F32x4::Splat(F32(1.0f)) + F32x4::Load(&c.hub.x)).Store(&e.x);
        num::Vec4Copy(c.centreRayFrom, c.hub);
        num::Vec4Copy(c.centreRayTo, e);
        num::Vec4Copy(c.centreRayDir, dir);
    }
    ray(recC, c.hub, endC, best, false);
    c.quadDepth = 0.0f;
    if (!recC.hit) {
        recC.valid = 0;
    } else {
        RaySurface(w, recC, R, FromBits(0x3e4ccccd));   // 0.2
        Vec4 qout;
        float tq;
        if (w.GridQuadSegmentQuery(&c.hub.x, &endC.x, &qout, &tq) && jmp::A(R, tq)) c.quadDepth = (F32(R) - F32(tq)).f();
    }
    const bool ab = recA.valid || recB.valid;
    if (!recC.valid && !ab) return false;
    num::Cross3(c.hubAxisWorld, dir, c.rollDir);
    const F32 half = F32(0.5f);
    float d = R;
    if (recA.valid && recB.valid) {
        d = ((F32(recB.depth) + F32(recA.depth)) * half).f();
        if (recC.valid && jmp::A(d, recC.depth)) {
            d = recC.depth;
            normal = AsVec4(recC.ground.normal, recC.ground.plane);
        } else {
            ContactAxis(recA, recB, normal);
        }
    } else {
        if (!recA.valid) recA = recB;   // the valid one of A / B into A
        if (!recC.valid) recC.depth = R;
        if (ab && !recC.valid) {
            d = ((F32(recC.depth) + F32(recA.depth)) * half).f();
            normal = AsVec4(recA.ground.normal, recA.ground.plane);
        } else if (ab && jmp::A(recC.depth, recA.depth)) {
            d = ((F32(recC.depth) + F32(recA.depth)) * half).f();
            ContactAxis(recC, recA, normal);
        } else {
            d = recC.depth;
            normal = AsVec4(recC.ground.normal, recC.ground.plane);
        }
    }
    c.groundDistance = d;
    (F32x4::Load(&dir.x) * F32x4::Splat(F32(d)) + F32x4::Load(&c.hub.x)).Store(&point.x);
    if (!recC.hit) recC = recA.hit ? recA : recB;
    SurfaceBlend(w, recC.ground.material[0], recC.ground.material[1], recC.ground.blend);
    if (recC.hit) num::Vec4Copy(c.contactPoint, point);
    if (recA.hit) c.rayAHit = AsVec4(recA.ground.point, recA.pointW);
    if (recB.hit) c.rayBHit = AsVec4(recB.ground.point, recB.pointW);
    return true;
}


// ---- the wheel's step

void Wheel::ContactPush(WheelRay& r, const float* p, Vec4& out)
{
    const Vec4 n = AsVec4(r.ground.normal, r.ground.plane);
    const float dot = ((F32(p[0]) * F32(n.x) + F32(p[1]) * F32(n.y)) + F32(p[2]) * F32(n.z)).f();
    Vec4 wn;
    num::Scale4(n, dot, wn);
    Vec4 d;
    d.x = (F32(p[0]) - F32(wn.x)).f();
    d.y = (F32(p[1]) - F32(wn.y)).f();
    d.z = (F32(p[2]) - F32(wn.z)).f();
    d.w = 0.0f;   // (the original's stack)
    const float len = num::Length3(d);
    const F32 q = ((F32(contact.down.y) * F32(n.y) + F32(contact.down.z) * F32(n.z)) + F32(contact.down.x) * F32(n.x)) * F32(r.gain);
    const F32 k = (F32(0.0f) - q) * F32(800000.0f);
    (F32x4::Load(&n.x) * F32x4::Splat(k) + F32x4::Load(&out.x)).Store(&out.x);
    if (jmp::A(len, FromBits(0x3a83126f))) {   // 0.001
        const F32 k2 = ((F32(-1.0f) / F32(len)) * k) * F32(FromBits(0x3e99999a));   // 0.3
        (F32x4::Load(&d.x) * F32x4::Splat(k2) + F32x4::Load(&out.x)).Store(&out.x);
    }
    num::Vec4Copy(contact.pushOffset, d);
}

bool Wheel::ContactForces(Vec4& out)
{
    WheelContact& c = contact;
    const bool b = c.rayB.valid && !(c.rayB.gain == 0.0f);
    const bool a = c.rayA.valid && !(c.rayA.gain == 0.0f);
    if (!b && !a) return false;
    Vec4 x = {};   // (x.w: the original's stack, unused)
    num::Cross3(c.hubAxisWorld, c.down, x);
    const F32 s = F32(c.spinRate) * F32(FromBits(0x3e4ccccd));   // 0.2
    (F32x4::Splat(s) * F32x4::Load(&x.x)).Store(&x.x);
    const float p[4] = {(F32(c.contactVelocity.x) + F32(x.x)).f(), (F32(c.contactVelocity.y) + F32(x.y)).f(), (F32(c.contactVelocity.z) + F32(x.z)).f(), 0.0f};
    if (b) ContactPush(c.rayB, p, out);
    if (a) ContactPush(c.rayA, p, out);
    return true;
}

void Wheel::BodySpin()
{
    Vec4& spin = contact.carSpin;
    FrameMoveCopy(WorldOf(car->frame), car->body.angularVelocity, car->frame, spin, Move::Dirs);   // the car's spin in its frame
    const F32 a = F32(contact.inverseArmB);
    const F32 abs = !jmp::BE(a.f(), 0.0f) ? a : F32(0.0f) - a;
    float s = 0.0f;   // the steering's turn (its rate)
    if (!jmp::BE(abs.f(), 0.001f)) s = (X87(car->carState.steeringRate) * (X87(contact.inverseArmA) / X87(contact.inverseArmB))).Store();
    (F32x4::Load(&contact.linkAxis.x) * F32x4::Splat(s) + F32x4::Load(&spin.x)).Store(&spin.x);
}

void Wheel::SpinCross(Vec4& out)
{
    BodySpin();
    const F32 s = F32(contact.spinRate);
    Vec4 a;   // the wheel's spin about the hub, with the car's
    (F32x4::Load(&contact.hubAxis.x) * F32x4::Splat(s) + F32x4::Load(&contact.carSpin.x)).Store(&a.x);
    Vec4 b;
    num::Scale4(contact.hubAxis, (s * F32(0.14f)).f(), b);
    num::Cross3(a, b, out);   // the gyroscopic torque
}

void Wheel::SuspensionStep(World& w, float dt)
{
    const WheelDef& D = *wheelDef;
    WheelSuspension& su = suspension;
    WheelContact& c = contact;
    CarState& L = car->carState;
    Drive& dr = car->drive;
    const Frame& root = *w.world;
    const Frame& cf = car->frame;   // the car body's frame
    Vec4 S50{}, S40{};   // the force and torque on the car body
    float S20 = 0.0f, S2c = 0.0f;
    // The travel clamped to the spring-damper's (its bump rubbers' less 0.05, its droop rubber's plus 0.02).
    const SpringDamperDef& P = *springDamper.def;
    const float lo = (F32(P.innerBumpRubberPos) - F32(0.05f)).f(), hi = (F32(P.outerBumpRubberPos) + F32(0.02f)).f(), x0 = su.travel;
    su.travel = jmp::A(lo, x0) ? lo : (jmp::A(x0, hi) ? hi : x0);
    SuspensionUpdate(su.travel, L.steeringAngle, (w.debugFlags & kRenderSuspensionGeometry) != 0);
    const float* const rot = &body.rotation.m[0];   // its rows 2 and 1 (w: the original's stack)
    const Vec4 S160{rot[6], rot[7], rot[8], 0.0f}, S120{rot[3], rot[4], rot[5], 0.0f};
    Vec4 S80, S150;
    FrameMoveCopy(cf, body.position, root, S80, Move::Points);
    FrameMoveCopy(cf, S160, root, S150, Move::Dirs);
    FrameMoveCopy(cf, c.hubAxis, root, c.hubAxisWorld, Move::Dirs);
    Vec4 S190, S170;
    MirrorOffsets(c.hubAxisWorld, S80, S190, S170);
    num::Scale4(S150, -1.0f, c.down);
    num::Vec4Copy(c.hub, S80);
    c.spinRate = dr.state->slots[slot].spin;
    float S7c = 0.0f, S34 = 0.0f;
    Vec4 SD0{}, S90{};
    Vec4 S130{}, S140{};
    const bool contacted = (w.physicsFlags & kCheckWheelColl) && GroundProbe(w, S130, S140);
    if (contacted) {
        onGround = 1;
        Vec4 S1c0, SA0, SE0{};
        FrameMoveCopy(cf, c.springAxis, root, S1c0, Move::Dirs);
        FrameMoveCopy(cf, body.position, root, SA0, Move::Dirs);
        num::Cross3(car->body.angularVelocity, SA0, SE0);   // the contact point's velocity: v + w x r
        SE0.x = (F32(car->body.velocity.x) + F32(SE0.x)).f();
        SE0.y = (F32(car->body.velocity.y) + F32(SE0.y)).f();
        SE0.z = (F32(car->body.velocity.z) + F32(SE0.z)).f();
        (F32x4::Load(&S1c0.x) * F32x4::Splat(F32(su.travelRate)) + F32x4::Load(&SE0.x)).Store(&SE0.x);   // + the suspension's rate
        num::Vec4Copy(c.contactVelocity, SE0);
        TireParticles particles{};
        std::memcpy(&particles.hub, &S80, 16);
        Vec4 S1b0{}, S1a0{}, S180;
        if (Entity* const g = c.movingGround) {   // a moving ground: relative to its velocity
            Solid* const gs = g->AsSolid();
            Vec4 S1d0{};
            if (gs) gs->body.PointVelocity(S130, S1d0);
            else Unreached("a moving ground that is not a solid");
            S180 = Vec4{(F32(c.contactVelocity.x) - F32(S1d0.x)).f(), (F32(c.contactVelocity.y) - F32(S1d0.y)).f(), (F32(c.contactVelocity.z) - F32(S1d0.z)).f(), 0.0f};
        } else {
            num::Vec4Copy(S180, c.contactVelocity);
        }
        float surfaceEntries[3];   // the contact's surface entries and blend, as the tyre's record keeps them
        std::memcpy(&surfaceEntries[0], &c.surfaceA, 4);
        std::memcpy(&surfaceEntries[1], &c.surfaceB, 4);
        surfaceEntries[2] = c.surfaceBlend;
        TyreStepInput in;
        in.spinRate = c.spinRate, in.dt = dt, in.hubHeight = c.groundDistance, in.quadDepth = c.quadDepth;
        in.surface = &c.surface, in.contact = surfaceEntries;
        in.normal = S140, in.axle = c.hubAxisWorld, in.velocity = S180, in.point = S130;
        in.spinAngle = su.spinAngle;
        in.renderMaterial = (w.debugFlags & kRenderMaterial) != 0;
        tyre.Step(in, S1b0, S1a0, S20, S2c, particles);
        ContactForces(S1b0);
        particles.slot = slot;
        num::Vec4Copy(particles.groundNormal, S140);
        std::memcpy(particles.surface, surfaceEntries, 12);
        if (w.tyreParticles) w.tyreParticles(particles);
        FrameMoveCopy(root, S1b0, cf, SD0, Move::Dirs);   // force and torque into the car body
        FrameMoveCopy(root, S1a0, cf, S90, Move::Dirs);
        S7c = ((F32(S90.z) * F32(S120.z) + F32(S90.y) * F32(S120.y)) + F32(S90.x) * F32(S120.x)).f();
        S34 = ((F32(c.springAxis.y) * F32(SD0.y) + F32(c.springAxis.z) * F32(SD0.z)) + F32(SD0.x) * F32(c.springAxis.x)).f();
        const F32 S18 = F32(0.0f) - F32(S34);
        Vec4 S60;
        (F32x4::Load(&c.springAxis.x) * F32x4::Splat(S18) + F32x4::Load(&SD0.x)).Store(&S60.x);   // its lateral part
        S50.x = (F32(S60.x) + F32(S50.x)).f();
        S50.y = (F32(S60.y) + F32(S50.y)).f();
        S50.z = (F32(S60.z) + F32(S50.z)).f();
        S40.x = (F32(S40.x) + F32(S90.x)).f();
        S40.y = (F32(S40.y) + F32(S90.y)).f();
        S40.z = (F32(S40.z) + F32(S90.z)).f();
        tyre.GapAboveRest(c.groundDistance);
    } else {
        onGround = 0;
        tyre.GapAboveRest(0.0f);
        tyre.state->stepped = 0;
        tyre.record.inContact = 0;
    }
    // The spring-damper and the roll bar.
    float S24, S28, S30;
    springDamper.Force(su.travel, (F32(D.springOffset) + F32(su.travel)).f(), su.travelRate, &S24, &S28, &S30);
    const float bar = L.rollBars[D.antiRollBarId].torque;
    const float S38 = D.antiRollBarSlot ? -bar : bar;
    const F32 m = F32(body.def->inverseMass), t = F32(dt);
    F32 r = (((((F32(S38) + F32(S24)) + F32(S28)) + F32(S34)) * m) * t + F32(su.travelRate)) / ((m * F32(S30)) * t + F32(1.0f));   // the new rate
    if (jmp::A(-100.0f, r.f())) r = F32(-100.0f);
    else if (jmp::A(r.f(), 100.0f)) r = F32(100.0f);
    const F32 old = F32(su.travelRate);
    su.travel = (r * t + F32(su.travel)).f();
    su.travelRate = r.f();
    const float S34b = (F32(S34) - ((r - old) / t) / m).f();
    (F32x4::Load(&c.springAxis.x) * F32x4::Splat(F32(S34b)) + F32x4::Load(&S50.x)).Store(&S50.x);
    c.shownRollBar = S38;
    c.shownSpring = S24;
    c.shownLateral = S34b;
    const F32 rs = r * F32(S30);
    S34 = ((rs + F32(S24)) + F32(S28)).f();
    c.shownDamper = (rs + F32(S28)).f();
    // The torque on the car body: (wheel - the top mount) x force + torques.
    const Vec4& pos = body.position;
    const Vec4 S110{(F32(pos.x) - F32(D.topMount.x)).f(), (F32(pos.y) - F32(D.topMount.y)).f(), (F32(pos.z) - F32(D.topMount.z)).f(), 0.0f};
    Vec4 SC0;
    num::Cross3(S110, S50, SC0);
    SC0.x = (F32(SC0.x) + F32(S40.x)).f();
    SC0.y = (F32(SC0.y) + F32(S40.y)).f();
    SC0.z = (F32(SC0.z) + F32(S40.z)).f();
    if (D.isSteered) {
        const F32 k = (((F32(c.linkAxis.z) * F32(SC0.z) + F32(c.linkAxis.y) * F32(SC0.y)) + F32(c.linkAxis.x) * F32(SC0.x)) * F32(c.inverseArmA)) * F32(c.inverseArmB);
        L.steeringTorque = (F32(L.steeringTorque) + k).f();
    }
    Vec4 S100;
    num::Cross3(pos, S50, S100);
    S40.x = (F32(S100.x) + F32(S40.x)).f();
    S40.y = (F32(S100.y) + F32(S40.y)).f();
    S40.z = (F32(S100.z) + F32(S40.z)).f();
    const F32 k38 = F32((X87(dr.state->slots[slot].spinAcceleration) * X87(-0.7f)).Store());
    (F32x4::Load(&c.hubAxis.x) * F32x4::Splat(k38) + F32x4::Load(&S40.x)).Store(&S40.x);
    Vec4 SF0;
    SpinCross(SF0);
    c.force.x = (F32(S50.x) + F32(c.force.x)).f();
    c.force.y = (F32(S50.y) + F32(c.force.y)).f();
    c.force.z = (F32(S50.z) + F32(c.force.z)).f();
    S40.x = (F32(S40.x) - F32(SF0.x)).f();
    S40.y = (F32(S40.y) - F32(SF0.y)).f();
    S40.z = (F32(S40.z) - F32(SF0.z)).f();
    c.torque.x = (F32(S40.x) + F32(c.torque.x)).f();
    c.torque.y = (F32(S40.y) + F32(c.torque.y)).f();
    c.torque.z = (F32(S40.z) + F32(c.torque.z)).f();
    if (w.physicsFlags & kEnableDamage) {   // the top mount's wear and the inclination knocked out by the loads
        WheelDamage& G = *damage;
        const F32 x = F32(G.loadFactor) * F32(S34);
        if (jmp::A(x.f(), D.wearLoadStart)) {
            const F32 u = (x - F32(D.wearLoadStart)) / (F32(D.wearLoadFull) - F32(D.wearLoadStart));
            G.topMountUp = (u * u + F32(G.topMountUp)).f();
            if (jmp::A(G.topMountUp, 1.0f)) G.topMountUp = 1.0f;
            TopMountDeform();
            damageChanged = 1;
        }
        SteerReduce((F32(damage->loadFactor) * F32(S7c)).f());
        WheelDamage& G2 = *damage;
        const F32 y = ((F32(c.hubAxis.z) * F32(S50.z) + F32(c.hubAxis.y) * F32(S50.y)) + F32(S50.x) * F32(c.hubAxis.x)) * F32(G2.loadFactor);
        const F32 z = F32(0.0f) - y;
        if (jmp::A(z.f(), 28000.0f)) {   // the rod bent by a big side load
            if (jmp::A(z.f(), 60000.0f)) {
                G2.rodBend = 1.0f;
            } else {
                const F32 v = (z - F32(28000.0f)) * F32(3.125000148429535e-05f);
                if (jmp::A(v.f(), G2.rodBend)) G2.rodBend = v.f();
            }
            damageChanged = 1;
        }
    }
    dr.slotTorques[slot] = S20;
    dr.slotBrakeExtra[slot] = S2c;
    L.rollBars[D.antiRollBarId].travel[D.antiRollBarSlot] = (F32(su.travel) - F32(damage->topMountUp) * F32(D.topMountMaxDeformUp)).f();
    // The spin angle, wrapped by 20 pi (as the original: down above it, up below it).
    su.spinAngle = (F32((X87(dr.state->slots[slot].spin) * X87(dt)).Store()) + F32(su.spinAngle)).f();
    const float wrap = 62.83185577392578f, a = su.spinAngle;
    if (jmp::A(a, wrap)) su.spinAngle = (F32(a) - F32(wrap)).f();
    else if (jmp::A(wrap, a)) su.spinAngle = (F32(a) + F32(wrap)).f();
    tyre.HeatTick(dt);
    shownSpinRate = dr.state->slots[slot].spin;
}

} // namespace phys::native
