#pragma once
// The drivetrain, natively (M6 step 2; the classic CDrive, core/drive.cpp): the engine's torque through the clutch and the
// gearbox into three differentials (the centre one feeding the front and the rear one) and four drive slots, one per
// wheel - each slot with its brake (the friction by the brake's temperature, its wear) and its latch to the drivetrain.
//
// Once a tick (Update) the engine steps (a hook: its torque at the engine speed), the starter adds its torque, the brakes
// their capacities, the gearbox its shift; the clutch locks or slips and the gearbox's torque goes into the solver that
// couples the slots through the differentials (a 4 x 4 system; DriveType 3 a 2WD one of 2 x 2), latching and freeing the
// slots and locking and freeing the differentials until nothing changes. The wheels read their slots' spins and write
// their torques (slotTorques, slotBrakeExtra). The classic code's arithmetic is kept bit for bit.
#include <cstdint>

#include "num/math.h"
#include "num/x87.h"

namespace phys::native {

using num::Vec4;

// A differential's def (the centre, the front, the rear).
struct DiffDef {
    uint32_t driven = 0;            // fed by the centre (an int: its float times the centre's ratio)
    uint32_t inputs[2] = {};        // its two inputs: the slots (front / rear), the differentials (the centre)
    float ratio = 0, maxTorque = 0;
    uint32_t lockFlag = 0;          // its index in the state's free flags (and the solver's rows)
};

// A drive slot's def (lf rf lr rr).
struct DriveWheelDef {
    float ratio = 0;                // into its differential
    uint32_t diff = 0;              // its differential (1 front, 2 rear)
    float brakeRadius = 0;          // the brake torque per unit of force and friction
    float brakeArea = 0;            // pressure to force
    float brakeRatio = 0;           // its share of the footbrake pressure
};

// The brake's def: its friction by temperature (micro-kelvin) and its wear.
struct BrakeDef {
    uint32_t coldTemperature = 0, fadeTemperature = 0, optimalTemperature = 0, wearTemperature = 0;
    float wearFactor = 0, wearPressure = 0;
    float coldFriction = 0, peakFriction = 0, fadedFriction = 0, wornFriction = 0;
    uint32_t wearLimit = 0;         // the wear count that spends it (the worn friction)
};

// A gear (0 reverse, 1 neutral, 2 first, ..): its ratio and torque limits.
struct GearDef {
    float ratio = 0, torqueMin = 0, torqueMax = 0;
};

// CDrive::CDef (the .lsp's Drive keys and the setup's).
struct DriveDef {
    DiffDef diffs[3];
    DriveWheelDef wheels[4];
    BrakeDef brake;
    float frictionRise[10] = {};    // cold to optimal (9 steps)
    float frictionFade[10] = {};    // optimal to faded
    float wearCurve[10] = {};       // the wear by temperature
    float maxFootbrakePressure = 0, maxHandbrakePressure = 0;
    uint32_t driveType = 0;         // 3: the 2WD drivetrain
    uint32_t numberOfGears = 0;
    float engineInertia = 0, gearBoxInertiaIn = 0, gearBoxInertiaOut = 0;
    float wheelInertia = 0;         // WheelInertia (the drive's step does not read it)
    GearDef gears[10];
    float finalRatio = 0;           // the final drive's over the drop gear's
    float shiftDelay = 0;           // seconds from the request to the clutch opening
    float shiftEngageDelay = 0;     // then to the new gear in
};

// The drivetrain's damage (the car damage handler's record).
struct DriveDamage {
    uint32_t driveShaft[4] = {};    // the slot is driven while its shaft holds
    float gears[10] = {};           // each gear's condition (1 new): its torque limits scale with it
    uint32_t changed = 0;           // a part changed: the reflected inertia to rebuild
    float wearFactor = 0;           // how fast the gears wear and the starter drains the battery
    uint32_t gearActuator = 0;      // gear changes possible
    uint32_t starter = 0;
    uint32_t brakeCircuit = 0;      // the footbrake works
    float clutch = 0;               // its torque capacity's factor
    float gearbox = 0;              // the gears' torque limits' factor
    float battery = 0;              // the starter's charge
    uint32_t hydraulics = 0;        // the differentials' locking
};

// A differential's state.
struct DrivePair {
    float speed = 0;                // its output's (the centre's: the drivetrain's)
    float inputSum = 0;             // its inputs' speeds summed
    int32_t direction = 0;          // locked: the sign (0 free)
    float threshold = 0;            // the lock torque
    float delta = 0;                // direction * threshold, or the solved torque while free
};

// A drive slot's state.
struct DriveSlot {
    float spinAcceleration = 0;     // the spin's change per second
    float spin = 0;                 // the wheel's spin rate
    int32_t turning = 0;            // the sign while the wheel turns against its brake (0 held)
    float inertia = 0;
    float drivenSpin = 0;           // its spin while latched: through its differential
    int32_t locked = 0;             // the sign while latched to the drivetrain (0 free)
    float weight = 0;               // its column's factor in the coupling matrix
    uint32_t temperature = 0, temperature2 = 0;   // the brake's, micro-kelvin
    float handbrakeRatio = 0;       // its share of the handbrake pressure
    float brakeTorque = 0;          // the brake's capacity this tick (the friction's and the wheel's extra)
    uint32_t wear = 0;              // the brake's wear count
};

// The engine's torque averaged over the last ticks (the classic RingAverage's ring).
struct TorqueRing {
    uint32_t count = 0, index = 0;
    float values[50] = {};
};

// CDrive::CState: what changes per tick.
struct DriveState {
    DrivePair pairs[3];             // the centre, the front, the rear
    DriveSlot slots[4];
    int32_t clutchLock = 0;         // the clutch locked: the sign (0 slipping)
    float gearboxTorque = 0;        // the clutch's torque through the gear and final ratios (0 out of gear)
    float gearboxTorqueNeg = 0;     // -gearboxTorque (the 2WD step)
    float drivetrainSpeed = 0;      // the gearbox output's
    TorqueRing engineTorqueAverage;
    float engineSpeed = 0;          // rad/s
    float inertia = 0;              // the engine and gearbox reflected through the gear and final ratios
    float coupling[16] = {};   // the 4 x 4 coupling of the slots (row r, column k at k + 4 r)
    float couplingInverse[16] = {};
    uint32_t freeFlags[4] = {};     // by the differentials' lockFlag: 1 free
    float coupling2[4] = {}, coupling2Inverse[4] = {};   // the 2WD drivetrain's 2 x 2
    uint32_t held2WD = 0;           // the 2WD drive force held at 0 (the 2WD rebuild clears it)
    uint32_t open2WD = 0;           // the 2WD pair free with a slot latched: its row holds that slot
    float demands[4] = {};   // the slots' (AxleTerm)
    uint32_t gear = 0;              // the gear in (0 reverse, 1 neutral, 2 first, ..)
    uint32_t engaged = 0;           // the clutch engaged with a gear
    float clutchBlend = 0;          // 1 just shifted, falling to 0
    uint32_t requestedGear = 0;
    float shiftTimer = 0;
    uint32_t gearChanged = 0;       // the reflected inertia to rebuild
    uint32_t random = 0;            // the gear wear's generator
    float updates = 0, rebuilds = 0;   // counts (as floats: the classic Tweaks')
    uint32_t latchCount = 0, releaseCount = 0, lockCount = 0, freeCount = 0, inertiaCount = 0;
    int32_t lockState = 0;          // 0 nothing latched, 1 some, 2 all
    uint32_t rebuildPending = 0;
};

// The gear-change signal's record (CSigMrRCarGearChange).
struct GearChange {
    uint32_t car = 0, gear = 0;
    float level = 0;                // sqrt(clamp(|torque| / 30, 0, 0.8) + 0.2)
    uint32_t engaged = 0;
};

// What the drive needs from its car and the game.
struct DriveHooks {
    num::X87 (*engineStep)(void* ctx, float dt, float engineSpeed) = nullptr;   // the engine's torque
    void (*gearOut)(void* ctx) = nullptr;                         // the gear thrown out (the limits, a shift)
    void (*gearChange)(void* ctx, const GearChange& c) = nullptr; // the gear engaging
    void* ctx = nullptr;
    bool gearLimits = false;        // the physics config's flag 0x80000: the gears' torque limits and wear
};

class Drive {
public:
    const DriveDef* def = nullptr;
    DriveState* state = nullptr;
    DriveDamage* damage = nullptr;
    DriveHooks hooks;
    uint32_t carId = 0;             // the owner's (the gear-change record)

    // The inputs (the control unit's).
    float throttle = 0, brake = 0, handbrake = 0;
    uint32_t starter = 0;           // the starter pressed
    float clutch = 0;               // 0 engaged, 1 open
    float brakePressure = 0, handbrakePressure = 0;
    // The solver's.
    float centreTorque = 0;         // its first component
    Vec4 solved{};                  // the coupling inverse times the demands
    float gates[4] = {};            // the slots' latch torques (0 open)
    float gates2[2] = {}, forces2[2] = {}, demands2[2] = {};   // the 2WD step's
    // The wheels'.
    float slotTorques[4] = {};      // the drive torque on each slot
    float slotBrakeExtra[4] = {};   // added to the slots' brake torque
    // The outputs.
    float gearLoads[10] = {};       // the torque each gear engaged with
    float engineTorque = 0;         // the engine's, averaged
    float starterTorque = 0, starterLevel = 0;
    float outputSpeed = 0;          // the drivetrain's with the clutch locked in gear, else the engine's
    float slipSpeed = 0;            // the clutch's
    float shownClutchBlend = 0;
    float clutchTorque = 0;         // |the clutch's out|
    float gearChangeLevel = 0;

    // The tick (DriveUpdate, 0x50a1e0).
    void Update(float dt);
    // Differential i's lock torque at t (0 .. 1) (RampScale, 0x504570; the control unit's diff maps).
    void DiffLockThreshold(int32_t i, float t);
    // The starter's torque over dt (StarterMotor, 0x506c90).
    float StarterMotor(float dt);
    // A gear change asked for: d up or down from the requested one, clamped to reverse and the top gear (GearRequest,
    // 0x5047b0); gear g directly, neutral past the top (GearSet, 0x506840).
    void GearRequest(int32_t d);
    void GearSet(int32_t g);
    // The control record's handbrake and starter (DriveApplyControl, 0x505970).
    void ApplyControl(float handbrake, uint32_t starter);

private:
    bool SlotDriven(uint32_t k) const { return damage->driveShaft[k] != 0; }
    num::X87 TorqueAverage(float x);
    // The brakes.
    bool BrakeWear(float force, DriveSlot& s, const DriveWheelDef& w, float dt);
    num::X87 BrakeFriction(float force, DriveSlot& s, const DriveWheelDef& w, float dt);
    num::X87 BrakeTorque(DriveSlot& s, const DriveWheelDef& w, float dt);
    // The gearbox and the clutch.
    bool GearLimits(float x);
    num::X87 ClutchSlipSpeed() const;
    void GearShiftStep(float dt);
    void GearEngage(float x);
    void ClutchLockStep(float torque, float* out, float capacity, float ratio, float dt);
    // The 4 x 4 solver.
    void AxleTerm(uint32_t k);
    void SlipLatch(uint32_t k);
    bool SlotRelease(uint32_t k);
    bool PairCheck(uint32_t j);
    void PairApply(uint32_t j);
    void LockSolve();
    void LockState();
    void SlotWeights(uint32_t k);
    void PairSums(uint32_t j);
    void LockInit();
    void SlotSlip(uint32_t k, float dt);
    void LockRebuild();
    void SlotRateOrSlip(uint32_t k, float dt);
    void Step(float dt);
    // The 2WD one (pair 1 on slots 0 and 1).
    void SlipLatch2WD(uint32_t k);
    void SlotDemand2WD(uint32_t k);
    void SlotWeights2WD(uint32_t k);
    void LockRebuild2WD();
    void Step2WD(float dt);
};

} // namespace phys::native
