#include "native/vcu.h"

#include "num/f32.h"
#include "num/bits.h"
#include "num/x87.h"
#include "num/x87math.h"

namespace phys::native {

using num::F32;
using num::X87;
using num::FromBits;
namespace jmp = num::jmp;

namespace {
// An 11-point curve at x (PiecewiseLinear32: at the first key, its value).
X87 Curve(const CurvePoint* p, float x, uint32_t n)
{
    if (!jmp::B(p[0].x, x)) return X87(p[0].y);   // comiss key0, x; jb
    if (jmp::A(x, p[n - 1].x)) return X87(p[n - 1].y);
    uint32_t i = 1;
    while (i < n && jmp::A(x, p[i].x)) ++i;
    const X87 f = (X87(x) - X87(p[i - 1].x)) / (X87(p[i].x) - X87(p[i - 1].x));
    const X87 g = X87(1.0f) - f;
    return (X87(1.0f) - g) * X87(p[i].y) + g * X87(p[i - 1].y);
}
float Abs(float v) { return jmp::A(v, 0.0f) ? v : (F32(0.0f) - F32(v)).f(); }   // fcompi; jbe
} // namespace

bool Vcu::GearDownBlocked() const   // GearGuard and NeutralLock (0x536fe0)
{
    if (gearGuard) {   // not into a gear the engine would over-rev in
        const int32_t g = static_cast<int32_t>(drive->state->gear);
        if (g > 2) {
            const float rpm = (X87(drive->state->engineSpeed) * X87(FromBits(0x4118c9eb))).Store();   // rad/s -> rpm
            const DriveDef& D = *drive->def;
            const F32 x = (F32(D.gears[g - 1].ratio) / F32(D.gears[g].ratio)) * F32(rpm);
            if (jmp::A(x.f(), def->rpmLimit)) return true;
        }
    }
    if (neutralLock && drive->state->gear == 2 && jmp::A(Abs(hooks.forwardSpeed(hooks.ctx)), FromBits(0x4031c71d))) return true;   // 10 km/h
    return false;
}

int Vcu::GearCommand(const ControlInput& c)   // 0x537a80
{
    VcuState& X = *state;
    int changed = 0;
    if (c.flags & 1) {   // up
        drive->GearRequest(1);
        X.shiftTimer = def->upshiftDelay;
        changed = 1;
    }
    if ((c.flags & 2) && !GearDownBlocked()) {   // down
        drive->GearRequest(-1);
        X.shiftTimer = def->downshiftDelay;
        changed = 1;
    }
    if (c.flags & 4) {   // the gear given
        drive->GearSet(c.gear);
        X.shiftTimer = def->downshiftDelay;
        changed = 1;
    } else if (!changed) {
        return 0;
    }
    if (twoPedals) X.reversePedals = static_cast<int32_t>(drive->state->requestedGear) < 1 ? 1u : 0u;
    X.shiftCut = 0.05f;
    return changed;
}

int Vcu::Command(const ControlInput& c)
{
    brake = c.brake;
    handbrake = c.handbrake;
    throttle = c.throttle;
    steering = c.steering;
    clutch = c.clutch;
    return GearCommand(c);
}

void Vcu::AutoGearbox(float dt)   // 0x537160
{
    if (!autoGears) return;
    const DriveState& E = *drive->state;
    if (E.gear != E.requestedGear) return;   // a change in progress
    const VcuDef& P = *def;
    VcuState& X = *state;
    auto idle = [&X]() { return X.shiftTimer == 0.0f; };
    auto shift = [&](int32_t d, float delay) {   // request, then the timer and the ignition cut
        drive->GearRequest(d);
        X.shiftTimer = delay;
        X.shiftCut = FromBits(0x3d4ccccd);   // 0.05
    };
    const float rpm = (X87(E.engineSpeed) * X87(FromBits(0x4118c9eb))).Store();   // rad/s -> rpm
    float aspeed = Abs(hooks.forwardSpeed(hooks.ctx));
    const F32 zero = 0.0f;
    const uint32_t g = E.gear;
    if (twoPedals) {
        if (g == 2) {   // first gear, nearly stopped, brake pressed: count up to 0.4 s, then select reverse (down one)
            if (jmp::A(0.5f, aspeed) && jmp::A(FromBits(0x3e99999a), throttle)) {
                if (jmp::A(0.0f, X.reverseTimer)) X.reverseTimer = dt;
                else if (jmp::A(X.reverseTimer, FromBits(0x3ecccccd))) {
                    if (idle()) {
                        X.reverseTimer = 0.0f;
                        shift(-1, P.downshiftDelay);
                    }
                } else {
                    X.reverseTimer = (F32(X.reverseTimer) + F32(dt)).f();
                }
            }
        } else if (g == 0) {   // reverse: the same towards first gear, counting down
            aspeed = Abs(hooks.forwardSpeed(hooks.ctx));
            if (jmp::A(0.5f, aspeed) && jmp::A(FromBits(0x3e99999a), brake)) {
                if (jmp::A(X.reverseTimer, 0.0f)) X.reverseTimer = (zero - F32(dt)).f();
                else if (jmp::A(FromBits(0xbecccccd), X.reverseTimer)) {
                    if (idle()) {
                        X.reverseTimer = 0.0f;
                        shift(1, P.downshiftDelay);
                    }
                } else {
                    X.reverseTimer = (F32(X.reverseTimer) - F32(dt)).f();
                }
            }
        } else if (g == 1) {   // neutral
            X.reversePedals = jmp::A(brake, throttle) ? 1 : 0;
            const float hi = FromBits(0x3f666666), lo = 0.5f, rev = 5000.0f;   // 0.9
            if (jmp::A(throttle, hi) && jmp::A(lo, brake) && idle() && jmp::A(rpm, rev)) shift(1, P.upshiftDelay);
            else if (jmp::A(lo, throttle) && jmp::A(brake, hi) && idle() && jmp::A(rpm, rev)) shift(-1, P.upshiftDelay);
        } else {
            X.reverseTimer = 0.0f;
            X.reversePedals = 0;
        }
    }
    // Up and down shifts by the gear's rpm thresholds (0 = none).
    if (E.gear != E.requestedGear) return;
    const GearShift& gear = P.gears[E.gear];
    if (!(gear.upshift == 0.0f) && jmp::A(rpm, gear.upshift) && idle()) {
        // no up shift while the clutch is engaged at a crawl (0 < speed < 7)
        const bool crawl = twoPedals && E.engaged && jmp::A(aspeed, 0.0f) && jmp::A(7.0f, aspeed);
        if (!crawl) {
            shift(1, P.upshiftDelay);
            return;
        }
    }
    if (!(gear.downshift == 0.0f) && jmp::A(gear.downshift, rpm) && idle()) {
        shift(-1, P.downshiftDelay);
        return;
    }
    if (jmp::A(X.shiftTimer, 0.0f)) {   // the shift timer runs down
        X.shiftTimer = (F32(X.shiftTimer) - F32(dt)).f();
        if (jmp::A(0.0f, X.shiftTimer)) X.shiftTimer = 0.0f;
    }
}

void Vcu::AutoClutch(float dt)   // ClutchHelp (0x5375b0)
{
    if (!clutchHelp) return;
    const DriveState& e = *drive->state;
    float target;
    if (e.gear == 1 && (e.requestedGear == 2 || e.requestedGear == 0)) {
        target = 1.0f;   // neutral, and not heading for first gear
    } else {
        const float w = e.engineSpeed;   // open below 200 rad/s, closed above 275
        if (jmp::A(200.0f, w)) target = 1.0f;
        else if (jmp::A(w, 275.0f)) target = 0.0f;
        else target = ((F32(w) - F32(275.0f)) * F32(FromBits(0xbc5a740e))).f();   // * -1/75
    }
    float& c = state->clutch;
    if (jmp::A(c, target)) {   // released at clutchRelease per second, not below the target
        c = (F32(c) - F32(def->clutchRelease) * F32(dt)).f();
        if (jmp::A(target, c)) c = target;
    } else {
        c = target;
    }
    drive->clutch = c;
}

void Vcu::SpeedFlags()   // the rev limiter and the shift cut: which cylinders may fire (EngineSpeedFlags, 0x536ee0)
{
    const float rpm = (X87(drive->state->engineSpeed) * X87(FromBits(0x4118c9eb))).Store();   // * 30/pi
    const F32 d = F32(rpm) - F32(def->rpmLimit);
    uint32_t* const w = engine->state->sparks;
    if (!jmp::BE(d.f(), 0.0f)) {   // over the limit: none, or half up to 300 over
        const uint32_t k = jmp::BE(d.f(), 300.0f) ? 1 : 0;
        w[0] = k, w[1] = 0, w[2] = k, w[3] = 0;
    } else {   // under: all, or half during the shift cut
        const uint32_t k = jmp::BE(state->shiftCut, 0.0f) ? 1 : 0;
        w[0] = k, w[1] = 1, w[2] = k, w[3] = 1;
    }
}

X87 Vcu::CurveMax3(const DiffMap& m, float speed) const   // 0x5376e0
{
    const float v1 = num::FloatCurveSample(m.throttle, throttle, 0.0f, 1.0f, 11).Store();
    const float v2 = num::FloatCurveSample(m.brake, brake, 0.0f, 1.0f, 11).Store();
    const float v3 = Curve(m.speed, speed, 11).Store();
    if (!jmp::B(v1, v2) && !jmp::B(v1, v3)) return X87(v1);
    if (!jmp::B(v2, v1) && !jmp::B(v2, v3)) return X87(v2);
    return X87(v3);
}

void Vcu::CurveRamps()   // the differentials' lock torques from the maps (0x537b70)
{
    const VcuDef& c = *def;
    const float s = (X87(drive->state->drivetrainSpeed) * X87(FromBits(0x3e99999a))).Store();   // * 0.3
    float v = 0.0f;
    if (!jmp::BE(c.centerDiffHandbrakeRelease, handbrake)) {   // the handbrake opens the centre diff
        const float k = c.leftFootBrakeThreshold;
        const bool leftFoot = !(k == 0.0f) && !jmp::BE(brake, k) && !jmp::BE(throttle, k);
        v = CurveMax3(c.maps[leftFoot ? 1 : 0], s).Store();
    }
    drive->DiffLockThreshold(0, v);
    drive->DiffLockThreshold(1, CurveMax3(c.maps[2], s).Store());
    drive->DiffLockThreshold(2, CurveMax3(c.maps[3], s).Store());
}

void Vcu::SetLight(uint32_t group, uint32_t on)   // a light group switched when it changes (0x4f1e40 / 0x4f1ed0)
{
    if (lights->on[group] == on) return;
    lights->on[group] = on;
    lights->changed[group] = 1;
}

void Vcu::Step(float dt)
{
    AutoGearbox(dt);
    if (clutchHelp) AutoClutch(dt);
    else drive->clutch = clutch;   // the clutch pedal as given
    SpeedFlags();
    // Throttle and brake, swapped in two-pedal reverse.
    const bool swap = twoPedals && state->reversePedals;
    const float t = swap ? brake : throttle, b = swap ? throttle : brake;
    engine->throttle = t;
    drive->throttle = t;
    drive->brake = b;
    drive->brakePressure = (F32(drive->def->maxFootbrakePressure) * F32(b)).f();
    SetLight(3, jmp::A(b, FromBits(0x3dcccccd)) ? 1 : 0);   // the brake lights above 0.1
    CurveRamps();
    SetLight(4, drive->state->gear == 0 ? 1 : 0);   // the reverse lights
    float& cut = state->shiftCut;
    if (jmp::A(cut, 0.0f)) {
        cut = (F32(cut) - F32(dt)).f();
        if (jmp::A(0.0f, cut)) cut = 0.0f;
    }
}

} // namespace phys::native
