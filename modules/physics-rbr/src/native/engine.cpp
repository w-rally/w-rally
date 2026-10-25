#include "native/engine.h"

#include <cstring>

#include "num/curves.h"
#include "num/f32.h"
#include "num/bits.h"
#include "num/math.h"
#include "num/trig.h"
#include "num/x87.h"
#include "num/x87math.h"

namespace phys::native {

using num::F32;
using num::X87;
using num::FromBits;
namespace jmp = num::jmp;

namespace {
F32 Abs(F32 v) { return !jmp::BE(v.f(), 0.0f) ? v : F32(0.0f) - v; }   // comiss v, 0; jbe
float* Floats(Cylinder& c) { return reinterpret_cast<float*>(&c); }
static_assert(sizeof(Cylinder) == 0x38, "EntryEval's argument layout");

// A 16-point curve at x (PiecewiseLinear32Strict: below the first key, its value).
X87 Curve(const CurvePoint* p, float x, uint32_t n)
{
    if (!jmp::BE(p[0].x, x)) return X87(p[0].y);   // comiss key0, x; jbe
    if (jmp::A(x, p[n - 1].x)) return X87(p[n - 1].y);
    uint32_t i = 1;
    while (i < n && jmp::A(x, p[i].x)) ++i;
    const X87 f = (X87(x) - X87(p[i - 1].x)) / (X87(p[i].x) - X87(p[i - 1].x));
    const X87 g = X87(1.0f) - f;
    return (X87(1.0f) - g) * X87(p[i].y) + g * X87(p[i - 1].y);
}

// A map's curves bracketing x (TableBracket12): i the upper, j = i - 1, their weights a (upper) and b.
void MapBracket(const GasMap& m, float x, uint32_t* i, uint32_t* j, float* a, float* b)
{
    const uint32_t n = static_cast<uint32_t>(m.curves.size());
    *i = 1;
    if (n - 1u > 1u) {
        do {
            if (jmp::BE(x, m.curves[*i].key)) break;
            ++*i;
        } while (*i < n - 1u);
    }
    *j = *i - 1u;
    const F32 one = F32(1.0f);
    const F32 f = (F32(x) - F32(m.curves[*j].key)) / (F32(m.curves[*i].key) - F32(m.curves[*j].key));
    const F32 g = one - f, h = one - g;
    *b = g.f();
    *a = h.f();
    if (!jmp::BE(h.f(), 1.0f)) {
        *a = 1.0f;
        *b = 0.0f;
    } else if (jmp::A(0.0f, h.f())) {
        *a = 0.0f;
        *b = 1.0f;
    }
}

// Two curves blended at x by their weights (BlendedCurveLookup, ascending keys: Asc; descending: the compressor's): y, z;
// below the first key the compressor's flags it (`low`), the turbine's gives {0, 0.5}.
template <bool Asc>
void MapLookup(float x, const GasCurve& A, const GasCurve& B, float a, float b, float* y, float* z, uint32_t* low)
{
    const GasCurvePoint* pa = A.points.data();
    const GasCurvePoint* pb = B.points.data();
    const uint32_t last = static_cast<uint32_t>(A.points.size()) - 1;
    const F32 fa = a, fb = b;
    uint32_t i = 0;
    F32 prev = 0.0f;
    F32 cur = F32(pa[0].key) * fa + F32(pb[0].key) * fb;
    if (last != 0) {
        while (Asc ? !jmp::BE(x, cur.f()) : !jmp::BE(cur.f(), x)) {
            prev = cur;
            ++i;
            cur = F32(pa[i].key) * fa + F32(pb[i].key) * fb;
            if (i >= last) break;
        }
    }
    if (i == 0) {
        if (Asc) {
            *y = 0.0f;
            *z = 0.5f;
        } else {
            *y = (F32(pa[0].y) * fa + F32(pb[0].y) * fb).f();
            *z = (F32(pa[0].z) * fa + F32(pb[0].z) * fb).f();
            *low = 1;
        }
        return;
    }
    const F32 one = 1.0f;
    F32 w1 = one - (F32(x) - prev) / (cur - prev);
    if (!jmp::BE(0.0f, w1.f())) w1 = 0.0f;
    else if (!jmp::BE(w1.f(), 1.0f)) w1 = one;
    const F32 w0 = one - w1;
    auto mixY = [&](uint32_t k) { return F32(pb[k].y) * fb + F32(pa[k].y) * fa; };
    auto mixZ = [&](uint32_t k) { return F32(pb[k].z) * fb + F32(pa[k].z) * fa; };
    const F32 zi = mixZ(i), zp = mixZ(i - 1), yi = mixY(i), yp = mixY(i - 1);
    if (Asc) *y = (yi * w0 + yp * w1).f();
    else *y = (yp * w1 + yi * w0).f();
    *z = (zi * w0 + zp * w1).f();
    if (!Asc) *low = 0;
}
} // namespace

float Engine::Random()
{
    uint32_t& s = *damage->random;
    s = s * 0x2000081u - 0x19dc0be3u;
    return (num::FildUnsigned(static_cast<int32_t>(s % 100001u)) * X87(FromBits(0x3727c5ac))).Store();   // * 1e-5
}

void Engine::CosineEase()
{
    const float m = def->idleThrottle;
    const F32 one = F32(1.0f);
    F32 t = (F32(throttle) - F32(m)) / (one - F32(m));
    if (jmp::A(0.0f, t.f())) t = F32(0.0f);
    else if (jmp::A(t.f(), 1.0f)) t = one;
    const float a = (((one - F32(m)) * t + F32(m)) * F32(FromBits(0x3fc90fdb))).f();   // pi/2
    easedThrottle = (X87(1.0f) - X87::Raw(num::CosOfFloat(a))).Store();
}

X87 Engine::RpmBandFactor() const
{
    const F32 rpm = F32(state->smoothedSpeed) * F32(FromBits(0x4118c9eb));   // 60 / 2pi
    if (jmp::A(500.0f, rpm.f())) return X87(0.0f);
    if (damage->headGasket == 0) return X87(0.0f);
    F32 w = (rpm - F32(3500.0f)) * F32(FromBits(0x39690453));                // 1 / 4500
    if (jmp::A(0.0f, w.f())) w = F32(0.0f);
    else if (jmp::A(w.f(), 1.0f)) w = F32(1.0f);
    const float d = (F32(throttle) - w).f();
    if (jmp::A(FromBits(0xbecccccd), d)) return X87(1.0f) - X87(d) * X87(10.0f);                  // -0.4
    if (jmp::A(d, FromBits(0x3ecccccd))) return X87(d) * X87(FromBits(0x3e4ccccd)) + X87(1.0f);   // 0.4, 0.2
    return X87(1.0f);
}

void Engine::BlendedQuadratic(float x, Cylinder& cyl) const
{
    const float* const w = Floats(const_cast<Cylinder&>(cyl));
    if (!(w[2] == 0.0f)) {
        cyl.burn = 0.0f;
        return;
    }
    const float* const c = def->chargeCurves;
    const float* const k = &def->cycle[255 * 8];
    const F32 fx = F32(x), x2 = fx * fx;
    auto q = [&](uint32_t at) { return (F32(c[at + 2]) * x2 + F32(c[at + 1]) * fx) + F32(c[at]); };
    const F32 a = (q(6) * F32(k[1])) * F32(w[4]);
    const F32 b = w[3] == 0.0f ? (q(12) * F32(k[2])) * F32(w[5]) : (q(0) * F32(k[0])) * F32(w[3]);
    cyl.burn = (a + b).f();
}

X87 Engine::EntryEval(uint32_t index, const Cylinder& cyl) const
{
    const float* e = &def->cycle[8 * index];
    const float* p = reinterpret_cast<const float*>(&cyl);
    const float s = p[2] == 0.0f ? ((F32(e[2]) * F32(p[5]) + F32(e[1]) * F32(p[4])) + F32(p[3]) * F32(e[0])).f() : e[3];
    const X87 r = (X87(e[7]) * X87(p[7]) + X87(s)) * X87(p[9]);
    return ((r + X87(e[5]) * X87(p[10])) + X87(e[4]) * X87(p[11])) + X87(e[6]) * X87(p[8]);
}

void Engine::CyclePhase(float da, int32_t* steps, float* angle)
{
    EngineState& e = *state;
    const uint32_t old = e.step;
    e.previousStep = old;
    e.crankAngle = (F32(da) + F32(e.crankAngle)).f();
    const F32 cycle = FromBits(0x41490fdb);   // 4pi
    if (!jmp::BE(e.crankAngle, cycle.f())) e.crankAngle = (F32(e.crankAngle) - cycle).f();
    else if (!jmp::BE(0.0f, e.crankAngle)) e.crankAngle = (F32(e.crankAngle) + cycle).f();
    const uint32_t now = static_cast<uint32_t>(num::Ftol(X87(e.crankAngle) * X87(FromBits(0x41a2f983))));   // 256/(4pi)
    e.step = now;
    *steps = 0;
    if (!jmp::BE(da, 0.0f)) *steps = static_cast<int32_t>(now >= old ? now - old : now - old + 0x100);
    else if (!jmp::BE(0.0f, da)) *steps = static_cast<int32_t>(now > old ? now - old - 0x100 : now - old);
    *angle = (F32(static_cast<float>(*steps)) * F32(FromBits(0x3d490fdb))).f();   // * 4pi/256
}

void Engine::RandomEvent(float dt)
{
    EngineState& e = *state;
    if (e.misfireLoad == 0.0f) {
        e.noise.misfire = 0.0f;
        return;
    }
    if (!jmp::BE(e.misfireTimer, e.misfireNext)) {
        const float r = Random();
        F32 a = F32(e.misfireTimer) * F32(40.0f);
        if (!jmp::BE(a.f(), 1.0f)) a = 1.0f;
        const float cap = FromBits(0x3f666666);   // 0.9
        F32 b = F32(e.misfireLoad) * F32(FromBits(0x4479ffff));   // 999.99994
        if (!jmp::BE(b.f(), cap)) b = cap;
        const bool fire = !jmp::BE((b * a).f(), r);
        e.misfireNext = (F32(e.misfireNext) + F32(FromBits(0x3dcccccd))).f();   // + 0.1
        if (fire) {
            F32 v = F32(e.misfireLoad) * F32(50.0f);
            if (!jmp::BE(v.f(), 1.0f)) v = 1.0f;
            misfire = v.f();
            e.noise.misfire = v.f();
            e.misfireTimer = 0.0f;
            e.misfireLoad = 0.0f;
            e.misfireNext = 0.0f;
            return;
        }
    }
    e.misfireTimer = (F32(e.misfireTimer) + F32(dt)).f();
    e.noise.misfire = 0.0f;
}

X87 Engine::DragTorque(float k)
{
    EngineState& e = *state;
    const EngineDef& d = *def;
    const F32 zero = 0.0f, one = 1.0f;
    const F32 aw = Abs(F32(e.crankSpeed));
    const F32 inv = !jmp::BE(e.friction, FromBits(0x3727c5ac)) ? one / F32(e.friction) : zero;   // 1e-5
    F32 acc = zero;
    for (uint32_t j = 0; j < 4; ++j) {
        const F32 wear = F32(damage->cylinders[j].dragWear) * F32(400.0f);
        acc = j == 0 ? wear : wear + acc;   // the first term starts the sum (no + 0)
        if (inv.f() == 0.0f) continue;
        F32 t = (F32(e.cylinders[j].stiffnessShare) * inv) / F32(d.cylinders[j].dragLimit);
        if (!jmp::BE(0.0f, t.f())) t = zero;
        else if (!jmp::BE(t.f(), 1.0f)) t = one;
        acc = (one - t) * F32(d.dragFade) + acc;
    }
    if (!jmp::BE(e.blockTemperature, d.hotTemperature)) acc = (F32(e.blockTemperature) - F32(d.hotTemperature)) * F32(d.hotDragFactor) + acc;
    const F32 drag = !jmp::BE(0.0f, e.crankSpeed) ? aw * F32(20.0f) : aw * zero;
    const F32 total = drag + acc;
    e.dragTorque = total.f();
    const F32 p = (total * F32(k)) * F32(e.smoothedSpeed);
    if (!jmp::BE(e.smoothedSpeed, 100.0f)) e.heat = (F32(e.heat) + Abs(p)).f();
    const float now = e.crankSpeed;
    const int32_t s = !jmp::BE(now, 0.0f) ? 1 : (!jmp::BE(0.0f, now) ? -1 : 0);
    return Neg(X87::Raw(static_cast<double>(s)) * X87(total));
}

X87 Engine::AbsSpeedTerm()
{
    EngineState& e = *state;
    e.friction = Abs(F32(def->frictionFactor) * F32(e.smoothedSpeed)).f();
    if (!(damage->oil == 0.0f) && !jmp::BE(e.smoothedSpeed, 0.0f)) return X87(e.friction);
    return X87(0.0f);
}

void Engine::SeriesStiffness()
{
    const EngineDef& d = *def;
    EngineState& e = *state;
    const F32 one = 1.0f;
    auto lerp = [](float lo, float hi, float wear) { return (F32(hi) - F32(lo)) * F32(wear) + F32(lo); };   // new to worn
    F32 s;
    for (uint32_t j = 0; j < 4; ++j) {
        e.cylinders[j].stiffness = lerp(d.cylinders[j].stiffness, d.cylinders[j].stiffnessWorn, damage->cylinders[j].stiffnessWear).f();
        const F32 inv = one / F32(e.cylinders[j].stiffness);
        s = j == 0 ? inv : inv + s;
    }
    if (d.turbo) {
        const F32 k = lerp(d.intercoolerStiffness, d.intercoolerStiffnessWorn, damage->turboWear);
        s = one / k + s;
        e.intercoolerStiffness = k.f();
    }
    e.stiffness = (one / s).f();
}

void Engine::GasFlow(float x0, float x, float s, const GasMap& map, float k, float* P, float* out)
{
    uint32_t i, j, low = 0;
    float a, b, y, z;
    MapBracket(map, x0, &i, &j, &a, &b);
    MapLookup<false>(x, map.curves[i], map.curves[j], a, b, &y, &z, &low);
    float v;
    if (!jmp::BE(x, 1.0f)) {
        v = ((((num::Root4(x) - X87(1.0f)) / X87(z)) + X87(1.0f)) * X87(s)).Store();
        const float m = num::Mul100000(s).Store();
        *out = (((X87(m) - num::Mul100000(v)) * X87(y)) * X87(k)).Store();
    } else {
        v = (num::Root4(x) * X87(s)).Store();
        *out = 0.0f;
    }
    F32 u = 0.0f;
    if (!low) {
        float q[4] = {(F32(y) * F32(k)).f(), v, P[2], 0.0f};
        num::RateTimes(q, 287.0f);
        num::CombinePair(P, q, 287.0f);
        u = y;
    }
    state->compressorFlow = (F32(state->compressorFlow) * F32(FromBits(0x3f666666)) + u * F32(FromBits(0x3dcccccd))).f();   // 0.9, 0.1
}

void Engine::GasOutflow(float x0, float x, const GasMap& map, float k, float* P, float* out)
{
    uint32_t i, j, low = 0;
    float a, b, y, z;
    MapBracket(map, x0, &i, &j, &a, &b);
    MapLookup<true>(x, map.curves[i], map.curves[j], a, b, &y, &z, &low);
    const float v = !jmp::BE(x, 1.0f) ? (num::Root4((F32(1.0f) / F32(x)).f()) * X87(P[1])).Store() : P[1];
    const float p1 = P[1];
    float& flow = state->turbineFlow;
    const F32 c = (F32(flow) * F32(FromBits(0x3c23d70a)) + F32(y) * F32(FromBits(0x3f7d70a4))) * F32(k);   // 0.01, 0.99
    const float t = num::LumpTake(P, c.f(), 101300.0f, 287.0f).Store();
    flow = (X87(t) / X87(k)).Store();
    const float m = num::Mul100000(((F32(P[1]) + F32(p1)) * F32(0.5f)).f()).Store();
    *out = (((X87(m) - num::Mul100000(v)) * X87(t)) * X87(z)).Store();
}

void Engine::HeadGasketWear(uint32_t i)
{
    EngineDamage& b = *damage;
    if (b.enabled == 0) return;
    const EngineState& a = *state;
    const Cylinder& cyl = a.cylinders[i];
    const F32 zero = F32(0.0f), one = F32(1.0f);
    auto clamp01 = [&](F32 v) {
        if (jmp::A(0.0f, v.f())) return zero;
        if (jmp::A(v.f(), 1.0f)) return one;
        return v;
    };
    const F32 base = (F32(cyl.blend[1]) * F32(FromBits(0x3f19999a)) + F32(FromBits(0x3ecccccd))) * F32(cyl.chargePressure);   // 0.6, 0.4
    float& gasket = b.cylinders[i].headGasket;
    const F32 c = clamp01(F32(gasket) * F32(10.0f));
    const F32 h = clamp01((F32(a.blockTemperature) - F32(600.0f)) * F32(FromBits(0xbba3d70a)));   // -0.005: a cold engine
    const F32 p = h * F32(344420.0f) + F32(FromBits(0x476d6c01));                                // 60780.004
    const F32 q = ((one - c) * F32(100.0f) + F32(4.0f)) * p;
    F32 u = clamp01((base - p) / (q - p));
    u = u * u;
    float* const g = hooks.gasketFactors;
    g[i] = ((one - u) * F32(g[i])).f();
    const float r = Random();
    if (jmp::BE(u.f(), r)) return;
    if (jmp::BE(1.0f, gasket)) return;
    gasket = ((F32(r) + F32(gasket)) + F32(FromBits(0x3c23d70a))).f();   // + 0.01
    if (jmp::A(gasket, 1.0f)) gasket = 1.0f;
}

void Engine::CylinderCharge(uint32_t i)
{
    EngineState& A = *state;
    Cylinder& C = A.cylinders[i];
    const EngineDef& E = *def;
    const F32 zero = 0.0f, one = 1.0f;
    C.work = 0.0f;
    C.phase = 0;
    CosineEase();
    if (E.turbo) {   // the spent charge into the exhaust plenum; the intake's pressure and temperature
        if (!(C.chargeMass == 0.0f)) {
            float p[4] = {C.chargeMass, 0.0f, (F32(C.chargePressure) * F32(4.75f)).f(), E.cylinderVolume};
            num::RatioUpdate(p, 287.0f);
            num::CombinePair(A.exhaust, p, 287.0f);
            A.spentChargeTemperature = p[1];
        }
        C.exhaustPressure = (F32(A.exhaust[2]) * F32(FromBits(0x3f933333))).f();   // * 1.15
        A.intakePressure = A.intake[2];
        A.intakeTemperature = A.intake[1];
    } else {   // the ambient
        A.intakePressure = 101300.0f;
        A.intakeTemperature = FromBits(0x439291ec);   // 293.14
        C.exhaustPressure = 116495.0f;
    }
    C.unknown20 = 0.0f;
    C.ambientPressure = 100000.0f;
    C.burn = 0.0f;
    const float sq = Sqrt(X87(A.intakeTemperature) * X87(287.0f)).Store();
    const float lc = Curve(E.ve, A.crankSpeed, 16).Store();   // the VE
    F32 f = one;
    if (jmp::A(Abs(F32(A.crankSpeed)).f(), FromBits(0x3c23d70a)) && jmp::A(lc, FromBits(0x3a83126f))) {   // 0.01, 0.001
        const F32 arg = (((F32(E.intakeArea) / Abs(F32(A.smoothedSpeed))) * F32(E.intakeFlow)) * F32(easedThrottle)) * F32(sq) /
                        (F32(E.cylinderVolume) * F32(lc));
        f = num::FloatCurveSample(E.flowCurve, arg.f(), 0.0f, 2.0f, 17).Store();
    }
    const F32 fl = f * F32(lc);
    C.chargePressure = (F32(A.intakePressure) * fl).f();
    A.chargePressure = (F32(A.intakePressure) * f).f();
    const float sc = (num::SoftCurve(fl.f()) * X87(E.cylinderVolume)).Store();
    F32 m;
    if (E.turbo) {   // the charge out of the intake plenum
        const float w = ((X87(sc) / X87(A.intake[3])) * X87(A.intake[0])).Store();
        num::SplitPair(A.intake, w, 287.0f);
        m = w;
    } else {
        m = F32(sc) * F32(FromBits(0x3f9a1f07));   // 1.2040719 (air density)
    }
    C.chargeMass = m.f();
    F32 rho = zero;
    if (!(m.f() == 0.0f)) {
        rho = (F32(E.cylinderVolume) * F32(C.chargePressure)) / (m * F32(287.0f));
        if (!jmp::BE(0.0f, rho.f())) rho = zero;
        else if (!jmp::BE(rho.f(), 800.0f)) rho = F32(800.0f);
    }
    BlendedQuadratic(rho.f(), C);
    C.burn = 0.0f;
    A.chargeDensity = rho.f();
    const float vf = RpmBandFactor().Store();
    const F32 q4 = F32(C.chargeMass) * F32(FromBits(0x3dcccccd));   // * 0.1
    F32 q3 = q4 * F32(vf);
    if (!jmp::B(vf, 2.0f)) q3 = q3 + F32(FromBits(0x3a03126f));    // + 0.0005
    const EngineDamage& B = *damage;
    const float rnd = Random();
    F32 pc = (F32(C.chargePressure) - F32(500.0f)) * F32(FromBits(0x3973dada));   // / 4300
    if (!jmp::BE(0.0f, pc.f())) pc = zero;
    else if (!jmp::BE(pc.f(), 1.0f)) pc = one;
    const F32 t = F32(B.cylinders[i].headGasket) * F32(FromBits(0x3fb6db6e));   // the gasket's wear * 10/7
    bool fire = false;
    if (A.sparks[i]) {
        const F32 thr = (one - pc * F32(FromBits(0x3f7eb852))) + t * t;   // 0.995
        fire = jmp::A(rnd, thr.f()) && jmp::A(A.smoothedSpeed, 80.0f) && !(B.cylinders[i].spark == 0.0f) && jmp::A(vf, 0.5f) && B.headGasket;
    }
    if (fire) {
        num::TriBlend(FromBits(0x3f733333), Floats(C));   // 0.95
        A.noise.fired[i] = 1;
        A.heat = (q4 * F32(2215000.0f) + F32(A.heat)).f();
        if (jmp::A(vf, 1.0f)) A.misfireLoad = ((q3 - q4) + F32(A.misfireLoad)).f();
    } else {
        C.blend[0] = 1.0f;
        C.blend[1] = C.blend[2] = C.blend[3] = 0.0f;
        A.noise.fired[i] = 0;
        A.misfireLoad = (q3 + F32(A.misfireLoad)).f();
    }
    HeadGasketWear(i);
}

void Engine::GasStep(float dt)
{
    const EngineDef& G = *def;
    EngineState& A = *state;
    const F32 zero = 0.0f, amb = 101300.0f;
    const F32 k005 = FromBits(0x3d4ccccd);   // 0.05
    const F32 w0 = F32(A.turboSpeed);
    const F32 ke = ((F32(G.turboInertia) * w0) * w0) * F32(0.5f);   // the turbo shaft's kinetic energy
    // The wastegate: open by how far the intake's pressure is over the charge pressure; the damaged intake's bleed.
    float s4 = 0.0f, s8 = 0.0f;
    F32 in = zero;
    const F32 dp = F32(A.intake[2]) - F32(A.chargePressure);
    if (jmp::A(dp.f(), G.wastegateOpen) && jmp::A(A.intake[2], 101300.0f)) {
        const F32 lo = F32(G.wastegateOpen), hi = F32(G.wastegateFull), flow = F32(G.wastegateFlow);
        const F32 f = jmp::A(dp.f(), G.wastegateFull) ? flow : ((dp - lo) * flow) / (hi - lo);
        in = ((F32(A.intake[2]) - amb) * f) * k005;
        s4 = f.f();
        s8 = in.f();
    }
    const float leak = damage->intakeLeak;
    F32 bleed = zero;
    if (!(leak == 0.0f) && jmp::A(A.intake[2], 101300.0f)) bleed = ((F32(G.bleedFlow) * F32(leak)) * (F32(A.intake[2]) - amb)) * k005;
    num::LumpTake(A.intake, ((in + bleed) * F32(dt)).f(), 101300.0f, 287.0f);
    const F32 kp = FromBits(0x37259e7e);   // 1 / 101300
    A.noise.boost = (F32(A.intake[2]) * kp).f();
    A.noise.turboSpeed = A.turboSpeed;
    A.noise.wastegateLoss = s8;
    A.noise.wastegateOpening = s4;
    // The exhaust bypass: open with the boost target by the crank speed, between target - 10000 and target + 20000.
    const X87 plx = Curve(G.boostTarget, A.crankSpeed, 16);
    const float pl = plx.Store();
    const float hi = (plx + X87(20000.0f)).Store();
    const F32 lo = F32(pl) - F32(10000.0f);
    const F32 p = A.intake[2];
    F32 ex = zero;
    if (jmp::A(p.f(), lo.f()) && jmp::A(A.exhaust[2], 101300.0f)) {
        const F32 f = jmp::A(p.f(), hi) ? F32(G.exhaustBypassFlow) : ((p - lo) * F32(G.exhaustBypassFlow)) / (F32(hi) - lo);
        ex = ((F32(A.exhaust[2]) - amb) * f) * k005;
    }
    num::LumpTake(A.exhaust, (ex * F32(dt)).f(), 101300.0f, 287.0f);
    float slot8 = pl, slot4 = hi;   // the original's two stack slots, overwritten by the calls
    GasFlow(A.turboSpeed, (F32(A.intake[2]) * kp).f(), FromBits(0x439291ec), G.compressorMap, dt, A.intake, &slot4);
    GasOutflow(A.turboSpeed, (F32(A.exhaust[2]) * kp).f(), G.turbineMap, dt, A.exhaust, &slot8);
    // The energy balance: friction (quadratic w^2 + linear w) * dt; the new speed w = sqrt(2 E / inertia), clamped.
    const F32 w = A.turboSpeed;
    const F32 e = ((F32(slot8) + F32(slot4)) - ((F32(G.turboFrictionQuadratic) * w) * w + F32(G.turboFriction) * F32(A.turboSpeed)) * F32(dt)) + ke;
    if (jmp::A(e.f(), 0.0f)) {
        const X87 q = X87(e.f()) / X87(G.turboInertia);
        float v = Sqrt(q + q).Store();
        if (jmp::A(0.0f, v)) v = 0.0f;
        else if (jmp::A(v, FromBits(0x46a39fe7))) v = FromBits(0x46a39fe7);   // 20943.95 (200000 rpm)
        A.turboSpeed = v;
    } else {
        A.turboSpeed = 0.0f;
    }
}

void Engine::WearStep(float k)
{
    const float v = AbsSpeedTerm().Store();
    EngineDamage& W = *damage;
    const F32 a = F32(W.oilPump) * F32(v);
    const F32 b = F32(v) - (F32(1.0f) - F32(W.oilCooler)) * a;
    SeriesStiffness();
    EngineState& e = *state;
    e.wearLoad = (F32(e.stiffness) * a).f();
    // Each stage's share of the wear load (by its stiffness), weighted by the damage record.
    auto share = [&e](float stiffness, float& out) {
        out = (F32(e.wearLoad) / F32(stiffness)).f();
        return F32(out);
    };
    auto cyl = [&](uint32_t j) { return share(e.cylinders[j].stiffness, e.cylinders[j].stiffnessShare); };
    F32 x = cyl(0) * F32(W.cylinders[0].stiffnessWear) + b;
    const F32 s1 = cyl(1);
    x = s1 * F32(W.cylinders[1].stiffnessWear) + x;
    const F32 y = cyl(2) * F32(W.cylinders[2].stiffnessWear);
    const F32 s3 = cyl(3);
    F32 z = s3 * F32(W.cylinders[3].stiffnessWear) + (y + x);
    if (def->turbo) z = share(e.intercoolerStiffness, e.intercoolerStiffnessShare) * F32(W.turboWear) + z;
    if (jmp::A(z.f(), 0.0f)) {
        F32 u = F32(W.oil) - (z * F32(k)) / F32(def->wearScale);   // the oil wears
        if (jmp::A(0.0f, u.f())) u = F32(0.0f);
        else if (jmp::A(u.f(), 1.0f)) u = F32(1.0f);
        W.oil = u.f();
    }
    friction = v;
}

void Engine::CoolantStep(float dt)
{
    const EngineDamage& W = *damage;
    if (!W.waterPump) return;
    const EngineDef& D = *def;
    EngineState& E = *state;
    float* const radiator = E.radiatorCoolant;
    float* const engine = E.engineCoolant;
    auto clampTo = [](F32 v, float hi) {
        if (jmp::A(0.0f, v.f())) return F32(0.0f);
        return jmp::A(v.f(), hi) ? F32(hi) : v;
    };
    // Out of the radiator: q = rate * dt, at most what is there and the engine side's capacity.
    F32 q = F32(D.circulationRate) * F32(dt);
    if (jmp::A(q.f(), radiator[0])) q = F32(radiator[0]);
    if (jmp::A(q.f(), D.engineCoolantCapacity)) q = F32(D.engineCoolantCapacity);
    float a[2] = {q.f(), radiator[1]};
    radiator[0] = (F32(radiator[0]) - q).f();
    // Back out of the engine side: what no longer fits.
    F32 r = q - (F32(D.engineCoolantCapacity) - F32(engine[0]));
    if (jmp::A(0.0f, r.f())) r = F32(0.0f);
    else if (jmp::A(r.f(), q.f())) r = q;
    float b[2] = {r.f(), engine[1]};
    engine[0] = (F32(engine[0]) - r).f();
    // A leak with the radiator's damage while the radiator holds more than the damage-blended level.
    const float t = (X87(1.0f) - X87(W.radiator)).Store();
    const float limit = ((F32(D.leakLevels[1]) - F32(D.leakLevels[0])) * F32(t) + F32(D.leakLevels[0])).f();
    if (!(W.radiator == 0.0f) && jmp::A(radiator[0], limit)) {
        F32 c = F32(W.radiator);
        if (jmp::A(0.0f, c.f())) c = F32(0.0f);
        else if (jmp::A(c.f(), 1.0f)) c = F32(1.0f);
        E.noise.coolantLeak = ((c / F32(dt)) * F32(b[0])).f();
        b[0] = ((F32(1.0f) - c * F32(FromBits(0x3dcccccd))) * F32(b[0])).f();   // 0.1
    } else {
        E.noise.coolantLeak = 0.0f;
    }
    num::MixLumps(a, engine);
    num::MixLumps(b, radiator);
    engine[0] = clampTo(F32(engine[0]), D.engineCoolantCapacity).f();
    radiator[0] = clampTo(F32(radiator[0]), D.radiatorCapacity).f();
    coolantPumped = (F32(coolantPumped) + q).f();
}

X87 Engine::CylinderAdvance(uint32_t i, uint32_t d)
{
    Cylinder& C = state->cylinders[i];
    const uint32_t phase = (def->cylinders[i].phaseOffset + d) & 0xff;
    float closed = 0.0f;
    if (phase < C.phase) {   // wrapped: the cycle's last stretch (step 0xff), then the next intake
        closed = (EntryEval(0xff, C) - X87(C.work)).Store();
        CylinderCharge(i);
    }
    const X87 v = EntryEval(phase, C);
    C.phase = phase;
    const float step = (v - X87(C.work)).Store();
    C.work = v.Store();
    return X87(step) + X87(closed);
}

void Engine::HeatStep(float dt)
{
    const EngineDef& D = *def;
    EngineState& A = *state;
    const float eps = FromBits(0x3a83126f);   // 0.001
    float q1 = 0.0f, q2 = 0.0f;
    if (jmp::A(A.engineCoolant[0], eps)) {   // the block's heat into the engine side's coolant
        q1 = (num::SubMulMul(A.blockTemperature, A.engineCoolant[1], D.coolantTransfer, D.coolantTransferArea) * X87(dt)).Store();
        num::AddHeatToLump(D.coolant, q1, A.engineCoolant);
    }
    num::AddDivProduct(D.block, (F32(A.heat) - F32(q1)).f(), &A.blockTemperature);
    if (jmp::A(A.radiatorCoolant[0], eps)) {   // the radiator cooled by the air flow through the car
        Vec4 v{};
        hooks.bodyVelocity(hooks.ctx, v);
        const Vec4& axis = D.radiatorAirAxis;
        F32 flow = (F32(axis.y) * F32(v.y) + F32(axis.z) * F32(v.z)) + F32(axis.x) * F32(v.x);
        if (jmp::A(0.0f, flow.f())) flow = F32(0.0f);
        const float k = (F32(D.radiatorAirFactor) * flow + F32(D.radiatorBase)).f();
        const float lo = D.leakLevels[0], hi = D.leakLevels[1];   // its fill range
        F32 r = (F32(A.radiatorCoolant[0]) - F32(lo)) / (F32(hi) - F32(lo));
        if (jmp::A(0.0f, r.f())) r = F32(0.0f);
        else if (jmp::A(r.f(), 1.0f)) r = F32(1.0f);
        q2 = (num::SubMulMul(A.radiatorCoolant[1], FromBits(0x439291ec), k, (r * F32(D.radiatorTransfer)).f()) * X87(dt)).Store();   // 293.14
        num::AddHeatToLump(D.coolant, Neg(X87(q2)).Store(), A.radiatorCoolant);
    }
    if (jmp::A(A.engineCoolant[1], D.circulationTemperature)) CoolantStep(dt);
    const float b1 = num::BoilOffLump(D.coolant, A.radiatorCoolant);
    const float inv = (F32(1.0f) / F32(dt)).f();
    const float b2 = num::BoilOffLump(D.coolant, A.engineCoolant);
    boilOffRate = ((X87(b2) + X87(b1)) * X87(inv)).Store();
    combustionHeatRate = (F32(inv) * F32(A.heat)).f();
    radiatorHeatRate = (F32(inv) * F32(q2)).f();
    coolantHeatRate = (F32(inv) * F32(q1)).f();
    A.heat = 0.0f;
}

X87 Engine::Step(float dt, float w)
{
    if (!jmp::BE(FromBits(0xc48ffd5b), w)) w = FromBits(0xc48ffd5b);
    else if (jmp::A(w, FromBits(0x448ffd5b))) w = FromBits(0x448ffd5b);   // +-1151.92 rad/s
    EngineState& E = *state;
    const EngineDef& D = *def;
    ++E.tics;
    E.crankSpeed = w;
    E.smoothedSpeed = (F32(E.crankSpeed) * F32(FromBits(0x3e99999a)) + F32(E.smoothedSpeed) * F32(FromBits(0x3f333333))).f();   // 0.3, 0.7
    E.noise.throttle = throttle;
    E.noise.speed = E.smoothedSpeed;
    if (D.turbo) GasStep(dt);
    int32_t steps;
    float angle;
    CyclePhase((F32(dt) * F32(w)).f(), &steps, &angle);
    stepAngle = angle;
    float torque = 0.0f;
    if (steps > 0) {   // the cylinders' work over the steps advanced, per radian
        float work = 0.0f;
        for (uint32_t i = 0; i < D.cylinderCount; ++i) work = (CylinderAdvance(i, E.step) + X87(work)).Store();
        torque = (F32(work) / F32(angle)).f();
    } else {   // no step: the phases from the offsets, and the work curve's slope at each (EntryEval inlined, in SSE)
        F32 acc = 0.0f;
        for (uint32_t i = 0; i < D.cylinderCount; ++i) {
            Cylinder& C = E.cylinders[i];
            const uint32_t phase = (D.cylinders[i].phaseOffset + E.step) & 0xff;
            C.phase = phase;
            const uint32_t at = phase ? phase : 1;
            const float* const p = reinterpret_cast<const float*>(&C);
            auto P = [p](uint32_t k) { return F32(p[k]); };
            auto eval = [&](uint32_t step) {
                const float* const e = &D.cycle[8 * step];
                auto cf = [e](uint32_t k) { return F32(e[k]); };
                const F32 s = P(2).f() == 0.0f ? (cf(2) * P(5) + cf(1) * P(4)) + P(3) * cf(0) : cf(3);
                return (((cf(7) * P(7) + s) * P(9) + cf(5) * P(10)) + cf(4) * P(11)) + cf(6) * P(8);
            };
            acc = (eval(at) - eval(at - 1)) * F32(FromBits(0x41a2f983)) + acc;   // 20.3718
        }
        torque = acc.f();
    }
    RandomEvent(dt);
    if (hooks.noise) hooks.noise(hooks.ctx, E.noise);
    const float total = (DragTorque(dt) + X87(torque)).Store();
    const F32 tt = F32(total);
    const F32 power = F32(E.crankSpeed) * tt;
    const F32 kf = FromBits(0x3d4cccd0), keep = FromBits(0x3f733333);   // 0.05, 0.95 (first-order filters)
    E.torque = (F32(E.torque) * keep + tt * kf).f();
    E.power = (F32(E.power) * keep + power * kf).f();
    if (hooks.torqueTelemetry && hooks.telemetry)   // the recorders (the classic code also checks FloorS32's arguments: a log line)
        hooks.telemetry(hooks.ctx, (F32(total) * F32(FromBits(0x3af4fe91))).f(), (F32(E.power) * F32(FromBits(0x369677e3))).f());
    if (E.tics % 10 == 0) HeatStep((F32(dt) * F32(10.0f)).f());
    if (E.tics % 20 == 0) WearStep((F32(dt) * F32(20.0f)).f());
    return X87(total);
}

} // namespace phys::native
