#include "native/tyre.h"

#include <cstddef>
#include <cstring>

#include "num/curves.h"
#include "num/f32.h"
#include "num/bits.h"
#include "num/trig.h"
#include "num/x87.h"
#include "num/x87math.h"

namespace phys::native {

using num::F32;
using num::F32x4;
using num::X87;
using num::FromBits;
namespace jmp = num::jmp;

namespace {
// A lerp between two entries of a def table by a bracket: (1 - t) [i0] + [i1] t.
F32 Lerp(const float* table, const Bracket& b) { return (F32(1.0f) - F32(b.t)) * F32(table[b.i0]) + F32(table[b.i1]) * F32(b.t); }
F32 Abs(F32 v) { return !jmp::BE(v.f(), 0.0f) ? v : F32(0.0f) - v; }   // comiss v, 0; jbe
} // namespace

void Tyre::GapAboveRest(float x)
{
    const float s = (F32(def->treadDepth) + F32(def->carcassRadius)).f();   // the outer radius
    const bool gap = !jmp::BE(x, 0.0f) && !jmp::BE(s, x);
    state->gap = gap ? (F32(s) - F32(x)).f() : 0.0f;
}

void Tyre::PairTableScale(int32_t i, int32_t j, float x, float y, float* p1, float* p2) const
{
    const TyreDef& D = *def;
    *p1 = D.surfaceFriction[i];
    *p2 = D.surfaceFriction[j];
    const F32 d1 = Abs(F32(D.wetnessDesign) - F32(x));
    const F32 d2 = Abs(F32(D.wetnessDesign) - F32(y));
    *p1 = (F32(D.wetSlope[i]) * d1 + F32(*p1)).f();
    *p2 = (F32(D.wetSlope[j]) * d2 + F32(*p2)).f();
    const F32 one = F32(1.0f);
    const F32 s = F32(wear[state->contactElement]);   // the element on the ground worn
    *p1 = (((F32(D.wearFactor[i]) - one) * s + one) * F32(*p1)).f();
    *p2 = (((F32(D.wearFactor[j]) - one) * s + one) * F32(*p2)).f();
}

void Tyre::RangeBlend()
{
    float a = 1.0f, b = 0.0f;   // grip below 1 m/s of slip, slide above 2
    if (!jmp::BE(2.0f, slipSpeed)) {
        if (!jmp::BE(1.0f, slipSpeed)) {
            a = 0.0f;
            unknown380 = 0.0f;
            unknown384 = 0.0f;
            b = 1.0f;
        } else {
            a = (F32(slipSpeed) - F32(1.0f)).f();
            b = (F32(1.0f) - F32(a)).f();
        }
    }
    gripBlend = a;
    slideBlend = b;
}

X87 Tyre::SignedProduct() const
{
    const float k = ((F32(contactLength) * F32(surface.rollingResistance)) * F32(0.5f)).f();
    const float s = spinRate;
    const int32_t sign = !jmp::BE(s, 0.0f) ? 1 : (jmp::A(0.0f, s) ? -1 : 0);
    return Neg(((X87(k) * X87(load)) * X87(gripBlend)) * X87::Raw(sign));
}

void Tyre::UnitAbs2(float a, float b, float c, float* o1, float* o2, float* o3) const
{
    const X87 x = X87(a) * X87(state->rollingRadius) + X87(c);
    const float xf = x.Store();
    const float r = Sqrt(x * X87(xf) + X87(b) * X87(b)).Store();
    if (jmp::BE(r, 0.001f)) {
        *o1 = 1.0f;
        *o2 = 1.0f;
        *o3 = 2.0f;
        return;
    }
    const F32 inv = F32(1.0f) / F32(r);
    const F32 cb = inv * F32(b);
    *o1 = (!jmp::BE(cb.f(), 0.0f) ? cb : F32(0.0f) - cb).f();
    F32 d = inv * F32(xf);
    if (!jmp::A(d.f(), 0.0f)) d = F32(0.0f) - d;
    *o2 = d.f();
    *o3 = (F32(*o1) + d).f();
}

bool Tyre::AxisFrame(const Vec4& a, const Vec4& b)
{
    Vec4& n = forward;   // the rolling direction: normal x axle
    num::Cross3(a, b, n);
    const float len = num::Length3(n);
    if (jmp::BE(len, 0.1f)) return false;
    const F32 inv = F32(1.0f) / F32(len);
    (F32x4::Splat(inv) * F32x4::Load(&n.x)).Store(&n.x);
    Vec4& c = lateral;
    num::Cross3(n, a, c);
    std::memcpy(&normal, &a, 16);
    const F32 da = (F32(a.y) * F32(b.y) + F32(a.z) * F32(b.z)) + F32(a.x) * F32(b.x);
    normalDotAxle = (F32(0.0f) - da).f();
    camberCos = ((F32(c.y) * F32(b.y) + F32(c.z) * F32(b.z)) + F32(c.x) * F32(b.x)).f();
    return true;
}

void Tyre::HeatExchange(int32_t i, float x)
{
    TyreState& O = *state;
    const TyreDef& D = *def;
    float* const tread = &O.elementTemperatures[3 * i + 1];
    const float q = *tread;
    F32 s = F32(slipSpeed) * F32(0.1f);
    if (!jmp::BE(s.f(), 1.0f)) s = F32(1.0f);
    const float c = ((s * F32(0.5f) + F32(1.0f)) * F32(30.0f)).f();
    const float k = FromBits(0x3d6631f9);   // 0.0562
    const X87 r1 = num::SubMulMul(q, FromBits(0x43951333), c, k);   // to the air, 298.15 K
    const float r1f = r1.Store();
    heatExchange = (r1 * X87(8.0f)).Store();
    const float d = ((F32(O.carcassTemperature) - F32(q)) * F32(FromBits(0x4479ffff))).f();
    const float r2 = num::NegMulMul(d, D.treadHeatCapacity[2], k).Store();
    const float v1 = Neg((X87(r2) + X87(r1f)) * X87(x)).Store();
    num::AddDivProduct(D.treadHeatCapacity, v1, tread);
    const float v2 = (X87(r2) * X87(x)).Store();
    num::AddDivProduct(D.carcassHeatCapacity, v2, &O.carcassTemperature);
}

void Tyre::AxisProject(const Vec4& v, float x)
{
    auto dot = [&v](const Vec4& r) { return ((F32(r.y) * F32(v.y) + F32(r.z) * F32(v.z)) + F32(r.x) * F32(v.x)).f(); };
    velocityLateral = dot(lateral);
    velocityForward = dot(forward);
    velocityNormal = dot(normal);
    const float s = (F32(velocityForward) * F32(velocityForward) + F32(velocityLateral) * F32(velocityLateral)).f();
    slipSpeed2 = s;
    slipSpeed = Sqrt(X87(s)).Store();
    slipVelocity = (F32(state->rollingRadius) * F32(x) + F32(velocityForward)).f();
    slipRatio = ((F32(-1.0f) / F32(state->rollingRadius)) * F32(velocityForward) - F32(x)).f();
}

void Tyre::WheelRecord(const Vec4& a, const float* b, float x)
{
    TyreRecord& R = record;
    std::memcpy(&R.contactPoint, &contactPoint, 16);
    std::memcpy(&R.contactVelocity, &a, 16);
    R.spinRate = spinRate;
    const F32 zero = F32(0.0f);
    const F32 w = F32(slipRatio);
    const F32 aw = !jmp::BE(w.f(), 0.0f) ? w : zero - w;
    R.slipSpeed = slipSpeed;
    const TyreDef& D = *def;
    R.slipRatio = aw.f();
    R.debugId = D.debugId;
    const float m = !jmp::BE(slipCurveLong, slipCurveLat) ? slipCurveLong : slipCurveLat;
    R.squeal = (F32(m) * F32(0.75f) + F32(0.5f)).f();
    R.load = load;
    std::memcpy(R.surface, b, 8);   // the surface entries and the blend
    R.unknown018 = 320.0f;
    R.work = (((F32(workDrive) + F32(workBrake)) + F32(workSlip)) * F32(1000.0f)).f();
    std::memcpy(&R.surfaceBlend, b + 2, 4);
    const F32 d = (((F32(D.treadDepth) + F32(D.carcassRadius)) - F32(hubHeight)) - F32(state->gap)) / F32(x);
    if (!jmp::BE(d.f(), R.compression)) R.compression = d.f();   // a running maximum
    const F32 e = F32(velocityForward) - F32(slipVelocity);
    R.slide = (jmp::A(e.f(), 0.0f) ? e : zero - e).f();
    R.inContact = 1;
}

void Tyre::CoefficientsTo(float a1, float a2, float a4, float a5, float* const* o) const
{
    const float args[6] = {0.0f, a1, a2, 0.0f, a4, a5};
    auto f32 = [&args](int k) { return F32(args[k]); };
    auto out = [o](int k) { return o[k - 7]; };
    std::memcpy(out(7), &cornerStiffness, 4);
    std::memcpy(out(8), &tractionStiffness, 4);
    const F32 one = F32(1.0f);
    auto lerp = [&](const float* table) {   // by the load
        const Bracket& br = state->loadBracket;
        const F32 f = F32(br.t);
        return (one - f) * F32(table[br.i0]) + F32(table[br.i1]) * f;
    };
    const TyreDef& D = *def;
    const F32 v1 = lerp(D.cornerFriction), v2 = lerp(D.tractionFriction);
    const F32 s = v1 * f32(5);
    *out(10) = (s * f32(1)).f();
    *out(12) = (s * f32(2)).f();
    *out(9) = (v1 * f32(4)).f();
    *out(11) = (v2 * f32(4)).f();
    auto clamped = [&](const float* table, int k, int kc) {
        const F32 v = lerp(table);
        *out(k) = v.f();
        const F32 v3 = v * F32(3.0f);
        *out(kc) = v3.f();
        if (!jmp::BE(v3.f(), 1.0f)) *out(kc) = 1.0f;
    };
    clamped(D.peakSlipCorner, 13, 14);
    clamped(D.peakSlipTraction, 15, 16);
}

void Tyre::HeatBalance(float x)
{
    TyreState& O = *state;
    const TyreDef& D = *def;
    const float k = FromBits(0x3f0a3d71);   // 0.54
    const X87 d0 = (X87(O.innerTemperature) - X87(O.carcassTemperature)) * X87(100.0f);
    const float b = ((F32(D.treadWidth) * F32(D.carcassRadius)) * F32(FromBits(0x40c90fdb))).f();   // the tread's area (2 pi)
    const float a = d0.Store();
    const float v1 = (num::NegMulMul(a, D.carcassHeatCapacity[2], b) * X87(x)).Store();
    const float v2 = (num::SubMulMul(O.innerTemperature, O.gasTemperature, 30.0f, k) * X87(x)).Store();
    const X87 r3 = num::SubMulMul(O.innerTemperature, FromBits(0x43951333), 20.0f, k);   // 298.15
    const float v3 = r3.Store();
    heatLoss = v3;
    num::AddDivProduct(D.carcassHeatCapacity, (F32(0.0f) - F32(v1)).f(), &O.carcassTemperature);
    const float w = ((X87(v1) - X87(v2)) - X87(v3) * X87(x)).Store();
    num::AddDivProduct(D.gasHeatCapacity, w, &O.innerTemperature);
    num::AccumulateRate(&O.gasMass, v2, FromBits(0x4439b6dc), FromBits(0x438f8000));   // 742.857, 287
    HeatExchange(static_cast<int32_t>(O.element & 7), (F32(x) * F32(8.0f)).f());
}

void Tyre::HeatTick(float x)
{
    HeatBalance(x);
    ++state->element;   // the next element
}

void Tyre::ContactSolve(float* S, float* O, const float* P, uint32_t depth, int regime)
{
    const bool series = regime >= 2;                   // k from both stiffnesses; S5 recomputed
    const bool tangent = regime == 0 || regime == 2;   // the P6 term; O1 from the quadratic
    const F32 zero = 0.0f, one = 1.0f;
    const F32 c0 = surface.soilThickness;
    F32 k, g;
    if (series) {
        k = one / (one / F32(P[7]) + one / F32(P[2]));
        g = F32(S[0]) * k;
    } else {
        S[5] = c0.f();
        k = P[2];
        g = (F32(S[0]) - c0) * k;
    }
    const F32 nk = zero - k;
    F32 num = F32(P[1]) - nk;
    if (tangent) num = num - (zero - F32(P[6]));
    const F32 u = num / F32(P[0]);
    const F32 ds = F32(S[2]) - F32(S[3]);
    const X87 h = X87(u) * X87(-0.5f);
    const X87 q = (X87(u) * X87(u)) * X87(0.25f);
    const X87 m = tangent ? (X87(ds) * X87(P[6]) + X87(g)) * (X87(-1.0f) / X87(P[0])) : (X87(-1.0f) / X87(P[0])) * X87(g);
    const X87 hr = h - Sqrt(q - m);
    const float s1 = hr.Store();
    S[1] = s1;
    const F32 o2 = (tangent ? F32(S[1]) * nk : nk * F32(S[1])) + g;   // operand order as the original
    S[6] = tangent ? (ds - F32(s1)).f() : ((X87(S[2]) - X87(S[3])) - hr).Store();
    O[2] = o2.f();
    if (series) S[5] = ((F32(S[0]) - F32(S[1])) - o2 / F32(P[2])).f();
    const bool a = series ? !jmp::BE((F32(S[5]) - c0).f(), FromBits(0x3727c5ac))   // > 1e-5
                          : !jmp::BE(o2.f(), FromBits(0xb727c5ac));                 // > -1e-5
    const bool b = tangent ? !jmp::BE(S[6], FromBits(0xb727c5ac)) : !jmp::BE(S[6], FromBits(0x3727c5ac));
    const bool wantA = regime <= 1, wantB = tangent;
    if ((a == wantA && b == wantB) || depth > 3) {
        if (tangent) {
            O[1] = ((F32(P[0]) * F32(S[1]) + F32(P[1])) * F32(S[1])).f();
        } else {
            S[6] = 0.0f;
            std::memcpy(&O[1], &O[2], 4);
        }
        state->regimeA = wantA;
        state->regimeB = wantB;
        return;
    }
    ContactDispatch(S, O, P, a, b, depth);
}

void Tyre::ContactDispatch(float* S, float* O, const float* P, uint32_t a, uint32_t b, uint32_t depth)
{
    const int regime = a ? (b ? 0 : 1) : (b ? 2 : 3);
    ContactSolve(S, O, P, depth + 1, regime);
}

void Tyre::ForceWork()
{
    const TyreState& O = *state;
    const TyreDef& D = *def;
    const F32 zero = 0.0f, one = 1.0f;
    auto g = [](float v) { return F32(v); };
    const F32 p = g(contactLength) * g(solver[5]);
    const F32 gg = ((g(solver[5]) - g(solver[6])) * F32(O.workBlend) + g(solver[6])) * F32(D.carcassWidth);
    const F32 a = Abs(g(velocityLateral)) * p;
    const F32 w = g(surface.soilDensity) * a;
    const F32 b = (Abs(g(velocityForward)) * gg) * g(surface.soilDensity);
    if (!jmp::BE(0.0f, (g(slipVelocity) * g(velocityForward)).f())) {
        workDrive = (Abs(g(slipVelocity)) * gg).f();
        workBrake = 0.0f;
    } else {
        workDrive = 0.0f;
        workBrake = (Abs(g(slipVelocity)) * gg).f();
    }
    const F32 x = (b * g(velocityForward)) * zero;
    const F32 y = zero - g(velocityLateral) * w;
    const F32 q = one - p / (F32(O.rollingRadius) * F32(2.0f));
    const F32 r = one - (one - g(lateralGripScale)) * g(camberCos);
    const F32 sum = Abs(y) * r + Abs(x) * q;
    const F32 k9 = FromBits(0x3f666666), k1 = FromBits(0x3dcccccd);   // 0.9, 0.1
    workTorsional = (((one - q) * x) * zero).f();
    load = (sum + g(load)).f();
    work = sum.f();
    workSlip = a.f();
    const F32 e504 = g(workSmoothed) * k9 + sum * k1;
    camberSmoothed = (r * k1 + g(camberSmoothed) * k9).f();
    const F32 e50c = g(patchSmoothed) * k9 + q * k1;
    workLateral = ((one - r) * y).f();
    workSmoothed = e504.f();
    patchSmoothed = e50c.f();
}

void Tyre::FrictionLimit(float l, float muLatched, float mu, float a, float b, float* o1, float* o2)
{
    TyreState& o = *state;
    const F32 lim = F32(l) * F32(o.frictionLatched ? muLatched : mu);
    const F32 u = F32(0.0f) - F32(o.deflectionLateral) * F32(a);
    const F32 v = ((F32(-1.0f) / F32(o.rollingRadius)) * F32(o.deflectionTorsional)) * F32(b);
    const float m2 = (v * v + u * u).f();
    uint32_t latch;
    float mark;
    if (!jmp::BE(m2, (lim * lim).f())) {   // past the friction circle: scaled back onto it
        F32 scale = 0.0f;
        if (!jmp::BE(m2, FromBits(0x3727c5ac)) && !jmp::BE(l, FromBits(0x3a83126f))) {   // 1e-5, 0.001
            const float r = ((X87(l) * X87(mu)) / Sqrt(X87(m2))).Store();
            scale = jmp::A(r, 1.0f) ? F32(1.0f) : F32(r);
        }
        *o1 = (F32(o.deflectionLateral) * scale).f();
        *o2 = (F32(o.deflectionTorsional) * scale).f();
        latch = 1;
        mark = 2.0f;
    } else {
        *o1 = o.deflectionLateral;
        *o2 = o.deflectionTorsional;
        latch = 0;
        mark = 0.0f;
    }
    o.frictionLatched = latch;
    slideMarkLong = mark;
    slideMarkLat = mark;
}

void Tyre::FrictionStep()
{
    if (!jmp::BE(slideBlend, 0.0f)) {
        FrictionLimit(load, surface.staticFriction, surface.kineticFriction, stiffnessLateral, stiffnessTorsional, &slideLateral, &slideTorsional);
        return;
    }
    slideLateral = 0.0f;
    slideTorsional = 0.0f;
    state->frictionLatched = 0;
}

namespace {
X87 SlipCurve(float x, float p, float lim, float F, float F2, float d, float e, float y0, float y1, float* a, float* b, const float* table,
              uint32_t n)
{
    const F32 one = 1.0f, two = 2.0f, three = 3.0f;
    const F32 c = F32(F) * F32(e);
    const F32 p0 = (c / F32(d)) * F32(FromBits(0x3f99999a));   // 1.2
    F32 pk = p;
    if (!jmp::BE(p0.f(), p)) {
        pk = p0;
    } else {
        const F32 hi = p0 * three;
        if (!jmp::BE(p, hi.f())) pk = hi;
    }
    if (!jmp::BE(pk.f(), x)) {
        const F32 s = F32(x) / pk;
        const F32 B = (pk / c) * F32(d);
        const F32 poly = ((three - B * two) + (B - two) * s) * (s * s) + s * B;
        const float force = (poly * F32(F)).f();
        *a = ((one - s) * F32(y0) + s * F32(y1)).f();
        *b = s.f();
        return X87(force);
    }
    if (!jmp::BE(lim, x)) {
        const F32 t = (F32(x) - pk) / (F32(lim) - pk);
        const X87 L = num::TableLerpScaled(t.f(), table, n);
        *b = (t + one).f();
        const X87 r = (X87(F) - X87(F2)) * L + X87(F2);
        *a = (L * X87(y1)).Store();
        return r;
    }
    *b = 2.0f;
    *a = 0.0f;
    return X87(F2);
}
} // namespace

void Tyre::HeatInput(float a, float b, float k)
{
    TyreState& O = *state;
    const TyreDef& D = *def;
    auto g = [](float v) { return F32(v); };
    const F32 zero = 0.0f, one = 1.0f;
    const uint32_t i = element;
    float* const temp = &O.elementTemperatures[3 * i + 1];   // the tread's
    const F32 l04 = g(surface.waterFactor) * F32(3.0f);
    const F32 x4 = l04 + F32(2.0f);
    heatFactor = x4.f();
    const F32 p = (Abs(g(rollingResistanceTorque) * g(spinRate)) * F32(0.5f) + Abs(F32(b) * g(slipVelocity))) + Abs(g(velocityLateral) * F32(a));
    heatPower = p.f();
    const F32 q = p * F32(k);
    const F32 q7 = q * F32(FromBits(0x3f333333));   // 0.7
    const F32 r1 = q - q7;
    const F32 q1 = q7 * F32(FromBits(0x3dcccccd));  // 0.1
    const F32 rT = q7 - q1;
    const F32 q2 = q1 * F32(FromBits(0x3e4ccccd));  // 0.2
    const F32 rA = r1 * F32(FromBits(0x3ecccccd));  // 0.4
    const F32 rC = (q1 - q2) + r1 * F32(FromBits(0x3f19999a));   // 0.6
    const F32 inv = one / (x4 / F32(D.treadHeatCapacity[2]) + one);
    F32 r = (((F32(O.carcassTemperature) + F32(*temp)) * F32(0.5f) - F32(273.15f)) - F32(60.0f)) * F32(FromBits(0x3c4ccccd));   // 0.0125
    float c;
    if (!jmp::BE(0.0f, r.f())) c = 0.0f;
    else c = jmp::A(r.f(), 1.0f) ? 1.0f : r.f();
    heatShare = inv.f();
    const Bracket& wb = O.workBracket;
    const X87 mix = (X87(1.0f) - X87(wb.t)) * X87(D.heatByWork[wb.i0]) + X87(D.heatByWork[wb.i1]) * X87(wb.t);
    const float h = ((mix * (X87(c) * X87(4500.0f) + X87(1500.0f))) * (X87(l04) + X87(1.0f))).Store();
    const X87 e = num::SubMulMul(*temp, 303.0f, h, contactStiffness) * X87(k);
    heatIn = (e / X87(k)).Store();
    const float f = (((((X87(c) * X87(FromBits(0xbf19999a)) + X87(1.0f)) * X87(inv)) * X87(rT)) + X87(q2)) - e).Store();   // -0.6
    num::AddDivProduct(D.treadHeatCapacity, f, temp);
    num::AddDivProduct(D.carcassHeatCapacity, rC.f(), &O.carcassTemperature);
    num::AddDivProduct(D.gasHeatCapacity, rA.f(), &O.innerTemperature);
    // The tread's wear.
    const F32 s2 = g(velocityLateral) * g(velocityLateral) + g(slipVelocity) * g(slipVelocity);
    if (jmp::BE(s2.f(), 0.4f)) {
        wearRate = 0.0f;
        return;
    }
    const float sp = Sqrt(X87(s2)).Store();
    const Bracket& tb = O.treadTemperatureBracket;
    const F32 rate = (one - F32(tb.t)) * F32(D.rubberWear[tb.i0]) + F32(D.rubberWear[tb.i1]) * F32(tb.t);
    F32 m = g(slipCurve) - one;
    if (!jmp::BE(0.0f, m.f())) m = zero;
    else if (jmp::A(m.f(), 1.0f)) m = one;
    const F32 w = (g(specificWork) * m) * rate;
    const F32 worn = (w * F32(sp)) * F32(k);
    const F32 scale = !jmp::BE(g(slipSpeed).f(), sp) ? F32(sp) / g(slipSpeed) : one;
    const F32 v = (w * scale) * F32(FromBits(0x36eae18b));   // 7e-6
    wearRate = jmp::A(FromBits(0x3d4ccccd), v.f()) ? 0.0f : v.f();   // 0.05
    float* const ew = wear + i;
    *ew = (worn * F32(FromBits(0x329a9e6b)) + F32(*ew)).f();   // 1.8e-8
    if (jmp::A(*ew, 1.0f)) *ew = 1.0f;
}

void Tyre::OutputRecord(TireParticles& P)
{
    const TyreDef& D = *def;
    auto g = [](float v) { return F32(v); };
    auto madd = [](const Vec4& v, F32 s, const Vec4& acc, Vec4& out) {   // out = v * s + acc (mulps, addps)
        (F32x4::Load(&v.x) * F32x4::Splat(s) + F32x4::Load(&acc.x)).Store(&out.x);
    };
    const F32 zero = 0.0f, one = 1.0f;
    P.valid = 1;
    num::Scale4(forward, slipVelocity, P.force);   // the force
    madd(lateral, g(velocityLateral), P.force, P.force);
    madd(normal, F32(FromBits(0x3e99999a)), P.force, P.force);   // 0.3
    P.work = (g(workDrive) * F32(1000.0f)).f();
    P.groundDepth = solver[5];
    P.slipCurve = slipCurve;
    P.quadVolume = (g(slipSpeed) * zero).f();
    if (solver[5] == 0.0f) {   // ucomiss; jp: not equal or unordered skips
        auto clamp = [zero, one](F32 v) { return !jmp::BE(0.0f, v.f()) ? zero : (!jmp::BE(v.f(), 1.0f) ? one : v); };
        P.wear[0] = clamp(g(wearRate)).f();
        P.wear[1] = clamp(g(wearRate)).f();
    } else {
        P.wear[0] = 0.0f;
        P.wear[1] = 0.0f;
    }
    if (!jmp::BE(slipSpeed2, 0.001f)) {   // the patch's two ends
        const F32 i = one / g(slipSpeed);
        const F32 l = g(velocityLateral) * i;
        Vec4 v;
        num::Scale4(lateral, (((g(velocityForward) * i) * F32(D.treadWidth)) * F32(0.5f)).f(), v);
        madd(forward, (g(contactLength) * l) * F32(-0.5f), v, v);
        madd(v, one, contactPoint, P.patchEnds[0]);
        madd(v, F32(-1.0f), contactPoint, P.patchEnds[1]);
    } else {
        const F32 h = F32(D.treadWidth) * F32(0.5f);
        madd(lateral, h, contactPoint, P.patchEnds[0]);
        madd(lateral, zero - h, contactPoint, P.patchEnds[1]);
    }
    if (!(quadDepth == 0.0f))
        P.quadVolume = ((g(contactLength) * Abs(g(velocityLateral)) + Abs(g(velocityForward)) * F32(D.treadWidth)) * g(quadDepth)).f();
    num::Vec4Copy(contactShown, contactPoint);
    num::Vec4Copy(forceShown, P.force);
    workShown = P.work;
    std::memcpy(&velocityForwardShown, &velocityForward, 4);
    std::memcpy(&slipVelocityShown, &slipVelocity, 4);
}

void Tyre::Force(float* out, Vec4* zero, float* roll, float* b1, float* b2)
{
    // The falloff table lives on the original's stack; TableLerpScaled may read one entry past it (index capped at n =
    // 10, then weight 0) - any finite value gives the same 0 * v, so a 12th entry 0 stands in for it.
    static const uint32_t kFalloff[12] = {0x3f800000, 0x3f7d70a4, 0x3f75c28f, 0x3f5eb852, 0x3f333333, 0x3f000000,
                                          0x3eb33333, 0x3e4ccccd, 0x3dcccccd, 0x3d4ccccd, 0x00000000, 0x00000000};
    float table[12];
    std::memcpy(table, kFalloff, sizeof table);
    const F32 zero0 = 0.0f, one = 1.0f;
    float u1, u2, u3;
    UnitAbs2(spinRate, velocityLateral, velocityForward, &u1, &u2, &u3);
    float dx, dy, fx, gx, fy, gy, px, lx, py, ly;
    float* const outs[10] = {&dx, &dy, &fx, &gx, &fy, &gy, &px, &lx, &py, &ly};
    const F32 d3c = def->referenceFriction;
    CoefficientsTo(u1, u2, (F32(surface.staticFriction) * d3c).f(), (F32(surface.kineticFriction) * d3c).f(), outs);
    F32 sx = F32(velocityLateral) / F32(slipSpeed) - F32(normalDotAxle) * F32(FromBits(0x3dcccccd));   // 0.1
    if (!jmp::BE(-1.0f, sx.f())) sx = -1.0f;
    else if (!jmp::BE(sx.f(), 1.0f)) sx = one;
    const float fwd = velocityForward;
    F32 sy = zero0;
    if (jmp::A(fwd, 0.001f) || jmp::A(FromBits(0xba83126f), fwd)) sy = (zero0 - F32(state->rollingRadius) * F32(spinRate)) / F32(fwd) - one;
    int sgx = 1, sgy = 1;
    if (jmp::A(0.0f, sx.f())) sx = zero0 - sx, sgx = -1;
    if (jmp::A(0.0f, sy.f())) sy = zero0 - sy, sgy = -1;
    const F32 z = jmp::A(0.0f, fwd) ? F32(-1.0f) : one;
    F32 nx;
    if (jmp::A(px, sx.f())) nx = sx / F32(px);
    else if (jmp::A(lx, sx.f())) nx = (sx - F32(px)) / (F32(lx) - F32(px)) + one;
    else nx = 2.0f;
    F32 ny = 2.0f;
    const float lim = sgy > 0 ? ly : 1.0f;
    if (jmp::A(py, sy.f())) ny = sy / F32(py);
    else if (jmp::A(lim, sy.f())) ny = (sy - F32(py)) / (F32(lim) - F32(py)) + one;
    const float m = Sqrt(X87(ny) * X87(ny) + X87(nx) * X87(nx)).Store();
    if (jmp::A(m, FromBits(0x3c23d70a))) {   // 0.01
        const F32 i = one / F32(m);
        const F32 ky = i * ny, kx = i * nx;
        px = (F32(lx) - (F32(lx) - F32(px)) * kx).f();
        py = (F32(ly) - (F32(ly) - F32(py)) * ky).f();
        fx = (kx * F32(fx)).f();
        fy = (ky * F32(fy)).f();
        if (jmp::A(gx, fx)) fx = gx;
        if (jmp::A(gy, fy)) fy = gy;
    }
    F32 c = F32(solver[5]) * F32(FromBits(0x42055556));   // 33.333336
    if (!jmp::BE(0.0f, c.f())) c = zero0;
    else if (jmp::A(c.f(), 1.0f)) c = one;
    const float y0 = (X87(c) * X87(FromBits(0xbdaaaaac)) + X87(FromBits(0x3eaaaaab))).Store();   // -1/12, 1/3
    const float y1 = (X87(c) * X87(FromBits(0xbe19999a)) + X87(FromBits(0x3d4ccccd))).Store();   // -0.15, 0.05
    const float e = load;
    float a;
    float x = SlipCurve(sx.f(), px, lx, fx, gx, dx, e, y0, y1, &a, b1, table, 10).Store();
    if (sgx < 0) x = (zero0 - F32(x)).f();
    *roll = ((F32(contactLength) * F32(a)) * F32(0.5f)).f();
    float y;
    if (sgy > 0) y = SlipCurve(sy.f(), py, ly, fy, gy, dy, e, y0, y1, &a, b2, table, 10).Store();
    else y = Neg(SlipCurve(sy.f(), py, 1.0f, fy, gy, dy, e, y0, y1, &a, b2, table, 10)).Store();
    out[0] = (zero0 - F32(x) * F32(e)).f();
    out[1] = ((F32(y) * z) * F32(e)).f();
    out[2] = 0.0f;
    num::Vec4Zero(*zero);
}

bool Tyre::ContactGeometry(float spinAngle, float* out)
{
    TyreState& o = *state;
    const TyreDef& d = *def;
    auto g = [](float v) { return F32(v); };
    const F32 zero = 0.0f, one = 1.0f, two = 2.0f;
    // The element on the ground, by the wheel's spin angle.
    const uint32_t i = static_cast<uint32_t>(num::Ftol((X87(spinAngle) + X87(FromBits(0x427b53d2))) * X87(FromBits(0x3fa2f983)))) & 7;
    element = i;
    o.contactElement = i;
    elementShown = i;
    const F32 h = g(hubHeight);
    const F32 r = (one - F32(wear[i])) * F32(d.treadDepth);   // the tread, worn
    radius = r.f();
    const float o40 = (F32(o.deflectionLateral) * F32(o.deflectionLateral)).f();
    const float dec = (F32(d.sideWallHeight) * F32(d.sideWallHeight)).f();
    lateralGripScale = jmp::A(dec, o40) ? Sqrt(X87(o40) / X87(dec)).Store() : 1.0f;
    const F32 s = g(normalDotAxle);
    const F32 as = !jmp::BE(s.f(), 0.0f) ? s : zero - s;
    heightRatio = jmp::A(as.f(), FromBits(0x38d1b717)) ? (h / s).f() : FromBits(0x4b189680);   // 1e-4, 1e7
    const F32 outer = r + F32(d.carcassRadius);
    const F32 dd = outer - h;   // the deflection
    if (jmp::A(FromBits(0x38d1b717), dd.f())) return false;
    const float a = Sqrt(((X87(outer) + X87(outer)) - X87(dd)) * X87(dd)).Store();   // half the patch
    const F32 l = (F32(a) * F32(d.carcassWidth)) * two;
    const F32 l4 = F32(d.treadCoverage) * l;
    contactLength = (X87(a) + X87(a)).Store();
    contactStiffness = l4.f();
    const F32 l18 = l - l4;
    bool solve = false;   // a ground that gives
    if (!jmp::BE(surface.sinkFactor, FromBits(0x2edbe6ff)) && !jmp::BE(surface.soilThickness, 0.0f))   // 1e-10
        solve = jmp::A(FromBits(0x4a989680), (l / g(surface.sinkFactor)).f());   // 5e6
    const F32 c = F32(d.pressureStiffness) * F32(o.pressure);
    const F32 ia = one / F32(a);
    const F32 k1 = (((ia * dd) * h) - F32(a)) / (dd * dd);
    F32 b = (ia * dd) * h - (k1 * dd) * two;
    b = ((b * c) * F32(d.carcassWidth)) * two + F32(o.carcassStiffnessVertical);
    const F32 A = ((k1 * c) * F32(d.carcassWidth)) * two;
    const F32 C = (F32(o.treadElasticity) * l4) / (F32(d.carcassThickness) + r);
    if (solve) {
        float* S = solver;
        S[0] = dd.f();
        S[2] = F32(d.carcassRadius).f();
        S[3] = h.f();
        S[4] = r.f();
        const F32 inv = one / g(surface.sinkFactor);
        const float P[8] = {A.f(), b.f(), C.f(), surface.sinkFactor, l4.f(), l18.f(), (inv * l18).f(), (inv * l4).f()};
        static_assert(offsetof(Tyre, loadSolved) == offsetof(Tyre, load) + 4 && offsetof(Tyre, loadGround) == offsetof(Tyre, load) + 8);
        float* const O = &load;   // {load, loadSolved, loadGround}: the solver's outputs
        ContactDispatch(S, O, P, o.regimeA, o.regimeB, 0);
        const F32 f = F32(S[5]) / g(surface.soilThickness);   // how far the ground gave: the frictions between firm and soft
        const F32 w = one - f;
        const float b1 = (g(surface.staticSoilFriction) * w + g(surface.staticFriction) * f).f();
        const float b2 = (g(surface.kineticSoilFriction) * w + g(surface.kineticFriction) * f).f();
        surface.staticFriction = b1;
        surface.kineticFriction = b2;
        *out = loadSolved;
        load = loadSolved;
        return true;
    }
    const X87 u = (X87(b) - Neg(X87(C))) / X87(A);
    const X87 hh = X87(-0.5f) * u;
    const X87 q = (u * u) * X87(0.25f);
    const X87 m = (X87(C) * X87(dd)) * (X87(-1.0f) / X87(A));
    const X87 v = hh - Sqrt(q - m);
    *out = (((X87(A) * v) + X87(b)) * v).Store();
    o.regimeA = 1;
    o.regimeB = 0;
    load = *out;
    loadSolved = *out;
    loadGround = *out;
    solver[1] = v.Store();
    solver[5] = 0.0f;
    solver[6] = 0.0f;
    return true;
}

X87 Tyre::AlignDirection(float k)
{
    TyreState& O = *state;
    Vec4& dir = O.heading;
    const F32 zero = 0.0f;
    auto reset = [&]() {
        num::Vec4Copy(dir, forward);
        return X87(0.0f);
    };
    if (slideBlend == 0.0f) return reset();
    if (!O.stepped) return reset();
    const F32 dot = (F32(normal.z) * F32(dir.z) + F32(normal.y) * F32(dir.y)) + F32(dir.x) * F32(normal.x);
    const F32 ad = !jmp::BE(dot.f(), 0.0f) ? dot : zero - dot;
    if (jmp::A(ad.f(), FromBits(0x3ba3d70a))) {   // 0.005: back into the ground's plane
        (F32x4::Load(&normal.x) * F32x4::Splat(zero - dot) + F32x4::Load(&dir.x)).Store(&dir.x);
        const float len = num::Length3(dir);
        if (jmp::BE(len, FromBits(0x3dcccccd))) return reset();
        (F32x4::Splat(F32(1.0f) / F32(len)) * F32x4::Load(&dir.x)).Store(&dir.x);
    }
    F32 s = (F32(lateral.z) * F32(dir.z) + F32(lateral.y) * F32(dir.y)) + F32(lateral.x) * F32(dir.x);
    const F32 lim = FromBits(0x3e99999a);   // 0.3
    F32 m = (F32(load) * F32(surface.kineticFriction)) * F32(FromBits(0x37ba69dc));   // 1 / 45000
    if (!jmp::BE(0.0f, m.f())) m = zero;
    else if (!jmp::BE(m.f(), lim.f())) m = lim;
    const F32 m2 = m * F32(slideBlend);
    const F32 a = F32(slipSpeed);
    F32 x = !jmp::BE(a.f(), 0.0f) ? a : zero - a;
    if (!jmp::BE(lim.f(), x.f())) x = lim;
    else if (!jmp::BE(x.f(), 100.0f)) x = F32(100.0f);
    const F32 r = (x - lim) * F32(k);
    bool moved = false;
    if (!jmp::BE(r.f(), 0.0f) && !jmp::BE(s.f(), 0.0f)) {
        s = !jmp::BE(r.f(), s.f()) ? zero : s - F32(static_cast<float>(num::SignOf(s.f()))) * r;
        moved = true;
    }
    const F32 as = !jmp::BE(s.f(), 0.0f) ? s : zero - s;
    if (!jmp::BE(as.f(), m2.f())) {
        const int32_t sg = !jmp::BE(s.f(), 0.0f) ? 1 : (!jmp::BE(0.0f, s.f()) ? -1 : 0);
        s = F32(static_cast<float>(sg)) * m2;
    } else if (!moved) {
        return X87(s.f()) * X87(-900.0f);
    }
    num::Scale4(forward, Sqrt(X87(1.0f) - X87(s.f()) * X87(s.f())).Store(), dir);
    (F32x4::Load(&lateral.x) * F32x4::Splat(s) + F32x4::Load(&dir.x)).Store(&dir.x);
    return X87(s.f()) * X87(-900.0f);
}

void Tyre::ForceStep()
{
    rollingTorque = 0.0f;
    if (gripBlend == 0.0f) {   // equal and ordered
        deflectionRateLateral = 0.0f;
        deflectionRateTorsional = 0.0f;
        slipCurveLong = 0.0f;
        slipCurveLat = 0.0f;
        forceLong = 0.0f;
        forceLat = 0.0f;
        return;
    }
    slipCurveLong = 0.0f;
    slipCurveLat = 0.0f;
    float out[4] = {};   // out[3]: stack garbage in the original, unused
    Vec4 zero;
    Force(out, &zero, &rollingTorque, &slipCurveLong, &slipCurveLat);
    const float k398 = stiffnessTorsional;
    num::ClampedRatio((F32(workLateral) + F32(out[0])).f(), (F32(camberCos) * F32(stiffnessLateral)).f(), state->deflectionLateral, deflectionRateLateral);
    num::ClampedRatio((F32(state->rollingRadius) * F32(out[1]) + F32(workTorsional)).f(), k398, state->deflectionTorsional, deflectionRateTorsional);
    forceLong = out[0];
    forceLat = out[1];
}

void Tyre::ApplyForces(float k, Vec4& F, Vec4& T)
{
    const TyreState* O = state;
    auto g = [](float v) { return F32(v); };
    const F32 zero = 0.0f, one = 1.0f;
    auto sign = [](float v) { return !jmp::BE(v, 0.0f) ? 1 : (!jmp::BE(0.0f, v) ? -1 : 0); };
    const F32 fx = zero - (F32(O->deflectionLateral) * g(stiffnessLateral)) * g(camberCos);
    const F32 fy = zero - F32(O->deflectionTorsional) * g(stiffnessTorsional);
    const F32 ld = g(load);
    F32 sx = fx, sy = fy / F32(O->rollingRadius), sz = fy;   // the original's three stack words
    if (!(slideBlend == 0.0f)) {
        const F32 base = g(gripBlend);
        if (sign(fx.f()) == sign(velocityLateral)) {
            const F32 a = !jmp::BE(slipRatio, 0.0f) ? g(slipRatio) : zero - g(slipRatio);
            F32 c = (a * a) * F32(-5.0f) + one;
            if (jmp::A(FromBits(0x3f19999a), c.f())) c = F32(FromBits(0x3f19999a));   // 0.6
            else if (jmp::A(c.f(), 1.0f)) c = one;
            sx = (c * g(slideBlend) + base) * fx;
        }
        if (sign(fy.f()) != sign(slipRatio)) {
            const F32 a = !jmp::BE(slipRatio, 0.0f) ? g(slipRatio) : zero - g(slipRatio);
            const F32 q = one / (F32(-1.0f) - a * F32(10.0f)) + one;
            const F32 r = (one - q) * g(slideBlend) + base;
            sy = r * sy;
            sz = r * fy;
        }
        rollingTorque = (g(gripBlend) * g(rollingTorque)).f();
    }
    num::Scale4(lateral, sx.f(), F);
    (F32x4::Load(&forward.x) * F32x4::Splat(sy) + F32x4::Load(&F.x)).Store(&F.x);
    forceForward = sy.f();
    forceLateral = sx.f();
    torqueAlign = sz.f();
    (F32x4::Load(&normal.x) * F32x4::Splat(ld) + F32x4::Load(&F.x)).Store(&F.x);
    (F32x4::Load(&lateral.x) * F32x4::Splat(g(unknown3d0) + sz) + F32x4::Load(&T.x)).Store(&T.x);
    const F32 v = (zero - (F32(O->rollingRadius) * g(camberCos)) * sx) + (g(normalDotAxle) * F32(O->rollingRadius)) * ld;
    (F32x4::Load(&forward.x) * F32x4::Splat(v) + F32x4::Load(&T.x)).Store(&T.x);
    const int32_t s = !jmp::BE(velocityForward, 0.0f) ? 1 : (jmp::A(0.0f, velocityForward) ? -1 : 0);
    const float w = ((X87::Raw(static_cast<double>(s)) * X87(rollingTorque)) * X87(sx.f()) - (X87(gripBlend) * X87(O->deflectionLateral)) * X87(sy.f())).Store();
    const float m = (AlignDirection(k) + X87(w)).Store();
    (F32x4::Load(&normal.x) * F32x4::Splat(F32(m)) + F32x4::Load(&T.x)).Store(&T.x);
}

void Tyre::Step(const TyreStepInput& a, Vec4& F, Vec4& Tq, float& align, float& drag, TireParticles& particles)
{
    TyreState& O = *state;
    const TyreDef& D = *def;
    auto bracket = [](const float* table, float x, uint32_t n, Bracket& res) { num::BracketSearch(table, x, n, &res); };
    if (!O.started) O.started = 1;
    if (a.renderMaterial) std::memcpy(contactSurface, a.contact, 12);
    surface = *a.surface;
    num::Vec4Copy(contactPoint, a.point);
    spinRate = a.spinRate;
    hubHeight = a.hubHeight;
    quadDepth = a.quadDepth;
    bracket(D.pressureBreaks, O.pressure, 4, O.pressureBracket);
    pressureFactor = Lerp(D.stiffnessFactor, O.pressureBracket).f();
    stiffnessLateral = (F32(D.carcassStiffnessLateral) * F32(pressureFactor)).f();
    stiffnessTorsional = (F32(D.carcassStiffnessTorsional) * F32(pressureFactor)).f();
    float geometry;
    if (!AxisFrame(a.normal, a.axle) || !ContactGeometry(a.spinAngle, &geometry)) {   // no frame: nothing in contact
        align = 0.0f;
        inContact = 0;
        num::Vec4Zero(F);
        num::Vec4Zero(Tq);
        std::memset(&record, 0, sizeof record);
        record.debugId = D.debugId;
        record.inContact = inContact;   // 0
        num::Vec4Copy(O.restPoint, a.point);
        O.deflectionLateral = 0.0f;
        O.deflectionTorsional = 0.0f;
        O.stepped = 0;
        return;
    }
    AxisProject(a.velocity, spinRate);
    inContact = 1;
    {   // the load factor: clamp01((slipSpeed - 10) * 0.1) * 0.9 + 0.1
        F32 c = (F32(slipSpeed) - F32(10.0f)) * F32(FromBits(0x3dcccccd));
        if (jmp::A(0.0f, c.f())) c = F32(0.0f);
        else if (jmp::A(c.f(), 1.0f)) c = F32(1.0f);
        stiffnessTorsional = ((c * F32(FromBits(0x3f666666)) + F32(FromBits(0x3dcccccd))) * F32(stiffnessTorsional)).f();
    }
    unknown3cc = 0.0f;
    unknown3d0 = 0.0f;
    unknown3d4 = 0.0f;
    ForceWork();
    const F32 s = (F32(work) + F32(unknown3cc)) + F32(loadGround);
    specificWork = (s / F32(contactStiffness)).f();
    workPerLoad = (s / F32(load)).f();
    groundShare = (F32(loadGround) / s).f();
    bracket(D.workBreaks, specificWork, 6, O.workBracket);
    bracket(D.loadBreaks, load, 8, O.loadBracket);
    cornerStiffness = (Lerp(D.cornerStiffness, O.loadBracket) * F32(pressureFactor)).f();
    tractionStiffness = (Lerp(D.tractionStiffness, O.loadBracket) * F32(pressureFactor)).f();
    {   // the lateral deflection: dot(axle, velocity) dt + (1 - camberCos) * -((1 - cos(velocityForward / heightRatio dt)) heightRatio) (x87)
        const float x = ((F32(velocityForward) / F32(heightRatio)) * F32(a.dt)).f();
        const X87 roll = Neg((X87(1.0f) - X87::Raw(num::CosOfFloat(x))) * X87(heightRatio));
        const Vec4& p = a.axle;
        const Vec4& q = a.velocity;
        const X87 dot = (X87(p.z) * X87(q.z) + X87(p.y) * X87(q.y)) + X87(p.x) * X87(q.x);
        O.deflectionLateral = ((dot * X87(a.dt) + (X87(1.0f) - X87(camberCos)) * roll) + X87(O.deflectionLateral)).Store();
    }
    O.deflectionTorsional = (F32(O.deflectionTorsional) - F32(slipRatio) * F32(a.dt)).f();
    bracket(D.temperatureBreaks, O.elementTemperatures[3 * element + 1], 8, O.treadTemperatureBracket);
    bracket(D.temperatureBreaks, O.carcassTemperature, 8, O.carcassTemperatureBracket);
    F32 g;
    {   // the grip by the temperatures: the tread's and the carcass's brackets weighted 0.6 / 0.4
        auto e = [&](int32_t i) { return F32(D.rubberFriction[i]); };
        const Bracket& tb = O.treadTemperatureBracket;
        const Bracket& cb = O.carcassTemperatureBracket;
        const F32 k4 = FromBits(0x3ecccccd), k6 = FromBits(0x3f19999a);
        const F32 t0 = ((F32(1.0f) - F32(cb.t)) * e(cb.i0)) * k4;
        const F32 t1 = ((F32(1.0f) - F32(tb.t)) * e(tb.i0)) * k6;
        const F32 t2 = (e(tb.i1) * F32(tb.t)) * k6;
        const F32 t3 = (e(cb.i1) * F32(cb.t)) * k4;
        g = ((t0 + t1) + t2) + t3;
    }
    if (jmp::A(surface.waterFactor, 0.0f)) {   // a wet surface: the worn tread's grip
        const F32 w = F32(wear[element]);
        g = ((((F32(D.wetnessWearFactor) - F32(1.0f)) * F32(surface.waterFactor) + F32(1.0f)) - F32(1.0f)) * w + F32(1.0f)) * g;
    }
    for (int i = 0; i < 2; ++i) surface.staticFriction = (F32(surface.staticFriction) * g).f();   // the peak friction scaled by the grip twice
    grip = g.f();
    RangeBlend();
    ForceStep();
    FrictionStep();
    O.deflectionLateral = (F32(slideLateral) * F32(slideBlend) + F32(deflectionRateLateral) * F32(gripBlend)).f();
    O.deflectionTorsional = (F32(slideTorsional) * F32(slideBlend) + F32(deflectionRateTorsional) * F32(gripBlend)).f();
    const float m1 = jmp::A(slipCurveLong, slipCurveLat) ? slipCurveLong : slipCurveLat;
    const float m2 = jmp::A(slideMarkLong, slideMarkLat) ? slideMarkLong : slideMarkLat;
    slipCurve = (F32(gripBlend) * F32(m1) + F32(slideBlend) * F32(m2)).f();
    ApplyForces(a.dt, F, Tq);
    const X87 sp = SignedProduct();
    rollingResistanceTorque = sp.Store();
    align = (sp + X87(torqueAlign)).Store();
    drag = ((F32(load) * F32(FromBits(0x3dcccccd))) * F32(slideBlend)).f();
    HeatInput(forceLateral, forceForward, a.dt);
    OutputRecord(particles);
    WheelRecord(a.velocity, a.contact, a.dt);
    O.stepped = 1;
}

} // namespace phys::native
