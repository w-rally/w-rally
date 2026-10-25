#pragma once
// Launch control - a control unit's feature, between the driver's controls and the physics (no physics change): while
// the car stands with the throttle floored, the throttle the engine gets is regulated to hold the revs at a target
// (a Rally1's ~7400 rpm before the start, below the limiter's 8100), instead of the engine bouncing off its limiter.
// It works in neutral (the input has no clutch pedal: a gear going in is the launch) and lets go when the driver lifts,
// a gear goes in or the car moves. The real one holds the revs with the ignition (cut, retard); through the throttle the
// revs - and the sound that follows them - come out much the same.
//
// The regulation: proportional on where the revs will be in `lead` seconds (the revs plus their rate: a free engine
// climbs ~8000 rpm a second and answers the throttle ~0.1 s late, so acting on the revs alone overshoots into the
// limiter) plus integral on the revs' error (per 1000 rpm) - only within 500 rpm of the target (else it winds up on the
// way there and overshoots), started at about what a hold takes (0.3) - the output never above the driver's pedal.

#include <algorithm>
#include <cmath>

namespace phys::drive {

class LaunchControl {
public:
    float target = 7400;   // rpm; 0 off
    float kp = 0.6f;       // throttle per 1000 rpm of error
    float ki = 4.0f;       // throttle per 1000 rpm per second
    float lead = 0.15f;    // seconds ahead the proportional part looks

    // The throttle to send for the driver's `pedal` (0..1) at `rpm`, `speed` (m/s) and `gear` (0 neutral), `dt` seconds
    // after the last call.
    float Throttle(float pedal, float rpm, float speed, int gear, float dt)
    {
        // The revs' rate, smoothed over ~30 ms (tick to tick it ripples with each firing - a jittery throttle).
        const float raw = m_lastRpm > 0 && dt > 0 ? (rpm - m_lastRpm) / dt : 0.0f;
        m_rate += (raw - m_rate) * std::min(1.0f, dt / 0.03f);
        const float rate = m_rate;
        m_lastRpm = rpm;
        if (target <= 0) return pedal;
        if (!m_active) {
            if (speed > 0.5f || pedal < 0.9f || gear != 0) return pedal;
            m_active = true;   // standing in neutral, floored: armed
            m_integral = 0.3f;
        }
        if (pedal < 0.5f || speed > 0.5f || gear != 0) {   // lifted, or the launch
            m_active = false;
            return pedal;
        }
        const float error = (target - rpm) / 1000, ahead = (target - (rpm + rate * lead)) / 1000;
        if (std::fabs(error) < 0.5f) m_integral = std::clamp(m_integral + ki * error * dt, 0.0f, 1.0f);
        return std::clamp(kp * ahead + m_integral, 0.0f, pedal);
    }

    bool Active() const { return m_active; }

private:
    bool m_active = false;
    float m_integral = 0, m_lastRpm = 0, m_rate = 0;
};

} // namespace phys::drive
