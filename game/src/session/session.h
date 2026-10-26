#pragma once
// wrally::session::Session - one stage run (docs/design.md 6.5, 6.6): the physics (a module), the stage clock, the
// driver layer between the keys and the car (the steering's filter, launch control, the anti-lag), the recording of
// the drive, or a replay of one checking itself; and what a frame draws - the last two physics steps.
//
// A frame hands it a FrameInput (its time, the keys as they are, the presses since the last frame) and takes a
// Snapshot. That is the whole surface, so the session runs as well on a thread of its own (session/sim_thread.h) as
// inline: nothing else of it is touched from outside while it runs.
//
// The start, as the game's (rules::StageClock): the entry (`entry` seconds from the load, the car held by its brakes),
// then the 7 s countdown - GO 9 s after the load, as in the game's recording of stage 41 (the car off at tick 1291 of
// 0.007 s). Faster than 1 m/s in the countdown's last 4 s is a false start. quickStart: no entry, the countdown from
// 3.5 s. A replay gets the same start (its own controls; nothing held).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "camera/camera.h"
#include "data/rec.h"
#include "physics/physics.h"
#include "rules/stage.h"
#include "session/anti_lag.h"
#include "session/launch_control.h"

namespace signals {
class Bus;
}

namespace wrally::session {

struct Config {
    physics::Config physics;
    std::filesystem::path physicsModules;   // the folder of physics DLLs
    std::string physicsId = "rbr";
    std::string dls;                        // the stage's .dls: its splits and finish
    std::string out = "drive.phr";          // the recording of the drive, written by Finish
    std::string replay;                     // a recording to replay instead of the keys ("" none)
    bool replayAids = false;                // the aids were given (a recording does not hold them: else none)
    bool quickStart = false;
    double entry = 2.0;                     // seconds from the load to the countdown
    uint32_t skipTo = 0;                    // run to this tick at full speed before the first frame
    float launchRpm = 7400.0f;              // launch control's revs (0 off)
    float antiLagCrack = 0.25f;             // the anti-lag's throttle crack (0 off)
    float chaseDistance = 5.2f, chasePull = 1.5f;
};

// The keys for one frame (all false and 0 when the window is not focused).
struct FrameInput {
    double seconds = 0.0;      // the frame's time; 0 paused (the frame stays between the same steps)
    bool throttle = false, brake = false, handbrake = false;
    float steer = 0.0f;        // where the steering keys point: -1 left, 0, 1 right
    uint32_t presses = 0;      // since the last frame: kGearUp, kGearDown, kBackOnRoad
    bool antiLagIdle = false;  // the anti-lag armed at standstill (N)
    float chaseDistance = 5.2f;
};
constexpr uint32_t kGearUp = 1u << 0, kGearDown = 1u << 1, kBackOnRoad = 0x10u;   // (the control record's flags)

// The car (its parts that can come loose too) and the stage's solids at one physics step, and the chase camera there.
struct Shown {
    physics::Pose pose{};
    physics::Wheel wheels[4]{};
    std::vector<physics::Part> parts;
    std::vector<physics::Body> bodies;
    camera::Chase chase;
};

// What a frame draws: between `before` and `after` at `t` (0..1).
struct Snapshot {
    Shown before, after;
    float t = 0.0f;
    uint32_t tick = 0;
    physics::Dashboard dash{};
    rules::StageClock clock;
    float centerOfGravity[3]{};
    bool failed = false;       // the physics faulted: the run is over (Error())
    bool replayEnded = false;  // a replay ran out of steps (the frame stays on its last)
};

class Session {
public:
    // The physics loaded and the replay read. Returns "" or what is wrong.
    std::string Open(const Config& c);
    // The physics' records, and the driver layer's DriverTick, on `bus` from now on.
    void Connect(signals::Bus* bus);
    // The start: the settling steps, the stage's marks, the skip. Returns "" or what is wrong.
    std::string Start();
    // One frame's time with its keys: the physics stepped through it.
    void Advance(const FrameInput& in);
    void Snap(Snapshot& out) const;
    // The run's end: a replay's check reported, the drive's recording written.
    void Finish();

    bool Replaying() const { return !m_replay.events.empty(); }
    bool Failed() const { return m_failed; }
    const std::string& Error() const { return m_error; }
    uint32_t Tick() const { return m_sim.Tick(); }
    const physics::Physics& Physics() const { return m_sim; }

private:
    bool Step(uint32_t n);
    bool ReplayStep();
    void CheckPose();
    void ClockStep();
    void Stepped();
    void Record(char type, uint32_t tick, uint32_t steps, const uint32_t* w, int n);
    bool InEntry() const { return m_sim.Tick() * m_dt < m_countdownAt; }
    void Capture(Shown& out);
    void Fail();

    Config m_config;
    physics::Physics m_sim;
    signals::Bus* m_bus = nullptr;
    double m_dt = 0.0;
    double m_countdownAt = 0.0;
    rules::StageClock m_clock;
    camera::ChaseRig m_chase;
    LaunchControl m_launch;
    AntiLag m_antiLag;
    float m_steering = 0.0f;
    uint32_t m_flags = 0;          // presses not yet carried by a control record
    double m_acc = 0.0;            // time not yet stepped
    Shown m_before, m_after;
    float m_cog[3]{};

    phys::data::Recording m_rec;
    phys::data::Recording m_replay;
    size_t m_replayAt = 0;
    uint32_t m_posesChecked = 0, m_posesDiffering = 0, m_firstDifference = 0;
    bool m_replayEnded = false;
    bool m_failed = false;
    std::string m_error;
};

} // namespace wrally::session
