#include "session/session.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "data/dls.h"
#include "signals/bus.h"
#include "signals/car.h"

namespace wrally::session {

std::string Session::Open(const Config& c)
{
    m_config = c;
    m_chase.distance = c.chaseDistance;
    m_chase.pull = c.chasePull;
    m_launch.target = c.launchRpm;
    m_antiLag.crack = c.antiLagCrack;
    m_antiLag.enabled = m_antiLag.crack > 0;

    physics::Config pc = c.physics;
    // --replay: the recording's controls and steps instead of the keys, with its settings (a recording does not hold
    // the driving aids: none unless they were given).
    if (!c.replay.empty()) {
        try {
            m_replay = phys::data::ReadRecording(c.replay);
        } catch (const std::exception& e) {
            return e.what();
        }
        if (m_replay.settings & 1) pc.settings = m_replay.settings;
        if (!c.replayAids)
            for (uint32_t& a : pc.aids) a = 0;
        // (stage 41's recording replays bit-exact without its spectators)
        if (!m_replay.anims.empty())
            std::printf("the replay's %zu animation objects left out (the physics has none yet)\n", m_replay.anims.size());
    }
    m_config.physics = pc;
    std::printf("loading the physics (%s)...\n", c.physicsId.c_str());
    if (std::string e = m_sim.Open(c.physicsModules, c.physicsId, pc); !e.empty()) return "physics: " + e;
    std::printf("physics module: %s\n", m_sim.ModuleName().c_str());
    std::printf("%sdt %.4f s\n", m_sim.Log().c_str(), m_sim.Dt());
    m_dt = m_sim.Dt();
    m_rec.settings = pc.settings | 1;
    return "";
}

void Session::Connect(signals::Bus* bus)
{
    m_bus = bus;
    m_sim.Connect(bus);
}

void Session::Record(char type, uint32_t tick, uint32_t steps, const uint32_t* w, int n)
{
    phys::data::RecEvent e{};
    e.type = type;
    e.tick = tick;
    e.steps = steps;
    if (w) std::memcpy(e.words, w, 4 * n);
    m_rec.events.push_back(e);
}

bool Session::Step(uint32_t n)
{
    const uint32_t tick = m_sim.Tick();
    if (!m_sim.Step(n)) return false;
    Record('S', tick, n, nullptr, 0);
    uint32_t pose[13];
    m_sim.PoseWords(pose);
    Record('P', 0, 0, pose, 13);
    return true;
}

void Session::Fail()
{
    m_failed = true;
    m_error = m_sim.Error();
    std::fprintf(stderr, "physics: %s\n", m_error.c_str());
}

std::string Session::Start()
{
    // The start as the game settles it: StepForward(40) until tick 160 (a replay has its own).
    for (int k = 0; k < (Replaying() ? 0 : 4); ++k)
        if (!Step(40)) return "physics: " + m_sim.Error();

    // The stage clock: the .dls's marks, the entry, the countdown.
    float finish = -1;
    std::vector<float> checkpoints;
    try {
        for (const phys::data::DlsPacenote& n : phys::data::ReadDls(m_config.dls).pacenotes) {
            if (n.type == phys::data::kDlsFinish) finish = n.distance;
            else if (n.type == phys::data::kDlsCheckpoint) checkpoints.push_back(n.distance);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "the stage's marks: %s\n", e.what());
    }
    m_clock.SetMarks(finish, checkpoints);
    std::printf("stage marks: %zu splits, finish %.0f m along the drive line; the car at %.0f m\n", checkpoints.size(), finish,
                m_sim.StageDistance());
    m_countdownAt = m_config.quickStart ? 0.0 : std::max(0.0, m_config.entry);
    m_clock.Reset(m_config.quickStart ? 3.5f : rules::StageClock::kCountdown);
    m_sim.CenterOfGravity(m_cog);

    // The skip: the physics run to skipTo at full speed before the first frame, nothing drawn - a replay's steps, or for
    // a drive no controls (recorded, so the recording still replays).
    if (m_config.skipTo > m_sim.Tick()) {
        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        const uint32_t from = m_sim.Tick();
        while (!m_failed && m_sim.Tick() < m_config.skipTo) {
            if (Replaying()) {
                if (!ReplayStep()) break;
                continue;
            }
            physics::Control ctl;
            uint32_t w[8];
            std::memcpy(w, &ctl, 32);
            m_sim.Apply(ctl);
            Record('C', 0, 0, w, 8);
            if (!Step(1)) Fail();
            ClockStep();
        }
        QueryPerformanceCounter(&t1);
        std::printf("skipped ticks %u to %u in %.1f s\n", from, m_sim.Tick(), double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart));
    }
    // No step before the first frame: both ends the same, the chase camera where it settles.
    Capture(m_after);
    m_after.chase = m_chase.Settle(m_after.pose);
    m_before = m_after;
    return "";
}

// After every physics step: the clock, its events printed (in the entry nothing).
void Session::ClockStep()
{
    if (InEntry()) return;
    const float time = m_clock.Time();
    switch (m_clock.Advance(static_cast<float>(m_dt), m_sim.Dash().speed, m_sim.StageDistance())) {
    case rules::StageClock::kGo:
        std::printf("GO (tick %u)\n", m_sim.Tick());
        break;
    case rules::StageClock::kFalseStart:
        std::printf("FALSE START (tick %u): %.2f s of the countdown left + %.0f s\n", m_sim.Tick(),
                    m_clock.Time() - rules::StageClock::kFalseStartPenalty, rules::StageClock::kFalseStartPenalty);
        break;
    case rules::StageClock::kSplit:
        std::printf("split %zu: %s (tick %u)\n", m_clock.Splits().size(), rules::StageClock::Text(m_clock.Splits().back()).c_str(),
                    m_sim.Tick());
        break;
    case rules::StageClock::kFinish:
        std::printf("FINISH: %s%s (tick %u)\n", rules::StageClock::Text(time).c_str(),
                    m_clock.FalseStart() ? " (the false start's 10 s in)" : "", m_sim.Tick());
        break;
    default:
        break;
    }
}

void Session::Capture(Shown& out)
{
    out.pose = m_sim.CarPose();
    m_sim.Wheels(out.wheels);
    out.bodies = m_sim.Bodies();
}

// After a step: the frame's later end moves on, the chase camera follows on the physics' time.
void Session::Stepped()
{
    std::swap(m_before, m_after);
    Capture(m_after);
    m_after.chase = m_chase.Follow(m_before.chase, m_after.pose, static_cast<float>(m_dt));
}

// A replay checks itself: the pose the recording kept after each step ('P') against the physics' now, word for word -
// the golden check (docs/design.md 4.2). Reported by Finish.
void Session::CheckPose()
{
    if (m_replayAt >= m_replay.events.size() || m_replay.events[m_replayAt].type != 'P') return;
    uint32_t pose[13];
    m_sim.PoseWords(pose);
    ++m_posesChecked;
    if (std::memcmp(pose, m_replay.events[m_replayAt].words, sizeof pose) && !m_posesDiffering++) m_firstDifference = m_sim.Tick();
    ++m_replayAt;
}

// One step of a replay: its controls up to its next step, then the step. False at its end or on a fault.
bool Session::ReplayStep()
{
    while (m_replayAt < m_replay.events.size() && m_replay.events[m_replayAt].type != 'S') {
        if (m_replay.events[m_replayAt].type == 'C') m_sim.ApplyWords(m_replay.events[m_replayAt].words);
        ++m_replayAt;
    }
    if (m_replayAt == m_replay.events.size()) {
        if (!m_replayEnded) std::printf("the replay ends (tick %u)\n", m_sim.Tick());
        m_replayEnded = true;
        return false;
    }
    if (!m_sim.Step(m_replay.events[m_replayAt++].steps)) {
        Fail();
        return false;
    }
    CheckPose();
    ClockStep();
    return true;
}

void Session::Advance(const FrameInput& in)
{
    if (m_failed) return;
    // The chase camera's distance changed ([ / ]): the eyes the steps keep move with it, so it shows at once.
    if (in.chaseDistance != m_chase.distance) {
        m_chase.distance = in.chaseDistance;
        for (Shown* sh : {&m_before, &m_after}) sh->chase.eye = m_chase.Eye(sh->pose, sh->chase.behind, sh->chase.pull);
    }
    m_flags |= in.presses & (kGearUp | kGearDown);
    m_acc += in.seconds;   // (paused: 0 - the frame stays between the same steps)
    // Back on the road: the game's outside assistance (flag 0x10) - the car put back on the stage's drive line after
    // 0.5 s and held for 4 s. The game refuses it while the car moves faster than 1.5 m/s.
    if (in.presses & kBackOnRoad) {
        if (m_sim.Dash().speed > 1.5f) std::printf("back on the road: stop the car first (%.1f m/s)\n", m_sim.Dash().speed);
        else m_flags |= kBackOnRoad;
    }
    const float target = in.steer;
    while (m_acc >= m_dt && Replaying()) {
        m_acc -= m_dt;
        if (!ReplayStep()) break;
        Stepped();
    }
    while (m_acc >= m_dt && !Replaying()) {
        m_acc -= m_dt;
        // The steering moves toward the keys' target at 3 / s (back to the centre at 5 / s), as a keyboard driver's
        // filter would.
        const float rate = static_cast<float>(m_dt) * (target == 0.0f ? 5.0f : 3.0f);
        m_steering += std::max(-rate, std::min(rate, target - m_steering));
        physics::Control ctl;
        ctl.throttle = in.throttle ? 1.0f : 0.0f;
        {   // launch control and the anti-lag between the pedal and the engine
            const physics::Dashboard dash = m_sim.Dash();
            const float pedal = ctl.throttle;
            const bool was = m_launch.Active();
            ctl.throttle = m_launch.Throttle(ctl.throttle, dash.rpm, dash.speed, dash.gear, static_cast<float>(m_dt));
            if (m_launch.Active() != was)
                std::printf("launch control %s at %.0f rpm (tick %u)\n", m_launch.Active() ? "holding" : "off", dash.rpm, m_sim.Tick());
            m_antiLag.idleArmed = in.antiLagIdle;
            ctl.throttle = m_antiLag.Throttle(ctl.throttle, dash.rpm, dash.gear, dash.speed, static_cast<float>(m_dt));
            if (m_bus) {
                signals::DriverTick driver;   // for the sound: the anti-lag's crack is no driver's part throttle
                driver.stamp = {0, m_sim.Tick()};
                driver.pedal = pedal;
                driver.throttle = ctl.throttle;
                driver.launchControl = m_launch.Active();
                driver.antiLag = m_antiLag.Active();
                m_bus->Emit(driver);
            }
        }
        ctl.brake = in.brake ? 1.0f : 0.0f;
        ctl.handbrake = in.handbrake ? 1.0f : 0.0f;
        ctl.steering = m_steering;
        ctl.flags = m_flags;
        m_flags = 0;
        if (InEntry()) ctl.brake = ctl.handbrake = 1.0f;   // the entry: the car held
        uint32_t w[8];
        std::memcpy(w, &ctl, 32);
        m_sim.Apply(ctl);
        Record('C', 0, 0, w, 8);
        if (!Step(1)) {
            Fail();
            break;
        }
        ClockStep();
        Stepped();
    }
}

void Session::Snap(Snapshot& out) const
{
    out.before = m_before;
    out.after = m_after;
    // This frame between the last two steps (past the last at a replay's end: the last).
    out.t = static_cast<float>(std::min(1.0, m_acc / m_dt));
    out.tick = m_sim.Tick();
    out.dash = m_sim.Dash();
    out.clock = m_clock;
    std::memcpy(out.centerOfGravity, m_cog, sizeof m_cog);
    out.failed = m_failed;
    out.replayEnded = m_replayEnded;
}

void Session::Finish()
{
    if (m_posesChecked) {
        if (m_posesDiffering)
            std::printf("replay check: %u of %u poses differ from the recording, the first at tick %u\n", m_posesDiffering,
                        m_posesChecked, m_firstDifference);
        else
            std::printf("replay check: %u poses, all bit-exact with the recording\n", m_posesChecked);
    }
    try {
        phys::data::WriteRecording(m_config.out, m_rec);
        std::printf("the drive: %zu events to %s (tick %u)\n", m_rec.events.size(), m_config.out.c_str(), m_sim.Tick());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
    }
}

} // namespace wrally::session
