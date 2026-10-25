#include "native/entity.h"

#include "num/math.h"
#include "native/flags.h"
#include "native/wheel.h"
#include "native/world.h"

#include <algorithm>

#include "num/f32.h"
#include "num/bits.h"
#include "num/x87.h"

namespace phys::native {

using num::F32;
using num::F32x4;
using num::X87;
using num::FromBits;
namespace jmp = num::jmp;

void Car::StepControls(float dt)
{
    CarState& s = carState;
    // The steering slews towards the controls' at most 20 per second.
    const F32 inv = F32(1.0f) / F32(dt);
    F32 d = F32(controlSteering) - F32(s.steering);
    const F32 rate = inv * d;
    const F32 a = jmp::A(rate.f(), 0.0f) ? rate : F32(0.0f) - rate;
    if (jmp::A(a.f(), 20.0f)) d = (d / a) * F32(20.0f);
    s.steering = (F32(s.steering) + d).f();
    const F32 old = F32(s.steeringAngle);
    s.steeringAngle = ((F32(s.steering) * F32(carDef->maxSteeringLock)) * F32(carDef->steeringRackRatio)).f();
    s.steeringRate = ((F32(s.steeringAngle) - old) * inv).f();
    s.steeringTorque = 0.0f;
    controlUnit.Step(dt);
    drive.Update(dt);
}

void Car::StepAfterChildren()
{
    CarState& s = carState;
    for (uint32_t k = 0; k < 2; ++k) {   // the anti-roll bars (CarRollBars, 0x4e66d0)
        RollBar& bar = s.rollBars[k];
        bar.torque = (F32(0.0f) - (F32(bar.travel[0]) - F32(bar.travel[1])) * F32(carDef->antiRollBarStiffness[k])).f();
    }
    // The steering feedback, blended by the speed (CarSpeedBlend, 0x4e7ff0); none while the car is held.
    if (!jmp::BE(s.assistanceTime, 0.0f)) {
        s.steeringFeedback = 0.0f;
    } else {
        F32 x = (F32(s.speed) - F32(7.0f)) * F32(FromBits(0x3e2aaaab));   // 1/6
        if (!jmp::BE(0.0f, x.f())) x = 0.0f;
        else if (!jmp::BE(x.f(), 1.0f)) x = 1.0f;
        const F32 b = x * F32(FromBits(0x3e4ccccc)) + F32(0.5f);         // 0.2
        s.steeringFeedback = ((F32(carDef->steeringRackRatio) * F32(s.steeringTorque)) * b + (F32(1.0f) - b) * F32(s.steeringFeedback)).f();
    }
    if (extraStep) Unreached("the car's extra step (0x4e9360)");
}

void Car::AeroDrag(World& w)
{
    const Vec4& v = body.velocity;
    const Vec4 nv{(F32(0.0f) - F32(v.x)).f(), (F32(0.0f) - F32(v.y)).f(), (F32(0.0f) - F32(v.z)).f(), 0.0f};   // w: stack
    const float len = num::Length3(nv);
    const float k = (((X87(carDef->dragCoefficient) * X87(carDef->frontArea)) * X87(len)) * X87(0.5f)).Store();
    Vec4 f, F;
    num::Scale4(nv, k, f);   // -v k |v|: quadratic drag
    FrameMoveCopy(*w.world, f, frame, F, Move::Dirs);
    Vec4 T;
    const Vec4 arm{0.0f, 0.0f, 0.05f, 0.0f};   // 0.05 above the centre (w: stack)
    num::Cross3(arm, F, T);
    Vec4 a;
    FrameMoveCopy(*w.world, body.angularVelocity, frame, a, Move::Dirs);
    const float wl = num::Length3(a);
    if (jmp::A(wl, 5.0f)) {   // spin damping above 5 rad/s: (|w| - 5)^2 * -5 / |w| * w
        const F32 d = F32(wl) - F32(5.0f);
        const F32 s = ((d * d) * F32(-5.0f)) / F32(wl);
        (F32x4::Load(&a.x) * F32x4::Splat(s) + F32x4::Load(&T.x)).Store(&T.x);
    }
    body.AccumulateForce(F);
    body.AccumulateTorque(T);
}

void Car::SpeedDrag(World& w)
{
    const float wall = carState.brakeWall;
    if (jmp::BE(wall, 0.0f)) return;
    const F32 zero = 0.0f, one = 1.0f;
    auto clamp01 = [&](F32 v) {
        if (!jmp::BE(0.0f, v.f())) return zero;
        return jmp::A(v.f(), 1.0f) ? one : v;
    };
    alignas(16) Vec4 lever = w.gravity, load;   // the car's force less its weight (in the world): z its down force
    FrameMoveCopy(frame, body.force, *w.world, load, Move::Dirs);
    const F32 im = F32(-1.0f) / F32(body.def->inverseMass);
    (F32x4::Load(&lever.x) * F32x4::Splat(im) + F32x4::Load(&load.x)).Store(&load.x);
    const F32 h = F32(load.z);
    if (jmp::A(0.0f, h.f())) return;
    Vec4 flat = body.velocity;
    flat.z = 0.0f;
    const float len = num::Length3(flat);
    if (jmp::BE(len, FromBits(0x3a83126f))) return;   // 0.001
    Vec4 dir, drag, torque{};
    num::Scale4(body.velocity, (one / F32(len)).f(), dir);
    const F32 s14 = clamp01(F32(wall) * F32(3.0f));
    const F32 s10 = clamp01(F32(len) * F32(2.0f));
    const F32 c2 = clamp01(F32(len) * F32(FromBits(0x3c23d70a)));   // 0.01
    const F32 k = ((c2 * F32(0.5f) + s10 * F32(FromBits(0x3e99999a))) * s14) * h;   // 0.3
    num::Scale4(dir, (zero - k).f(), drag);
    F32 r = F32(body.angularVelocity.z) * F32(2.5f);
    if (!jmp::BE(-1.0f, r.f())) r = F32(-1.0f);
    else if (jmp::A(r.f(), 1.0f)) r = one;
    const F32 m03 = FromBits(0xbe99999a);   // -0.3
    lever.x = zero.f(), lever.y = zero.f(), lever.z = m03.f();   // (w the gravity's, as the original's frame slot)
    num::Cross3(lever, drag, torque);
    torque.z = ((((r * s10) * s14) * h) * m03 + F32(torque.z)).f();   // the yaw damped
    Vec4 dl, tl;
    FrameMoveCopy(*w.world, drag, frame, dl, Move::Dirs);
    FrameMoveCopy(*w.world, torque, frame, tl, Move::Dirs);
    body.AccumulateForce(dl);
    body.AccumulateTorque(tl);
}

void Car::Forces(World& w)
{
    // (The classic's first call - the car's vslot 0 under debug flag 0x4000 - is the debug display's.)
    AeroDrag(w);
    SpeedDrag(w);
    // The gyroscopic torque: w in the car's frame, w x (-w / I) (I the inverse inertia's diagonal).
    Vec4 a;
    FrameMoveCopy(*w.world, body.angularVelocity, frame, a, Move::Dirs);
    const Vec4& I = body.def->localInverseInertia;
    const Vec4 q{((F32(-1.0f) / F32(I.x)) * F32(a.x)).f(), ((F32(-1.0f) / F32(I.y)) * F32(a.y)).f(), ((F32(-1.0f) / F32(I.z)) * F32(a.z)).f(), 0.0f};
    Vec4 t{};
    num::Cross3(a, q, t);
    body.AccumulateTorque(t);
    Solid::Forces(w);
    // The acceleration felt in the car, smoothed (0.7 old + 0.3 new).
    Vec4 f;
    FrameMoveCopy(*w.world, body.stepAcceleration, frame, f, Move::Dirs);
    float* const s = &carState.feltAcceleration.x;
    (F32x4::Splat(F32(0.7f)) * F32x4::Load(s)).Store(s);
    (F32x4::Load(&f.x) * F32x4::Splat(F32(0.3f)) + F32x4::Load(s)).Store(s);
    if (w.debugFlags & kRenderGgDiag) {   // the g-g diagram's force feedback
        if (!feedbackOn) feedbackOn = 0;
        else if (w.ggFeedback) {
            const F32 g = F32(-0.10193679481744766f);   // -1 / 9.81
            w.ggFeedback((F32(carState.feltAcceleration.x) * g).f(), (F32(carState.feltAcceleration.y) * g).f());
        }
    }
    for (MountedSolid* m : mounted)   // a loose part hanging on its base point
        if (m && m->Stick() == 1) m->StickForce(w);
}

void MountedSolid::StickForce(World& w)
{
    if (!car) {
        Solid::Forces(w);
        return;
    }
    Vec4 at{}, a{}, b{};
    FrameMoveCopy(car->frame, basePoint, *w.world, at, Move::Points);
    car->body.PointAccel(at, a);
    a.x = (F32(a.x) - F32(w.gravity.x)).f();
    a.y = (F32(a.y) - F32(w.gravity.y)).f();
    a.z = (F32(a.z) - F32(w.gravity.z)).f();
    FrameMoveCopy(*w.world, a, car->frame, b, Move::Dirs);
    Vec4& F = body.measuredAcceleration;   // its acceleration (its step's)
    num::Scale4(b, -1.0f, F);
    const Vec4& p = body.position;
    const Vec4 d{(F32(p.x) - F32(basePoint.x)).f(), (F32(p.y) - F32(basePoint.y)).f(), (F32(p.z) - F32(basePoint.z)).f(), 0.0f};   // w: stack
    (F32x4::Load(&d.x) * F32x4::Splat(F32(FromBits(0xc47a0000))) + F32x4::Load(&F.x)).Store(&F.x);                 // -1000
    (F32x4::Load(&body.velocity.x) * F32x4::Splat(F32(FromBits(0xc1200000))) + F32x4::Load(&F.x)).Store(&F.x);   // -10
}

void Passenger::Step(World& w, const Solid& car, float dt)
{
    // The car's measured acceleration with gravity, into its frame (the w lane stack in the original: 0 here).
    const Vec4& a = car.body.measuredAcceleration;
    const Vec4 g{(F32(w.gravity.x) + F32(a.x)).f(), (F32(a.y) + F32(w.gravity.y)).f(), (F32(a.z) + F32(w.gravity.z)).f(), 0.0f};
    FrameMoveCopy(*w.world, g, car.frame, felt, Move::Dirs);
    auto get = [](const Vec4& v, int k) { return F32((&v.x)[k]); };
    {   // SpringFollowStep (0x5233e0): the head after the torso
        alignas(16) const float kp[4] = {30000.0f, 30000.0f, 30000.0f, 0.0f}, kd[4] = {900.0f, 900.0f, 900.0f, 0.0f};
        alignas(16) float dp[4] = {}, dv[4] = {}, fp[4], fv[4], f[4] = {};
        for (int k = 0; k < 3; ++k) {
            dp[k] = (get(headPosition, k) - get(torsoPosition, k)).f();
            dv[k] = (get(headVelocity, k) - get(torsoVelocity, k)).f();
        }
        num::Mul4(*reinterpret_cast<const Vec4*>(kp), *reinterpret_cast<const Vec4*>(dp), *reinterpret_cast<Vec4*>(fp));
        num::Mul4(*reinterpret_cast<const Vec4*>(kd), *reinterpret_cast<const Vec4*>(dv), *reinterpret_cast<Vec4*>(fv));
        for (int k = 0; k < 3; ++k) f[k] = (F32(fv[k]) + F32(fp[k])).f();
        (F32x4::Load(&felt.x) * F32x4::Splat(F32(10.0f)) + F32x4::Load(f)).Store(f);
        (F32x4::Load(f) * F32x4::Splat(F32(dt) * F32(-0.1f)) + F32x4::Load(&headVelocity.x)).Store(&headVelocity.x);
        (F32x4::Load(&headVelocity.x) * F32x4::Splat(F32(dt)) + F32x4::Load(&headPosition.x)).Store(&headPosition.x);
    }
    {   // SpringStopStep (0x523470): the torso on the seat, springs and stops
        auto vec = [](const float* q) -> const Vec4& { return *reinterpret_cast<const Vec4*>(q); };
        alignas(16) const float kStop[4] = {20000.0f, 60000.0f, 50000.0f, 0.0f}, kDamp[4] = {800.0f, 900.0f, 900.0f, 0.0f};
        alignas(16) const float kPos[4] = {20000.0f, 35000.0f, 35000.0f, 0.0f}, kVel[4] = {300.0f, 500.0f, 400.0f, 0.0f};
        const F32 zero = 0.0f;
        auto excess = [&](F32 v, float lo, float hi) {   // outside the deadzone [lo, hi]: the excess, else 0 (NaN: 0)
            if (!jmp::BE(0.0f, v.f())) {
                const F32 d = v - F32(lo);
                return !jmp::BE(0.0f, d.f()) ? d : zero;
            }
            const F32 d = v - F32(hi);
            return jmp::A(d.f(), 0.0f) ? d : zero;
        };
        const Vec4& tp = torsoPosition;
        alignas(16) float e[4] = {excess(F32(tp.x), FromBits(0xbba3d70a), FromBits(0x3ba3d70a)).f(),   // -0.005, 0.005
                                  excess(F32(tp.y), FromBits(0xbca3d70a), FromBits(0x3c23d70a)).f(),   // -0.02, 0.01
                                  excess(F32(tp.z), FromBits(0xbc23d70a), FromBits(0x3c23d70a)).f(), 0.0f};   // -0.01, 0.01
        alignas(16) float sa[4], sb[4], f[4] = {};
        num::Mul4(vec(kStop), vec(e), *reinterpret_cast<Vec4*>(sa));
        num::Mul4(vec(kDamp), torsoVelocity, *reinterpret_cast<Vec4*>(sb));
        auto sign = [](float v) { return !jmp::BE(v, 0.0f) ? 1 : (!jmp::BE(0.0f, v) ? -1 : 0); };
        for (int k = 0; k < 3; ++k) {
            f[k] = (F32(sb[k]) + F32(sa[k])).f();
            if (sign(f[k]) != sign(e[k])) f[k] = 0.0f;   // the stop only pushes outward
        }
        num::Mul4(vec(kPos), torsoPosition, *reinterpret_cast<Vec4*>(sa));
        for (int k = 0; k < 3; ++k) f[k] = (F32(sa[k]) + F32(f[k])).f();
        num::Mul4(vec(kVel), torsoVelocity, *reinterpret_cast<Vec4*>(sa));
        for (int k = 0; k < 3; ++k) f[k] = (F32(sa[k]) + F32(f[k])).f();
        (F32x4::Load(&felt.x) * F32x4::Splat(F32(30.0f)) + F32x4::Load(f)).Store(f);
        (F32x4::Load(f) * F32x4::Splat(F32(dt) * F32(FromBits(0xbd088889))) + F32x4::Load(&torsoVelocity.x)).Store(&torsoVelocity.x);   // * -1/30
        (F32x4::Load(&torsoVelocity.x) * F32x4::Splat(F32(dt)) + F32x4::Load(&torsoPosition.x)).Store(&torsoPosition.x);
    }
}

void MountedSolid::StickStep(World& w, float dt)
{
    if (!car) {
        Solid::Step(w, dt);
        return;
    }
    const F32x4 t = F32x4::Splat(F32(dt));
    (F32x4::Load(&body.measuredAcceleration.x) * t + F32x4::Load(&body.velocity.x)).Store(&body.velocity.x);   // its force (StickForce)
    (F32x4::Load(&body.velocity.x) * t + F32x4::Load(&body.position.x)).Store(&body.position.x);
    auto clamp = [](float d, uint32_t lo, uint32_t hi) {
        if (jmp::A(FromBits(lo), d)) return FromBits(lo);
        if (jmp::A(d, FromBits(hi))) return FromBits(hi);
        return d;
    };
    float* const p = &body.position.x;
    const float* const b = &basePoint.x;
    float d[3];
    for (int k = 0; k < 3; ++k) d[k] = (F32(p[k]) - F32(b[k])).f();
    d[0] = clamp(d[0], 0xbca3d70a, 0x3ca3d70a);   // -0.02 .. 0.02
    d[1] = clamp(d[1], 0xbca3d70a, 0x3ba3d70a);   // -0.02 .. 0.005
    d[2] = clamp(d[2], 0xbcf5c28f, 0x3c23d70a);   // -0.03 .. 0.01
    p[0] = (F32(d[0]) + F32(b[0])).f();
    p[1] = (F32(b[1]) + F32(d[1])).f();
    p[2] = (F32(b[2]) + F32(d[2])).f();
    if (!poseSent) {   // its first pose (a woken body's event)
        if (w.woken) w.woken(*this);
        poseSent = 1;
    }
    if (w.stepped) w.stepped(*this);   // its pose (CSigMrRBodyState)
}

void Car::Step(World& w, float dt)
{
    CarState& cs = carState;
    if (jmp::A(cs.assistanceTime, 0.0f)) {   // a countdown to 0
        const F32 t = F32(cs.assistanceTime) - F32(dt);
        cs.assistanceTime = jmp::A(t.f(), 0.0f) ? t.f() : 0.0f;
    }
    driver.Step(w, *this, dt);
    Solid::Step(w, dt);
    // A brake wall: the velocity into it cancelled, the signal, the time spent on it.
    float onWall = 0.0f;
    BrakeWall::Hit hit{};
    if (w.brakeWall && w.brakeWall->PointHit(&body.position.x, hit, w.brakeWallQuad)) {
        body.CancelNormalVelocity(hit.normal, hit.distance, dt, w.brakeWallBuffer, w.brakeWallFactor);
        if (w.brakeWallHit) w.brakeWallHit(*this, hit);
        onWall = hit.onWall;
        if ((hit.flags & kWallHitFlag) && jmp::A(onWall, 0.5f)) brakeWallTime = (F32(brakeWallTime) + F32(dt)).f();
        else brakeWallTime = 0.0f;
    }
    cs.brakeWall = onWall;
    Vec4 lv;
    FrameMoveCopy(*w.world, body.velocity, frame, lv, Move::Dirs);
    cs.forwardSpeed = (F32(0.0f) - F32(lv.y)).f();
    // The hinged parts: woken when asleep; one torn off is let go.
    for (Solid*& slot : hinged) {
        if (!slot) continue;
        auto& h = static_cast<HingedSolid&>(*slot);
        if (!h.damage) {
            Unreached("a hinged part without its damage");
            continue;
        }
        if (h.damage->lock != 1 && !h.Awake()) w.WakeSolid(h);
        if (h.damage->lock != 1 && h.damage->attached != 1 && w.ReleaseHinged(h)) slot = nullptr;
    }
    // The mounted parts: a loose one stepped, one off let go.
    for (MountedSolid*& slot : mounted) {
        if (!slot) continue;
        if (slot->Stick() == 1) {
            slot->StickStep(w, dt);
        } else if (slot->Stick() == 2 && w.ReleaseStick(*slot)) {
            if (slot->hasExclusions)   // EntityChildRemove: the car out of the part's exclusions
                if (const auto it = std::find(slot->exclusions.begin(), slot->exclusions.end(), this); it != slot->exclusions.end()) slot->exclusions.erase(it);
            slot = nullptr;
        }
    }
    if (w.stageTiming) w.stageTiming(*this, w.driveLine ? w.driveLine->Distance(driveLineDistance) : 0.0f);
    const Vec4& da = body.measuredAcceleration;
    const F32 v2 = (F32(da.z) * F32(da.z) + F32(da.y) * F32(da.y)) + F32(da.x) * F32(da.x);
    if (jmp::A(v2.f(), (F32(carDef->dangerousAcceleration) * F32(carDef->dangerousAcceleration)).f())) cs.overDangerousAcceleration = 1;
    uint32_t down = 0;   // the wheels on the ground
    for (const Wheel* wh : wheels) down += wh && wh->onGround ? 1 : 0;
    fewWheelsTime = down < 3 ? (F32(dt) + F32(fewWheelsTime)).f() : 0.0f;
    if (w.raceState) w.raceState(*this);
    cs.distanceRolled = ((X87(drive.state->drivetrainSpeed) * X87(carDef->effWheelRadius)) + X87(cs.distanceRolled)).Store();
    const Vec4& v = body.velocity;
    const float speed = num::Length3(v);
    cs.travelSpeed = speed;
    if (jmp::A(0.001f, speed)) num::Vec4Zero(cs.travelDirection);
    else num::Scale4(v, (F32(-1.0f) / F32(speed)).f(), cs.travelDirection);   // the direction of travel, negated
    cs.speed = num::Length3(v);
    if (w.carPosition) w.carPosition(*this);
    if (w.carState) w.carState(*this);
}

void Car::HeldTick(World& w, float dt)
{
    const float t = carState.resetTimer;
    if (!(t == 0.0f)) {   // running (or NaN): counts down to 0
        const F32 n = F32(t) - F32(dt);
        carState.resetTimer = jmp::A(0.0f, n.f()) ? 0.0f : n.f();
        return;
    }
    Vec4 a{}, b{};
    if (w.driveLine) w.driveLine->ResetPoint(driveLineDistance, a, b);
    (F32x4::Splat(F32(-1.0f)) * F32x4::Load(&b.x)).Store(&b.x);
    Vec4 at = a;   // the ground under it (CarResetGround, 0x4e54a0): 10 m above to 10 m below
    GroundHit hit{};
    if (w.collision && w.maps) {
        num::Vec3 from{a.x, a.y, (F32(a.z) + F32(10.0f)).f()}, to{a.x, a.y, (F32(a.z) - F32(10.0f)).f()};
        if (w.collision->SegmentQuery(*w.maps, from, to, hit)) at = Vec4{hit.point.x, hit.point.y, hit.point.z, 0.0f};
    }
    PlaceAt(w, at, b);
    drive.GearSet(1);   // neutral
    SaveState();        // (vslot 18)
}

void Car::Retire(World& w)
{
    if (assistanceAllowed) OutsideAssistance(w, true, 0.5f, 4.0f);
    else OutsideAssistance(w, false, 30.0f, 4.0f);
}

void Car::OutsideAssistance(World& w, bool request, float t1, float t2)
{
    carState.resetTimer = t1;
    carState.assistanceTime = t2;
    turnedOver = request && jmp::A(fewWheelsTime, 1.0f) ? 1u : 0u;   // on its roof or side: put back turned over
    w.Hold(*this);
    Control(ControlInput{});   // (held: the controls' own record)
    carState.steeringFeedback = 0.0f;
}

bool Car::AssistanceTest(World& w, uint32_t& out) const
{
    if (held) return out = 0, false;
    if (jmp::A(num::Length3(body.velocity), 1.5f)) return out = 4, false;   // still moving
    if (assistanceFlag) return out = 0, true;
    const Vec4 up{0.0f, 0.0f, 1.0f, 0.0f};
    Vec4 wu;
    FrameMoveCopy(frame, up, *w.world, wu, Move::Dirs);   // (0x4e68e0)
    const F32 z = (F32(wu.z) * F32(up.z) + F32(wu.y) * F32(up.y)) + F32(up.x) * F32(wu.x);
    const bool upsideDown = jmp::A(0.0f, z.f());
    GroundHit hit{};
    const Vec4& p = body.position;
    if (w.collision && w.maps &&
        w.collision->SegmentQuery(*w.maps, num::Vec3{p.x, p.y, (F32(p.z) + F32(10.0f)).f()}, num::Vec3{p.x, p.y, (F32(p.z) - F32(10.0f)).f()}, hit)) {
        if (hit.blendFlag) return out = 1, false;
        if (upsideDown && hit.lightFlag) return out = 2, false;
    }
    return out = 0, true;
}

void Car::PlaceAt(World& w, const Vec4& at, const Vec4& dir)
{
    const Vec4 up{0.0f, 0.0f, 1.0f, 0.0f};
    Vec4 side{};
    num::Cross3(dir, up, side);
    const float inv = (X87(1.0f) / X87(num::Length3(side))).Store();   // fdivr, fstp
    (F32x4::Splat(F32(inv)) * F32x4::Load(&side.x)).Store(&side.x);
    Vec4 upv{};
    num::Cross3(side, dir, upv);
    const float m[9] = {side.x, side.y, side.z, dir.x, dir.y, dir.z, upv.x, upv.y, upv.z};
    Vec4 q, p;
    num::QuatFromMat33(m, q);
    (F32x4::Load(&upv.x) * F32x4::Splat(F32(0.4f)) + F32x4::Load(&at.x)).Store(&p.x);
    if (turnedOver) {   // turned about its direction, and higher
        Vec4 t = dir;
        (F32x4::Splat(F32(-0.2617993950843811f)) * F32x4::Load(&t.x)).Store(&t.x);
        t.w = 1.0f;
        Vec4 r;
        num::QuatMul(t, q, r);
        num::Normalize4ApproxTo(r, q);
        p.z = (F32(p.z) + F32(0.4f)).f();
        turnedOver = 0;
    }
    SetPose(w, p, q);
}

void Car::SetPose(World& w, const Vec4& p, const Vec4& q)
{
    body.orientation = q;
    body.position = p;
    body.UpdateRotation();   // (the state's vslot 17)
    Vec4 o{};                // the car body's origin in the world (0x4e8130)
    if (carBody) {
        Vec4 c{};
        FrameMoveCopy(carBody->frame, Vec4{}, frame, c, Move::Points);
        FrameMoveCopy(frame, c, *w.world, o, Move::Points);
    }
    const float z = body.position.z;
    body.position.z = ((F32(z) - F32(o.z)) + F32(z)).f();
    num::Vec4Zero(body.force), num::Vec4Zero(body.torque);   // (SolidClearAccumulators)
    num::Vec4Zero(body.velocity), num::Vec4Zero(body.angularVelocity);
    w.WakeSolid(*this);
    if (contactGroup) contactGroup->body = nullptr, contactGroup = nullptr;
    brakeWallTime = 0.0f;
}

void Car::TickBegin(World& w, float dt)
{
    if (!meshPlaced && mesh) {   // CarOnceInit (0x4e7270): the mesh moved by the car body's origin in the car's frame, once
        if (!carBody) Unreached("the car's mesh placed without its body");
        Vec4 p{};
        FrameMoveCopy(carBody->frame, Vec4{}, frame, p, Move::Points);   // (0x4e6a30)
        for (uint32_t i = 0; i < mesh->vertexCount; ++i) {   // MeshTranslate (0x4f65b0)
            Vec4& v = mesh->vertices[i];
            v.x = (F32(v.x) + F32(p.x)).f(), v.y = (F32(v.y) + F32(p.y)).f(), v.z = (F32(v.z) + F32(p.z)).f();
        }
        meshPlaced = 1;
    }
    Solid::TickBegin(w, dt);
}

void Wheel::TickBegin(World& w, float dt)
{
    num::Vec4Zero(body.velocity);
    num::Vec4Zero(body.angularVelocity);
    Notify();   // vslot 16
    Solid::TickBegin(w, dt);
}

void Wheel::PrepareForces(World&)
{
    auto& p = static_cast<Solid&>(*parent);   // (the car)
    p.body.AccumulateForce(contact.force);
    p.body.AccumulateTorque(contact.torque);
    num::Vec4Zero(contact.force);
    num::Vec4Zero(contact.torque);
    contact.flushed = 0;
}

void Car::StepChildren(World& w, float dt)
{
    StepControls(dt);
    Solid::StepChildren(w, dt);   // the awake children's (the wheels')
    StepAfterChildren();
}

void Car::Control(const ControlInput& rec)
{
    ControlInput own;   // zeros, the handbrake 1
    own.handbrake = 1.0f;
    const ControlInput& r = jmp::A(carState.assistanceTime, 0.0f) ? own : rec;
    controlUnit.Command(r);
    drive.ApplyControl(r.handbrake, r.starter);
    controlSteering = r.steering;
    if (!world) return;
    if (rec.flags & 0x20) {
        if (!held) {
            turnedOver = 0;
            world->Hold(*this);
        }
    } else if (rec.flags & 0x10) {
        uint32_t out = 0;
        if (AssistanceTest(*world, out)) OutsideAssistance(*world, true, 0.5f, 4.0f);
    }
}

} // namespace phys::native
