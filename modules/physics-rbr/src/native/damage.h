#pragma once
// The car's damage handler, natively (M6 step 2; the classic CCarDamageHandler, core/contact.cpp and core/damage.cpp): what a
// hit does to the car. 64 body zones take a hit's amount (each its factor; the deformation the renderer shows, spread to its
// children), and with the level's multiplier the zones' effects act on the parts - the radiator, the gearbox, the clutch,
// the differentials, the drive shafts, the brake circuit, the starter, the gear actuator, the water and oil pumps, the
// lamps, the top mounts, the hinged parts' locks and attachments, the mounted parts, the windscreens - each above its
// threshold, a random spread on the amount. A hard enough hit with realistic damage sets the engine on fire.
//
// The handler owns the parts' damage records (the drive's, the engine's, the wheels', the dampers', the tyres' wear, the
// hinged and mounted parts'); the car's systems point into them. The classic code's arithmetic is kept bit for bit.
#include <cstdint>

#include "num/math.h"
#include "native/drive.h"
#include "native/engine.h"
#include "native/car_solids.h"
#include "native/suspension.h"
#include "native/wheel.h"

namespace phys::native {

// The effects (the game's member functions in the zone table, by address).
enum class DamageEffectKind : uint8_t {
    None,              // 0x4ed300 (nothing)
    Radiator,          // 0x4ec660: the radiator damaged
    Gearbox,           // 0x4ed020: the gearbox's torque limits lowered
    Starter,           // 0x4ec820: the starter broken
    GearActuator,      // 0x4ef5f0: the gear actuator broken (a signal)
    WaterPump,         // 0x4ec930
    HingedOff,         // 0x4ecaa0: a hinged part torn off (unless locked)
    LampPairA,         // 0x4ecbb0: lamp pairs 8.. (lights group 2)
    LampPairB,         // 0x4ecc30: lamp pairs 0.. (group 1)
    Differential,      // 0x4ec7b0
    OilPump,           // 0x4ec890
    BrakeCircuit,      // 0x4ec9a0
    TopMountSide,      // 0x4eca00
    Lock,              // 0x4ecb20: a hinged part's lock broken (the bonnet's at level 2+)
    LampPairC,         // 0x4eccb0: lamp pairs 24.. (group 3)
    LampPairD,         // 0x4ecd30: lamp pairs 16.. (group 6)
    LampPairE,         // 0x4ecdb0: lamp pairs 32.. (group 4)
    Clutch,            // 0x4eceb0: at most 0.3
    DriveShaft,        // 0x4ecf20
    Part3dd8,          // 0x4ecfa0: (a part cleared at level 2+)
    Windscreen,        // 0x4ecfe0
    Mounted,           // 0x4ed200: a mounted part loosened or torn off
};
// The kind of a game address (false: an unknown one).
bool DamageEffectOf(uint32_t address, DamageEffectKind& out);

struct DamageEffectDef {
    DamageEffectKind kind = DamageEffectKind::None;
    float threshold = 0;     // a: the strength above which it acts
    float strength = 0;      // b: its rate; the strongest acting effect is the hit's
    uint32_t index = 0;      // c: which of the parts
};

struct DamageEffectZone {
    DamageEffectDef effects[4];
    uint32_t count = 0;
};

struct DamageZoneLink {
    float weight = 0;
    uint32_t zone = 0;       // 0 ends a list
};

struct DamageBodyZone {
    float fireFactor = 0;    // the hit times this against the engine fire's threshold
    float factor = 0;        // the hit's amount times this raises the value
    float value = 0;         // the largest so far
    DamageZoneLink children[4];        // the zones a hit spreads to
    DamageZoneLink effectChildren[4];  // the effect zones it also triggers
};

struct LightsDamage {
    uint32_t changed[7] = {};      // by light group: set when the group changes (the renderer clears them)
    uint32_t lampPairs[40] = {};   // five blocks of four pairs, each with a lamp flag
    uint32_t electrical = 0;
};

// The CSigCarDamage record.
struct CarDamageSignal {
    int32_t effect = 0;            // the strongest effect's result
    uint32_t kind = 0;             // 2: a hit
    uint32_t effectIndex = 0, id = 0;
    float amount = 0, b = 0;
    Vec4 direction{}, point{};
};

struct DamageHooks {
    void (*emit)(void* ctx, const CarDamageSignal& s) = nullptr;   // CSigCarDamage
    void (*fire)(void* ctx) = nullptr;                             // CSigEngineFire
    void (*gearActuator)(void* ctx, uint32_t value) = nullptr;     // the gear actuator's signal (0x4ae0d0)
    void* ctx = nullptr;
    bool enabled = false;          // the config's damage flag
};

class DamageHandler : public DamageListener {
public:
    DamageEffectZone effectZones[64];
    DamageBodyZone bodyZones[64];
    float zoneDamage[64] = {};     // the body zones' values clamped to 0 .. 1 (the deformation)
    uint32_t zoneChanged[64] = {};
    uint32_t changed = 0;          // a zone changed
    // The parts' damage records.
    DriveDamage drive;
    uint32_t diffWorking[3] = {};  // the differentials' (centre, front, rear)
    EngineDamage engine;           // (its random seed: `random` below)
    LightsDamage lights;
    WheelDamage wheels[4];
    DamperDamage dampers[4];
    float tyres[32] = {};          // eight a tyre (the tread's wear)
    PartDamage hinged[4];
    uint32_t mounted[4] = {};      // 0 on, 1 loose, 2 off
    uint32_t windscreens[5] = {};
    uint32_t parts3dd8[4] = {};
    uint32_t random = 0;           // the seed (the effects' and the engine's generator)
    float multiplier = 0;          // the level's: 0, 0.2, 0.7, 1
    int32_t level = 0;             // the game option's damage level (0 off .. 3 realistic)
    uint32_t lastEffectZone = 0, lastZone = 0;
    float lastAmount = 0, largestAmount = 0;
    uint32_t lastId = 0;
    struct Event {
        uint32_t type = 0, index = 0;
    } events[10];                  // the last effects (a ring)
    uint32_t eventIndex = 0;
    CarDamageSignal signal;
    DamageHooks hooks;

    // A hit (DamageHandlerHit, vslot 1, 0x4f12a0): the zone raised, the effect zones' effects with the level's multiplier,
    // the engine fire's check, the record.
    void Hit(uint32_t effectZone, uint32_t zone, uint32_t id, float amount, float b, const Vec4& p, const Vec4& q);
    void Hit(const ImpulseHit& h) override;           // an impulse's: the surfaces, the material, the normal impulse, the slide
    void Impact(const ContactImpact& c) override;     // a resting contact's (DamageHandlerImpact, vslot 2: the force times 0.08)

    // As DamageHandlerCreate makes it (the constructor 0x4ef410 and vslot 3: native/damage_tables.inc, the captured words).
    void Create();
    // The game option's damage level (DamageHandlerSetLevel, 0x4ebfb0): its multiplier 0, 0.2, 0.7 or 1.
    void SetLevel(uint32_t level);
    // The setup's reset (DamageHandlerReset, vslot 4): the zones' effects anew, the level's load factors and wear, the zones'
    // damage clamped, the record cleared, no zone changed.
    void Reset();

private:
    bool ZoneHit(uint32_t i, float amount);                       // DamageZoneHit (0x4edee0)
    void ZoneEffects(uint32_t zone, float amount, float* best);   // DamageZoneEffects (0x4ec560)
    int Effect(const DamageEffectDef& e, float v);
    bool FireCheck(float a, float b);                             // EngineFireCheck
    void Ring(uint32_t type, uint32_t c);
    float Scaled(float v) const;
};

} // namespace phys::native
