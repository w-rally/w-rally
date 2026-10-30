#include "audio/audio.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace audio {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kCycle = 4 * kPi;       // a four-stroke cycle: two turns
constexpr uint32_t kBurst = 96;         // S0: a firing's exciting burst, samples (2 ms)
constexpr uint32_t kCombLength = 288;   // S0: the exhaust's reflection, samples (6 ms)
constexpr double kLag = 4.0;            // real time: how many ticks behind the newest step the sound plays
constexpr float kSmooth = 1.0f / 480;   // a loop's gain eases to its target over ~10 ms
constexpr float kRadToRpm = 9.5492966f;
// The mix into the soft limiter (tanh): sounds play at their recordings' level, which together run near and over full
// scale - scaled down first so the limiter only rounds the peaks (the volume comes after it).
constexpr float kHeadroom = 0.6f;

// The mix (S1: by ear against RBR's; 1 is a loop at its recorded level).
constexpr float kEngineGain = 0.55f, kGrainGain = 1.0f, kUnfiredGain = 0.8f, kUnfiredOnGain = 0.3f, kCutGain = 0.25f, kIdleSpread = 0.10f, kWanderTime = 3.0f * kRate / 1000, kWanderLevel = 0.06f, kTurboGain = 0.18f, kValveGain = 0.45f, kTransmissionGain = 0.10f;
constexpr float kRollGain = 0.22f, kSkidGain = 0.35f, kWindGain = 0.25f;
// The gear change and the dog ring: RBR's samples are a cabin's clunk; outside, a sequential box's upshift is near
// silent - its sound is the ignition cut (a Rally1 recording, the user). Up again inside the car (S3).
constexpr float kGearGain = 0.06f, kDogGain = 0.04f, kPopGain = 0.40f, kBangGain = 0.55f;
// The anti-lag's bangs: a Rally1's peak ~8 dB over its engine (each one ~5-8 dB over the 30 ms before it).
constexpr float kAntiLagGain = 1.0f, kAntiLagEngine = 0.4f;   // the engine itself quieter: the ignition retarded

float Wrap(float a)
{
    a = std::fmod(a, kCycle);
    return a < 0 ? a + kCycle : a;
}

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

float Lerp(float a, float b, float u) { return a + (b - a) * u; }

// A crossfade's position x (0..1) with the blend narrowed to `width` of the way, in its middle.
float Narrow(float x, float width)
{
    if (width >= 1) return x;
    if (width <= 0) return x < 0.5f ? 0.0f : 1.0f;
    return Clamp01((x - (0.5f - width / 2)) / width);
}

} // namespace

void Audio::Biquad::BandPass(float f, float q)
{
    const float w = 2 * kPi * f / kRate, alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
    b0 = alpha / a0, b1 = 0, b2 = -alpha / a0, a1 = -2 * std::cos(w) / a0, a2 = (1 - alpha) / a0;
}

void Audio::Biquad::LowPass(float f, float q)
{
    const float w = 2 * kPi * f / kRate, alpha = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + alpha;
    b0 = (1 - c) / 2 / a0, b1 = (1 - c) / a0, b2 = b0, a1 = -2 * c / a0, a2 = (1 - alpha) / a0;
}

void Audio::Biquad::LowShelf(float f, float gain)   // RBJ, slope 1; `gain` the linear amplitude below f
{
    const float A = std::sqrt(gain), w = 2 * kPi * f / kRate, c = std::cos(w), s = std::sin(w) / 2 * std::sqrt(2.0f);
    const float r = 2 * std::sqrt(A) * s, a0 = (A + 1) + (A - 1) * c + r;
    b0 = A * ((A + 1) - (A - 1) * c + r) / a0;
    b1 = 2 * A * ((A - 1) - (A + 1) * c) / a0;
    b2 = A * ((A + 1) - (A - 1) * c - r) / a0;
    a1 = -2 * ((A - 1) + (A + 1) * c) / a0;
    a2 = ((A + 1) + (A - 1) * c - r) / a0;
}

float Audio::Biquad::Run(float x)
{
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

float Audio::Loop::Next()
{
    gain += (target - gain) * kSmooth;
    if (!sample || sample->Empty() || gain < 1e-5f) return 0;
    const std::vector<float>& d = sample->data;
    const size_t n = d.size(), i = static_cast<size_t>(position);
    const float f = static_cast<float>(position - double(i)), v = d[i] + (d[(i + 1) % n] - d[i]) * f;
    position += double(pitch) * sample->rate / kRate;
    if (position >= double(n)) position = std::fmod(position, double(n));
    return v * gain;
}

Audio::Audio(Mode mode) : m_mode(mode)
{
    m_pipe[0].BandPass(120, 2.5f);
    m_pipe[1].BandPass(420, 3.0f);
    m_tone.LowPass(1800, 0.7f);
}

void Audio::SetBank(const Bank* bank)
{
    m_bank = bank;
    m_engine.clear();
    m_engineRpm.clear();
    if (!bank) return;
    for (const EngineLoop& l : bank->engine) {
        m_engine.push_back(Loop{&l.sample});
        m_engineRpm.push_back(l.rpm);
    }
    std::sort(m_engineRpm.begin(), m_engineRpm.end());
    m_engineRpm.erase(std::unique(m_engineRpm.begin(), m_engineRpm.end()), m_engineRpm.end());
    m_shelf.LowShelf(bank->lowShelfFrequency, bank->lowShelfGain);
    m_grainLoops = CutGrains(bank->engine);
    m_grains.Set(&m_grainLoops);
    m_turbo.sample = &bank->turbo;
    m_transmission.sample = &bank->transmission;
    m_wind.sample = &bank->wind;
    // The stage's surface picks the tyres' loops (CarInfo, later).
    m_roll.sample = &bank->roll[0];
    m_skid.sample = &bank->skid[0];
}

void Audio::SetMix(const Mix& mix)
{
    std::lock_guard<std::mutex> lock(m_mixLock);
    m_nextMix = mix;
    m_mixChanged = true;
}

void Audio::Connect(signals::Bus& bus, uint32_t car)
{
    m_car = car;
    size_t k = 0;
    auto listen = [&](auto type) {
        using T = decltype(type);
        m_connections[k++] = bus.Connect<T>([this](const T& r) {
            if (r.stamp.car == m_car) Push(r);
        });
    };
    listen(signals::CarInfo{});
    listen(signals::EngineInfo{});
    listen(signals::EngineTick{});
    listen(signals::DriveTick{});
    listen(signals::WheelsTick{});
    listen(signals::BodyTick{});
    listen(signals::GearChange{});
    listen(signals::DriverTick{});
    m_connections[k++] = bus.Connect<signals::StepEnd>([this](const signals::StepEnd& r) {
        if (r.stamp.car != m_car) return;
        Push(r);
        if (m_mode != Mode::Offline) return;
        // Offline: the ticks this step advanced, rendered now.
        Drain();
        if (m_count < 2) return;
        const double newest = m_history[m_count - 1].tick;
        if (m_play < 0) m_play = m_eventsTo = m_history[m_count - 2].tick;
        const uint32_t frames = static_cast<uint32_t>(std::lround((newest - m_play) * m_samplesPerTick));
        static std::vector<float> buffer;
        buffer.resize(2 * size_t(frames));
        Produce(buffer.data(), frames, 1.0);
        m_play = newest;
        if (sink && frames) sink(buffer.data(), frames);
    });
}

void Audio::Disconnect(signals::Bus& bus)
{
    for (signals::Bus::Connection& c : m_connections) {
        if (c) bus.Disconnect(c);
        c = 0;
    }
}

void Audio::Push(const Msg& m)
{
    const size_t tail = m_tail.load(std::memory_order_relaxed), next = (tail + 1) % kQueue;
    if (next == m_head.load(std::memory_order_acquire)) return;   // full: the render side stalled - dropped
    m_queue[tail] = m;
    m_tail.store(next, std::memory_order_release);
}

void Audio::Drain()
{
    size_t head = m_head.load(std::memory_order_relaxed);
    while (head != m_tail.load(std::memory_order_acquire)) {
        const Msg& m = m_queue[head];
        if (const auto* car = std::get_if<signals::CarInfo>(&m)) {
            m_surface = std::min<uint32_t>(car->surface, 2);
            if (m_bank) m_roll.sample = &m_bank->roll[m_surface], m_skid.sample = &m_bank->skid[m_surface];
        } else if (const auto* info = std::get_if<signals::EngineInfo>(&m)) {
            m_info = *info;
            m_hasInfo = true;
            if (info->dt > 0) m_samplesPerTick = info->dt * kRate;
        } else if (const auto* tick = std::get_if<signals::EngineTick>(&m)) {   // a step's records begin
            m_building = Frame{};
            m_building.tick = tick->stamp.tick;
            m_building.engine = *tick;
            std::copy(m_pendingGears, m_pendingGears + m_pendingCount, m_building.gears);
            m_building.gearCount = m_pendingCount;
            m_building.driver = m_pendingDriver;
            m_pendingCount = 0;
            m_buildingOn = true;
        } else if (const auto* drive = std::get_if<signals::DriveTick>(&m)) {
            if (m_buildingOn && drive->stamp.tick == m_building.tick) m_building.drive = *drive;
        } else if (const auto* wheels = std::get_if<signals::WheelsTick>(&m)) {
            if (m_buildingOn && wheels->stamp.tick == m_building.tick) m_building.wheels = *wheels;
        } else if (const auto* body = std::get_if<signals::BodyTick>(&m)) {
            if (m_buildingOn && body->stamp.tick == m_building.tick) m_building.body = *body;
        } else if (const auto* driver = std::get_if<signals::DriverTick>(&m)) {
            m_pendingDriver = *driver;
        } else if (const auto* gear = std::get_if<signals::GearChange>(&m)) {
            if (m_pendingCount < 4) m_pendingGears[m_pendingCount++] = *gear;
        } else if (const auto* end = std::get_if<signals::StepEnd>(&m)) {   // the step complete: kept
            if (m_buildingOn && end->stamp.tick == m_building.tick) {
                if (m_count && m_building.tick <= m_history[m_count - 1].tick) {   // time went back (a restart): anew
                    m_count = 0;
                    m_play = -1;
                    m_lastAngle = -1;
                }
                if (m_count == kHistory) {
                    std::move(m_history.begin() + 1, m_history.end(), m_history.begin());
                    --m_count;
                }
                m_history[m_count++] = m_building;
            }
            m_buildingOn = false;
        }
        head = (head + 1) % kQueue;
        m_head.store(head, std::memory_order_release);
    }
}

void Audio::Render(float* out, uint32_t frames)
{
    Drain();
    Voices* const voices = overlay.load(std::memory_order_acquire);   // the game's one-shots, after the car's
    if (m_count < 2 || muted.load(std::memory_order_relaxed)) {
        std::fill(out, out + 2 * size_t(frames), 0.0f);
        if (voices) voices->Mix(out, frames, static_cast<float>(kRate));
        return;
    }
    const double newest = m_history[m_count - 1].tick, oldest = m_history[0].tick;
    if (m_play < 0 || newest - m_play > 8 * kLag || m_play > newest) {   // started, or fell far behind: to the lag
        m_play = std::max(oldest, newest - kLag);
        m_eventsTo = static_cast<uint32_t>(m_play);
        m_lastAngle = -1;
    }
    // Faster when further behind than the lag, slower when closer: the physics' bursts even out.
    const double rate = std::clamp(1.0 + (newest - m_play - kLag) * 0.03, 0.5, 1.5);
    Produce(out, frames, rate);
    if (voices) voices->Mix(out, frames, static_cast<float>(kRate));
}

const Audio::Frame* Audio::At(double tick, const Frame** next, float* u) const
{
    // The steps bracketing `tick` (the history holds them in order).
    size_t k = m_count - 1;
    while (k > 0 && m_history[k].tick > tick) --k;
    const Frame& a = m_history[k];
    const Frame& b = k + 1 < m_count ? m_history[k + 1] : a;
    *next = &b;
    *u = &a == &b ? 0.0f : Clamp01(static_cast<float>((tick - a.tick) / double(b.tick - a.tick)));
    return &a;
}

Audio::Controls Audio::Interpolate(double tick) const
{
    const Frame* b;
    float u;
    const Frame* a = At(tick, &b, &u);
    Controls c;
    c.rpm = Lerp(a->engine.crankSpeed, b->engine.crankSpeed, u) * kRadToRpm;
    c.throttle = Lerp(a->engine.throttle, b->engine.throttle, u);
    c.boost = Lerp(a->engine.boost, b->engine.boost, u);
    c.turboSpeed = Lerp(a->engine.turboSpeed, b->engine.turboSpeed, u);
    c.outputSpeed = Lerp(a->drive.outputSpeed, b->drive.outputSpeed, u);
    c.engineTorque = Lerp(a->drive.engineTorque, b->drive.engineTorque, u);
    c.speed = Lerp(a->body.speed, b->body.speed, u);
    c.gear = b->drive.gear;
    c.antiLag = b->driver.antiLag;
    float slip = 0, ground = 0;
    for (int k = 0; k < 4; ++k) {
        const signals::WheelsTick::Wheel &wa = a->wheels.wheels[k], &wb = b->wheels.wheels[k];
        if (!wb.onGround) continue;
        ground += 1;
        slip += Lerp(std::hypot(wa.slipLongitudinal, wa.slipLateral), std::hypot(wb.slipLongitudinal, wb.slipLateral), u);
    }
    c.slip = ground > 0 ? slip / ground : 0;
    c.ground = ground / 4;
    return c;
}

float Audio::Random()
{
    m_random = m_random * 1664525u + 1013904223u;
    return static_cast<float>(m_random >> 8) / float(1 << 24);
}

void Audio::Play(const Sample& s, float gain, float pitch)
{
    if (s.Empty() || gain <= 0) return;
    Shot* slot = &m_shots[0];   // a free one, else the one furthest through
    for (Shot& sh : m_shots) {
        if (!sh.sample) {
            slot = &sh;
            break;
        }
        if (sh.position / sh.sample->data.size() > (slot->sample ? slot->position / slot->sample->data.size() : 0)) slot = &sh;
    }
    *slot = Shot{&s, 0.0, gain, pitch};
}

void Audio::Play(const std::vector<Sample>& set, float gain, float pitch)
{
    if (set.empty()) return;
    Play(set[std::min(set.size() - 1, static_cast<size_t>(Random() * set.size()))], gain, pitch * (0.95f + 0.1f * Random()));
}

// What a step brings when the play reaches it: the gear changes, the limiter's cut, a misfire.
void Audio::StepEvents(const Frame& f)
{
    if (!m_bank) return;
    for (uint32_t i = 0; i < f.gearCount; ++i)
        if (f.gears[i].engaged) {
            Play(m_bank->gearChanges, kGearGain * m_mix.Gain(Mix::kGear) * (0.5f + 0.5f * f.gears[i].level));
            Play(m_bank->dogRings, kDogGain * m_mix.Gain(Mix::kDogRing));
        }
    const float rpm = f.engine.crankSpeed * kRadToRpm;
    if ((f.engine.sparks & 0xf) != 0xf && rpm > 0.9f * m_info.rpmLimit && Random() < 0.15f)   // the limiter cutting
        Play(m_bank->pops, kPopGain * m_mix.Gain(Mix::kPops) * 0.8f);
    if (f.engine.misfire > 0 && f.engine.throttle < 0.2f && rpm > 2500)   // unburnt charge lit in the exhaust
        Play(m_bank->bangs, kBangGain * m_mix.Gain(Mix::kBangs) * std::min(1.0f, f.engine.misfire));
}

// The voices' targets from the controls, every block (dt seconds).
void Audio::Steer(const Controls& c, float dt)
{
    const bool running = c.rpm > 300;
    // The throttle eased: quick on, slower off (the engine's on / off loops).
    m_load += (c.throttle - m_load) * Clamp01(dt / (c.throttle > m_load ? 0.04f : 0.12f));

    // The engine's loops: the two recorded revs around these, each pitched to them.
    if (!m_engine.empty() && !m_engineRpm.empty()) {
        const std::vector<float>& R = m_engineRpm;
        size_t i = 0;
        while (i + 1 < R.size() && R[i + 1] <= c.rpm) ++i;
        const float x = Narrow(i + 1 < R.size() && c.rpm > R[i] ? Clamp01((c.rpm - R[i]) / (R[i + 1] - R[i])) : 0.0f, m_mix.rpmBlend);
        const float lower = std::cos(x * kPi / 2), upper = std::sin(x * kPi / 2), load = Narrow(m_load, m_mix.loadBlend);
        const float on = std::sqrt(load) * m_mix.Gain(Mix::kEngineOn), off = std::sqrt(1 - load) * m_mix.Gain(Mix::kEngineOff);
        const float level = kEngineGain * (0.7f + 0.3f * m_load);
        // The grains take the same blend (fired at the firings, in Produce); the loops are silent then.
        m_useGrains = m_mix.grains >= 0.5f && !m_grainLoops.empty();
        m_grainLoad = load;
        // (No level by the throttle for the grains: a recording's on and off loops carry their own - its overrun is
        // nearly as loud as its pull.)
        // The anti-lag retards the ignition: the cylinders hardly burn - the engine quieter, its exhaust's bangs on top.
        const float burn = c.antiLag ? kAntiLagEngine : 1.0f;
        m_grainOn = running ? m_mix.Gain(Mix::kEngineOn) * burn : 0.0f;
        m_grainOff = running ? m_mix.Gain(Mix::kEngineOff) * burn : 0.0f;
        m_grainRpm = c.rpm;
        for (size_t k = 0; k < m_engine.size(); ++k) {
            const EngineLoop& l = m_bank->engine[k];
            float w = 0;
            if (l.rpm == R[i]) w = lower;
            else if (i + 1 < R.size() && l.rpm == R[i + 1]) w = upper;
            w *= l.load > 0.5f ? on : off;
            m_engine[k].target = running && !m_useGrains ? level * w : 0.0f;
            m_engine[k].pitch = std::clamp(c.rpm / l.rpm, 0.25f, 4.0f);
        }
    }

    // The turbo: its whistle with its speed, louder with the boost.
    // The anti-lag keeps the turbo spinning by burning in the exhaust - which the physics does not model: its boost falls
    // (1.05 lifted with the anti-lag against 1.01 without), so the whistle is held up here while it is on.
    const float spool = c.antiLag ? std::max(Clamp01(c.turboSpeed / 21000), 0.7f) : Clamp01(c.turboSpeed / 21000);
    const float boost = c.antiLag ? std::max(Clamp01((c.boost - 1) / 1.5f), 0.5f) : Clamp01((c.boost - 1) / 1.5f);
    m_turbo.pitch = 0.4f + 1.2f * spool;
    m_turbo.target = m_hasInfo && m_info.turbo ? kTurboGain * m_mix.Gain(Mix::kTurbo) * (0.2f * spool + 0.8f * boost * (c.antiLag ? 1.0f : m_load)) : 0.0f;
    // The blow-off valve: the throttle shut on boost.
    m_valveWait = std::max(0.0f, m_valveWait - dt);
    if (m_bank && m_lastThrottle > 0.5f && c.throttle < 0.2f && boost > 0.25f && m_valveWait == 0) {
        Play(m_bank->valve, kValveGain * m_mix.Gain(Mix::kValve) * boost);
        m_valveWait = 0.5f;
    }

    // Pops on the overrun: off the throttle at revs, thinning out over two seconds.
    if (c.throttle < 0.1f && m_lastThrottle >= 0.1f) m_liftTime = 0;
    m_liftTime += dt;
    m_popTimer -= dt;
    if (m_bank && running && c.throttle < 0.1f && c.rpm > 3500 && m_liftTime < 2 && m_popTimer <= 0) {
        Play(m_bank->pops, kPopGain * m_mix.Gain(Mix::kPops) * (1 - m_liftTime / 2));
        m_popTimer = 0.05f + 0.2f * Random() + 0.2f * m_liftTime;
    }
    m_lastThrottle = c.throttle;

    // The transmission's whine with the drivetrain's speed, louder under torque.
    const float shaft = Clamp01(std::fabs(c.outputSpeed) / 600);
    m_transmission.pitch = std::clamp(std::fabs(c.outputSpeed) / 600, 0.2f, 2.5f);
    m_transmission.target = c.gear != 0 ? kTransmissionGain * m_mix.Gain(Mix::kTransmission) * shaft * (0.4f + 0.6f * Clamp01(std::fabs(c.engineTorque) / 800)) : 0.0f;

    // The tyres: rolling with the speed, sliding with the slip; the wind.
    const float v = Clamp01(c.speed / 30);
    m_roll.pitch = 0.7f + 0.6f * v;
    m_roll.target = kRollGain * m_mix.Gain(Mix::kRoll) * Clamp01(c.speed / 25) * c.ground;
    m_skid.pitch = 0.9f + 0.2f * v;
    m_skid.target = kSkidGain * m_mix.Gain(Mix::kSkid) * Clamp01((c.slip - 1.5f) / 6) * c.ground;
    const float air = Clamp01(c.speed / 45);
    m_wind.pitch = 0.8f + 0.4f * air;
    m_wind.target = kWindGain * m_mix.Gain(Mix::kWind) * air * air;
}

void Audio::Produce(float* out, uint32_t frames, double rate)
{
    const double newest = m_history[m_count - 1].tick, step = rate / m_samplesPerTick;
    const float gain = volume.load(std::memory_order_relaxed), dt = float(kBlock) / kRate;
    const bool loops = m_bank && !m_engine.empty();
    for (uint32_t i = 0; i < frames; ++i) {
        if (i % kBlock == 0) {
            if (m_mixChanged.load(std::memory_order_acquire) && m_mixLock.try_lock()) {
                m_mix = m_nextMix;
                m_mixChanged = false;
                m_mixLock.unlock();
            }
            // The steps the play has reached: their events.
            for (size_t k = 0; k < m_count; ++k)
                if (m_history[k].tick > m_eventsTo && m_history[k].tick <= m_play) {
                    StepEvents(m_history[k]);
                    m_eventsTo = m_history[k].tick;
                }
            m_held = m_play >= newest ? m_held + dt : 0;
            m_wanderT += dt / m_wanderLength;
            if (m_wanderT >= 1) {
                m_wanderT = 0;
                m_wanderLength = 0.12f + 0.23f * Random();
                for (int k = 0; k < 2; ++k) m_wanderFrom[k] = m_wanderTo[k], m_wanderTo[k] = 2 * Random() - 1;
            }
            const float ease = m_wanderT * m_wanderT * (3 - 2 * m_wanderT);
            for (int k = 0; k < 2; ++k) m_wander[k] = m_wanderFrom[k] + (m_wanderTo[k] - m_wanderFrom[k]) * ease;
            {   // the anti-lag at standstill: how long it has run
                const Frame* fb;
                float u;
                At(m_play, &fb, &u);
                const bool idleAntiLag = fb->driver.antiLag && fb->body.speed < 1 && fb->drive.gear == 0;
                m_antiLagIdle = idleAntiLag ? m_antiLagIdle + dt : 0.0f;
            }
            Controls c = Interpolate(m_play);
            if (m_held > 0.15f) c = Controls{};   // held at the newest step (a pause): silence
            Steer(c, dt);
        }
        const Frame* after;
        uint32_t passed;
        const uint32_t fired = Firings(m_play, &after, &passed);
        float engine = 0;
        if (!loops) {
            engine = PulseSample(fired, *after) * m_mix.Gain(Mix::kEngineOn);
        } else {
            // A grain at every cylinder the crank passes: one that burnt its charge as the throttle blends off / on, one
            // that did not (the physics' overrun and low-charge skips, a misfire, the limiter's cut) as the overrun -
            // the engine still pumps its air through the exhaust.
            m_grains.jumps = m_mix.grainJumps;
            if (passed && m_useGrains && m_grainRpm > 300) {
                const float spacing = kRate * 120.0f / (m_grainRpm * float(m_hasInfo && m_info.cylinders ? m_info.cylinders : 4));
                // One combustion to the next is never the same: a little at random, firing to firing. And an idle
                // hunts - its firings drift early and late (up to 6 ms) and swell and fade, a few times a second
                // (the wander curves): a real idle's harmonics are smeared ~3 %, a steady one's sharp. Off the
                // throttle at low revs only. The physics' idle is steady; this is the sound's (until the physics hunts).
                const float idle = (1 - m_grainLoad) * Clamp01((3500 - m_grainRpm) / 1500) * m_mix.idleWander;
                const float spread = 0.06f + kIdleSpread * idle;
                const float delay = kWanderTime * idle * (1 + m_wander[0]), swell = 1 + kWanderLevel * idle * m_wander[1];
                for (uint32_t c = 0; c < 4; ++c)
                    if (passed >> c & 1) {
                        // The anti-lag: some firings' unburnt charge lit in the exhaust - a bang, irregular. At standstill
                        // (neutral) its own set, building up over its first 3 s (a Rally1's: +2 dB to +15 over its engine).
                        const bool standing = after->body.speed < 1 && after->drive.gear == 0;
                        if (after->driver.antiLag && Random() < (standing ? m_mix.antiLagIdleBangs : m_mix.antiLagBangs)) {
                            // Without bang files, RBR's short pops (15-90 ms, as the recorded bangs it was tuned on: 25-45
                            // ms) - not its Bang samples, half-second backfires at full scale: on 40 % of the firings
                            // dozens overlapped into a wall the limiter flattened (the game's anti-lag "loud, aggressive").
                            const std::vector<Sample>& set = standing && !m_bank->antiLagIdleBangs.empty() ? m_bank->antiLagIdleBangs
                                                             : !m_bank->antiLagBangs.empty() ? m_bank->antiLagBangs : m_bank->pops;
                            const float build = standing ? std::min(1.0f, 0.25f + m_antiLagIdle / 4) : 1.0f;
                            Play(set, kAntiLagGain * m_mix.Gain(Mix::kBangs) * build * (0.6f + 0.4f * Random()));
                        }
                        const float vary = (1 + spread * (2 * Random() - 1)) * swell;
                        if (fired >> c & 1) m_grains.Fire(m_grainRpm, m_grainLoad, m_grainOn * vary, m_grainOff * vary, spacing, m_mix.rpmBlend, delay);
                        // A cylinder whose spark was cut (the rev limiter, the shift cut) is near silent - the limiter's
                        // stutter; one that had its spark but did not burn still pumps: off the throttle (the overrun)
                        // an overrun pulse, under load a weak pull's - a gap in it, not the overrun. (As overrun pulses
                        // at any throttle, the physics' unburnt firings under full throttle - 2-6 % at 4000-7200 rpm,
                        // ~20 % on the limiter - put the off loops within 12 dB of the on ones in 13-46 % of a drive's
                        // full-throttle moments: heard as noise in the pull.)
                        else if (!(after->engine.sparks >> c & 1)) m_grains.Fire(m_grainRpm, m_grainLoad, m_grainOn * kCutGain * vary, m_grainOff * kCutGain * vary, spacing, m_mix.rpmBlend, delay);
                        else m_grains.Fire(m_grainRpm, m_grainLoad, m_grainOn * kUnfiredOnGain * vary, m_grainOff * kUnfiredGain * vary, spacing, m_mix.rpmBlend, delay);
                    }
            }
            // The grains at the recording's own level; RBR's engines get Cars.ini's low shelf after, its boost taken
            // back here (1 / sqrt of it: ~3 dB for its x2) so they do not ride the limiter.
            engine = m_grains.Next() * kGrainGain / std::sqrt(m_bank->lowShelfGain);
            for (Loop& l : m_engine) engine += l.Next();
            engine = m_shelf.Run(engine);
        }
        float s = engine + m_turbo.Next() + m_transmission.Next() + m_roll.Next() + m_skid.Next() + m_wind.Next();
        for (Shot& sh : m_shots) {
            if (!sh.sample) continue;
            const std::vector<float>& d = sh.sample->data;
            const size_t k = static_cast<size_t>(sh.position);
            if (k + 1 >= d.size()) {
                sh.sample = nullptr;
                continue;
            }
            const float f = static_cast<float>(sh.position - double(k));
            s += (d[k] + (d[k + 1] - d[k]) * f) * sh.gain;
            sh.position += double(sh.pitch) * sh.sample->rate / kRate;
        }
        s = std::tanh(s * kHeadroom) * gain;   // the soft limiter, its input kept in its gentle range
        out[2 * i] = s;
        out[2 * i + 1] = s;
        m_play = std::min(newest, m_play + step);   // at the newest it holds
    }
}

// The cylinders that fire at this sample (bit c): the crank, between the steps, passing a cylinder's point in the
// cycle (`passed`), and that cylinder having burnt its charge (the step after it says). `after`: that step.
uint32_t Audio::Firings(double tick, const Frame** after, uint32_t* passed)
{
    *passed = 0;
    float u;
    const Frame* fa = At(tick, after, &u);
    const signals::EngineTick &a = fa->engine, &b = (*after)->engine;
    // What the crank turned between the steps: their angles' difference, and the whole cycles a step of many ticks
    // (a recording's settling step) hides in it - as many as the mean speed over the ticks says.
    float turned = fa == *after || b.crankSpeed <= 0 ? 0.0f : Wrap(b.crankAngle - a.crankAngle);
    if (turned > 0 && m_hasInfo && (*after)->tick - fa->tick > 1) {
        const float estimate = 0.5f * (a.crankSpeed + b.crankSpeed) * m_info.dt * float((*after)->tick - fa->tick);
        turned += kCycle * std::max(0.0f, std::round((estimate - turned) / kCycle));
    }
    const float angle = Wrap(a.crankAngle + turned * u);
    uint32_t fired = 0;
    if (m_lastAngle >= 0 && m_hasInfo) {
        const float moved = Wrap(angle - m_lastAngle);
        if (moved > 0 && moved < kPi)
            for (uint32_t c = 0; c < m_info.cylinders && c < 4; ++c) {
                const float at = Wrap(m_info.phaseOffset[c] * (kCycle / 256) - m_lastAngle);
                if (!(at > 0 && at <= moved)) continue;
                *passed |= 1u << c;
                if (b.fired >> c & 1) fired |= 1u << c;
            }
    }
    m_lastAngle = angle;
    return fired;
}

// S0's engine: a pulse into an exhaust model at each firing.
float Audio::PulseSample(uint32_t fired, const Frame& after)
{
    for (uint32_t c = 0; c < 4; ++c)
        if (fired >> c & 1) {
            const float charge = after.engine.chargeMass[c];
            m_peakCharge = std::max(m_peakCharge, charge);
            m_excite = 0.3f + 0.7f * Clamp01(charge / m_peakCharge);
            m_burst = kBurst;
        }
    float x = 0;
    if (m_burst) {
        const float n = 2 * Random() - 1, env = float(m_burst) / kBurst;
        x = m_excite * env * env * (0.6f + 0.4f * n);
        --m_burst;
    }
    const float pipe = m_pipe[0].Run(x) * 3.0f + m_pipe[1].Run(x) * 1.5f + m_tone.Run(x) * 0.6f;
    const float y = pipe + 0.35f * m_comb[m_combAt];
    m_comb[m_combAt] = y;
    m_combAt = (m_combAt + 1) % kCombLength;
    return y;
}

} // namespace audio
