#pragma once
// The car's records (signals/bus.h): what the physics tells the other modules about a car. The ...Tick records come
// after every physics step (Stamp::tick the tick after it, `ticks` how many it advanced: 1 when stepped tick by tick),
// StepEnd last; the events when they happen inside a step (before that step's ...Ticks). Units SI (m, s, rad, N, Pa);
// the physics' frame (z up).

#include "signals/bus.h"

namespace signals {

// The car's fixed facts, once when it is loaded.
struct CarInfo {
    static constexpr uint32_t kKind = Kind('C', 'a', 'r', 'I');
    enum Surface : uint32_t { kTarmac, kGravel, kSnow };
    Stamp stamp;
    uint32_t surface = kTarmac;    // the stage's (the car's setup and tyres are for it)
};

// The engine's fixed facts, once when the car is loaded (and before its first EngineTick).
struct EngineInfo {
    static constexpr uint32_t kKind = Kind('E', 'n', 'g', 'I');
    Stamp stamp;
    uint32_t cylinders = 0;        // up to 4 (the physics' model)
    uint32_t phaseOffset[4] = {};  // each cylinder's firing point in the cycle: steps of 256 over 4 pi (two turns)
    float rpmLimit = 0;            // the control unit's rev limiter
    float idleThrottle = 0;        // the eased throttle's minimum
    bool turbo = false;
    float dt = 0;                  // the physics' tick, s
};

// The engine after a step.
struct EngineTick {
    static constexpr uint32_t kKind = Kind('E', 'n', 'g', 'T');
    Stamp stamp;
    uint32_t ticks = 1;
    float crankSpeed = 0;          // rad/s (rpm = * 30 / pi)
    float crankAngle = 0;          // 0 .. 4 pi, the cycle's (EngineInfo::phaseOffset in its steps: angle / 4 pi * 256)
    float throttle = 0;            // the input 0..1
    float easedThrottle = 0;       // what the engine takes (idleThrottle at least)
    float torque = 0;              // N m, filtered (0.95 / 0.05 a tick)
    float power = 0;               // W, filtered
    uint32_t fired = 0;            // bit i: cylinder i burnt its last charge (0: a misfire, the limiter's cut, a stall)
    uint32_t sparks = 0;           // bit i: cylinder i may fire (the rev limiter and the shift cut clear bits)
    float chargeMass[4] = {};      // kg, each cylinder's last charge: the load
    float misfire = 0;             // > 0 the tick a random misfire event struck (its strength, up to 1)
    float boost = 0;               // the intake pressure over the ambient, a ratio (1: none)
    float turboSpeed = 0;          // rad/s
    float wastegateOpening = 0;
    float intakePressure = 0;      // Pa, the plenum after the compressor
    float exhaustPressure = 0;     // Pa, before the turbine
    float starter = 0;             // the starter motor's strength, 0 off
    float blockTemperature = 0;    // K
};

// The drivetrain after a step.
struct DriveTick {
    static constexpr uint32_t kKind = Kind('D', 'r', 'v', 'T');
    Stamp stamp;
    uint32_t ticks = 1;
    int32_t gear = 0;              // -1 reverse, 0 neutral, 1..
    float clutch = 0;              // 0 engaged, 1 open (the pedal or the auto clutch)
    bool clutchLocked = false;
    float clutchSlip = 0;          // rad/s across the clutch
    float clutchTorque = 0;        // N m through it
    float outputSpeed = 0;         // rad/s, the drivetrain's (in gear) else the engine's
    float engineTorque = 0;        // N m, averaged
    float shiftBlend = 0;          // 1 just shifted, falling to 0
    float brake = 0, handbrake = 0;
};

// The wheels after a step (lf, rf, lb, rb).
struct WheelsTick {
    static constexpr uint32_t kKind = Kind('W', 'h', 'l', 'T');
    Stamp stamp;
    uint32_t ticks = 1;
    float radius = 0;              // m, the rolling radius
    struct Wheel {
        float spinRate = 0;        // rad/s
        float contactVelocity[3] = {};   // m/s, the tyre's point on the ground (the car's v + w x r): slip with spinRate
        bool onGround = false;
        float travel = 0;          // m, the suspension's position
        float travelRate = 0;      // m/s
        uint32_t surfaceA = 0, surfaceB = 0;   // the ground's surface entries (the stage's .mat) and the blend to B
        float surfaceBlend = 0;
        float force[3] = {};       // N, the wheel's force on the car (the world's axes)
        float rollingSpeed = 0;    // m/s, spinRate * radius
        float slipLongitudinal = 0;   // m/s, |the ground's speed along the tyre - its rolling speed| (spin, lock)
        float slipLateral = 0;     // m/s, |the ground's speed along the axle| (a slide)
    } wheels[4];
};

// The car body after a step.
struct BodyTick {
    static constexpr uint32_t kKind = Kind('B', 'o', 'd', 'T');
    Stamp stamp;
    uint32_t ticks = 1;
    float position[3] = {};        // the centre of gravity
    float rotation[4] = {};        // a quaternion (x, y, z, w)
    float velocity[3] = {};
    float angularVelocity[3] = {};
    float feltAcceleration[3] = {};   // in the car's frame, smoothed
    float speed = 0;
};

// The step's records are all out: what came since the last StepEnd is that step's.
struct StepEnd {
    static constexpr uint32_t kKind = Kind('S', 't', 'p', 'E');
    Stamp stamp;
    uint32_t ticks = 1;
};

// What the driver asked and what the car's control layer sent on (phys/drive: launch control, the anti-lag), emitted
// before the step it drives - the sound tells an anti-lag's throttle crack from a driver's part throttle by it.
struct DriverTick {
    static constexpr uint32_t kKind = Kind('D', 'r', 'v', 'r');
    Stamp stamp;
    float pedal = 0;               // the driver's throttle 0..1
    float throttle = 0;            // sent to the engine
    bool launchControl = false;    // holding the revs, standing
    bool antiLag = false;          // the anti-lag on: the throttle cracked off the pedal, the exhaust lit
};

// A gear went in or out (the physics' CSigMrRCarGearChange).
struct GearChange {
    static constexpr uint32_t kKind = Kind('G', 'e', 'a', 'r');
    Stamp stamp;
    int32_t gear = 0;              // the gear now: -1 reverse, 0 neutral, 1..
    float level = 0;               // how hard: sqrt(clamp(|slip / 30|, 0, 0.8) + 0.2)
    bool engaged = false;          // the clutch engaged with the gear
};

// A solid's damping contact (the physics' CSigDampingCollision): a scrape or a hit.
struct Collision {
    static constexpr uint32_t kKind = Kind('C', 'o', 'l', 'l');
    Stamp stamp;
    float depth = 0;               // m into the other
    uint32_t material = 0;
    float point[3] = {};           // the world's
    float velocity[3] = {};        // the solid's at the point, m/s
};

// Any of the classic physics' signal emits (RBR's CMultiSignal1<CSig...>::Emit), as it is: `emitter` the original's
// Emit address names the signal, `record` its record, valid only during the call. For listeners that decode what no
// typed record above carries yet; prefer the typed ones.
struct ClassicSignal {
    static constexpr uint32_t kKind = Kind('C', 'S', 'i', 'g');
    Stamp stamp;
    uint32_t emitter = 0;
    const void* record = nullptr;
};

} // namespace signals
