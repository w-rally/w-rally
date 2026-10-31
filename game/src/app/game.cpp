#include "app/game.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "app/settings.h"
#include "app/window.h"
#include "assets/car_lights.h"
#include "assets/lamp_lab.h"
#include "assets/car_render.h"
#include "assets/fences.h"
#include "assets/catalog.h"
#include "assets/legacy_stage.h"
#include "camera/camera.h"
#include "camera/replay_camera.h"
#include "core/math.h"
#include "data/lbs.h"
#include "data/sgc.h"
#include "input/bindings.h"
#include "input/keyboard.h"
#include "render/renderer.h"
#include "render/stage_objects.h"
#include "session/session.h"
#include "session/sim_thread.h"
#include "signals/bus.h"
#include "sound/codriver.h"
#include "sound/sound.h"
#include "ui/hud.h"
#include "ui/imgui_layer.h"
#include "ui/menus.h"

namespace wrally::app {

using math::V3;
using input::Action;
using input::Press;

namespace {

// The frame between two physics steps at t: the car's pose and its wheels (the spin the shorter way round).
void Between(const session::Shown& a, const session::Shown& b, float t, physics::Pose& p, physics::Wheel wheels[4],
             std::vector<physics::Part>& parts)
{
    // The parts as at the later step, blended where both steps had them in the same frame (a part just torn off is
    // where it is now).
    parts = b.parts;
    for (size_t i = 0; i < parts.size() && i < a.parts.size(); ++i) {
        if (a.parts[i].inCar != b.parts[i].inCar) continue;
        math::Lerp(a.parts[i].position, b.parts[i].position, t, 3, parts[i].position);
        math::Nlerp(a.parts[i].rotation, b.parts[i].rotation, t, parts[i].rotation);
    }
    math::Lerp(a.pose.position, b.pose.position, t, 3, p.position);
    math::Nlerp(a.pose.rotation, b.pose.rotation, t, p.rotation);
    math::Lerp(a.pose.velocity, b.pose.velocity, t, 3, p.velocity);
    for (int k = 0; k < 4; ++k) {
        const physics::Wheel &wa = a.wheels[k], &wb = b.wheels[k];
        math::Lerp(wa.position, wb.position, t, 3, wheels[k].position);
        math::Nlerp(wa.rotation, wb.rotation, t, wheels[k].rotation);
        const float d = std::remainder(wb.spin - wa.spin, 6.2831853f);   // the spin is wrapped
        wheels[k].spin = wa.spin + d * t;
    }
}

// What lives from the start to the end: the settings, the window, the renderer on it and ImGui - the stages and the
// menus come and go on them.
struct Shell {
    Settings settings;
    Window window;
    render::Renderer renderer;
    ui::ImGuiLayer ui;
    int width = 0, height = 0;   // the size the renderer has
    bool rendererLog = false;

    // The renderer follows the window's size.
    void FollowSize()
    {
        if (window.Width() == width && window.Height() == height) return;
        renderer.Resize(static_cast<uint32_t>(window.Width()), static_cast<uint32_t>(window.Height()));
        width = window.Width();
        height = window.Height();
    }
};

iface::RenderSettings ToRender(const Settings& s)
{
    iface::RenderSettings r{};
    r.mode = iface::WindowMode::Windowed;
    r.vsync = s.vsync;
    r.quality = std::min(s.quality, 2u);
    r.rayTracing = s.rayTracing;
    r.taa = s.taa;
    r.shadows = s.shadows;
    r.autoExposure = 1;
    r.renderScale = 1;
    return r;
}

// The window, the renderer (module `rendererId`) on it and ImGui. Returns "" or what failed.
std::string OpenShell(Shell& sh, const std::string& rendererId)
{
    if (std::string e = sh.window.Create(L"WRally"); !e.empty()) return e;
    // The renderer: a module DLL in modules/renderer (--renderer, "wrenderer" by default: WRenderer.dll, NGG), on the
    // window. PHYS_DRIVE_NGGLOG: its log on stdout.
    sh.rendererLog = std::getenv("PHYS_DRIVE_NGGLOG") != nullptr;
    sh.width = sh.window.Width();
    sh.height = sh.window.Height();
    const std::string e = sh.renderer.Open(ExeDir() / "modules" / "renderer", rendererId, sh.window.Handle(),
                                           static_cast<uint32_t>(sh.width), static_cast<uint32_t>(sh.height), ToRender(sh.settings));
    if (sh.rendererLog) std::printf("%s", sh.renderer.TakeLog().c_str());
    if (!e.empty()) return "renderer: " + e;
    std::printf("renderer module: %s\n", sh.renderer.ModuleName().c_str());
    if (const std::string u = sh.ui.Init(sh.window.Handle(), sh.renderer); !u.empty()) std::fprintf(stderr, "the HUD: %s\n", u.c_str());
    return "";
}

// One frame of the menus without a stage: the window's messages, `draw`'s ImGui on the backdrop. False once the window
// was closed.
template <class F> bool MenuFrame(Shell& sh, F&& draw)
{
    if (!sh.window.Pump()) return false;
    sh.FollowSize();
    sh.ui.NewFrame();
    ui::DrawBackdrop(sh.width, sh.height);
    draw();
    iface::FrameDesc fd{};
    fd.flags = iface::kFrameNoWorld;
    fd.ui = sh.ui.Render();
    if (!sh.renderer.RenderFrame(fd)) std::fprintf(stderr, "renderer: %s\n", sh.renderer.Error().c_str());
    sh.renderer.Present();
    if (const std::string log = sh.renderer.TakeLog(); sh.rendererLog && !log.empty()) std::printf("%s", log.c_str());
    if (!sh.settings.vsync) Sleep(5);   // (nothing to hurry for)
    return true;
}

enum class StageEnd { Quit, ExitToMenu, Restart, Failed };

// The in-stage menus, Esc's.
enum class StageMenu { None, Pause, Settings, Debug };

// A stage run, with the in-stage menu on Esc. `fromMenu`: started from Quick rally (a fault goes back to the menu, not
// out). `error`: why it failed.
StageEnd RunStage(Shell& sh, const Options& o, bool fromMenu, std::string& error)
{
    auto fail = [&](const std::string& e) {
        std::fprintf(stderr, "%s\n", e.c_str());
        error = e;
        return StageEnd::Failed;
    };
    Window& window = sh.window;
    render::Renderer& renderer = sh.renderer;
    ui::ImGuiLayer& ui = sh.ui;
    const input::Bindings& keys = sh.settings.keys;   // (Settings -> Controls: changes apply at once)
    // The modules hear the physics on the bus (before what it connects, which goes first).
    signals::Bus bus;
    // The physics, through the session (a module DLL in modules/physics: --physics, "rbr" by default).
    session::Session sim;
    if (const std::string e = sim.Open(o.session); !e.empty()) return fail(e);
    // The sound plays from its device's thread, a few ticks behind the physics.
    std::string mixPath;   // the mix file, watched ("" none)
    FILETIME mixWritten{};
    sound::MixEdit mixEdit;   // its values, as the Debug menu and the mix panel set them
    std::string mixStatus;
    bool mixPanel = false;    // the mix panel up while driving
    // The sound: a module DLL in modules/audio (--audio, "waudio" by default: WAudio.dll), every record on the bus
    // forwarded to it.
    sound::Sound sound;
    bool soundOn = false;   // the module opened, the car's voice made
    if (o.sound) {
        sound::CarSound car;
        car.engine = o.engine;
        car.bangs = o.bangs;
        car.idleBangs = o.idleBangs;
        std::string e = sound.Open(ExeDir() / "modules" / "audio", o.audioId, o.game);
        if (e.empty()) e = sound.CreateCar(car);
        std::printf("%s", sound.TakeLog().c_str());
        if (!e.empty()) {
            std::printf("no sound: %s\n", e.c_str());
        } else {
            std::printf("audio module: %s\n", sound.ModuleName().c_str());
            sound.Connect(&bus);
            mixPath = o.mix;
            soundOn = true;
        }
    }
    // The co-driver: the stage's pacenotes read out (sound/codriver.h), with RBR's voice - loaded on its own thread while
    // the stage loads here; it starts calling once ready (its report then, in the frame loop). The live game's: there it
    // always runs, for the HUD's pacenotes, with the voice when the sound and the co-driver are on; a replay has it only
    // with --codriver-replay (the voice, no HUD).
    sound::CoDriver codriver;
    const bool live = !sim.Replaying(), voice = soundOn && o.codriver;
    if (live || (voice && o.codriverReplay)) {
        try {
            codriver.LoadAsync(voice ? &sound : nullptr, o.game, phys::data::ReadDls(o.session.dls).pacenotes);
            if (voice) sound.SetVolume(sound::Sound::Bus::CoDriver, o.codriverVolume);
        } catch (const std::exception& ex) {
            std::printf("no co-driver: %s\n", ex.what());
        }
    }
    bool soundPaused = false;
    std::string lastLogged;   // (WRALLY_CODRIVER_LOG's)
    sim.Connect(&bus);
    const std::vector<phys::data::RecMesh> collision =
        phys::data::CarCollisionMeshes(phys::data::ReadSgcCollision(o.session.physics.sgc));

    std::vector<char> bytes;
    // An RBR .lbs instead of an .ngg: imported here (assets/legacy_stage.h), its textures for the road's wetness. Its
    // cards are lit by the renderer's legacy profile (B unless NGG_LEGACY_PROFILE says; the Debug menu switches it).
    const bool imported = !_stricmp(std::filesystem::path(o.ngg).extension().string().c_str(), ".lbs");
    int legacyProfile = !imported ? -1 : std::getenv("NGG_LEGACY_PROFILE") ? std::atoi(std::getenv("NGG_LEGACY_PROFILE")) : 2;
    // The road's wetness, as the physics has it: 0 dry, 1 damp, 2 wet. It picks an import's textures and how wet the
    // renderer draws every surface (FrameDesc::wetness: 0, 0.5, 1 - puddles from damp on).
    const int wet = o.session.physics.wetness >= 0 ? o.session.physics.wetness : static_cast<int>(o.session.physics.weather / 3);
    if (imported) {
        assets::LegacyImport imp;
        if (const std::string e = assets::ImportLegacyStage(o.ngg, wet, imp); !e.empty()) return fail(e);
        std::printf("%s", imp.log.c_str());
        bytes.assign(imp.ngg.begin(), imp.ngg.end());
    } else if (FILE* f = std::fopen(o.ngg.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        bytes.resize(static_cast<size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        bytes.resize(std::fread(bytes.data(), 1, bytes.size(), f));
        std::fclose(f);
    }
    if (bytes.empty()) return fail("cannot read " + o.ngg);

    const bool rendererLog = sh.rendererLog;
    iface::RenderSettings settings = ToRender(sh.settings);
    renderer.ApplySettings(settings);
    // NGG_DEBUG_VIEW=<n>: the stage starts in that debug view (V cycles them); otherwise the normal one.
    const int startView = std::getenv("NGG_DEBUG_VIEW") ? std::atoi(std::getenv("NGG_DEBUG_VIEW")) : 0;
    renderer.SetDebugView(startView);
    if (imported) renderer.SetLegacyProfile(legacyProfile);   // (the renderer keeps it from stage to stage)
    const iface::SceneHandle scene = renderer.LoadScene(bytes.data(), bytes.size());
    if (rendererLog) std::printf("%s", renderer.TakeLog().c_str());
    if (!scene) return fail("renderer: " + renderer.Error());
    bytes.clear();
    // The scene goes when the stage does (and the car's model with it: it lives in the scene).
    struct Unload {
        render::Renderer& r;
        iface::SceneHandle s;
        ~Unload() { r.Api()->unloadScene(r.Instance(), s); }
    } unload{renderer, scene};
    // --weather: one of the .ngg's weather presets by name (else its default, 0).
    int32_t weatherPreset = 0;
    if (!o.weather.empty()) {
        uint32_t index = 0;
        if (renderer.Api()->findWeatherPreset(renderer.Instance(), scene, o.weather.c_str(), &index) == iface::Status::Ok) {
            weatherPreset = static_cast<int32_t>(index);
            std::printf("weather %s (preset %u)\n", o.weather.c_str(), index);
        } else {
            std::printf("weather %s: not among the .ngg's presets - its default\n", o.weather.c_str());
        }
    }
    {
        uint32_t keyed = 0, solids = 0;
        renderer.Api()->sceneKeyStats(renderer.Instance(), scene, &keyed, &solids);
        if (keyed)   // the stage's objects, before ours
            std::printf("movable objects: %u drawn objects keyed to %u solids\n", keyed, solids);
        else
            std::printf("movable objects: the .ngg has no object keys (NGGK) - reconvert it; nothing will move\n");
    }
    render::StageObjects stageObjects;
    assets::CarRender car;
    std::vector<physics::Part> parts;   // the car's parts that can come loose, as the physics loaded them
    sim.Physics().Parts(parts);
    car.Create(renderer, o.session.physics.sgc, o.session.physics.surface, collision, parts);
    // The stage's fences (its .fnc), knocked about by the car's hull (assets/fences.h).
    assets::Fences fences;
    if (const std::string e = fences.Create(renderer, o.session.physics.fnc, o.game); !e.empty())
        std::printf("fences: %s\n", e.c_str());
    else if (fences.Posts())
        std::printf("fences: %zu posts, %zu tapes\n", fences.Posts(), fences.Tapes());
    if (!collision.empty()) fences.SetCar(collision[0]);
    std::vector<iface::InstanceUpdate> instances;   // the frame's: the car's, the fences' that moved

    if (const std::string e = sim.Start(); !e.empty()) return fail(e);

    // The renderer's switches: T its TAA, O its ambient occlusion (quality medium / low), L its shadows, K its ray
    // tracing (off: the cascaded shadow maps, no RT AO), V its debug views, F1 its readout. P a pause (the physics, a
    // replay and the rendering stop - the picture stays as it was; a switch draws one frame), F2 a screenshot
    // (shot-NNN.bmp beside the recording).
    Press taaKey, pauseKey, aoKey, shadowKey, shotKey, viewKey, rtKey, debugHudKey, muteKey, antiLagIdleKey;
    Press gearUpKey, gearDownKey, cameraKey, resetKey;
    int debugView = startView;
    uint32_t pauseAt = o.pauseAt;
    bool taa = settings.taa != 0, paused = false, ao = settings.quality != 0, shadows = settings.shadows != 0, rt = settings.rayTracing != 0;
    bool antiLagIdle = false;   // N: the anti-lag armed at standstill (the driver's switch before a start)
    bool debugHud = std::getenv("PHYS_DRIVE_READOUT") != nullptr;   // PHYS_DRIVE_READOUT: the readout on from the start
    double fpsSmooth = 0.0;     // wall-clock frames per second, eased, for the readout
    LARGE_INTEGER freq, last, now, fpsLast{};
    QueryPerformanceFrequency(&freq);
    int shots = 0;
    // PHYS_DRIVE_OFF=taa,ao,shadows,rt: any of them off from the start, as the keys would; vsync, the GPU's frame time
    // measured at full clock (with the frame capped by the display, an integrated GPU clocks down into the slack).
    if (const char* off = std::getenv("PHYS_DRIVE_OFF")) {
        if (std::strstr(off, "vsync")) settings.vsync = 0;
        if (std::strstr(off, "taa")) settings.taa = 0, taa = false;
        if (std::strstr(off, "ao")) settings.quality = 0, ao = false;
        if (std::strstr(off, "shadows")) settings.shadows = 0, shadows = false;
        if (std::strstr(off, "rt")) settings.rayTracing = 0, rt = false;
        renderer.ApplySettings(settings);
    }
    // A replay's TV cameras (camera 4, a replay's first): the stage's .dls cameras by the car's distance along its drive
    // line; the chase camera where none covers it.
    std::unique_ptr<camera::ReplayCameras> tv;
    if (sim.Replaying()) {
        try {
            tv = std::make_unique<camera::ReplayCameras>(phys::data::ReadDls(o.session.dls),
                                                         phys::data::ReadLbsScripts(o.session.physics.lbs).driveLine);
            if (tv->Empty()) tv.reset();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "the replay cameras: %s\n", e.what());
        }
    }
    const int cameras = tv ? 5 : 4;
    int cameraMode = (o.camera >= 0 ? o.camera : tv ? camera::kTv : camera::kChase) % cameras;
    float orbit = o.cameraAngle;   // the beside camera's place round the car, radians from its left side toward its front
    // PHYS_DRIVE_LOOK_UP=<degrees>: every view tilted up by that much - the sky's moon and stars to look at.
    const float lookUp = std::getenv("PHYS_DRIVE_LOOK_UP") ? static_cast<float>(std::atof(std::getenv("PHYS_DRIVE_LOOK_UP"))) : 0.0f;
    // PHYS_DRIVE_TEST_LIGHTS: test lamps on the car until it carries its own (assets::TestLayout, design 5.4 N3).
    // =ramp (or any other value) two main beams and a 4-pod ramp, =bars two main beams and two LED bars, =low the
    // dipped beams; =lamp a lamp above and behind the car alone (its shadow of the car); =noshadow the ramp layout never
    // occluded (no light rays - the shading's cost alone).
    // (An EMPTY value is off: a shell can leave a variable set to "" when it is cleared.)
    const char* const testLightsEnv = std::getenv("PHYS_DRIVE_TEST_LIGHTS");
    const bool testLights_ = testLightsEnv && *testLightsEnv;
    const bool testLampOnly = testLights_ && !std::strcmp(testLightsEnv, "lamp");
    const bool testNoShadow = testLights_ && !std::strcmp(testLightsEnv, "noshadow");
    std::vector<assets::Lamp> testLamps;
    if (testLights_ && !testLampOnly) {
        testLamps = assets::TestLayout(testLightsEnv);
        if (testLamps.empty()) testLamps = assets::TestLayout("ramp");
    }
    if (testLights_)
        std::printf("test lights: %s\n", testLampOnly ? "the lamp behind" : (std::to_string(testLamps.size()) + " lamps").c_str());
    float chaseDistance = o.session.chaseDistance;
    bool chaseChanged = false;     // reported once the key is let go
    const uint32_t jitterTo = std::getenv("PHYS_DRIVE_JITTER") ? std::strtoul(std::getenv("PHYS_DRIVE_JITTER"), nullptr, 0) : 0;
    const uint32_t shotEvery = std::getenv("PHYS_DRIVE_SHOTS") ? std::strtoul(std::getenv("PHYS_DRIVE_SHOTS"), nullptr, 0) : 0;
    uint32_t shotTick = 0;
    double mixCheck = 0, title = 0;
    // The session on its own thread from here on (session/sim_thread.h): in lockstep with the frames under --step, so
    // those runs stay the same frame for frame.
    session::SimThread simThread;
    simThread.Start(sim, o.fixedStep > 0.0);
    session::Snapshot snap;
    simThread.Latest(snap);

    // Esc's menu: the stage held while it is up, the game's keys not read.
    StageMenu menu = StageMenu::None;
    ui::DebugSwitches dbg;
    std::vector<std::string> views;
    for (int32_t v = 0; const char* n = renderer.Api()->debugViewName(v); ++v) views.emplace_back(n);
    std::string settingsLog;

    QueryPerformanceCounter(&last);
    bool quit = false;
    StageEnd end = StageEnd::Quit;
    // A quit (the close box, the menu, --exit-at, a fault) still finishes its frame - its steps are in the recording.
    while (!quit) {
        if (!window.Pump()) quit = true, end = StageEnd::Quit;
        // Esc: the menu up, down again, or a step back from its Settings / Debug (the settings kept as they are).
        const UINT keyDown = window.TakeKeyDown();   // (for Settings -> Controls)
        if (window.TakeEscape() && !ui::CapturingKey()) {
            if (menu == StageMenu::Settings) SaveSettings(sh.settings);
            menu = menu == StageMenu::None ? StageMenu::Pause : menu == StageMenu::Pause ? StageMenu::None : StageMenu::Pause;
        }
        QueryPerformanceCounter(&now);
        // --step: every frame advances the physics by exactly that, whatever the frame took.
        const double frame = o.fixedStep > 0.0 ? o.fixedStep : std::min(0.1, double(now.QuadPart - last.QuadPart) / double(freq.QuadPart));
        last = now;
        const bool focused = window.Focused() && menu == StageMenu::None;
        session::FrameInput in;
        if (gearUpKey(focused && keys.Down(Action::GearUp))) in.presses |= session::kGearUp;
        if (gearDownKey(focused && keys.Down(Action::GearDown))) in.presses |= session::kGearDown;
        if (cameraKey(focused && keys.Down(Action::Camera))) cameraMode = (cameraMode + 1) % cameras;
        bool redraw = false;   // a renderer switch changed: draw one frame even while paused
        auto toggle = [&](bool& on, uint32_t& field, uint32_t onValue, const char* name, const char* offText) {
            on = !on;
            field = on ? onValue : 0;
            renderer.ApplySettings(settings);
            redraw = true;
            std::printf("%s %s\n", name, on ? "on" : offText);
        };
        if (taaKey(focused && keys.Down(Action::Taa))) toggle(taa, settings.taa, 1, "TAA", "off");
        if (aoKey(focused && keys.Down(Action::Ao))) toggle(ao, settings.quality, 1, "ambient occlusion", "off");
        if (shadowKey(focused && keys.Down(Action::Shadows))) toggle(shadows, settings.shadows, 1, "shadows", "off");
        if (rtKey(focused && keys.Down(Action::RayTracing))) toggle(rt, settings.rayTracing, 1, "ray tracing", "off (cascaded shadow maps)");
        // The mix file saved: read again (checked about twice a second).
        if (!mixPath.empty() && (mixCheck += frame) > 0.5) {
            mixCheck = 0;
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExA(mixPath.c_str(), GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &mixWritten)) {
                mixWritten = a.ftLastWriteTime;
                std::ifstream f(mixPath, std::ios::binary);
                mixEdit.Load(std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()));
                const std::string e = sound.SetMix(mixEdit.Text());
                std::printf("mix %s\n%s%s", mixPath.c_str(), sound.TakeLog().c_str(), e.empty() ? "" : (e + "\n").c_str());
            }
        }
        if (antiLagIdleKey(focused && keys.Down(Action::AntiLagIdle))) {
            antiLagIdle = !antiLagIdle;
            std::printf("anti-lag at standstill %s\n", antiLagIdle ? "armed" : "off");
        }
        if (muteKey(focused && keys.Down(Action::Mute))) {
            sound.SetMuted(!sound.Muted());
            std::printf("sound %s\n", sound.Muted() ? "muted" : "on");
        }
        if (debugHudKey(focused && keys.Down(Action::Readout))) {
            debugHud = !debugHud;
            redraw = true;
        }
        if (pauseKey(focused && keys.Down(Action::Pause))) {
            paused = !paused;
            std::printf("%s at tick %u\n", paused ? "paused" : "running", snap.tick);
        }
        if (pauseAt && snap.tick >= pauseAt) {
            pauseAt = 0;
            paused = true;
            std::printf("paused at tick %u (--pause-at)\n", snap.tick);
        }
        if (o.exitAt && snap.tick >= o.exitAt) {
            std::printf("exit at tick %u (--exit-at)\n", snap.tick);
            quit = true, end = StageEnd::Quit;
        }
        if (viewKey(focused && keys.Down(Action::DebugView))) {
            if (!renderer.Api()->debugViewName(++debugView)) debugView = 0;
            renderer.SetDebugView(debugView);
            redraw = true;
            std::printf("view %d: %s\n", debugView, renderer.DebugViewName(debugView));
        }
        // , / .: the beside camera round the car at 90 degrees / s; while paused each frame it turns is drawn.
        const int turn = (focused && keys.Down(Action::OrbitRight) ? 1 : 0) - (focused && keys.Down(Action::OrbitLeft) ? 1 : 0);
        if (turn && cameraMode == camera::kBeside) {
            orbit = std::remainder(orbit + turn * static_cast<float>(frame) * 1.5707963f, 6.2831853f);
            redraw = true;
        }
        // [ / ]: the chase camera in / out at 2 m / s - shown at once, paused too (the session moves its eyes).
        const int zoom = (focused && keys.Down(Action::ChaseOut) ? 1 : 0) - (focused && keys.Down(Action::ChaseIn) ? 1 : 0);
        if (zoom && (cameraMode == camera::kChase || cameraMode == camera::kTv)) {
            chaseDistance = std::max(2.0f, std::min(20.0f, chaseDistance + zoom * static_cast<float>(frame) * 2.0f));
            redraw = true;
            chaseChanged = true;
        } else if (chaseChanged) {
            chaseChanged = false;
            std::printf("chase camera %.1f m behind the car\n", chaseDistance);
        }
        // The keys for the session: R back on the road, the pedals, the steering's target.
        if (resetKey(focused && keys.Down(Action::BackOnRoad))) in.presses |= session::kBackOnRoad;
        in.seconds = paused || menu != StageMenu::None ? 0.0 : frame;   // a pause keeps the frame where it is, between the same steps
        in.throttle = focused && keys.Down(Action::Throttle);
        in.brake = focused && keys.Down(Action::Brake);
        in.handbrake = focused && keys.Down(Action::Handbrake);
        in.steer = focused ? (keys.Down(Action::SteerLeft) ? -1.0f : 0.0f) + (keys.Down(Action::SteerRight) ? 1.0f : 0.0f) : 0.0f;
        in.antiLagIdle = antiLagIdle;
        in.chaseDistance = chaseDistance;
        simThread.Post(in);
        simThread.Latest(snap);
        // The co-driver's calls ahead of the car; held with the stage (a pause, the menu), the world's sound with them.
        {
            if (const std::string r = codriver.TakeReport(); !r.empty()) {
                std::printf("%s\n", r.c_str());
                // WRALLY_CODRIVER_LIST=<n>: the first n calls in the log.
                if (const char* n = std::getenv("WRALLY_CODRIVER_LIST")) std::printf("%s", codriver.Describe(std::strtoul(n, nullptr, 0)).c_str());
            }
            const bool hold = paused || menu != StageMenu::None;
            if (soundOn && hold != soundPaused) sound.SetPaused(soundPaused = hold);
            if (!hold) codriver.Update(snap.distance, std::fabs(snap.dash.speed), o.codriverLead,
                                       snap.clock.GetState() == rules::StageClock::State::Running);   // (from GO to the finish)
            // WRALLY_CODRIVER_LOG: each call in the log, with its tick and the car's place.
            static const bool logCalls = std::getenv("WRALLY_CODRIVER_LOG") != nullptr;
            if (logCalls && codriver.LastCall() != lastLogged) {
                lastLogged = codriver.LastCall();
                std::printf("call at tick %u, %.0f m: %s\n", snap.tick, snap.distance, lastLogged.c_str());
            }
        }
        if (snap.failed) quit = true, end = fromMenu ? StageEnd::ExitToMenu : StageEnd::Failed, error = "the physics failed";
        // A replay's end ends the run (its check reported, as at a quit), unless --hold keeps its last frame up.
        if (snap.replayEnded && !o.hold) quit = true, end = fromMenu ? StageEnd::ExitToMenu : StageEnd::Quit;

        // The frame between the last two steps.
        const float t = snap.t;
        std::vector<physics::Body>& bodies = snap.after.bodies;   // (the later end, blended in place)
        for (size_t i = 0; i < bodies.size() && i < snap.before.bodies.size(); ++i) {
            math::Lerp(snap.before.bodies[i].position, bodies[i].position, t, 3, bodies[i].position);
            math::Nlerp(snap.before.bodies[i].rotation, bodies[i].rotation, t, bodies[i].rotation);
        }
        const std::vector<iface::StageObjectUpdate>& moved = stageObjects.Update(bodies);
        physics::Pose p;
        physics::Wheel wheels[4];
        Between(snap.before, snap.after, t, p, wheels, parts);
        const iface::InstanceUpdate& carUpdate = car.Place(p, wheels, snap.centerOfGravity, parts);
        instances.clear();
        if (car.Ready()) instances.push_back(carUpdate);
        for (const iface::InstanceUpdate& u : fences.Step(in.seconds, p, snap.centerOfGravity)) instances.push_back(u);
        const V3 chaseEye = snap.before.chase.eye + (snap.after.chase.eye - snap.before.chase.eye) * t;
        const camera::View view = camera::Look(cameraMode, p, chaseEye, orbit, tv.get());
        sh.FollowSize();
        const int shownWidth = sh.width, shownHeight = sh.height;
        float viewM[16], proj[16];
        V3 eyeR = math::ToRender(view.eye), atR = math::ToRender(view.at);
        if (lookUp != 0.0f) {   // PHYS_DRIVE_LOOK_UP: the view tilted up by that many degrees (a check of the sky)
            const V3 d = atR - eyeR;
            const float horiz = std::sqrt(d.x * d.x + d.z * d.z), len = std::sqrt(horiz * horiz + d.y * d.y);
            if (horiz > 1e-4f && len > 1e-4f) {
                const float e = std::atan2(d.y, horiz) + lookUp * 3.14159265f / 180.0f;
                atR = eyeR + V3{d.x / horiz * std::cos(e), std::sin(e), d.z / horiz * std::cos(e)} * len;
            }
        }
        math::LookAt(eyeR, atR, viewM);
        const float aspect = shownHeight ? float(shownWidth) / float(shownHeight) : 16.0f / 9.0f;
        math::Perspective(view.fovY, aspect, 0.25f, 8000.0f, proj);

        ui.NewFrame();
        ui::DrawDashboard(ui, snap.dash, shownWidth, shownHeight);
        ui::DrawStageClock(ui, snap.clock, shownWidth, shownHeight);
        if (live) {   // (the live game's): the call being spoken, and the next when close at the speed
            std::string current, next;
            codriver.Upcoming(snap.distance, std::fabs(snap.dash.speed), o.pacenoteNext, current, next);
            ui::DrawPacenotes(ui, current, next, shownWidth, shownHeight);
        }
        {   // the readout: last frame's numbers (the renderer's are read after its frame)
            LARGE_INTEGER tn;
            QueryPerformanceCounter(&tn);
            if (fpsLast.QuadPart && tn.QuadPart > fpsLast.QuadPart) {
                const double fps = double(freq.QuadPart) / double(tn.QuadPart - fpsLast.QuadPart);
                fpsSmooth = fpsSmooth > 0.0 ? fpsSmooth + (fps - fpsSmooth) * 0.1 : fps;
            }
            fpsLast = tn;
        }
        std::vector<std::string> lines;
        if (debugHud) {
            const iface::RenderStats st = renderer.Stats();
            char b[192];
            auto line = [&](const char* fmt, auto... a) {
                std::snprintf(b, sizeof b, fmt, a...);
                lines.emplace_back(b);
            };
            static const char* const kQuality[] = {"low", "medium", "high"};
            line("%.1f fps   %ux%u   gpu frame %.2f ms, main %.2f ms (env %.2f, aerial fill %.2f)", fpsSmooth, shownWidth,
                 shownHeight, st.frameMs, st.passMs,
                 st.envMs, st.aerialMs);
            line("tick %u%s   camera %d   chase %.1f m%s", snap.tick, paused ? " PAUSED" : "", cameraMode, chaseDistance,
                 o.fixedStep > 0.0 ? "   fixed step" : "");
            line("K ray tracing %s   L shadows %s   O AO %s   T TAA %s   quality %s",
                 !st.rtAvailable ? "n/a" : !rt ? "off" : st.rtActive ? "ON" : "wanted", st.shadows ? "on" : "off",
                 st.ambientOcclusion ? "on" : "off", st.taa ? "on" : "off", kQuality[st.quality > 2 ? 1 : st.quality]);
            if (st.rtActive)
                line("   rt %.2f ms (ao %.2f, lights %.2f, denoise %.2f)   %u px, %.1f%% shadowed   %u lights", st.rtMs,
                     st.aoMs, st.lightsMs, st.denoiseMs, st.rtPixels, st.rtPixels ? 100.0 * st.rtShadowed / st.rtPixels : 0.0,
                     st.lightCount);
            else if (st.rtFallback[0])
                line("   shadows from cascades: %s", st.rtFallback);
            if (st.autoExposure)
                line("exposure x%.2f   metered %+.2f EV   adapted %+.2f EV", st.exposure, st.meteredEv, st.adaptedEv);
            else
                line("exposure x%.2f (manual)", st.exposure);
            line("V view %u: %s", st.debugView, renderer.DebugViewName(static_cast<int32_t>(st.debugView)));
            if (legacyProfile >= 0) line("legacy cards: %s", legacyProfile == 1 ? "A (bake)" : legacyProfile == 2 ? "B (bake as occlusion)" : "off");
            if (codriver.Phrases()) line("co-driver: %s   (%.0f m along the stage)", codriver.LastCall().empty() ? "-" : codriver.LastCall().c_str(), snap.distance);
            auto key = [&](Action a) { return input::KeyName(keys[a]); };
            line("%s readout  %s pause  %s shot  %s camera  %s %s chase  %s %s orbit", key(Action::Readout).c_str(),
                 key(Action::Pause).c_str(), key(Action::Screenshot).c_str(), key(Action::Camera).c_str(),
                 key(Action::ChaseIn).c_str(), key(Action::ChaseOut).c_str(), key(Action::OrbitLeft).c_str(),
                 key(Action::OrbitRight).c_str());
        }
        ui::DrawReadout(ui, debugHud, lines, o.stageWarning, shownHeight);

        // The sound mix from the Debug menu or its panel: to the sound at once; Save into the mix file (the watch above
        // then reads it back), Reload it (the watch again), the module's defaults.
        auto mixDone = [&](ui::MixChoice c) {
            switch (c) {
            case ui::MixChoice::Defaults: mixEdit.Defaults(); [[fallthrough]];
            case ui::MixChoice::Changed: mixStatus = sound.SetMix(mixEdit.Text()); break;
            case ui::MixChoice::Save:
                mixStatus = mixPath.empty() ? "no mix file (game/config/sound-mix.ini)" : mixEdit.Save(mixPath);
                if (mixStatus.empty()) mixStatus = "saved to " + mixPath;
                break;
            case ui::MixChoice::Reload:
                mixWritten = {}, mixCheck = 1;
                mixStatus = mixPath.empty() ? "no mix file (game/config/sound-mix.ini)" : "read again: " + mixPath;
                break;
            case ui::MixChoice::None: break;
            }
        };
        if (soundOn && mixPanel && menu == StageMenu::None)
            mixDone(ui::MixPanel(mixEdit.values, mixStatus, mixPanel, shownWidth, shownHeight));

        // Esc's menu over the stage.
        if (menu == StageMenu::Pause) {
            switch (ui::PauseMenu(ui, shownWidth, shownHeight)) {
            case ui::PauseChoice::Resume: menu = StageMenu::None; break;
            case ui::PauseChoice::Restart: quit = true, end = StageEnd::Restart; break;
            case ui::PauseChoice::Settings: menu = StageMenu::Settings; break;
            case ui::PauseChoice::Debug:
                dbg.taa = taa, dbg.ao = ao, dbg.shadows = shadows, dbg.rayTracing = rt, dbg.readout = debugHud;
                dbg.debugView = debugView, dbg.camera = cameraMode, dbg.cameras = cameras, dbg.chaseDistance = chaseDistance;
                dbg.legacyProfile = legacyProfile;
                dbg.orbitDegrees = orbit * 57.29578f, dbg.muted = sound.Muted(), dbg.antiLagIdle = antiLagIdle;
                dbg.mixPanel = mixPanel, dbg.mix = soundOn ? &mixEdit.values : nullptr;
                menu = StageMenu::Debug;
                break;
            case ui::PauseChoice::ExitToMenu: quit = true, end = StageEnd::ExitToMenu; break;
            case ui::PauseChoice::None: break;
            }
        } else if (menu == StageMenu::Settings) {
            const ui::SettingsChoice c = ui::SettingsUi(ui, sh.settings, true, settingsLog, keyDown, shownWidth, shownHeight);
            if (c == ui::SettingsChoice::Graphics) {
                settings = ToRender(sh.settings);
                taa = settings.taa != 0, ao = settings.quality != 0, shadows = settings.shadows != 0, rt = settings.rayTracing != 0;
                renderer.ApplySettings(settings);
            } else if (c == ui::SettingsChoice::Back) {
                settingsLog = SaveSettings(sh.settings);
                if (!settingsLog.empty()) std::fprintf(stderr, "%s\n", settingsLog.c_str());
                menu = StageMenu::Pause;
            }
        } else if (menu == StageMenu::Debug) {
            ui::MixChoice mixChoice;
            dbg.mixStatus = mixStatus;
            const ui::DebugChoice c = ui::DebugMenu(ui, dbg, views, keys, shownWidth, shownHeight, mixChoice);
            mixDone(mixChoice);
            if (c == ui::DebugChoice::Changed) {
                if (dbg.taa != taa) toggle(taa, settings.taa, 1, "TAA", "off");
                if (dbg.ao != ao) toggle(ao, settings.quality, std::max(1u, std::min(sh.settings.quality, 2u)), "ambient occlusion", "off");
                if (dbg.shadows != shadows) toggle(shadows, settings.shadows, 1, "shadows", "off");
                if (dbg.rayTracing != rt) toggle(rt, settings.rayTracing, 1, "ray tracing", "off (cascaded shadow maps)");
                debugHud = dbg.readout;
                if (dbg.debugView != debugView) renderer.SetDebugView(debugView = dbg.debugView);
                if (legacyProfile >= 0 && dbg.legacyProfile != legacyProfile) renderer.SetLegacyProfile(legacyProfile = dbg.legacyProfile);
                cameraMode = dbg.camera;
                chaseDistance = dbg.chaseDistance;
                orbit = dbg.orbitDegrees / 57.29578f;
                if (dbg.muted != sound.Muted()) sound.SetMuted(dbg.muted);
                antiLagIdle = dbg.antiLagIdle;
                mixPanel = dbg.mixPanel;
            } else if (c == ui::DebugChoice::Back) {
                menu = StageMenu::Pause;
            }
        }

        // PHYS_DRIVE_JITTER=<tick>: how steadily the car sits in the picture - per frame its centre's place on the
        // screen (pixels) and the eye's distance to it; their second differences (frame to frame changes of the motion)
        // printed as RMS on reaching that tick, and the drive ends. A steady car against the camera has them near 0.
        if (jitterTo && !paused) {
            static double hist[3][3];   // the last frames' x, y, distance
            static double sum[3];
            static int frames = 0;
            const V3 pos{p.position[0], p.position[1], p.position[2]};
            const V3 r = math::ToRender(pos);
            float clip[4];
            for (int j = 0; j < 4; ++j) {
                float v = viewM[12 + j];
                for (int i = 0; i < 3; ++i) v += (&r.x)[i] * viewM[4 * i + j];
                clip[j] = v;
            }
            float c4[4];
            for (int j = 0; j < 4; ++j) c4[j] = clip[0] * proj[j] + clip[1] * proj[4 + j] + clip[2] * proj[8 + j] + clip[3] * proj[12 + j];
            const V3 d = pos - view.eye;
            const double now3[3] = {(c4[0] / c4[3] * 0.5 + 0.5) * shownWidth, (0.5 - c4[1] / c4[3] * 0.5) * shownHeight,
                                    std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) * 1000.0};
            for (int k = 0; k < 3; ++k) {
                hist[k][0] = hist[k][1];
                hist[k][1] = hist[k][2];
                hist[k][2] = now3[k];
                if (frames >= 30) {   // the camera settled
                    const double dd = hist[k][2] - 2 * hist[k][1] + hist[k][0];
                    sum[k] += dd * dd;
                }
            }
            ++frames;
            if (snap.tick >= jitterTo) {
                const int n = std::max(1, frames - 30);
                std::printf("jitter over %d frames to tick %u: screen x %.3f px, y %.3f px, distance %.2f mm (RMS of second differences)\n",
                            n, snap.tick, std::sqrt(sum[0] / n), std::sqrt(sum[1] / n), std::sqrt(sum[2] / n));
                quit = true, end = StageEnd::Quit;
            }
        }
        // PHYS_DRIVE_SHOTS=<n>: a capture every n ticks as well, for a replay looked at afterwards - the renderer's own
        // frame (captureFrame), which a covered or minimised window does not spoil. F2: the window as shown.
        const bool timedShot = shotEvery && !paused && snap.tick >= shotTick + shotEvery;
        if (timedShot) shotTick = snap.tick;
        const bool keyShot = shotKey(focused && keys.Down(Action::Screenshot));
        std::string shotPath;
        if (keyShot || timedShot) {
            const size_t slash = o.out.find_last_of("/\\");
            char name[32];
            std::snprintf(name, sizeof name, "shot-%03d.bmp", shots++);
            shotPath = (slash == std::string::npos ? std::string() : o.out.substr(0, slash + 1)) + name;
            if (timedShot) renderer.CaptureFrame(shotPath);
        }
        // PAUSED, nothing is drawn either, so the picture stays as it was when the pause began: rendering on, TAA
        // would settle the still image within a few frames and wipe out what was being looked at. A renderer switch
        // draws one frame - that frame alone, no settling.
        const iface::UiDrawList& uiList = ui.Render();   // (an ImGui frame ends every frame, drawn or not)
        if (!paused || redraw || timedShot || menu != StageMenu::None) {
            iface::FrameDesc fd{};
            fd.scene = scene;
            std::memcpy(fd.camera.view, viewM, sizeof viewM);
            std::memcpy(fd.camera.proj, proj, sizeof proj);
            fd.camera.nearZ = 0.25f;
            fd.camera.farZ = 8000.0f;
            fd.camera.fovY = view.fovY;
            fd.weatherPreset = weatherPreset;   // the .ngg's preset (0 its own weather)
            fd.wetness = 0.5f * static_cast<float>(std::clamp(wet, 0, 2));   // the road's: dry 0, damp 0.5, wet 1
            if (o.skyTimeSet) {   // the time and place: the sun from them (NGG design 5.4)
                fd.skyTime = {1, o.skyYear, o.skyMonth, o.skyDay, static_cast<float>(o.skyHour), static_cast<float>(o.skyUtcOffset),
                              static_cast<float>(o.skyLatitude), static_cast<float>(o.skyLongitude), static_cast<float>(o.skyNorth)};
            }
            // PHYS_DRIVE_TEST_LIGHTS: the test lamps on the car at its pose (above), or the lamp above and behind it.
            std::vector<iface::LightDesc> testLights;
            for (const assets::Lamp& lamp : testLamps) testLights.push_back(assets::LampLight(lamp, p, !testNoShadow));
            if (testLampOnly) {
                // In the car's frame: x left, -y forward, z up, from its centre of gravity (the pose's origin).
                iface::LightDesc l{};
                l.type = iface::kLightPoint;
                const V3 r = math::ToRender(V3{p.position[0], p.position[1], p.position[2]} +
                                            math::Rotate(p.rotation, V3{0.0f, 4.0f, 2.0f}));
                l.position[0] = r.x, l.position[1] = r.y, l.position[2] = r.z;
                assets::KelvinToRgb(2700.0f, l.colour);
                l.intensity = 2000.0f;
                l.radius = 0.15f;
                testLights.push_back(l);
            }
            fd.lights = testLights.data();
            fd.lightCount = static_cast<uint32_t>(testLights.size());
            fd.instances = instances.data();
            fd.instanceCount = static_cast<uint32_t>(instances.size());
            fd.stageObjects = moved.data();
            fd.stageObjectCount = static_cast<uint32_t>(moved.size());
            fd.ui = uiList;
            if (!renderer.RenderFrame(fd)) std::fprintf(stderr, "renderer: %s\n", renderer.Error().c_str());
            renderer.Present();
        } else {
            Sleep(15);
        }
        if (const std::string log = renderer.TakeLog(); rendererLog && !log.empty()) std::printf("%s", log.c_str());
        if (!shotPath.empty()) {
            const bool saved = timedShot || window.Screenshot(shotPath);
            std::printf("screenshot %s%s (tick %u)\n", shotPath.c_str(), saved ? "" : " failed", snap.tick);
        }
        title += frame;
        if (title > 0.25) {
            title = 0;
            const float speed = std::sqrt(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1] + p.velocity[2] * p.velocity[2]);
            wchar_t text[128];
            std::swprintf(text, 128, L"WRally - %.0f km/h - tick %u - %.0f fps", speed * 3.6f, snap.tick, frame > 0 ? 1.0 / frame : 0.0);
            if (tv) {
                const size_t n = std::wcslen(text);
                if (view.tvCamera >= 0) std::swprintf(text + n, 128 - n, L" - %.0f m, camera %d", tv->Distance(), view.tvCamera);
                else std::swprintf(text + n, 128 - n, L" - %.0f m", tv->Distance());
            }
            window.SetTitle(text);
        }
    }
    simThread.Stop();   // (the steps posted so far done: the recording is whole)
    sim.Finish();
    window.SetTitle(L"WRally");
    return end;
}

// Quick rally's choice as a command line - a run from the menu is one too. Empty, and `error`, when it cannot be.
std::vector<std::string> QuickArgs(const Settings& s, const assets::Catalog& catalog, std::string& error)
{
    const assets::Stage* st = catalog.FindStage(s.stage);
    const assets::Car* car = catalog.FindCar(s.car);
    if (!st || !car) return error = "choose a stage and a car", std::vector<std::string>{};
    std::error_code ec;
    const std::string sgc = std::filesystem::relative(car->sgc, s.rbrPath, ec).string();
    // The car's physics: a physics.rbz folder, or its own folder (a full path, as the physics wrapper takes one).
    std::string physics = car->physics;
    if (physics.find('/') != std::string::npos) {
        physics = s.rbrPath + "\\" + physics;
        std::replace(physics.begin(), physics.end(), '/', '\\');
        while (physics.back() == '\\') physics.pop_back();
    }
    std::vector<std::string> a = {"WRally", s.rbrPath, st->ngg, "--stage", st->stage, "--sgc", ec ? car->sgc : sgc, "--car", physics,
                                  "--damage", std::to_string(std::min(s.damage, 3u)), "--out", (ExeDir() / "drive.phr").string()};
    if (!s.setup.empty()) a.insert(a.end(), {"--setup", s.rbrPath + "\\" + s.setup});
    if (!s.weather.empty()) a.insert(a.end(), {"--weather", s.weather});
    static const char* const kRoad[3] = {"dry", "damp", "wet"};
    a.insert(a.end(), {"--road", kRoad[s.road ? std::min(s.road, 3u) - 1 : assets::RoadOfWeather(s.weather)]});
    if (!s.devEngine.empty()) a.insert(a.end(), {"--eng", s.devEngine});
    else if (!car->engine.empty()) a.insert(a.end(), {"--eng", car->engine});
    if (!s.devBangs.empty()) a.insert(a.end(), {"--als-bangs", s.devBangs});
    if (!s.devIdleBangs.empty()) a.insert(a.end(), {"--als-idle-bangs", s.devIdleBangs});
    if (!s.sound) a.push_back("--no-sound");
    if (!s.codriver) a.push_back("--no-codriver");
    if (s.codriverReplay) a.push_back("--codriver-replay");
    a.insert(a.end(), {"--pacenote-next", std::to_string(s.pacenoteNext)});
    a.insert(a.end(), {"--codriver-lead", std::to_string(s.codriverLead), "--codriver-volume", std::to_string(s.codriverVolume)});
    return a;
}

enum class Screen { Main, Quick, Settings };

// The main menu and its screens until Exit or the window closes. Returns the exit code.
int Menus(Shell& sh, std::string note)
{
    assets::Catalog catalog;
    auto scan = [&] {
        const std::string e = catalog.Scan(sh.settings);
        std::printf("content: %zu stages, %zu cars%s%s\n", catalog.stages.size(), catalog.cars.size(), e.empty() ? "" : " - ", e.c_str());
        return e;
    };
    std::string scanned = scan();
    if (note.empty()) note = scanned;
    Screen screen = Screen::Main;
    std::string settingsLog;
    Settings atOpen;   // the settings as the Settings screen opened (the content changed: scanned again)
    for (;;) {
        bool start = false, exit = false;
        const UINT keyDown = sh.window.TakeKeyDown();   // (for Settings -> Controls)
        const bool capturing = ui::CapturingKey();      // (Esc then the binding's, not the menu's)
        const bool open = MenuFrame(sh, [&] {
            switch (screen) {
            case Screen::Main:
                switch (ui::MainMenu(sh.ui, sh.width, sh.height, note)) {
                case ui::MainChoice::QuickRally: screen = Screen::Quick; break;
                case ui::MainChoice::Settings: screen = Screen::Settings, atOpen = sh.settings, settingsLog.clear(); break;
                case ui::MainChoice::Exit: exit = true; break;
                case ui::MainChoice::None: break;
                }
                break;
            case Screen::Quick:
                switch (ui::QuickRally(sh.ui, catalog, sh.settings, sh.width, sh.height)) {
                case ui::QuickChoice::Start: start = true; break;
                case ui::QuickChoice::Back: screen = Screen::Main, SaveSettings(sh.settings); break;
                case ui::QuickChoice::None: break;
                }
                break;
            case Screen::Settings:
                switch (ui::SettingsUi(sh.ui, sh.settings, false, settingsLog, keyDown, sh.width, sh.height)) {
                case ui::SettingsChoice::Graphics: sh.renderer.ApplySettings(ToRender(sh.settings)); break;
                case ui::SettingsChoice::Rescan: {
                    const std::string e = scan();
                    settingsLog = std::to_string(catalog.stages.size()) + " stages, " + std::to_string(catalog.cars.size()) + " cars" +
                                  (e.empty() ? "" : "\n" + e);
                    break;
                }
                case ui::SettingsChoice::Extract:
                    scan();
                    settingsLog = assets::ExtractSlotPhysics(sh.settings.rbrPath, catalog);
                    std::printf("%s", settingsLog.c_str());
                    scan();
                    break;
                case ui::SettingsChoice::Back:
                    if (const std::string e = SaveSettings(sh.settings); !e.empty()) std::fprintf(stderr, "%s\n", e.c_str());
                    if (sh.settings.rbrPath != atOpen.rbrPath || sh.settings.nggFolders != atOpen.nggFolders ||
                        sh.settings.basePhysics != atOpen.basePhysics)
                        scanned = scan();
                    note = scanned;
                    screen = Screen::Main;
                    break;
                case ui::SettingsChoice::None: break;
                }
                break;
            }
            if (sh.window.TakeEscape() && !capturing && screen != Screen::Main) {   // Esc: back, the settings kept as they are
                SaveSettings(sh.settings);
                screen = Screen::Main;
            }
        });
        if (!open || exit) return 0;
        if (!start) continue;
        SaveSettings(sh.settings);
        std::string error;
        const std::vector<std::string> args = QuickArgs(sh.settings, catalog, error);
        Options o;
        if (args.empty() || Parse(args, o)) {
            note = error.empty() ? "the stage could not be set up (the reason is in the log)" : error;
            screen = Screen::Main;
            continue;
        }
        std::string line;
        for (const std::string& a : args) line += (line.empty() ? "" : " ") + a;
        std::printf("quick rally: %s\n", line.c_str());
        StageEnd end;
        while ((end = RunStage(sh, o, true, error)) == StageEnd::Restart) {}
        if (end == StageEnd::Quit) return 0;
        note = end == StageEnd::Failed ? error : "";
        screen = Screen::Main;
    }
}

} // namespace

// THE LAMP LAB (--lab, NGG design 5.4 N3): the test lamps (PHYS_DRIVE_TEST_LIGHTS, the ramp by default) on a car standing
// in a synthetic scene (assets::BuildLampLab) - no stage, no physics, no car model yet. The sky's --time / --date as a
// stage's (23:00 by default); --camera 0 chase, 1 bonnet (the default), 2 above, 3 beside; --exit-at <frame>.
// PHYS_DRIVE_SHOTS=<n> a capture at frame n and every n frames after; PHYS_DRIVE_OFF and NGG_DEBUG_VIEW as on a stage.
int RunLab(Shell& sh, const std::vector<std::string>& args)
{
    auto arg = [&](const char* name, const char* fallback) {
        for (size_t i = 1; i + 1 < args.size(); ++i)
            if (args[i] == name) return args[i + 1];
        return std::string(fallback);
    };
    Options o;
    o.skyMonth = 11, o.skyDay = 9;   // a long night, unless --date says otherwise
    if (const int code = ParseSkyTime(args, o, "23:00")) return code;
    const int cameraMode = std::atoi(arg("--camera", "1").c_str());
    const uint32_t exitAt = std::strtoul(arg("--exit-at", "0").c_str(), nullptr, 10);
    const uint32_t shotEvery = std::getenv("PHYS_DRIVE_SHOTS") ? std::strtoul(std::getenv("PHYS_DRIVE_SHOTS"), nullptr, 0) : 0;

    render::Renderer& renderer = sh.renderer;
    iface::RenderSettings settings = ToRender(sh.settings);
    if (const char* off = std::getenv("PHYS_DRIVE_OFF")) {
        if (std::strstr(off, "vsync")) settings.vsync = 0;
        if (std::strstr(off, "taa")) settings.taa = 0;
        if (std::strstr(off, "ao")) settings.quality = 0;
        if (std::strstr(off, "shadows")) settings.shadows = 0;
        if (std::strstr(off, "rt")) settings.rayTracing = 0;
    }
    renderer.ApplySettings(settings);
    renderer.SetDebugView(std::getenv("NGG_DEBUG_VIEW") ? std::atoi(std::getenv("NGG_DEBUG_VIEW")) : 0);
    const std::vector<uint8_t> bytes = assets::BuildLampLab();
    const iface::SceneHandle scene = renderer.LoadScene(bytes.data(), bytes.size());
    if (!scene) return std::fprintf(stderr, "the lab: %s\n", renderer.Error().c_str()), 3;

    // The car at the origin, unturned (facing the physics frame's -y), its lamps the test layout's.
    physics::Pose p{};
    p.position[2] = assets::kLabCogHeight;
    p.rotation[3] = 1.0f;
    const char* layoutEnv = std::getenv("PHYS_DRIVE_TEST_LIGHTS");
    const std::string layout = layoutEnv && *layoutEnv ? layoutEnv : "ramp";
    const bool noShadow = layout == "noshadow";
    std::vector<assets::Lamp> lamps = assets::TestLayout(noShadow ? "ramp" : layout.c_str());
    if (lamps.empty()) lamps = assets::TestLayout("ramp");
    std::vector<iface::LightDesc> lights;
    for (const assets::Lamp& lamp : lamps) lights.push_back(assets::LampLight(lamp, p, !noShadow));
    std::printf("the lab: %zu lamps (%s), camera %d\n", lamps.size(), layout.c_str(), cameraMode);

    const V3 chaseEye = V3{p.position[0], p.position[1], p.position[2]} + V3{0.0f, 5.2f, 1.6f};
    uint32_t frame = 0;
    int shots = 0;
    while (sh.window.Pump() && !sh.window.TakeEscape()) {
        sh.FollowSize();
        const camera::View view = camera::Look(cameraMode, p, chaseEye, 0.0f, nullptr);
        float viewM[16], proj[16];
        math::LookAt(math::ToRender(view.eye), math::ToRender(view.at), viewM);
        math::Perspective(view.fovY, sh.height ? float(sh.width) / float(sh.height) : 16.0f / 9.0f, 0.25f, 8000.0f, proj);
        if (shotEvery && frame >= shotEvery && frame % shotEvery == 0) {
            char name[32];
            std::snprintf(name, sizeof name, "shot-%03d.bmp", shots++);
            renderer.CaptureFrame(name);
            std::printf("screenshot %s (frame %u)\n", name, frame);
        }
        sh.ui.NewFrame();
        iface::FrameDesc fd{};
        fd.scene = scene;
        std::memcpy(fd.camera.view, viewM, sizeof viewM);
        std::memcpy(fd.camera.proj, proj, sizeof proj);
        fd.camera.nearZ = 0.25f;
        fd.camera.farZ = 8000.0f;
        fd.camera.fovY = view.fovY;
        fd.skyTime = {1, o.skyYear, o.skyMonth, o.skyDay, static_cast<float>(o.skyHour), static_cast<float>(o.skyUtcOffset),
                      static_cast<float>(o.skyLatitude), static_cast<float>(o.skyLongitude), static_cast<float>(o.skyNorth)};
        fd.lights = lights.data();
        fd.lightCount = static_cast<uint32_t>(lights.size());
        fd.ui = sh.ui.Render();
        if (!renderer.RenderFrame(fd)) std::fprintf(stderr, "renderer: %s\n", renderer.Error().c_str());
        renderer.Present();
        if (const std::string log = renderer.TakeLog(); sh.rendererLog && !log.empty()) std::printf("%s", log.c_str());
        if (exitAt && ++frame >= exitAt) break;
        if (!exitAt) ++frame;
    }
    return 0;
}

int RunGame(const std::vector<std::string>& args)
{
    Shell sh;
    sh.settings = LoadSettings();
    if (std::find(args.begin(), args.end(), std::string("--lab")) != args.end()) {
        if (const std::string e = OpenShell(sh, "wrenderer"); !e.empty()) return std::fprintf(stderr, "%s\n", e.c_str()), 3;
        return RunLab(sh, args);
    }
    // A command line: that stage at once (its Esc menu's "Exit to main menu" goes to the menus).
    if (args.size() > 1) {
        Options o;
        if (const int code = Parse(args, o)) return code;
        if (const std::string e = OpenShell(sh, o.rendererId); !e.empty()) return std::fprintf(stderr, "%s\n", e.c_str()), 3;
        std::string error;
        StageEnd end;
        while ((end = RunStage(sh, o, false, error)) == StageEnd::Restart) {}
        if (end == StageEnd::Quit) return 0;
        if (end == StageEnd::Failed) return 3;
        return Menus(sh, "");
    }
    if (const std::string e = OpenShell(sh, "wrenderer"); !e.empty()) return std::fprintf(stderr, "%s\n", e.c_str()), 3;
    return Menus(sh, "");
}

} // namespace wrally::app
