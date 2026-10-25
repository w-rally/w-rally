#pragma once
// The control unit, natively (M6 step 2; the classic CVehicleControlUnit, core/drive.cpp and core/engine.cpp): between
// the driver's controls and the car. A control record (Command) sets the pedals and asks for gears; once a tick (Step)
// the driving aids act - the automatic gearbox (shift points by rpm, two-pedal reverse), the clutch help, the gear guard
// and the neutral lock -, the pedals go into the engine and the drive, the rev limiter and the shift's ignition cut pick
// the cylinders that may fire, the brake and reverse lights switch, and the differentials' lock torques come from the
// maps by throttle, brake and drivetrain speed. The classic code's arithmetic is kept bit for bit.
#include <cstdint>

#include "native/drive.h"
#include "native/engine.h"

namespace phys::native {

// A gear's automatic shift points (rpm; 0 none).
struct GearShift {
    float upshift = 0, downshift = 0;
};

// A differential's locking map: by the throttle, by the brake (11 points over 0..1), by the drivetrain speed.
struct DiffMap {
    float throttle[11] = {};
    float brake[11] = {};
    CurvePoint speed[11];
};

// CVehicleControlUnit::CDef (the .lsp's ControlUnit keys, the maps the setup's).
struct VcuDef {
    GearShift gears[10];
    float upshiftDelay = 0, downshiftDelay = 0;   // the shift timer after a shift
    float clutchRelease = 0;                      // the clutch help's release per second
    float rpmLimit = 0;                           // the rev limiter's and the gear guard's
    float centerDiffHandbrakeRelease = 0;         // the centre differential opens above it
    float leftFootBrakeThreshold = 0;             // the left-foot map with throttle and brake above it
    DiffMap maps[4];                              // the centre, the centre left-foot braking, the front, the rear
};

struct VcuState {
    float shiftTimer = 0;           // no automatic shift until it runs down
    float clutch = 0;               // the clutch help's
    uint32_t reversePedals = 0;     // two-pedal reverse: throttle and brake swapped
    float reverseTimer = 0;         // held still on a pedal: counts towards the reverse / first change (+- 0.4 s)
    float shiftCut = 0;             // the ignition cut after a shift (0.05 s)
};

// The car's light groups (low beam, high beam, rear, brake, reverse, -) and the damage record's changed flags.
struct LightGroups {
    uint32_t on[6] = {};
    uint32_t* changed = nullptr;    // [7]: set when a group changes (the renderer clears them)
};

// The control record (the game's per tick).
struct ControlInput {
    float throttle = 0, brake = 0, handbrake = 0, clutch = 0, steering = 0;
    uint32_t flags = 0;             // 1 gear up, 2 down, 4 the gear given
    int32_t gear = 0;
    uint32_t starter = 0;
};

struct VcuHooks {
    float (*forwardSpeed)(void* ctx) = nullptr;   // the car's (m/s)
    void* ctx = nullptr;
};

class Vcu {
public:
    const VcuDef* def = nullptr;
    VcuState* state = nullptr;
    Drive* drive = nullptr;
    Engine* engine = nullptr;
    LightGroups* lights = nullptr;
    VcuHooks hooks;

    float throttle = 0, brake = 0, handbrake = 0, steering = 0, clutch = 0;   // the control record's
    uint32_t autoGears = 0;         // the automatic gearbox
    uint32_t gearGuard = 0;         // no down shift over the rev limit
    uint32_t neutralLock = 0;       // no shift below first above 10 km/h
    uint32_t clutchHelp = 0;
    uint32_t twoPedals = 0;         // the brake reverses when stopped

    // A control record (ControlCommand, 0x537f20): the pedals, then its gear command; 1 when a gear was asked for.
    int Command(const ControlInput& c);
    // The tick (ControlsStep, 0x537f50).
    void Step(float dt);

private:
    int GearCommand(const ControlInput& c);
    bool GearDownBlocked() const;
    void AutoGearbox(float dt);
    void AutoClutch(float dt);
    void SpeedFlags();
    num::X87 CurveMax3(const DiffMap& m, float speed) const;
    void CurveRamps();
    void SetLight(uint32_t group, uint32_t on);
};

} // namespace phys::native
