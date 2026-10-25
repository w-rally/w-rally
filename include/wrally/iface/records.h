// ---------------------------------------------------------------------------
// wrally/iface/records.h - what a physics module tells the game about a car,
// through PhysicsApi's RecordSink.
//
// These are plugin-rbr/signals/car.h's records with the layout pinned for a
// DLL boundary: bool becomes uint32_t, sizes are static_assert'ed, and there
// are no pointers (car.h's ClassicSignal, which carries one, does not cross:
// it belongs to the classic in-arena physics, which no module runs).
//
// The game forwards all of them, plus its own DriverTick, to the audio module
// (AudioApi::submitRecord) - the one stream drives the car's sound.
//
// The ...Tick records follow every step (Stamp::tick the tick after it,
// `ticks` how many it advanced), StepEnd last; events (GearChange, Collision)
// when they happen inside the step, before that step's ticks. SI units,
// physics axes (z up).
//
// ADDING A RECORD is not an interface change: the game ignores kinds it does
// not know. CHANGING one is: bump kPhysicsInterfaceVer.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace wrally::iface::records {

constexpr uint32_t Kind(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

#pragma pack(push, 4)

struct Stamp {
    uint32_t car;      // index in WorldDesc::cars
    uint32_t tick;
};

struct CarInfo {
    static constexpr uint32_t kKind = Kind('C', 'a', 'r', 'I');
    Stamp    stamp;
    uint32_t surface;              // iface::Surface
};

struct EngineInfo {
    static constexpr uint32_t kKind = Kind('E', 'n', 'g', 'I');
    Stamp    stamp;
    uint32_t cylinders;
    uint32_t phaseOffset[4];       // firing points, steps of 256 over 4 pi
    float    rpmLimit;
    float    idleThrottle;
    uint32_t turbo;
    float    dt;
};

struct EngineTick {
    static constexpr uint32_t kKind = Kind('E', 'n', 'g', 'T');
    Stamp    stamp;
    uint32_t ticks;
    float    crankSpeed, crankAngle;
    float    throttle, easedThrottle;
    float    torque, power;
    uint32_t fired, sparks;
    float    chargeMass[4];
    float    misfire;
    float    boost, turboSpeed, wastegateOpening;
    float    intakePressure, exhaustPressure;
    float    starter;
    float    blockTemperature;
};

struct DriveTick {
    static constexpr uint32_t kKind = Kind('D', 'r', 'v', 'T');
    Stamp    stamp;
    uint32_t ticks;
    int32_t  gear;
    float    clutch;
    uint32_t clutchLocked;
    float    clutchSlip, clutchTorque;
    float    outputSpeed, engineTorque;
    float    shiftBlend;
    float    brake, handbrake;
};

struct WheelsTick {
    static constexpr uint32_t kKind = Kind('W', 'h', 'l', 'T');
    Stamp    stamp;
    uint32_t ticks;
    float    radius;
    struct Wheel {
        float    spinRate;
        float    contactVelocity[3];
        uint32_t onGround;
        float    travel, travelRate;
        uint32_t surfaceA, surfaceB;
        float    surfaceBlend;
        float    force[3];
        float    rollingSpeed;
        float    slipLongitudinal, slipLateral;
    } wheels[4];
};

struct BodyTick {
    static constexpr uint32_t kKind = Kind('B', 'o', 'd', 'T');
    Stamp    stamp;
    uint32_t ticks;
    float    position[3], rotation[4], velocity[3], angularVelocity[3];
    float    feltAcceleration[3];
    float    speed;
};

struct StepEnd {
    static constexpr uint32_t kKind = Kind('S', 't', 'p', 'E');
    Stamp    stamp;
    uint32_t ticks;
};

struct GearChange {
    static constexpr uint32_t kKind = Kind('G', 'e', 'a', 'r');
    Stamp    stamp;
    int32_t  gear;
    float    level;
    uint32_t engaged;
};

struct Collision {
    static constexpr uint32_t kKind = Kind('C', 'o', 'l', 'l');
    Stamp    stamp;
    float    depth;
    uint32_t material;
    float    point[3];
    float    velocity[3];
};

// THE GAME'S record, not the physics': what the driver asked and what the
// game's driver layer (launch control, anti-lag - phys/drive) sent on, emitted
// by the game before the step it drives. The audio tells an anti-lag's
// throttle crack from a driver's part throttle by it.
struct DriverTick {
    static constexpr uint32_t kKind = Kind('D', 'r', 'v', 'r');
    Stamp    stamp;
    float    pedal;                // the driver's throttle 0..1
    float    throttle;             // sent to the physics
    uint32_t launchControl;        // holding the revs, standing
    uint32_t antiLag;              // the throttle cracked off the pedal, the exhaust lit
};

#pragma pack(pop)

static_assert(sizeof(Stamp) == 8, "Stamp");
static_assert(sizeof(DriverTick) == 24, "DriverTick");
static_assert(sizeof(EngineInfo) == 44, "EngineInfo");
static_assert(sizeof(WheelsTick) == 16 + 4 * 64, "WheelsTick");
static_assert(sizeof(Collision) == 40, "Collision");

} // namespace wrally::iface::records
