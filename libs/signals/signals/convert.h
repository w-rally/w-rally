#pragma once
// signals/car.h's records <-> wrally/iface/records.h's, field by field. The physics module turns its bus's records into
// the interface's (PhysicsApi's RecordSink); the game turns them back and emits them on its own bus. The two layouts
// differ only where a bool crosses the DLL boundary as a uint32_t, so a round trip is exact.
//
// Each FromIface returns false for a kind it does not know (a newer module's record): the game ignores it.

#include <cstring>

#include "signals/car.h"
#include "wrally/iface/records.h"

namespace signals {

namespace rec = wrally::iface::records;

inline rec::Stamp ToIface(const Stamp& s) { return {s.car, s.tick}; }
inline Stamp FromIface(const rec::Stamp& s) { return {s.car, s.tick}; }

inline rec::CarInfo ToIface(const CarInfo& r) { return {ToIface(r.stamp), r.surface}; }
inline CarInfo FromIface(const rec::CarInfo& r)
{
    CarInfo o;
    o.stamp = FromIface(r.stamp);
    o.surface = r.surface;
    return o;
}

inline rec::EngineInfo ToIface(const EngineInfo& r)
{
    rec::EngineInfo o{};
    o.stamp = ToIface(r.stamp);
    o.cylinders = r.cylinders;
    std::memcpy(o.phaseOffset, r.phaseOffset, sizeof o.phaseOffset);
    o.rpmLimit = r.rpmLimit;
    o.idleThrottle = r.idleThrottle;
    o.turbo = r.turbo ? 1u : 0u;
    o.dt = r.dt;
    return o;
}
inline EngineInfo FromIface(const rec::EngineInfo& r)
{
    EngineInfo o;
    o.stamp = FromIface(r.stamp);
    o.cylinders = r.cylinders;
    std::memcpy(o.phaseOffset, r.phaseOffset, sizeof o.phaseOffset);
    o.rpmLimit = r.rpmLimit;
    o.idleThrottle = r.idleThrottle;
    o.turbo = r.turbo != 0;
    o.dt = r.dt;
    return o;
}

// EngineTick, DriveTick, WheelsTick, BodyTick, StepEnd, GearChange, Collision: one template per direction, the fields
// listed once.
#define SIGNALS_ENGINE_TICK(X)                                                                                        \
    X(ticks) X(crankSpeed) X(crankAngle) X(throttle) X(easedThrottle) X(torque) X(power) X(fired) X(sparks) X(misfire) \
    X(boost) X(turboSpeed) X(wastegateOpening) X(intakePressure) X(exhaustPressure) X(starter) X(blockTemperature)
#define SIGNALS_DRIVE_TICK(X)                                                                                         \
    X(ticks) X(gear) X(clutch) X(clutchSlip) X(clutchTorque) X(outputSpeed) X(engineTorque) X(shiftBlend) X(brake)    \
    X(handbrake)
#define SIGNALS_WHEEL(X)                                                                                              \
    X(spinRate) X(travel) X(travelRate) X(surfaceA) X(surfaceB) X(surfaceBlend) X(rollingSpeed) X(slipLongitudinal)  \
    X(slipLateral)
#define SIGNALS_COPY(f) o.f = r.f;
#define SIGNALS_COPY_ARRAY(f) std::memcpy(o.f, r.f, sizeof o.f);

inline rec::EngineTick ToIface(const EngineTick& r)
{
    rec::EngineTick o{};
    o.stamp = ToIface(r.stamp);
    SIGNALS_ENGINE_TICK(SIGNALS_COPY)
    SIGNALS_COPY_ARRAY(chargeMass)
    return o;
}
inline EngineTick FromIface(const rec::EngineTick& r)
{
    EngineTick o;
    o.stamp = FromIface(r.stamp);
    SIGNALS_ENGINE_TICK(SIGNALS_COPY)
    SIGNALS_COPY_ARRAY(chargeMass)
    return o;
}

inline rec::DriveTick ToIface(const DriveTick& r)
{
    rec::DriveTick o{};
    o.stamp = ToIface(r.stamp);
    SIGNALS_DRIVE_TICK(SIGNALS_COPY)
    o.clutchLocked = r.clutchLocked ? 1u : 0u;
    return o;
}
inline DriveTick FromIface(const rec::DriveTick& r)
{
    DriveTick o;
    o.stamp = FromIface(r.stamp);
    SIGNALS_DRIVE_TICK(SIGNALS_COPY)
    o.clutchLocked = r.clutchLocked != 0;
    return o;
}

inline rec::WheelsTick ToIface(const WheelsTick& in)
{
    rec::WheelsTick out{};
    out.stamp = ToIface(in.stamp);
    out.ticks = in.ticks;
    out.radius = in.radius;
    for (int k = 0; k < 4; ++k) {
        const auto& r = in.wheels[k];
        auto& o = out.wheels[k];
        SIGNALS_WHEEL(SIGNALS_COPY)
        SIGNALS_COPY_ARRAY(contactVelocity)
        SIGNALS_COPY_ARRAY(force)
        o.onGround = r.onGround ? 1u : 0u;
    }
    return out;
}
inline WheelsTick FromIface(const rec::WheelsTick& in)
{
    WheelsTick out;
    out.stamp = FromIface(in.stamp);
    out.ticks = in.ticks;
    out.radius = in.radius;
    for (int k = 0; k < 4; ++k) {
        const auto& r = in.wheels[k];
        auto& o = out.wheels[k];
        SIGNALS_WHEEL(SIGNALS_COPY)
        SIGNALS_COPY_ARRAY(contactVelocity)
        SIGNALS_COPY_ARRAY(force)
        o.onGround = r.onGround != 0;
    }
    return out;
}

#define SIGNALS_BODY_TICK(X) X(ticks) X(speed)
#define SIGNALS_BODY_ARRAYS(X) X(position) X(rotation) X(velocity) X(angularVelocity) X(feltAcceleration)
inline rec::BodyTick ToIface(const BodyTick& r)
{
    rec::BodyTick o{};
    o.stamp = ToIface(r.stamp);
    SIGNALS_BODY_TICK(SIGNALS_COPY)
    SIGNALS_BODY_ARRAYS(SIGNALS_COPY_ARRAY)
    return o;
}
inline BodyTick FromIface(const rec::BodyTick& r)
{
    BodyTick o;
    o.stamp = FromIface(r.stamp);
    SIGNALS_BODY_TICK(SIGNALS_COPY)
    SIGNALS_BODY_ARRAYS(SIGNALS_COPY_ARRAY)
    return o;
}

inline rec::StepEnd ToIface(const StepEnd& r) { return {ToIface(r.stamp), r.ticks}; }
inline StepEnd FromIface(const rec::StepEnd& r) { return {FromIface(r.stamp), r.ticks}; }

inline rec::GearChange ToIface(const GearChange& r) { return {ToIface(r.stamp), r.gear, r.level, r.engaged ? 1u : 0u}; }
inline GearChange FromIface(const rec::GearChange& r)
{
    GearChange o;
    o.stamp = FromIface(r.stamp);
    o.gear = r.gear;
    o.level = r.level;
    o.engaged = r.engaged != 0;
    return o;
}

inline rec::Collision ToIface(const Collision& r)
{
    rec::Collision o{};
    o.stamp = ToIface(r.stamp);
    o.depth = r.depth;
    o.material = r.material;
    SIGNALS_COPY_ARRAY(point)
    SIGNALS_COPY_ARRAY(velocity)
    return o;
}
inline Collision FromIface(const rec::Collision& r)
{
    Collision o;
    o.stamp = FromIface(r.stamp);
    o.depth = r.depth;
    o.material = r.material;
    SIGNALS_COPY_ARRAY(point)
    SIGNALS_COPY_ARRAY(velocity)
    return o;
}

#undef SIGNALS_ENGINE_TICK
#undef SIGNALS_DRIVE_TICK
#undef SIGNALS_WHEEL
#undef SIGNALS_BODY_TICK
#undef SIGNALS_BODY_ARRAYS
#undef SIGNALS_COPY
#undef SIGNALS_COPY_ARRAY

// A record from the interface (kind, bytes) emitted on `bus` as its signals record. False: a kind it does not know,
// or the size does not match (then nothing is emitted).
inline bool EmitFromIface(Bus& bus, uint32_t kind, const void* data, uint32_t bytes)
{
    auto as = [&](auto tag) -> bool {
        using R = decltype(tag);
        if (bytes != sizeof(R)) return false;
        R r;
        std::memcpy(&r, data, sizeof r);
        bus.Emit(FromIface(r));
        return true;
    };
    switch (kind) {
    case rec::CarInfo::kKind: return as(rec::CarInfo{});
    case rec::EngineInfo::kKind: return as(rec::EngineInfo{});
    case rec::EngineTick::kKind: return as(rec::EngineTick{});
    case rec::DriveTick::kKind: return as(rec::DriveTick{});
    case rec::WheelsTick::kKind: return as(rec::WheelsTick{});
    case rec::BodyTick::kKind: return as(rec::BodyTick{});
    case rec::StepEnd::kKind: return as(rec::StepEnd{});
    case rec::GearChange::kKind: return as(rec::GearChange{});
    case rec::Collision::kKind: return as(rec::Collision{});
    default: return false;
    }
}

} // namespace signals
