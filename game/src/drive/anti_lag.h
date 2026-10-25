#pragma once
// The anti-lag (ALS) - a control unit's feature, between the driver's controls and the physics (no physics change):
// when the driver lifts at revs in gear, the throttle stays cracked open, so the engine keeps breathing and the turbo
// spinning (the boost is there when the pedal comes back). The real one also retards the ignition so the charge burns in
// the exhaust - the bangs (the sound adds them: audio's DriverTick::antiLag) - and the engine makes next to no torque;
// through the throttle alone the crack stays small, or the car would keep pulling.
//
// On when the pedal is under `pedalOff` in gear above `onRpm`; off under `offRpm` (hysteresis) or when the pedal comes
// back. The crack is relative to the engine's own idle throttle (the physics never goes under it: 0.1425 for
// cars/s_i2003.ini) - 0.25 on s_i2003: the engine's torque about 0 while lifted (-4..+10 N m against -19 without; 0.3
// still pulls, +30..+55), the revs hanging (4618 rpm 2 s after a lift at 6700, against 4344).
//
// At standstill, armed (`idleArmed`: the driver's switch before a start), in neutral off the pedal: the idle with the
// anti-lag - the revs hunt (a Rally1's: irregular, bubbling; the sound adds its bangs): a target wandering along a
// smooth random curve between idleRpmLow and idleRpmHigh (a new point every 0.1 - 0.35 s), the throttle regulated to
// it (PI, the proportional part on the revs `lead` s ahead, the integral within 0..0.3, from 0.05: an unloaded engine
// runs away above ~0.3, and the start's hold - the throttle ignored - would wind it up) - the engine is too touchy for
// a fixed crack (0.21 holds ~3900 rpm, 0.30 runs to 6000). Through the throttle the revs only
// go above the engine's own idle (the physics never goes under its idle throttle), not under it as the real one's
// retarded ignition takes them.

#include <algorithm>
#include <cmath>

namespace phys::drive {

class AntiLag {
public:
    bool enabled = true;
    float crack = 0.25f;      // the throttle held while on
    float pedalOff = 0.15f;
    float onRpm = 3500, offRpm = 2800;
    bool idleArmed = false;
    float idleRpmLow = 2000, idleRpmHigh = 2600;
    float kp = 0.2f, ki = 1.5f, lead = 0.25f;   // the idle's regulation (gentler than the launch control's: an unloaded engine climbs fast and coasts down slowly)

    // The throttle to send for the driver's `pedal` at `rpm` in `gear` (0 neutral) at `speed` (m/s), `dt` seconds after
    // the last call.
    float Throttle(float pedal, float rpm, int gear, float speed, float dt)
    {
        if (!enabled || pedal > pedalOff) {
            m_active = m_idle = false;
            return pedal;
        }
        const float raw = m_lastRpm > 0 && dt > 0 ? (rpm - m_lastRpm) / dt : 0.0f;
        m_rate += (raw - m_rate) * std::min(1.0f, dt / 0.03f);
        m_lastRpm = rpm;
        if (gear == 0) {   // in neutral: only the idle mode
            m_active = false;
            const bool was = m_idle;
            m_idle = idleArmed && speed < 0.5f;
            if (!m_idle) return pedal;
            if (!was) m_integral = 0.05f;
            m_t += dt / m_length;
            if (m_t >= 1) {
                m_t = 0;
                m_length = 0.1f + 0.25f * Random();
                m_from = m_to;
                m_to = Random();
            }
            const float ease = m_t * m_t * (3 - 2 * m_t), u = m_from + (m_to - m_from) * ease;
            const float target = idleRpmLow + (idleRpmHigh - idleRpmLow) * u;
            const float error = (target - rpm) / 1000, ahead = (target - (rpm + m_rate * lead)) / 1000;
            m_integral = std::clamp(m_integral + ki * error * dt, 0.0f, 0.3f);
            return std::max(pedal, std::clamp(kp * ahead + m_integral, 0.0f, 1.0f));
        }
        m_idle = false;
        if (!m_active && rpm > onRpm) m_active = true;
        if (m_active && rpm < offRpm) m_active = false;
        return m_active ? std::max(pedal, crack) : pedal;
    }

    bool Active() const { return m_active || m_idle; }
    bool Idle() const { return m_idle; }   // the standstill mode

private:
    float Random()
    {
        m_random = m_random * 1664525u + 1013904223u;
        return static_cast<float>(m_random >> 8) / float(1 << 24);
    }

    bool m_active = false, m_idle = false;
    float m_t = 1, m_length = 0.2f, m_from = 0.5f, m_to = 0.5f;
    float m_integral = 0, m_lastRpm = 0, m_rate = 0;
    unsigned m_random = 0x51ed2701u;
};

} // namespace phys::drive
