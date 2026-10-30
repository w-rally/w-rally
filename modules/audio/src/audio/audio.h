#pragma once
// audio - the game's sound (doc/sound-engine-plan.md). It listens to the car's records on the signal bus
// (signals/car.h) and renders 48 kHz stereo float.
//
// Two ways to run:
//  - real time (Mode::RealTime): the bus's listeners queue the records (the physics' thread); the device's thread
//    pulls Render. The sound plays a few ticks behind the newest step with its pace nudged to keep that lag, so the
//    physics stepping in bursts (a frame's worth of ticks at once) still plays smoothly. Held at the newest step (a
//    pause) it falls silent.
//  - offline (Mode::Offline): every step (its StepEnd) renders its ticks' samples at once on the emitting thread into
//    `sink` - the same run makes the same samples, for a replay rendered to a WAV.
//
// The sounds come from a Bank (S1: RBR's set). The engine (S4, audio/grains.h): its loops cut into one grain per
// firing, a grain played at each firing the physics reports (EngineTick: the crank passing a cylinder's point and that
// cylinder having burnt its charge), from the loops around the revs and off / on the throttle; or (the mix's
// engine.grains 0) S1's way, the loops pitched to the revs (each by revs / its own) and crossfaded; without an engine in
// the bank, S0's voice - a pulse into an exhaust model at each firing. Around it: the
// turbo (pitch by its speed, level by the boost) and the blow-off valve when the throttle shuts on boost, pops on the
// overrun and at the rev limiter's cut, a bang on a misfire off the throttle, the gear change and the dog ring, the
// transmission's whine (pitch by the drivetrain's speed), the tyres rolling and sliding on the stage's surface, and
// the wind. One mono mix for now (S3 places the sources).

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <variant>
#include <vector>

#include "audio/bank.h"
#include "audio/grains.h"
#include "audio/mix.h"
#include "audio/voices.h"
#include "signals/car.h"

namespace audio {

constexpr uint32_t kRate = 48000;

class Audio {
public:
    enum class Mode { RealTime, Offline };
    explicit Audio(Mode mode);

    // The sounds to play (kept by the caller, alive while this plays); nullptr: S0's pulse engine alone. Set it before
    // Connect / the device opens.
    void SetBank(const Bank* bank);

    // The mix by hand (audio/mix.h); any thread, any time: the render picks it up at its next block.
    void SetMix(const Mix& mix);

    // The car's records from `bus` (the physics' thread), as car `car`.
    void Connect(signals::Bus& bus, uint32_t car = 0);
    void Disconnect(signals::Bus& bus);

    // Real time: the device's thread pulls `frames` stereo frames (interleaved L R).
    void Render(float* out, uint32_t frames);

    // Offline: where the samples go.
    std::function<void(const float* stereo, uint32_t frames)> sink;

    std::atomic<float> volume{0.8f};
    std::atomic<bool> muted{false};
    // Real time: the game's one-shots (audio/voices.h), mixed in after the car's sound (null: none). The car's samples
    // stay as they were - the voices only add.
    std::atomic<Voices*> overlay{nullptr};

private:
    using Msg = std::variant<signals::CarInfo, signals::EngineInfo, signals::EngineTick, signals::DriveTick, signals::WheelsTick,
                             signals::BodyTick, signals::GearChange, signals::StepEnd, signals::DriverTick>;
    static constexpr size_t kQueue = 4096;
    static constexpr size_t kHistory = 64;
    static constexpr uint32_t kBlock = 32;   // samples between control updates (0.67 ms)

    // A step's records, as the render side keeps them.
    struct Frame {
        uint32_t tick = 0;
        signals::EngineTick engine;
        signals::DriveTick drive;
        signals::WheelsTick wheels;
        signals::BodyTick body;
        signals::GearChange gears[4];
        uint32_t gearCount = 0;
        signals::DriverTick driver;   // the controls that drove the step (none: zero)
    };

    // What the voices follow, between two steps.
    struct Controls {
        float rpm = 0, throttle = 0, boost = 1, turboSpeed = 0, outputSpeed = 0, engineTorque = 0, speed = 0;
        float slip = 0;      // m/s, the wheels on the ground's mean slide
        bool antiLag = false;   // the anti-lag on (DriverTick)
        float ground = 0;    // the share of the wheels on the ground
        int gear = 0;
    };

    struct Loop {
        const Sample* sample = nullptr;
        double position = 0;
        float gain = 0, target = 0, pitch = 1;
        float Next();
    };
    struct Shot {
        const Sample* sample = nullptr;
        double position = 0;
        float gain = 0, pitch = 1;
    };
    struct Biquad {
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void BandPass(float f, float q);
        void LowPass(float f, float q);
        void LowShelf(float f, float gain);
        float Run(float x);
    };

    void Push(const Msg& m);
    void Drain();
    void Produce(float* out, uint32_t frames, double rate);
    const Frame* At(double tick, const Frame** next, float* u) const;
    Controls Interpolate(double tick) const;
    void StepEvents(const Frame& f);
    void Steer(const Controls& c, float dt);
    void Play(const std::vector<Sample>& set, float gain, float pitch = 1);
    void Play(const Sample& s, float gain, float pitch = 1);
    float Random();
    uint32_t Firings(double tick, const Frame** after, uint32_t* passed);
    float PulseSample(uint32_t fired, const Frame& after);

    Mode m_mode;
    uint32_t m_car = 0;
    const Bank* m_bank = nullptr;
    std::array<signals::Bus::Connection, 9> m_connections{};

    // The physics' side writes m_queue / m_tail, the render side reads them and owns all below. (On the heap: an
    // Audio is often a local.)
    std::vector<Msg> m_queue = std::vector<Msg>(kQueue);
    std::atomic<size_t> m_head{0}, m_tail{0};

    signals::EngineInfo m_info;
    bool m_hasInfo = false;
    uint32_t m_surface = signals::CarInfo::kTarmac;
    Frame m_building;                 // the step being received (its EngineTick seen, its StepEnd not yet)
    bool m_buildingOn = false;
    signals::GearChange m_pendingGears[4];   // the events before the step's records
    uint32_t m_pendingCount = 0;
    signals::DriverTick m_pendingDriver;     // the controls for the next step
    std::vector<Frame> m_history = std::vector<Frame>(kHistory);   // the complete steps, the newest last
    size_t m_count = 0;
    double m_play = -1;               // the play position, in ticks (-1: not started)
    uint32_t m_eventsTo = 0;          // the steps up to this tick have had their events
    float m_samplesPerTick = 336;
    float m_held = 0;                 // seconds the play has sat at the newest step

    Mix m_mix, m_nextMix;             // in use; set by SetMix, taken under m_mixLock
    std::mutex m_mixLock;
    std::atomic<bool> m_mixChanged{false};

    // The voices.
    std::vector<Loop> m_engine;       // one per bank loop
    std::vector<float> m_engineRpm;   // the bank's rpm points, sorted, unique
    Loop m_turbo, m_transmission, m_wind, m_roll, m_skid;
    std::array<Shot, 16> m_shots{};
    Biquad m_shelf;
    float m_load = 0;                 // the throttle, eased (the engine's on / off blend)
    float m_antiLagIdle = 0;          // seconds the anti-lag has run at standstill (its bangs build up)
    float m_lastThrottle = 0, m_liftTime = 10, m_popTimer = 0, m_valveWait = 0;
    uint32_t m_random = 0x9e3779b9;

    // S4's grain engine (on m_bank's loops).
    std::vector<GrainLoop> m_grainLoops;
    GrainEngine m_grains;
    bool m_useGrains = false;                     // this block: the grains play (else the loops)
    float m_grainLoad = 0, m_grainOn = 0, m_grainOff = 0, m_grainRpm = 0;
    // The idle's wander: two smooth random curves in -1..1 (the firings' timing, their level), a new target every
    // 0.12..0.35 s, eased (smoothstep) from the last.
    float m_wanderFrom[2] = {}, m_wanderTo[2] = {}, m_wander[2] = {};
    float m_wanderT = 1, m_wanderLength = 0.2f;

    // S0's pulse engine.
    float m_lastAngle = -1;
    float m_peakCharge = 1e-9f;
    float m_excite = 0;
    uint32_t m_burst = 0;
    Biquad m_pipe[2], m_tone;
    std::array<float, 1024> m_comb{};
    uint32_t m_combAt = 0;
};

} // namespace audio
