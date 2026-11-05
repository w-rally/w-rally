// audio-check - the sound through WAudio.dll against the sound linked in (audio::Audio), rendered offline from the same
// records: they must be the same samples, bit for bit (docs/design.md 9, phase 3).
//
//   audio-check [--mix <file>] [--wav <out.wav>]
//       made-up records: a 4-cylinder turbo engine revved up and down through the gears, S0's pulse engine (no bank,
//       no data needed)
//   audio-check --game <RBR folder> --stage track-41_M --replay <run.phr> [--ticks n] [--eng Focus03-08] [--wav <out.wav>]
//       the records of a recording replayed through the physics module (WPhysicsRBR.dll), RBR's sounds - the linked
//       sound reads them from the folder, the module through the game's VFS
//   audio-check --game <RBR folder> --rpm-curve <file> [--eng Focus03-08] [--wav <out.wav>]
//       made-up records along a recording's revs: lines of "<seconds> <rpm> <throttle 0..1>", in third gear - our
//       engine beside the recording it was made from, at the same revs
//
// Both sounds hear one signals::Bus: the linked one connected to it as phys-drive connected it, the module through the
// game's own wrally::sound::Sound (every record forwarded). Prints the frames rendered and where they first differ.
// Exit code 0 the same, 1 different, 2 usage or setup.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "audio/bank.h"
#include "audio/mix.h"
#include "audio/wav.h"
#include "data/rec.h"
#include "physics/physics.h"
#include "rules/stage.h"
#include "signals/bus.h"
#include "signals/car.h"
#include "sound/sound.h"

namespace {

std::string Arg(int argc, char** argv, const char* name, const char* def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], name)) return argv[i + 1];
    return def;
}

std::filesystem::path ExeDir()
{
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path();
}

struct CurvePoint {
    float t, rpm, throttle;
};

std::vector<CurvePoint> ReadCurve(const std::string& path)
{
    std::vector<CurvePoint> v;
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        for (CurvePoint p; std::fscanf(f, "%f %f %f", &p.t, &p.rpm, &p.throttle) == 3;) v.push_back(p);
        std::fclose(f);
    }
    return v;
}

// Made-up records: 6000 ticks of 0.007 s - idle, revs up through four gears, a lift with the turbo spinning, back down.
// With a curve: its revs and throttle (linear between its points), third gear, for as long as it lasts.
void SyntheticDrive(signals::Bus& bus, const std::vector<CurvePoint>& curve = {})
{
    constexpr float kDt = 0.007f, kPi = 3.14159265f;
    signals::CarInfo info;
    info.surface = signals::CarInfo::kGravel;
    bus.Emit(info);
    signals::EngineInfo engine;
    engine.cylinders = 4;
    const uint32_t phases[4] = {0, 128, 64, 192};
    std::memcpy(engine.phaseOffset, phases, sizeof phases);
    engine.rpmLimit = 7500;
    engine.idleThrottle = 0.14f;
    engine.turbo = true;
    engine.dt = kDt;
    bus.Emit(engine);

    float angle = 0, rpm = 1000, speed = 0;
    int gear = 0;
    const uint32_t ticks = curve.empty() ? 6000 : static_cast<uint32_t>(curve.back().t / kDt);
    size_t at = 0;
    for (uint32_t tick = 1; tick <= ticks; ++tick) {
        const float t = tick * kDt;
        bool lift = std::fmod(t, 6.0f) > 4.5f;
        float pedal = t < 2 ? 0.0f : lift ? 0.0f : 1.0f;
        if (!curve.empty()) {
            while (at + 2 < curve.size() && curve[at + 1].t <= t) ++at;
            const CurvePoint &a = curve[at], &b = curve[at + 1];
            const float u = std::clamp((t - a.t) / std::fmax(b.t - a.t, 1e-6f), 0.0f, 1.0f);
            pedal = a.throttle + (b.throttle - a.throttle) * u;
            lift = pedal < 0.5f;
            rpm = a.rpm + (b.rpm - a.rpm) * u;
            gear = 3;
        }
        signals::DriverTick driver;
        driver.stamp = {0, tick - 1};
        driver.pedal = driver.throttle = pedal;
        bus.Emit(driver);
        // The revs: up on the throttle, a gear at 7000, down off it.
        if (curve.empty()) rpm += pedal > 0 ? 2600 * kDt : -1800 * kDt;
        if (curve.empty() && rpm > 7000 && gear < 5) {
            rpm = 4800;
            ++gear;
            signals::GearChange g;
            g.stamp = {0, tick};
            g.gear = gear;
            g.level = 0.7f;
            g.engaged = true;
            bus.Emit(g);
        }
        rpm = std::fmax(rpm, 950.0f);
        speed = gear ? rpm / 7000 * 12.0f * gear : 0.0f;
        const float crankSpeed = rpm * kPi / 30;
        angle = std::fmod(angle + crankSpeed * kDt, 4 * kPi);

        signals::EngineTick e;
        e.stamp = {0, tick};
        e.crankSpeed = crankSpeed;
        e.crankAngle = angle;
        e.throttle = pedal;
        e.easedThrottle = std::fmax(pedal, engine.idleThrottle);
        e.torque = 200 * e.easedThrottle;
        e.power = e.torque * crankSpeed;
        e.fired = pedal > 0 || rpm < 1200 ? 0xfu : 0x5u;
        e.sparks = 0xf;
        for (float& m : e.chargeMass) m = 0.0004f * e.easedThrottle;
        e.boost = 1 + 1.4f * pedal * std::fmin(1.0f, rpm / 4000);
        e.turboSpeed = 9000 * (e.boost - 1) + 200;
        e.intakePressure = 101325 * e.boost;
        e.exhaustPressure = 101325 * (1 + e.boost) * 0.5f;
        e.blockTemperature = 360;
        bus.Emit(e);
        signals::DriveTick d;
        d.stamp = e.stamp;
        d.gear = gear;
        d.outputSpeed = crankSpeed;
        d.engineTorque = e.torque;
        bus.Emit(d);
        signals::WheelsTick w;
        w.stamp = e.stamp;
        w.radius = 0.32f;
        for (auto& wh : w.wheels) {
            wh.onGround = true;
            wh.spinRate = speed / w.radius;
            wh.rollingSpeed = speed;
            wh.slipLateral = lift ? 3.0f : 0.2f;
            wh.slipLongitudinal = pedal * 1.5f;
            wh.surfaceA = wh.surfaceB = 2;
        }
        bus.Emit(w);
        signals::BodyTick b;
        b.stamp = e.stamp;
        b.velocity[0] = b.speed = speed;
        b.rotation[3] = 1;
        bus.Emit(b);
        bus.Emit(signals::StepEnd{e.stamp, 1});
    }
}

// The records of a recording replayed through the physics module: its controls, its steps.
std::string ReplayDrive(signals::Bus& bus, int argc, char** argv, const std::string& game)
{
    const std::string stage = Arg(argc, argv, "--stage", "track-41_M"), replayPath = Arg(argc, argv, "--replay", "");
    if (replayPath.empty()) return "--replay needed with --game";
    const uint32_t ticks = static_cast<uint32_t>(std::strtoul(Arg(argc, argv, "--ticks", "20000").c_str(), nullptr, 10));
    phys::data::Recording replay;
    try {
        replay = phys::data::ReadRecording(replayPath);
    } catch (const std::exception& e) {
        return e.what();
    }
    wrally::physics::Config c;
    c.root = game;
    const std::string maps = game + "/Maps/" + stage;
    c.trk = maps + ".trk", c.col = maps + ".col", c.mat = maps + ".mat", c.fnc = maps + ".fnc", c.lbs = maps + ".lbs";
    c.sgc = game + "/Cars/i20_R5/i20_R5.sgc";
    c.surface = wrally::rules::StageSurface(game, wrally::rules::StageMap(stage));
    c.tyreIndex = wrally::rules::SurfaceTyreType(c.surface, c.wetness >= 0 ? c.wetness : static_cast<int32_t>(c.weather / 3));
    if (replay.settings & 1) c.settings = replay.settings;
    wrally::physics::Physics sim;
    if (std::string e = sim.Open(ExeDir() / "modules" / "physics", "rbr", c); !e.empty()) return "physics: " + e;
    sim.Connect(&bus);
    for (size_t at = 0; at < replay.events.size() && sim.Tick() < ticks; ++at) {
        const phys::data::RecEvent& ev = replay.events[at];
        if (ev.type == 'C') sim.ApplyWords(ev.words);
        else if (ev.type == 'S' && !sim.Step(ev.steps)) return "physics: " + sim.Error();
    }
    std::printf("replayed %s to tick %u\n", replayPath.c_str(), sim.Tick());
    return "";
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string game = Arg(argc, argv, "--game", ""), wav = Arg(argc, argv, "--wav", "");
    const std::string engine = game.empty() ? "none" : Arg(argc, argv, "--eng", "Focus03-08");
    const std::string curvePath = Arg(argc, argv, "--rpm-curve", "");
    signals::Bus bus;   // first: both sounds disconnect from it as they go
    // --mix <file>: one mix for both - the linked sound reads it, the module gets its text (setCarMix).
    const std::string mixPath = Arg(argc, argv, "--mix", "");
    std::string mixText;
    if (!mixPath.empty()) {
        FILE* f = std::fopen(mixPath.c_str(), "rb");
        if (!f) return std::fprintf(stderr, "cannot open %s\n", mixPath.c_str()), 2;
        char b[4096];
        for (size_t n; (n = std::fread(b, 1, sizeof b, f)) > 0;) mixText.append(b, n);
        std::fclose(f);
    }

    // The linked sound, as phys-drive set it up.
    std::vector<float> linked;
    audio::Bank bank;
    audio::Audio sound(audio::Audio::Mode::Offline);
    sound.sink = [&](const float* s, uint32_t frames) { linked.insert(linked.end(), s, s + 2 * size_t(frames)); };
    if (engine != "none") {
        std::string missing = bank.LoadRbr(game, engine);
        std::printf("linked: %zu engine loops%s%s", bank.engine.size(), missing.empty() ? "\n" : ", missing:\n", missing.c_str());
        sound.SetBank(&bank);
    }
    if (!mixPath.empty()) {
        audio::Mix mix;
        const std::string e = mix.Read(mixPath);
        sound.SetMix(mix);
        std::printf("linked: %smix:%s\n", e.c_str(), mix.Describe().c_str());
    }

    // The module, through the game's wrapper.
    wrally::sound::Sound module;
    if (std::string e = module.Open(ExeDir() / "modules" / "audio", "waudio", game, true); !e.empty())
        return std::fprintf(stderr, "%s\n", e.c_str()), 2;
    wrally::sound::CarSound car;
    car.engine = engine;
    if (std::string e = module.CreateCar(car); !e.empty()) return std::fprintf(stderr, "%s\n", e.c_str()), 2;
    if (!mixPath.empty())
        if (std::string e = module.SetMix(mixText); !e.empty()) std::printf("module: the mix: %s\n", e.c_str());
    std::printf("module %s: %s", module.ModuleName().c_str(), module.TakeLog().c_str());

    sound.Connect(bus);
    module.Connect(&bus);
    // --engine-log <file>: per tick the driver's pedal and throttle, the engine's throttle, its revs, which cylinders
    // fired and had a spark, the gear (a CSV) - what the sound's engine hears.
    FILE* engineLog = nullptr;
    signals::DriverTick lastDriver{};
    int32_t lastGear = 0;
    if (const std::string path = Arg(argc, argv, "--engine-log", ""); !path.empty() && (engineLog = std::fopen(path.c_str(), "w"))) {
        std::fprintf(engineLog, "tick,pedal,driverThrottle,engineThrottle,easedThrottle,rpm,fired,sparks,gear,antiLag,launch\n");
        bus.Connect<signals::DriverTick>([&](const signals::DriverTick& d) { lastDriver = d; });
        bus.Connect<signals::DriveTick>([&](const signals::DriveTick& d) { lastGear = d.gear; });
        bus.Connect<signals::EngineTick>([&](const signals::EngineTick& e) {
            std::fprintf(engineLog, "%u,%.3f,%.3f,%.3f,%.3f,%.0f,%x,%x,%d,%u,%u\n", e.stamp.tick, lastDriver.pedal, lastDriver.throttle,
                         e.throttle, e.easedThrottle, e.crankSpeed * 30 / 3.14159265f, e.fired, e.sparks, lastGear, lastDriver.antiLag,
                         lastDriver.launchControl);
        });
    }
    if (game.empty()) {
        SyntheticDrive(bus);
    } else if (!curvePath.empty()) {
        const std::vector<CurvePoint> curve = ReadCurve(curvePath);
        if (curve.size() < 2) return std::fprintf(stderr, "%s: no curve\n", curvePath.c_str()), 2;
        SyntheticDrive(bus, curve);
    } else if (std::string e = ReplayDrive(bus, argc, argv, game); !e.empty()) {
        return std::fprintf(stderr, "%s\n", e.c_str()), 2;
    }

    std::vector<float> fromModule(linked.size() + 2 * 48000);
    const uint32_t frames = module.RenderOffline(fromModule.data(), static_cast<uint32_t>(fromModule.size() / 2));
    fromModule.resize(2 * size_t(frames));
    float more[2];
    const bool leftOver = module.RenderOffline(more, 1) != 0;

    if (!wav.empty()) {
        audio::WavWriter w;
        if (w.Open(wav, audio::kRate)) w.Write(fromModule.data(), frames), w.Close();
    }
    double peak = 0;
    for (float s : linked) peak = std::fmax(peak, std::fabs(s));
    std::printf("linked %zu frames (peak %.3f), module %u frames\n", linked.size() / 2, peak, frames);
    if (linked.empty() || peak == 0) return std::printf("nothing was rendered - no check\n"), 1;
    if (fromModule.size() != linked.size() || leftOver) return std::printf("DIFFERENT: frame counts differ\n"), 1;
    for (size_t i = 0; i < linked.size(); ++i)
        if (std::memcmp(&linked[i], &fromModule[i], sizeof(float))) {
            std::printf("DIFFERENT: from frame %zu (%g vs %g)\n", i / 2, double(linked[i]), double(fromModule[i]));
            return 1;
        }
    std::printf("the same, bit for bit\n");
    return 0;
}
