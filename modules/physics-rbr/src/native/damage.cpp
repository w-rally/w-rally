#include "native/damage.h"

#include "num/f32.h"
#include "num/bits.h"
#include "num/x87.h"

namespace phys::native {

using num::F32;
using num::X87;
using num::FromBits;
namespace jmp = num::jmp;

bool DamageEffectOf(uint32_t a, DamageEffectKind& out)
{
    using K = DamageEffectKind;
    switch (a) {
    case 0x4ed300: out = K::None; return true;
    case 0x4ec660: out = K::Radiator; return true;
    case 0x4ed020: out = K::Gearbox; return true;
    case 0x4ec820: out = K::Starter; return true;
    case 0x4ef5f0: out = K::GearActuator; return true;
    case 0x4ec930: out = K::WaterPump; return true;
    case 0x4ecaa0: out = K::HingedOff; return true;
    case 0x4ecbb0: out = K::LampPairA; return true;
    case 0x4ecc30: out = K::LampPairB; return true;
    case 0x4ec7b0: out = K::Differential; return true;
    case 0x4ec890: out = K::OilPump; return true;
    case 0x4ec9a0: out = K::BrakeCircuit; return true;
    case 0x4eca00: out = K::TopMountSide; return true;
    case 0x4ecb20: out = K::Lock; return true;
    case 0x4eccb0: out = K::LampPairC; return true;
    case 0x4ecd30: out = K::LampPairD; return true;
    case 0x4ecdb0: out = K::LampPairE; return true;
    case 0x4eceb0: out = K::Clutch; return true;
    case 0x4ecf20: out = K::DriveShaft; return true;
    case 0x4ecfa0: out = K::Part3dd8; return true;
    case 0x4ecfe0: out = K::Windscreen; return true;
    case 0x4ed200: out = K::Mounted; return true;
    }
    return false;
}

namespace {
float Clamp01Low(F32 y)   // 0 > y -> 0, else y > 1 -> 1
{
    if (jmp::A(0.0f, y.f())) return 0.0f;
    if (jmp::A(y.f(), 1.0f)) return 1.0f;
    return y.f();
}
X87 Random(float lo, float hi, uint32_t* state)   // DamageRandom (0x4ebd70)
{
    *state = *state * 0x2000081u - 0x19dc0be3u;
    const uint32_t r = *state % 100001u;
    return (X87::Raw(static_cast<double>(r)) * X87(1e-5f)) * (X87(hi) - X87(lo)) + X87(lo);
}
} // namespace

void DamageHandler::Ring(uint32_t type, uint32_t c)
{
    eventIndex = (eventIndex + 1) % 10u;
    events[eventIndex] = {type, c};
}

float DamageHandler::Scaled(float v) const   // the hit without the level's multiplier (0 at level 0)
{
    return jmp::A(multiplier, 0.0f) ? (F32(v) / F32(multiplier)).f() : 0.0f;
}

int DamageHandler::Effect(const DamageEffectDef& e, float v)
{
    const float a = e.threshold, b = e.strength;
    const uint32_t c = e.index;
    using K = DamageEffectKind;
    // A flag cleared at level 2+ (DamageEffectClear) / a pair of lamps cleared with its group's changed flag (ClearPair).
    auto clear = [&](uint32_t& flag, uint32_t type, bool signal) {
        if (level < 2 || !jmp::A(v, a) || !flag) return 0;
        flag = 0;
        Ring(type, c);
        if (signal && hooks.gearActuator) hooks.gearActuator(hooks.ctx, 8);
        return 0;
    };
    auto pair = [&](uint32_t first, uint32_t group, uint32_t type, int result) {
        if (!jmp::A(v, a)) return 0;
        uint32_t* const p = &lights.lampPairs[first + 2 * c];
        if (!p[0] && !p[1]) return 0;
        p[0] = p[1] = 0;
        lights.changed[group] = 1;
        Ring(type, c);
        return result;
    };
    switch (e.kind) {
    case K::None: return 0;
    case K::Radiator: {
        float& radiator = engine.radiator;
        if (level < 2 || !jmp::A(v, a) || !jmp::A(1.0f, radiator)) return 0;
        const F32 k = level >= 3 ? F32(0.8999999761581421f) : F32(0.6000000238418579f);
        F32 x = (F32(v) - F32(a)) * F32(b) + F32(radiator);
        if (jmp::A(0.0f, x.f())) x = F32(0.0f);
        else if (jmp::A(x.f(), k.f())) x = k;
        radiator = x.f();
        Ring(0, c);
        return 0;
    }
    case K::Gearbox: {
        if (level < 2 || !jmp::A(v, a)) return 0;
        const float cur = drive.gearbox;
        if (cur == 0.0f) return 0;
        const F32 lo = level >= 3 ? F32(0.0f) : F32(0.4000000059604645f);
        F32 x = F32(cur) - (F32(v) - F32(a)) * F32(b);
        drive.changed = 1;
        if (jmp::A(lo.f(), x.f())) x = lo;
        else if (jmp::A(x.f(), 1.0f)) x = F32(1.0f);
        drive.gearbox = x.f();
        Ring(1, c);
        return 5;
    }
    case K::Starter: return clear(drive.starter, 0xb, false);
    case K::GearActuator: return clear(drive.gearActuator, 2, true);
    case K::WaterPump: return clear(engine.waterPump, 0xf, false);
    case K::HingedOff: {
        if (!jmp::A(v, a)) return 0;
        PartDamage& h = hinged[c];
        if (h.lock || !h.attached) return 0;
        h.attached = 0;
        Ring(7, c);
        return 0;
    }
    case K::LampPairA: return pair(8, 2, 0x13, 1);
    case K::LampPairB: return pair(0, 1, 0x14, 1);
    case K::LampPairC: return pair(24, 3, 0x15, 2);
    case K::LampPairD: return pair(16, 6, 0x15, 2);
    case K::LampPairE: return pair(32, 4, 0x16, 2);
    case K::Differential: {
        if (c >= 3 || !jmp::A(v, a) || !diffWorking[c]) return 0;
        diffWorking[c] = 0;
        Ring(4, c);
        return 0;
    }
    case K::OilPump: {
        if (level < 2 || !jmp::A(v, a)) return 0;
        const float cur = engine.oilPump;
        if (cur == 0.0f) return 0;
        engine.oilPump = Clamp01Low(F32(cur) - (F32(v) - F32(a)) * F32(b));
        Ring(0xe, c);
        return 0;
    }
    case K::BrakeCircuit: {
        if (!jmp::A(v, a) || !drive.brakeCircuit) return 0;
        drive.brakeCircuit = 0;
        Ring(0xd, c);
        return 0;
    }
    case K::TopMountSide: {
        if (c >= 4 || !jmp::A(v, a)) return 0;
        float& side = wheels[c].topMountSide;
        if (!jmp::A(1.0f, side)) return 0;
        side = Clamp01Low((F32(v) - F32(a)) * F32(b) + F32(side));
        Ring(10, c);
        return 0;
    }
    case K::Lock: {
        if (!jmp::A(Scaled(v), a)) return 0;
        uint32_t& lock = hinged[c].lock;
        if (!lock || (c == 2 && level < 2)) return 0;
        lock = 0;
        Ring(8, c);
        return 0;
    }
    case K::Clutch: {
        if (level < 2 || !jmp::A(v, a)) return 0;
        const float k = 0.30000001192092896f;
        if (!jmp::A(drive.clutch, k)) return 0;
        drive.clutch = k;
        Ring(0x10, c);
        return 0;
    }
    case K::DriveShaft: {
        if (!jmp::A(v, a) || !drive.driveShaft[c]) return 0;
        drive.changed = 1;
        drive.driveShaft[c] = 0;
        Ring(0x11, c);
        return 0;
    }
    case K::Part3dd8: {
        uint32_t& part = parts3dd8[c];
        if (level < 2 || !jmp::A(v, a) || part != 1) return 0;
        part = 0;
        return 3;
    }
    case K::Windscreen: {
        uint32_t& screen = windscreens[c];
        if (!jmp::A(v, a) || screen != 1) return 0;
        screen = 0;
        return 4;
    }
    case K::Mounted: {
        const float x = Scaled(v);
        uint32_t& st = mounted[c];
        if (st == 0) {
            if (!jmp::A(x, a)) return 0;
            st = jmp::A(x, (F32(a) * F32(2.0f)).f()) ? 2 : 1;
        } else if (st == 1) {
            if (!jmp::A(x, a)) return 0;
            st = 2;
        } else {
            return 0;
        }
        Ring(0x1b, c);
        return 0;
    }
    }
    return 0;
}

bool DamageHandler::ZoneHit(uint32_t i, float amount)
{
    // Zone z's value raised to w: its damage (below 1: min-like of w and 1; NaN: 1) and the changed flags.
    auto raise = [&](uint32_t z, F32 w) {
        DamageBodyZone& e = bodyZones[z];
        if (!jmp::A(w.f(), e.value)) return false;
        const bool open = jmp::A(1.0f, zoneDamage[z]);
        e.value = w.f();
        if (open) {
            zoneDamage[z] = jmp::A(1.0f, w.f()) ? w.f() : 1.0f;
            zoneChanged[z] = 1;
            changed = 1;
        }
        return true;
    };
    const DamageBodyZone& e = bodyZones[i];
    const F32 v = F32(e.factor) * F32(amount);
    bool done = raise(i, v);
    for (const DamageZoneLink& child : e.children) {   // zone 0 ends the list
        if (!child.zone) break;
        if (done) continue;   // (sic: the first child whose value rises stops the propagation; the zone itself too)
        done = raise(child.zone, F32(child.weight) * v);
    }
    return done;
}

void DamageHandler::ZoneEffects(uint32_t zone, float amount, float* best)
{
    const DamageEffectZone& z = effectZones[zone];
    for (uint32_t i = 0; i < z.count; ++i) {
        const DamageEffectDef& e = z.effects[i];
        random = random * 0x2000081u - 0x19dc0be3u;
        const uint32_t r = random % 100001u;
        // DamageRandom(0.9, 1.1) inlined, times the amount
        const float v = (((X87::Raw(static_cast<double>(r)) * X87(1e-5f)) * X87(0.20000004768371582f) + X87(0.9f)) * X87(amount)).Store();
        const int got = Effect(e, v);
        if (!got || jmp::B(e.strength, *best)) continue;
        signal.effect = got;   // the strongest effect so far
        signal.effectIndex = e.index;
        *best = e.strength;
    }
}

bool DamageHandler::FireCheck(float a, float b)
{
    if (level < 3 || !jmp::A(a, b) || !engine.headGasket) return false;
    engine.headGasket = 0;   // the engine dies: no starter, no electrics; the lamp flag [0]
    drive.starter = 0;
    lights.electrical = 0;
    lights.changed[0] = 1;
    const X87 r = Random(0.0f, 1.0f, &random);
    if (jmp::A(0.05f, r.d) && hooks.fire) hooks.fire(hooks.ctx);   // 5 %: CSigEngineFire
    return true;
}

void DamageHandler::Hit(uint32_t effectZone, uint32_t zone, uint32_t id, float amount, float b, const Vec4& p, const Vec4& q)
{
    CarDamageSignal& s = signal;
    s.effect = 0;
    bool hit = false;
    if (hooks.enabled) {   // damage on
        if (zone && zone < 0x40) hit = ZoneHit(zone, amount);
        if (jmp::A(multiplier, 0.0)) {   // the effects, scaled by the level's multiplier
            if (zone >= 0x40) Unreached("a hit's body zone past the table (its children and fire factor read beyond)");
            float best = 0.0f;
            const float v = (F32(multiplier) * F32(amount)).f();
            s.effect = 0;
            if (effectZone && effectZone < 0x40) ZoneEffects(effectZone, v, &best);
            for (const DamageZoneLink& child : bodyZones[zone].effectChildren)
                if (child.zone && child.zone < 0x40) ZoneEffects(child.zone, (F32(child.weight) * F32(v)).f(), &best);
            FireCheck((F32(bodyZones[zone].fireFactor) * F32(v)).f(), 11000.0f);
        }
    }
    if (hit || jmp::A(amount, 500.0)) {   // the damage record (CSigCarDamage)
        s.amount = amount;
        s.kind = 2;
        s.b = b;
        s.id = id;
        s.point = p;
        s.direction = q;
        if (hooks.emit) hooks.emit(hooks.ctx, s);
    }
    if (effectZone) lastEffectZone = effectZone;   // the last hit
    if (zone) lastZone = zone;
    lastAmount = amount;
    if (jmp::A(amount, largestAmount)) largestAmount = amount;
    lastId = id > 0xff ? 0 : id;
}

void DamageHandler::Hit(const ImpulseHit& h) { Hit(h.surfaceA, h.surfaceB, h.material, h.normal, h.slide, h.pointInBody, h.point); }

void DamageHandler::Impact(const ContactImpact& c)
{
    const float k = (F32(c.force) * F32(num::FromBits(0x3da3d70a))).f();   // * 0.08
    Hit(c.surfaceA, c.surfaceB, c.material, k, c.slide, c.bodyPoint, c.point);
}

namespace {
#include "native/damage_tables.inc"
} // namespace

void DamageHandler::Create()
{
    DamageHandlerCreated(*this);
    engine.random = &random;
}

void DamageHandler::SetLevel(uint32_t l)
{
    level = static_cast<int32_t>(l);
    const uint32_t m = l == 0 ? 0u : l == 1 ? 0x3e4ccccdu : l == 2 ? 0x3f333333u : 0x3f800000u;   // 0, 0.2, 0.7, 1
    std::memcpy(&multiplier, &m, 4);
}

void DamageHandler::Reset()
{
    DamageZonesBuilt(*this);
    engine.random = &random;
    float a = 0.0f, b = 0.0f, k = 0.0f;   // by the level: the suspension's and the dampers' load factors, the gears' wear
    uint32_t flag = 0;
    switch (level) {
    case 1: a = b = 0.5f, k = FromBits(0x3e4ccccd); break;                                           // 0.2
    case 2: a = FromBits(0x3f4ccccd), b = FromBits(0x3f733333), k = FromBits(0x3f19999a); break;   // 0.8 / 0.95 / 0.6
    case 3: a = b = k = 1.0f, flag = 1; break;
    default: break;
    }
    for (WheelDamage& w : wheels) w.loadFactor = a;
    for (DamperDamage& s : dampers) s.loadFactor = b;
    engine.enabled = flag;
    drive.wearFactor = k;
    for (uint32_t z = 0; z < 64; ++z) {   // the zones' damage: their values clamped
        float x = bodyZones[z].value;
        if (jmp::A(0.0f, x)) x = 0.0f;
        else if (jmp::A(x, 1.0f)) x = 1.0f;
        zoneDamage[z] = x;
    }
    signal.effectIndex = 0, signal.effect = 0, signal.kind = 0;   // (the reset's and the record's signals: not sent here)
    signal.amount = 0.0f, signal.b = 0.0f, signal.id = 0;
    signal.point = Vec4{}, signal.direction = Vec4{};
    changed = 0;
    std::memset(zoneChanged, 0, sizeof zoneChanged);
}

} // namespace phys::native
