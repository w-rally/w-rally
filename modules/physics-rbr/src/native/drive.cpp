#include "native/drive.h"

#include <cstring>

#include "num/f32.h"
#include "num/bits.h"
#include "num/x87.h"
#include "num/x87math.h"
#include "native/unreached.h"

namespace phys::native {

using num::F32;
using num::X87;
using num::FromBits;
namespace jmp = num::jmp;

namespace {
F32 Abs(F32 v) { return !jmp::BE(v.f(), 0.0f) ? v : F32(0.0f) - v; }   // comiss v, 0; jbe
F32 IntF(int32_t v) { return F32(static_cast<float>(v)); }             // cvtsi2ss
F32 IntF(uint32_t v) { return IntF(static_cast<int32_t>(v)); }
int32_t Sign(float v) { return !jmp::BE(v, 0.0f) ? 1 : (!jmp::BE(0.0f, v) ? -1 : 0); }
} // namespace

X87 Drive::TorqueAverage(float x)
{
    TorqueRing& r = state->engineTorqueAverage;
    if (++r.index > r.count - 1) r.index = 0;
    r.values[r.index] = x;
    F32 s = 0.0f;
    for (uint32_t k = 0; k < r.count; ++k) s = F32(r.values[k]) + s;
    return X87(s) / num::FildUnsigned(static_cast<int32_t>(r.count));
}

// ---- the brakes ----

bool Drive::BrakeWear(float force, DriveSlot& s, const DriveWheelDef& w, float dt)
{
    const BrakeDef& E = def->brake;
    const DriveDef& D = *def;
    const uint32_t n = s.wear;
    if (n >= E.wearLimit) return false;
    const uint32_t a = s.temperature, b = E.wearTemperature;
    float t;
    if (a < b) t = D.wearCurve[0];
    else if (a < E.fadeTemperature)
        t = num::TableLerpU(D.wearCurve, static_cast<int32_t>(a), static_cast<int32_t>(b), static_cast<int32_t>(E.fadeTemperature), 9).Store();
    else t = D.wearCurve[9];
    const float p = (F32(E.wearPressure) * F32(force)).f();
    const float r = Abs(F32(s.spin)).f();
    const X87 prod = (((((((X87(r) * X87(w.brakeRadius)) * X87(dt)) * X87(0.5f)) * X87(E.wearFactor)) * X87(p)) * X87(t)) * X87(FromBits(0xd1ba43b7)));
    s.wear = n - static_cast<uint32_t>(num::Ftol(prod));
    return true;
}

X87 Drive::BrakeFriction(float force, DriveSlot& s, const DriveWheelDef& w, float dt)
{
    const BrakeDef& e = def->brake;
    const uint32_t k = s.temperature;
    if (!BrakeWear(force, s, w, dt)) return X87(e.wornFriction);
    if (k < e.coldTemperature) return X87(e.coldFriction);
    auto lerp = [&](const float* table, uint32_t lo, uint32_t hi, float base) {
        const X87 t = num::TableLerpU(table, static_cast<int32_t>(k), static_cast<int32_t>(lo), static_cast<int32_t>(hi), 9);
        return t * (X87(e.peakFriction) - X87(base)) + X87(base);
    };
    if (k < e.optimalTemperature) return lerp(def->frictionRise, e.coldTemperature, e.optimalTemperature, e.coldFriction);
    if (k < e.fadeTemperature) return lerp(def->frictionFade, e.optimalTemperature, e.fadeTemperature, e.fadedFriction);
    return X87(e.fadedFriction);
}

X87 Drive::BrakeTorque(DriveSlot& s, const DriveWheelDef& w, float dt)
{
    F32 x = F32(w.brakeRatio) * F32(brakePressure) + F32(s.handbrakeRatio) * F32(handbrakePressure);
    if (!damage->brakeCircuit) x = 0.0f;
    const float v = (F32(w.brakeArea) * x).f();
    const float b = BrakeFriction(v, s, w, dt).Store();
    const float r = ((F32(w.brakeRadius) * F32(b)) * F32(v)).f();
    s.temperature = 500000000;   // (the classic code's temperatures end here: 500 K)
    s.temperature2 = 500000000;
    return X87(r);
}

float Drive::StarterMotor(float dt)
{
    const DriveState& e = *state;
    DriveDamage& t = *damage;
    const float s = e.engineSpeed;
    const bool engaged = starter || (e.gear == 1 && !jmp::BE(throttle, 0.5f));
    if (!jmp::BE(400.0f, s) && engaged && t.starter) {
        F32 c = t.battery;
        if (!jmp::BE(s, 300.0f)) c = F32(1.0f) - (F32(s) - F32(300.0f)) * F32(FromBits(0x3c23d70a));   // 0.01
        starterLevel = c.f();
        const F32 m = ((F32(t.wearFactor) * F32(FromBits(0x3ca3d70a))) * c) * F32(dt);                // 0.02
        const float torque = (c * F32(80.0f)).f();
        t.battery = (F32(t.battery) - m).f();   // the battery drains
        starterTorque = torque;
        return X87(torque).Store();
    }
    starterLevel = 0.0f;
    starterTorque = 0.0f;
    return 0.0f;
}

void Drive::DiffLockThreshold(int32_t i, float t)
{
    DrivePair& pair = state->pairs[i];
    float s = t;
    if (jmp::A(0.0f, s)) s = 0.0f;
    else if (jmp::A(s, 1.0f)) s = 1.0f;
    if (damage->hydraulics == 0) {   // no locking without the hydraulics
        pair.threshold = 0.0f;
        return;
    }
    const F32 a = Abs(F32(pair.speed));
    if (jmp::A(2.0f, a.f())) {   // slow: fading in from 1 to 2
        if (jmp::A(1.0f, a.f())) s = 0.0f;
        else s = ((a - F32(1.0f)) * F32(s)).f();
    }
    pair.threshold = (F32(def->diffs[i].maxTorque) * F32(s)).f();
}

void Drive::GearRequest(int32_t d)
{
    const int32_t n = static_cast<int32_t>(state->requestedGear + static_cast<uint32_t>(d));
    const int32_t top = static_cast<int32_t>(def->numberOfGears) - 1;
    state->requestedGear = static_cast<uint32_t>(n < 0 ? 0 : n > top ? top : n);
}

void Drive::GearSet(int32_t g)
{
    if (g >= static_cast<int32_t>(def->numberOfGears)) g = 1;   // neutral
    state->requestedGear = static_cast<uint32_t>(g);
}

void Drive::ApplyControl(float h, uint32_t s)
{
    handbrake = h;
    handbrakePressure = (F32(def->maxHandbrakePressure) * F32(h)).f();
    starter = s;
}

// ---- the gearbox and the clutch ----

bool Drive::GearLimits(float x)
{
    if (!hooks.gearLimits) return false;
    DriveState& e = *state;
    const GearDef& gear = def->gears[e.gear];
    const F32 k = damage->gears[e.gear];   // the gear's condition
    const F32 lo = (F32(damage->gearbox) * F32(gear.torqueMin)) * k;
    bool out = !jmp::BE(lo.f(), x);
    if (!out) out = !jmp::BE(x, ((F32(damage->gearbox) * F32(gear.torqueMax)) * k).f());
    if (!out) return false;
    e.engaged = 0;   // out of gear
    e.gearChanged = 1;
    if (hooks.gearOut) hooks.gearOut(hooks.ctx);
    return true;
}

X87 Drive::ClutchSlipSpeed() const
{
    const DriveState& e = *state;
    if (e.engaged) return X87(0.0f);
    const float r = def->gears[e.gear].ratio;
    if (r == 0.0f) return X87(0.0f);   // ucomiss; jnp
    return X87(e.engineSpeed) / X87(r) - X87(def->finalRatio) * X87(e.drivetrainSpeed);
}

void Drive::GearShiftStep(float dt)
{
    DriveState& e = *state;
    const DriveDef& d = *def;
    if (e.gear != e.requestedGear && damage->gearActuator) {
        if (!jmp::A(d.shiftDelay, e.shiftTimer)) {
            if (jmp::BE((F32(d.shiftEngageDelay) + F32(d.shiftDelay)).f(), e.shiftTimer)) {   // done: the new gear in
                e.engaged = 1;
                e.clutchBlend = 1.0f;
                e.gearChanged = 1;
                e.gear = e.requestedGear;
                e.shiftTimer = 0.0f;
                return;
            }
            if (e.engaged) {   // the clutch opens
                e.engaged = 0;
                e.gearChanged = 1;
                if (hooks.gearOut) hooks.gearOut(hooks.ctx);
            }
        }
        e.shiftTimer = (F32(e.shiftTimer) + F32(dt)).f();
        return;
    }
    if (e.engaged) {
        slipSpeed = 0.0f;
        shownClutchBlend = e.clutchBlend;
        return;
    }
    const float s = ClutchSlipSpeed().Store();
    const F32 a = jmp::A(s, 0.0f) ? F32(s) : F32(0.0f) - F32(s);
    F32 c = ((F32(e.clutchBlend) * F32(e.clutchBlend)) * a) * F32(FromBits(0x3ccccccd));   // 0.025
    if (!jmp::BE(FromBits(0x3e99999a), c.f())) c = FromBits(0x3e99999a);                   // 0.3
    else if (!jmp::BE(c.f(), 4.0f)) c = 4.0f;
    F32 v = F32(e.clutchBlend) - c * F32(dt);
    if (!jmp::BE(0.0f, v.f())) v = 0.0f;
    else if (!jmp::BE(v.f(), 1.0f)) v = 1.0f;
    e.clutchBlend = v.f();
    slipSpeed = a.f();
    shownClutchBlend = e.clutchBlend;
}

void Drive::GearEngage(float x)
{
    const F32 zero = 0.0f;
    const F32 ax = jmp::A(x, 0.0f) ? F32(x) : zero - F32(x);
    DriveState& e = *state;
    if (hooks.gearLimits) {   // the gear's wear
        const X87 q = (X87(ax.f()) * X87(damage->wearFactor)) * X87(FromBits(0x3a03126f));   // * 0.0005
        e.random = e.random * 0x2000081u - 0x19dc0be3u;
        float& w = damage->gears[e.gear];
        const X87 r = num::FildUnsigned(static_cast<int32_t>(e.random % 100001u)) * X87(FromBits(0x3727c5ac));   // * 1e-5
        const float nw = (X87(w) - r * (q * q)).Store();
        if (!jmp::BE(0.0f, nw)) w = 0.0f;
        else w = jmp::A(nw, 1.0f) ? 1.0f : nw;
        gearLoads[e.gear] = ax.f();
    }
    const F32 as = Abs(ax * F32(FromBits(0x3d088889)));   // / 30
    float level;
    if (jmp::A(0.0f, as.f())) level = 0.0f;
    else level = jmp::A(as.f(), FromBits(0x3f4ccccd)) ? FromBits(0x3f4ccccd) : as.f();   // 0.8
    gearChangeLevel = Sqrt(X87(level) + X87(FromBits(0x3e4ccccd))).Store();   // + 0.2
    if (hooks.gearChange) hooks.gearChange(hooks.ctx, GearChange{carId, e.gear, gearChangeLevel, e.engaged});
}

void Drive::ClutchLockStep(float torque, float* out, float capacity, float ratio, float dt)
{
    const F32 zero = 0.0f;
    DriveState& e = *state;
    const DriveDef& d = *def;
    auto gearRatio = [&]() { return d.gears[e.gear].ratio; };
    auto lock = [&](float v) {   // locked in direction sign(v)
        e.clutchLock = Sign(v);
        e.gearChanged = 1;
    };
    if (!(gearRatio() == 0.0f) && e.engaged && !e.clutchLock) {
        if (!e.gearChanged) {
            if (jmp::A(Abs(F32(torque)).f(), capacity)) {
                lock(torque);
                *out = (IntF(e.clutchLock) * F32(capacity)).f();
                GearLimits(*out);
            } else if (!GearLimits(torque)) {
                e.engineSpeed = (F32(e.drivetrainSpeed) * F32(ratio)).f();
                *out = torque;
            }
        } else {   // just changed: the engine's speed taken up by the drivetrain's
            const F32 x2 = F32(e.drivetrainSpeed) * F32(ratio);
            const F32 s = (F32(d.gearBoxInertiaIn) + F32(d.engineInertia)) * (x2 - F32(e.engineSpeed));
            const F32 v = F32(torque) - s / F32(dt);
            if (jmp::A(Abs(v).f(), capacity)) {
                lock(v.f());
                *out = (IntF(e.clutchLock) * F32(capacity)).f();
            } else {
                e.engineSpeed = x2.f();
                *out = v.f();
            }
            GearEngage(s.f());
        }
    }
    if (gearRatio() == 0.0f || !e.engaged) {   // free: the engine alone
        e.engineSpeed = ((F32(torque) / (F32(d.gearBoxInertiaIn) + F32(d.engineInertia))) * F32(dt) + F32(e.engineSpeed)).f();
        e.clutchLock = 0;
        *out = 0.0f;
        return;
    }
    if (!e.clutchLock) return;
    const F32 m = IntF(e.clutchLock) * F32(capacity);
    e.engineSpeed = (((F32(torque) - m) / F32(d.engineInertia)) * F32(dt) + F32(e.engineSpeed)).f();
    *out = (IntF(e.clutchLock) * F32(capacity)).f();
    const F32 target = F32(e.drivetrainSpeed) * F32(ratio);
    if (Sign((F32(e.engineSpeed) - target).f()) != e.clutchLock) {   // crossed: unlock
        e.gearChanged = 1;
        e.clutchLock = 0;
        e.engineSpeed = target.f();
    }
}

// ---- the 4 x 4 solver ----

void Drive::AxleTerm(uint32_t k)
{
    const DriveDef& D = *def;
    DriveState& S = *state;
    const DriveWheelDef& w = D.wheels[k];
    const DriveSlot& s = S.slots[k];
    if (s.locked == 0) {
        S.demands[k] = 0.0f;
        return;
    }
    const DiffDef& e = D.diffs[w.diff];
    F32 x = IntF(e.driven) * F32(D.diffs[0].ratio);
    const F32 y = F32(e.ratio) * F32(w.ratio);
    F32 r = F32(0.0f);
    if (SlotDriven(k)) r = (F32(0.0f) - IntF(s.locked) * F32(s.brakeTorque)) + F32(slotTorques[k]);
    r = r - F32(S.pairs[w.diff].delta);
    const F32 h = (y * F32(0.5f)) * F32(S.pairs[0].delta);
    x = ((x * y) * F32(0.25f)) * F32(S.gearboxTorque);
    S.demands[k] = ((r - h) + x).f();
}

void Drive::SlipLatch(uint32_t k)
{
    DriveState& S = *state;
    DriveSlot& s = S.slots[k];
    if (s.locked != 0) {
        gates[k] = 0.0f;
        return;
    }
    const DriveDef& D = *def;
    const DriveWheelDef& w = D.wheels[k];
    const DiffDef& e = D.diffs[w.diff];
    F32 x = F32(e.ratio) * F32(w.ratio);
    const F32 y = F32(S.gearboxTorque) - F32(S.inertia) * F32(centreTorque);
    F32 z = (IntF(e.driven) * F32(D.diffs[0].ratio)) * x;
    x = (x * F32(0.5f)) * F32(S.pairs[0].delta);
    z = z * F32(0.25f);
    x = x + F32(S.pairs[w.diff].delta);
    const F32 r = y * z - x;
    F32 hi = F32(0.0f), lo = F32(0.0f);
    if (SlotDriven(k)) {
        hi = F32(s.brakeTorque);
        lo = F32(slotTorques[k]);
    }
    int32_t latch = 0;
    if (!jmp::BE(r.f(), (hi - lo).f())) latch = 1;
    else if (!jmp::BE((F32(0.0f) - (hi + lo)).f(), r.f())) latch = -1;
    if (!latch) {
        gates[k] = 0.0f;
        return;
    }
    gates[k] = w.ratio;
    s.locked = latch;
    if (S.rebuildPending == 0) ++S.latchCount;
    S.rebuildPending = 1;
}

bool Drive::SlotRelease(uint32_t k)
{
    if (!(gates[k] == 0.0f)) return false;   // ucomiss; jp
    state->slots[k].drivenSpin = 0.0f;
    state->slots[k].locked = 0;
    return true;
}

bool Drive::PairCheck(uint32_t j)
{
    const uint32_t a = def->diffs[j].inputs[0], b = def->diffs[j].inputs[1];
    DriveState& s = *state;
    const int32_t ca = s.slots[a].locked, cb = s.slots[b].locked;
    const int idle = (ca == 0) + (cb == 0);
    DrivePair& pair = s.pairs[j];
    if (idle == 2) {
        pair.speed = 0.0f;
        pair.inputSum = 0.0f;
        pair.direction = 0;
        return false;
    }
    if (idle != 1 || pair.direction) return false;
    if (!(gates[a] == 0.0f) || !(gates[b] == 0.0f)) {
        pair.direction = s.slots[b].locked + s.slots[a].locked;
        pair.speed = 0.0f;
        pair.inputSum = 0.0f;
        return false;
    }
    if (ca) SlotRelease(a);
    else if (cb) SlotRelease(b);
    pair.speed = 0.0f;
    pair.inputSum = 0.0f;
    return true;
}

void Drive::PairApply(uint32_t j)
{
    const DriveDef& d = *def;
    const uint32_t a = d.diffs[j].inputs[0];   // (the classic code sets slot a twice, never b)
    DriveSlot& slot = state->slots[a];
    if (!slot.locked) return;
    const DrivePair& p = state->pairs[j];
    slot.drivenSpin = ((F32(d.wheels[a].ratio) / F32(d.diffs[j].ratio)) * F32(p.speed) + F32(p.inputSum) * F32(0.5f)).f();
}

void Drive::LockSolve()
{
    const DriveDef& d = *def;
    DriveState& s = *state;
    DrivePair& centre = s.pairs[0];
    DrivePair& front = s.pairs[1];
    DrivePair& rear = s.pairs[2];
    const F32 half = 0.5f, k2 = FromBits(0xc0000000);   // -2
    auto slotV = [&s](uint32_t i) { return F32(s.slots[i].drivenSpin); };
    auto rate = [&d](uint32_t i) { return F32(d.wheels[i].ratio); };
    for (;;) {
        bool changed = false, changed2 = false, flagA = false, flagB = false;
        int32_t cntA = 0, cntB = 0;
        for (uint32_t k = 0; k < 4; ++k) {   // release the slots whose direction flipped (gate open)
            DriveSlot& slot = s.slots[k];
            const int32_t type = static_cast<int32_t>(d.wheels[k].diff);
            if (!slot.locked) {
                if (type == 1) ++cntA;
                else if (type == 2) ++cntB;
                continue;
            }
            if (Sign(slot.drivenSpin) == slot.locked) continue;
            if (!(gates[k] == 0.0f)) continue;
            slot.drivenSpin = 0.0f;
            slot.locked = 0;
            changed = true;
            if (type == 1) {
                ++cntA;
                flagA = true;
            } else if (type == 2) {
                ++cntB;   // (the classic code leaves its flag clear)
            }
        }
        if (PairCheck(1)) {
            ++cntA;
            flagA = true;
            changed = true;
        }
        if (PairCheck(2)) {
            ++cntB;
            flagB = true;
            changed = true;
        } else if (!changed) {
            return;   // (changed2 is still false here)
        }
        if (!s.rebuildPending) ++s.releaseCount;
        s.rebuildPending = 1;
        // A differential left with one latched input: its sums from that input, or freed.
        auto single = [&](uint32_t j, DrivePair& p, int32_t& cnt, bool frontFirst) {
            const uint32_t i1 = d.diffs[j].inputs[0], i2 = d.diffs[j].inputs[1];
            const F32 sum = frontFirst ? slotV(i1) + slotV(i2) : slotV(i2) + slotV(i1);
            p.inputSum = sum.f();
            if (sum.f() == IntF(p.direction).f()) {   // ucomiss; jnp
                p.speed = ((rate(i2) * slotV(i2) + rate(i1) * slotV(i1)) * (F32(d.diffs[j].ratio) * half)).f();
            } else {
                p.speed = 0.0f;
                p.inputSum = 0.0f;
                p.direction = 0;
                if (PairCheck(j)) {
                    cnt = 2;
                    changed = true;
                }
            }
        };
        if (flagA && cntA == 1) single(1, front, cntA, true);
        if (flagB && cntB == 1) single(2, rear, cntB, false);
        const F32 n2 = IntF(d.diffs[2].driven), n1 = IntF(d.diffs[1].driven);
        const F32 cr = F32(d.diffs[0].ratio);
        const F32 t = (F32(front.speed) * n1 + F32(rear.speed) * n2) * (cr * half);
        // The centre's share from its speed, its sign latched in its direction (a flip releases it).
        auto share = [&](F32 n) {
            const F32 v = ((F32(centre.speed) / cr) * n) * k2;
            centre.inputSum = v.f();
            if (Sign(v.f()) != centre.direction) {
                centre.inputSum = 0.0f;
                centre.direction = 0;
                changed2 = true;
                centre.speed = 0.0f;
            }
        };
        auto speedOf = [&](const DiffDef& diff) { return (IntF(diff.driven) / cr) * F32(centre.speed) + F32(centre.inputSum) * half; };
        auto other = [&](uint32_t j) {   // the free input of a differential with one latched
            const DiffDef& diff = d.diffs[j];
            return s.slots[diff.inputs[0]].locked ? diff.inputs[1] : diff.inputs[0];
        };
        if (!jmp::A(Abs(t).f(), Abs(F32(centre.speed)).f())) {
            centre.speed = t.f();
            const F32 sum = F32(front.speed) + F32(rear.speed);
            centre.inputSum = sum.f();
            if (Sign(sum.f()) != centre.direction) {
                centre.inputSum = 0.0f;
                centre.direction = 0;
                changed2 = true;
            }
        } else if (cntA == 2) {
            if (cntB == 2) {
                centre.speed = centre.inputSum = 0.0f;
                front.speed = front.inputSum = 0.0f;
                rear.speed = rear.inputSum = 0.0f;
            } else {
                share(n1);
                rear.speed = speedOf(d.diffs[2]).f();
                PairApply(2);
            }
        } else if (cntB == 2) {
            share(n2);
            front.speed = speedOf(d.diffs[1]).f();
            PairApply(1);
        } else {
            front.speed = speedOf(d.diffs[1]).f();
            if (cntA == 1) front.inputSum = (((rate(other(1)) / F32(d.diffs[1].ratio)) * F32(front.speed)) * k2).f();
            PairApply(1);
            rear.speed = speedOf(d.diffs[2]).f();
            if (cntB == 1) rear.inputSum = (((rate(other(2)) / F32(d.diffs[2].ratio)) * F32(rear.speed)) * k2).f();
            PairApply(2);
        }
        if (!changed && !changed2) return;
    }
}

void Drive::LockState()
{
    const DriveDef& D = *def;
    DriveState& S = *state;
    S.freeFlags[0] = 0;
    bool free = true, locked = true;
    for (const DriveSlot& slot : S.slots) {
        if (slot.locked) free = false;
        else locked = false;
    }
    if (free) {
        S.lockState = 0;
        S.freeFlags[1] = S.freeFlags[2] = S.freeFlags[3] = 0;
        return;
    }
    auto flag = [&](uint32_t pair, bool v) { S.freeFlags[D.diffs[pair].lockFlag] = v ? 1 : 0; };
    auto bothFree = [&](const DiffDef& diff) { return S.slots[diff.inputs[0]].locked == 0 && S.slots[diff.inputs[1]].locked == 0; };
    S.lockState = locked ? 2 : 1;
    flag(0, S.pairs[0].direction == 0);
    flag(1, S.pairs[1].direction == 0 && !bothFree(D.diffs[1]));
    flag(2, S.pairs[2].direction == 0 && !bothFree(D.diffs[2]));
}

void Drive::SlotWeights(uint32_t k)
{
    const DriveDef& D = *def;
    DriveState& S = *state;
    const DriveSlot& Sl = S.slots[k];
    const DiffDef& diff = D.diffs[D.wheels[k].diff];
    const F32 x1 = IntF(diff.driven) * F32(D.diffs[0].ratio);
    const F32 x0 = F32(diff.ratio) * F32(D.wheels[k].ratio);
    const bool count = Sl.locked != 0;
    const F32 v = F32(Sl.weight), half = 0.5f;
    auto cell = [&](uint32_t r, F32 val) { S.coupling[k + 4 * r] = val.f(); };
    cell(0, count ? v / (x1 * x0) + ((F32(S.inertia) * x1) * x0) * F32(0.25f) : v / (x1 * x0));
    const uint32_t r1 = D.diffs[0].lockFlag;
    cell(r1, !S.freeFlags[r1] ? v / (x0 * F32(2.0f)) : count ? x0 * half : F32(0.0f));
    const uint32_t r2 = diff.lockFlag;
    cell(r2, !S.freeFlags[r2] ? v * half : count ? F32(1.0f) : F32(0.0f));
}

void Drive::PairSums(uint32_t j)
{
    const DriveDef& D = *def;
    DriveState& S = *state;
    const DiffDef& diff = D.diffs[j];
    DrivePair& pair = S.pairs[j];
    const uint32_t a = diff.inputs[0], b = diff.inputs[1];
    auto v = [&](uint32_t i) { return F32(S.slots[i].drivenSpin); };
    auto rate = [&](uint32_t i) { return F32(D.wheels[i].ratio); };
    pair.inputSum = (v(b) + v(a)).f();
    pair.speed = ((rate(b) * v(b) + rate(a) * v(a)) * (F32(diff.ratio) * F32(0.5f))).f();
    if (pair.inputSum == 0.0f) {   // the latches' summed sign (ucomiss: NaN takes the sign)
        const int32_t n = S.slots[a].locked + S.slots[b].locked;
        pair.direction = n > 0 ? 1 : (n < 0 ? -1 : 0);
    } else {
        pair.direction = Sign(pair.inputSum);
    }
}

void Drive::LockInit()
{
    const DriveDef& D = *def;
    DriveState& S = *state;
    PairSums(1);
    PairSums(2);
    const F32 s10 = F32(S.pairs[2].speed) + F32(S.pairs[1].speed);
    S.pairs[0].inputSum = s10.f();
    const F32 n2 = IntF(D.diffs[2].driven), n1 = IntF(D.diffs[1].driven);
    S.pairs[0].speed = ((n2 * F32(S.pairs[2].speed) + n1 * F32(S.pairs[1].speed)) * (F32(D.diffs[0].ratio) * F32(0.5f))).f();
    S.pairs[0].direction = Sign(s10.f());
}

void Drive::SlotSlip(uint32_t k, float dt)
{
    DriveSlot& Sl = state->slots[k];
    const F32 x = F32(slotTorques[k]);
    if (const int32_t c = Sl.turning) {
        const float old = Sl.spin;
        Sl.spin = (((x - IntF(c) * F32(Sl.brakeTorque)) * (F32(1.0f) / F32(Sl.inertia))) * F32(dt) + F32(Sl.spin)).f();
        if (Sign(Sl.spin) != Sl.turning) {   // stopped: held again
            Sl.spin = 0.0f;
            Sl.turning = 0;
        }
        Sl.spinAcceleration = ((F32(Sl.spin) - F32(old)) / F32(dt)).f();
        return;
    }
    if (jmp::A(Abs(x).f(), Sl.brakeTorque)) {   // breaks away
        Sl.turning = Sign(x.f());
        Sl.spinAcceleration = 0.0f;
    }
}

void Drive::LockRebuild()
{
    DriveState& S = *state;
    auto build = [&]() {
        LockState();
        std::memset(S.coupling, 0, sizeof S.coupling);
        for (uint32_t k = 0; k < 4; ++k) SlotWeights(k);
    };
    build();
    std::memset(gates, 0, sizeof gates);
    num::Mat44Invert(S.coupling, S.couplingInverse);
    S.rebuilds = (F32(S.rebuilds) + F32(1.0f)).f();
    S.rebuildPending = 0;
    if (S.couplingInverse[0] != S.couplingInverse[0]) {   // _isnan: once more
        build();
        num::Mat44Invert(S.coupling, S.couplingInverse);
    }
}

void Drive::SlotRateOrSlip(uint32_t k, float dt)
{
    if (!SlotDriven(k)) {
        SlotSlip(k, dt);
        return;
    }
    DriveSlot& Sl = state->slots[k];
    Sl.spinAcceleration = ((F32(Sl.drivenSpin) - F32(Sl.spin)) / F32(dt)).f();
    Sl.spin = Sl.drivenSpin;   // the spin and its sign follow the latched ones
    Sl.turning = Sl.locked;
}

void Drive::Step(float dt)
{
    const DriveDef& D = *def;
    DriveState& S = *state;
    const float* const v4 = &solved.x;
    if (S.rebuildPending) LockRebuild();
    auto pairDelta = [&S]() {
        for (DrivePair& p : S.pairs) p.delta = (IntF(p.direction) * F32(p.threshold)).f();
    };
    if (!S.lockState) {   // nothing latched
        centreTorque = 0.0f;
        for (DrivePair& p : S.pairs) p.delta = 0.0f;
        for (uint32_t k = 0; k < 4; ++k) SlipLatch(k);
        LockInit();
    } else {
        pairDelta();
        for (uint32_t k = 0; k < 4; ++k) AxleTerm(k);
        for (;;) {   // a free differential whose demand exceeds its threshold locks
            num::Mat44MulVec4(S.couplingInverse, reinterpret_cast<const Vec4*>(S.demands), &solved);
            bool changed = false;
            for (uint32_t j = 0; j < 3; ++j) {
                const uint32_t r = D.diffs[j].lockFlag;
                if (S.freeFlags[r] != 1) continue;
                if (!jmp::A(Abs(F32(v4[r])).f(), S.pairs[j].threshold)) continue;
                S.pairs[j].direction = Sign(v4[r]);
                changed = true;
            }
            if (!changed) break;
            ++S.lockCount;
            LockRebuild();
            pairDelta();
            for (uint32_t k = 0; k < 4; ++k) AxleTerm(k);
        }
        for (uint32_t j = 0; j < 3; ++j) {
            const uint32_t r = D.diffs[j].lockFlag;
            if (S.freeFlags[r] == 1) S.pairs[j].delta = v4[r];
        }
        float v[4];
        for (uint32_t i = 0; i < 4; ++i) v[i] = S.freeFlags[i] ? 0.0f : v4[i];
        centreTorque = 0.0f;
        for (uint32_t k = 0; k < 4; ++k) SlipLatch(k);
        centreTorque = v[0];
        const F32 t = F32(dt);
        S.pairs[0].speed = (F32(v[0]) * t + F32(S.pairs[0].speed)).f();
        for (uint32_t j = 0; j < 3; ++j) S.pairs[j].inputSum = (F32(v[D.diffs[j].lockFlag]) * t + F32(S.pairs[j].inputSum)).f();
        for (DrivePair& p : S.pairs) {   // a differential whose sum changed sign frees (and asks for a rebuild)
            if (Sign(p.inputSum) == p.direction) continue;
            p.inputSum = 0.0f;
            p.direction = 0;
            if (!S.rebuildPending) ++S.freeCount;
            S.rebuildPending = 1;
        }
        const F32 h = F32(S.pairs[0].inputSum) * F32(0.5f);
        const F32 cr = F32(D.diffs[0].ratio);
        S.pairs[1].speed = ((IntF(D.diffs[1].driven) / cr) * F32(S.pairs[0].speed) + h).f();
        S.pairs[2].speed = ((IntF(D.diffs[2].driven) / cr) * F32(S.pairs[0].speed) + h).f();
        for (uint32_t k = 0; k < 4; ++k) {   // latched slots with an open gate: their speed through their differential
            DriveSlot& slot = S.slots[k];
            if (!slot.locked || !(gates[k] == 0.0f)) continue;
            const uint32_t pt = D.wheels[k].diff;
            slot.drivenSpin = ((F32(S.pairs[pt].speed) / F32(D.diffs[pt].ratio)) * F32(D.wheels[k].ratio) + F32(S.pairs[pt].inputSum) * F32(0.5f)).f();
        }
        LockSolve();
    }
    S.drivetrainSpeed = S.pairs[0].speed;
    for (uint32_t k = 0; k < 4; ++k) SlotRateOrSlip(k, dt);
}

// ---- the 2WD solver ----

void Drive::SlipLatch2WD(uint32_t k)
{
    const DriveDef& D = *def;
    DriveState& S = *state;
    DriveSlot& Sl = S.slots[k];
    const uint32_t j = k & 1;
    if (Sl.locked) {
        gates2[j] = 0.0f;
        return;
    }
    const DriveWheelDef& W = D.wheels[k];
    const F32 r = (F32(S.gearboxTorqueNeg) - F32(S.inertia) * F32(centreTorque)) * ((F32(D.diffs[W.diff].ratio) * F32(W.ratio)) * F32(0.5f)) -
                  F32(S.pairs[W.diff].delta);
    F32 lo = 0.0f, hi = 0.0f;
    if (SlotDriven(k)) {
        lo = F32(slotTorques[k]);
        hi = F32(Sl.brakeTorque);
    }
    int32_t latch = 0;
    if (!jmp::BE(r.f(), (hi - lo).f())) latch = 1;
    else if (!jmp::BE((F32(0.0f) - (hi + lo)).f(), r.f())) latch = -1;
    if (!latch) {
        gates2[j] = 0.0f;
        return;
    }
    gates2[j] = W.ratio;
    Sl.locked = latch;
    if (!S.rebuildPending) ++S.latchCount;
    S.rebuildPending = 1;
}

void Drive::SlotDemand2WD(uint32_t k)
{
    const DriveDef& D = *def;
    const DriveState& S = *state;
    const DriveSlot& Sl = S.slots[k];
    const uint32_t j = k & 1;
    if (!Sl.locked) {
        demands2[j] = 0.0f;
        return;
    }
    const uint32_t t = D.wheels[k].diff;
    const F32 x0 = F32(D.diffs[t].ratio) * F32(D.wheels[k].ratio);
    F32 x1 = 0.0f;
    if (SlotDriven(k)) x1 = (F32(0.0f) - IntF(Sl.locked) * F32(Sl.brakeTorque)) + F32(slotTorques[k]);
    demands2[j] = ((x0 * F32(0.5f)) * F32(S.gearboxTorqueNeg) + (x1 - F32(S.pairs[t].delta))).f();
}

void Drive::SlotWeights2WD(uint32_t k)
{
    const DriveDef& D = *def;
    DriveState& S = *state;
    const F32 x0 = F32(D.diffs[D.wheels[k].diff].ratio) * F32(D.wheels[k].ratio);
    const uint32_t j = k & 1;
    const F32 v = F32(S.slots[k].weight);
    const bool count = S.slots[k].locked != 0;
    S.coupling2[2 * j] = (count ? (F32(S.inertia) * x0) * F32(0.5f) + v / x0 : v / x0).f();
    S.coupling2[2 * j + 1] = !S.open2WD ? (v * F32(0.5f)).f() : count ? 1.0f : 0.0f;
}

void Drive::LockRebuild2WD()
{
    const DriveDef& D = *def;
    DriveState& S = *state;
    const uint32_t a = D.diffs[1].inputs[0], b = D.diffs[1].inputs[1];
    S.held2WD = 0;
    S.open2WD = !S.pairs[1].direction && (S.slots[a].locked || S.slots[b].locked) ? 1 : 0;
    std::memset(S.coupling2, 0, sizeof S.coupling2);
    SlotWeights2WD(a);
    SlotWeights2WD(b);
    const F32 m00 = F32(S.coupling2[0]), m01 = F32(S.coupling2[1]), m10 = F32(S.coupling2[2]), m11 = F32(S.coupling2[3]);
    const F32 inv = F32(1.0f) / (m11 * m00 - m10 * m01);
    S.coupling2Inverse[0] = (inv * m11).f();
    S.coupling2Inverse[1] = (F32(0.0f) - inv * m01).f();
    S.coupling2Inverse[2] = (F32(0.0f) - inv * m10).f();
    S.coupling2Inverse[3] = (inv * m00).f();
    S.rebuilds = (F32(S.rebuilds) + F32(1.0f)).f();
    S.rebuildPending = 0;
}

void Drive::Step2WD(float dt)
{
    const DriveDef& D = *def;
    DriveState& S = *state;
    DrivePair& pair = S.pairs[1];   // the one coupling
    S.gearboxTorqueNeg = (F32(0.0f) - F32(S.gearboxTorque)).f();
    if (S.rebuildPending) LockRebuild2WD();
    if (!S.slots[0].locked && !S.slots[1].locked) {   // both slots free
        centreTorque = 0.0f;
        pair.delta = 0.0f;
        SlipLatch2WD(0);
        SlipLatch2WD(1);
        PairSums(1);
    } else {
        auto demands = [&]() {
            pair.delta = (IntF(pair.direction) * F32(pair.threshold)).f();
            SlotDemand2WD(0);
            SlotDemand2WD(1);
        };
        demands();
        float dc;
        for (;;) {   // forces = inverse * demands; a free pair (flag 1) whose solved demand passes the threshold locks
            const float* const M = S.coupling2Inverse;
            forces2[0] = (F32(M[0]) * F32(demands2[0]) + F32(M[1]) * F32(demands2[1])).f();
            dc = (F32(M[2]) * F32(demands2[0]) + F32(M[3]) * F32(demands2[1])).f();
            forces2[1] = dc;
            const uint32_t r = D.diffs[1].lockFlag;
            if (S.freeFlags[r] != 1) break;
            const float x = (&solved.x)[r];   // (the 4 x 4 solver's vector: nothing here changes it)
            if (!jmp::A(Abs(F32(x)).f(), pair.threshold)) break;
            pair.direction = Sign(x);
            ++S.lockCount;
            LockRebuild2WD();
            demands();
            Unreached("Drive::Step2WD re-lock");   // the classic code would spin here
        }
        if (S.open2WD == 1) pair.delta = dc;
        const float a = S.held2WD ? 0.0f : forces2[0];
        const float b = S.open2WD ? 0.0f : forces2[1];
        centreTorque = 0.0f;
        for (uint32_t k = 0; k < 2; ++k) SlipLatch2WD(k);
        centreTorque = a;
        const F32 t = F32(dt);
        pair.speed = (F32(a) * t + F32(pair.speed)).f();
        pair.inputSum = (F32(b) * t + F32(pair.inputSum)).f();
        if (Sign(pair.inputSum) != pair.direction) {   // the sum changed sign: free (and ask for a rebuild)
            pair.inputSum = 0.0f;
            pair.direction = 0;
            if (!S.rebuildPending) ++S.freeCount;
            S.rebuildPending = 1;
        }
        for (uint32_t k = 0; k < 2; ++k) {   // latched slots with an open gate: their speed through their differential
            DriveSlot& slot = S.slots[k];
            if (!slot.locked || !(gates2[k] == 0.0f)) continue;
            const uint32_t pt = D.wheels[k].diff;
            const F32 h = F32(S.pairs[pt].inputSum) * F32(0.5f);
            slot.drivenSpin = ((F32(S.pairs[pt].speed) / F32(D.diffs[pt].ratio)) * F32(D.wheels[k].ratio) + h).f();
        }
        PairSums(1);
        for (;;) {   // a latched slot whose speed left its sign (gate open) frees, then the pair sums again
            bool changed = false;
            for (uint32_t k = 0; k < 2; ++k) {
                DriveSlot& slot = S.slots[k];
                if (slot.locked == Sign(slot.drivenSpin) || !(gates2[k] == 0.0f)) continue;
                slot.drivenSpin = 0.0f;
                slot.locked = 0;
                changed = true;
            }
            if (!changed) break;
            PairSums(1);
        }
    }
    S.drivetrainSpeed = (F32(0.0f) - F32(pair.speed)).f();
    for (uint32_t k = 0; k < 2; ++k) SlotRateOrSlip(k, dt);
    for (uint32_t k = 2; k < 4; ++k) SlotSlip(k, dt);
}

// ---- the tick ----

void Drive::Update(float dt)
{
    DriveState& S = *state;
    const DriveDef& D = *def;
    S.updates = (F32(S.updates) + F32(1.0f)).f();
    const X87 engine = hooks.engineStep(hooks.ctx, dt, S.engineSpeed);
    const X87 avg = TorqueAverage((engine * X87(FromBits(0x3f866666))).Store());   // * 1.05
    engineTorque = avg.Store();
    const float torque = (X87(StarterMotor(dt)) + X87(avg.Store())).Store();
    for (uint32_t k = 0; k < 4; ++k)   // the slots' brake torques, plus their extra
        S.slots[k].brakeTorque = (BrakeTorque(S.slots[k], D.wheels[k], dt) + X87(slotBrakeExtra[k])).Store();
    GearShiftStep(dt);
    const float scaled = (X87(D.gears[S.gear].ratio) * X87(D.finalRatio)).Store();   // the gear ratio times the final drive
    const float capacity = (((F32(1.0f) - F32(clutch)) * F32(damage->clutch)) * F32(900.0f)).f();
    float out = 0.0f;
    ClutchLockStep(torque, &out, capacity, scaled, dt);
    const bool inGear = S.engaged && !(D.gears[S.gear].ratio == 0.0f);   // (ucomiss: NaN counts as one)
    S.gearboxTorque = inGear ? (F32(out) * F32(scaled)).f() : 0.0f;
    if (S.gearChanged || damage->changed) {   // a gear or clutch change: the reflected inertia, rebuild
        F32 j = 0.0f;
        if (S.engaged) {
            F32 in = F32(D.gearBoxInertiaIn);
            if (!S.clutchLock) in = F32(D.engineInertia) + in;
            const F32 r = F32(D.gears[S.gear].ratio);
            j = (r * r) * in;
        }
        const F32 fd = F32(D.finalRatio);
        S.inertia = (((F32(D.gearBoxInertiaOut) + j) * fd) * fd).f();
        S.rebuildPending = 1;
        ++S.inertiaCount;
        S.gearChanged = 0;
        damage->changed = 0;
    }
    if (static_cast<int32_t>(D.driveType) == 3) Step2WD(dt);
    else Step(dt);
    outputSpeed = S.clutchLock && S.engaged && !(scaled == 0.0f) ? S.drivetrainSpeed : S.engineSpeed;
    clutchTorque = Abs(F32(out)).f();
}

} // namespace phys::native
