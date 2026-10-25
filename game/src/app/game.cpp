#include "app/game.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "app/window.h"
#include "assets/car_render.h"
#include "camera/camera.h"
#include "camera/replay_camera.h"
#include "core/math.h"
#include "data/lbs.h"
#include "data/sgc.h"
#include "input/keyboard.h"
#include "render/renderer.h"
#include "render/stage_objects.h"
#include "session/session.h"
#include "session/sim_thread.h"
#include "signals/bus.h"
#include "sound/sound.h"
#include "ui/hud.h"
#include "ui/imgui_layer.h"

namespace wrally::app {

using math::V3;
using namespace input;

namespace {

// The frame between two physics steps at t: the car's pose and its wheels (the spin the shorter way round).
void Between(const session::Shown& a, const session::Shown& b, float t, physics::Pose& p, physics::Wheel wheels[4])
{
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

} // namespace

int RunGame(const Options& o)
{
    // The physics, through the session (a module DLL in modules/physics: --physics, "rbr" by default).
    session::Session sim;
    if (const std::string e = sim.Open(o.session); !e.empty()) return std::fprintf(stderr, "%s\n", e.c_str()), 3;
    // The modules hear the physics on the bus: the sound plays from its device's thread, a few ticks behind it.
    signals::Bus bus;
    std::string mixPath;   // the mix file, watched ("" none)
    FILETIME mixWritten{};
    // The sound: a module DLL in modules/audio (--audio, "waudio" by default: WAudio.dll), every record on the bus
    // forwarded to it.
    sound::Sound sound;
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
        }
    }
    sim.Connect(&bus);
    const std::vector<phys::data::RecMesh> collision =
        phys::data::CarCollisionMeshes(phys::data::ReadSgcCollision(o.session.physics.sgc));

    std::vector<char> bytes;
    if (FILE* f = std::fopen(o.ngg.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        bytes.resize(static_cast<size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        bytes.resize(std::fread(bytes.data(), 1, bytes.size(), f));
        std::fclose(f);
    }
    if (bytes.empty()) return std::fprintf(stderr, "cannot read %s\n", o.ngg.c_str()), 3;

    Window window;
    if (const std::string e = window.Create(L"WRally"); !e.empty()) return std::fprintf(stderr, "%s\n", e.c_str()), 3;
    // The renderer: a module DLL in modules/renderer (--renderer, "wrenderer" by default: WRenderer.dll, NGG), on the
    // window. PHYS_DRIVE_NGGLOG: its log on stdout.
    const bool rendererLog = std::getenv("PHYS_DRIVE_NGGLOG") != nullptr;
    iface::RenderSettings settings{};
    settings.mode = iface::WindowMode::Windowed;
    settings.vsync = 1;
    settings.quality = 1;
    settings.rayTracing = settings.taa = settings.autoExposure = settings.shadows = 1;
    settings.renderScale = 1;
    render::Renderer renderer;
    {
        const std::string e = renderer.Open(ExeDir() / "modules" / "renderer", o.rendererId, window.Handle(),
                                            static_cast<uint32_t>(window.Width()), static_cast<uint32_t>(window.Height()), settings);
        if (rendererLog) std::printf("%s", renderer.TakeLog().c_str());
        if (!e.empty()) return std::fprintf(stderr, "renderer: %s\n", e.c_str()), 3;
        std::printf("renderer module: %s\n", renderer.ModuleName().c_str());
    }
    int shownWidth = window.Width(), shownHeight = window.Height();
    const iface::SceneHandle scene = renderer.LoadScene(bytes.data(), bytes.size());
    if (rendererLog) std::printf("%s", renderer.TakeLog().c_str());
    if (!scene) return std::fprintf(stderr, "renderer: %s\n", renderer.Error().c_str()), 3;
    bytes.clear();
    {
        uint32_t keyed = 0, keys = 0;
        renderer.Api()->sceneKeyStats(renderer.Instance(), scene, &keyed, &keys);
        if (keyed)   // the stage's objects, before ours
            std::printf("movable objects: %u drawn objects keyed to %u solids\n", keyed, keys);
        else
            std::printf("movable objects: the .ngg has no object keys (NGGK) - reconvert it; nothing will move\n");
    }
    render::StageObjects stageObjects;
    assets::CarRender car;
    car.Create(renderer, o.session.physics.sgc, o.session.physics.surface, collision);
    ui::ImGuiLayer ui;
    if (const std::string e = ui.Init(window.Handle(), renderer); !e.empty()) std::fprintf(stderr, "the HUD: %s\n", e.c_str());

    if (const std::string e = sim.Start(); !e.empty()) return std::fprintf(stderr, "%s\n", e.c_str()), 3;

    // The renderer's switches: T its TAA, O its ambient occlusion (quality medium / low), L its shadows, K its ray
    // tracing (off: the cascaded shadow maps, no RT AO), V its debug views, F1 its readout. P a pause (the physics, a
    // replay and the rendering stop - the picture stays as it was; a switch draws one frame), F2 a screenshot
    // (shot-NNN.bmp beside the recording).
    Press taaKey, pauseKey, aoKey, shadowKey, shotKey, viewKey, rtKey, debugHudKey, muteKey, antiLagIdleKey;
    Press gearUpKey, gearDownKey, cameraKey, resetKey;
    int debugView = 0;
    uint32_t pauseAt = o.pauseAt;
    bool taa = true, paused = false, ao = true, shadows = true, rt = true;
    bool antiLagIdle = false;   // N: the anti-lag armed at standstill (the driver's switch before a start)
    bool debugHud = std::getenv("PHYS_DRIVE_READOUT") != nullptr;   // PHYS_DRIVE_READOUT: the readout on from the start
    double fpsSmooth = 0.0;     // wall-clock frames per second, eased, for the readout
    LARGE_INTEGER freq, last, now, fpsLast{};
    QueryPerformanceFrequency(&freq);
    int shots = 0;
    // PHYS_DRIVE_OFF=taa,ao,shadows,rt: any of them off from the start, as the keys would.
    if (const char* off = std::getenv("PHYS_DRIVE_OFF")) {
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

    QueryPerformanceCounter(&last);
    bool quit = false;
    // A quit (Esc, the close box, --exit-at, a fault) still finishes its frame - its steps are in the recording.
    while (!quit) {
        if (!window.Pump()) quit = true;
        QueryPerformanceCounter(&now);
        // --step: every frame advances the physics by exactly that, whatever the frame took.
        const double frame = o.fixedStep > 0.0 ? o.fixedStep : std::min(0.1, double(now.QuadPart - last.QuadPart) / double(freq.QuadPart));
        last = now;
        const bool focused = window.Focused();
        session::FrameInput in;
        if (gearUpKey(focused && DownAt(kGearUp))) in.presses |= session::kGearUp;
        if (gearDownKey(focused && DownAt(kGearDown))) in.presses |= session::kGearDown;
        if (cameraKey(focused && DownAt(kCamera))) cameraMode = (cameraMode + 1) % cameras;
        bool redraw = false;   // a renderer switch changed: draw one frame even while paused
        auto toggle = [&](bool& on, uint32_t& field, uint32_t onValue, const char* name, const char* offText) {
            on = !on;
            field = on ? onValue : 0;
            renderer.ApplySettings(settings);
            redraw = true;
            std::printf("%s %s\n", name, on ? "on" : offText);
        };
        if (taaKey(focused && DownAt(kTaa))) toggle(taa, settings.taa, 1, "TAA", "off");
        if (aoKey(focused && DownAt(kAo))) toggle(ao, settings.quality, 1, "ambient occlusion", "off");
        if (shadowKey(focused && DownAt(kShadows))) toggle(shadows, settings.shadows, 1, "shadows", "off");
        if (rtKey(focused && DownAt(kRt))) toggle(rt, settings.rayTracing, 1, "ray tracing", "off (cascaded shadow maps)");
        // The mix file saved: read again (checked about twice a second).
        if (!mixPath.empty() && (mixCheck += frame) > 0.5) {
            mixCheck = 0;
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExA(mixPath.c_str(), GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &mixWritten)) {
                mixWritten = a.ftLastWriteTime;
                std::ifstream f(mixPath, std::ios::binary);
                sound.SetMix(std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()));
                std::printf("mix %s\n%s", mixPath.c_str(), sound.TakeLog().c_str());
            }
        }
        if (antiLagIdleKey(focused && DownAt(kAntiLagIdle))) {
            antiLagIdle = !antiLagIdle;
            std::printf("anti-lag at standstill %s\n", antiLagIdle ? "armed" : "off");
        }
        if (muteKey(focused && DownAt(kMute))) {
            sound.SetMuted(!sound.Muted());
            std::printf("sound %s\n", sound.Muted() ? "muted" : "on");
        }
        if (debugHudKey(focused && Down(VK_F1))) {
            debugHud = !debugHud;
            redraw = true;
        }
        if (pauseKey(focused && DownAt(kPause))) {
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
            quit = true;
        }
        if (viewKey(focused && DownAt(kView))) {
            if (!renderer.Api()->debugViewName(++debugView)) debugView = 0;
            renderer.SetDebugView(debugView);
            redraw = true;
            std::printf("view %d: %s\n", debugView, renderer.DebugViewName(debugView));
        }
        // , / .: the beside camera round the car at 90 degrees / s; while paused each frame it turns is drawn.
        const int turn = (focused && DownAt(kOrbitRight) ? 1 : 0) - (focused && DownAt(kOrbitLeft) ? 1 : 0);
        if (turn && cameraMode == camera::kBeside) {
            orbit = std::remainder(orbit + turn * static_cast<float>(frame) * 1.5707963f, 6.2831853f);
            redraw = true;
        }
        // [ / ]: the chase camera in / out at 2 m / s - shown at once, paused too (the session moves its eyes).
        const int zoom = (focused && DownAt(kChaseOut) ? 1 : 0) - (focused && DownAt(kChaseIn) ? 1 : 0);
        if (zoom && (cameraMode == camera::kChase || cameraMode == camera::kTv)) {
            chaseDistance = std::max(2.0f, std::min(20.0f, chaseDistance + zoom * static_cast<float>(frame) * 2.0f));
            redraw = true;
            chaseChanged = true;
        } else if (chaseChanged) {
            chaseChanged = false;
            std::printf("chase camera %.1f m behind the car\n", chaseDistance);
        }
        // The keys for the session: R back on the road, the pedals, the steering's target.
        if (resetKey(focused && DownAt(kReset))) in.presses |= session::kBackOnRoad;
        in.seconds = paused ? 0.0 : frame;   // a pause keeps the frame where it is, between the same steps
        in.throttle = focused && DownAt(kThrottle);
        in.brake = focused && DownAt(kBrake);
        in.handbrake = focused && Down(VK_SPACE);
        in.steer = focused ? (Down(VK_LEFT) ? -1.0f : 0.0f) + (Down(VK_RIGHT) ? 1.0f : 0.0f) : 0.0f;
        in.antiLagIdle = antiLagIdle;
        in.chaseDistance = chaseDistance;
        simThread.Post(in);
        simThread.Latest(snap);
        if (snap.failed) quit = true;
        // A replay's end ends the run (its check reported, as at a quit), unless --hold keeps its last frame up.
        if (snap.replayEnded && !o.hold) quit = true;

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
        Between(snap.before, snap.after, t, p, wheels);
        const iface::InstanceUpdate& carUpdate = car.Place(p, wheels, snap.centerOfGravity);
        const V3 chaseEye = snap.before.chase.eye + (snap.after.chase.eye - snap.before.chase.eye) * t;
        const camera::View view = camera::Look(cameraMode, p, chaseEye, orbit, tv.get());
        if (window.Width() != shownWidth || window.Height() != shownHeight) {
            renderer.Resize(static_cast<uint32_t>(window.Width()), static_cast<uint32_t>(window.Height()));
            shownWidth = window.Width();
            shownHeight = window.Height();
        }
        float viewM[16], proj[16];
        math::LookAt(math::ToRender(view.eye), math::ToRender(view.at), viewM);
        const float aspect = shownHeight ? float(shownWidth) / float(shownHeight) : 16.0f / 9.0f;
        math::Perspective(view.fovY, aspect, 0.25f, 8000.0f, proj);

        ui.NewFrame();
        ui::DrawDashboard(ui, snap.dash, shownWidth, shownHeight);
        ui::DrawStageClock(ui, snap.clock, shownWidth, shownHeight);
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
            line("%.1f fps   %ux%u   gpu main %.2f ms (env %.2f, aerial fill %.2f)", fpsSmooth, shownWidth, shownHeight, st.passMs,
                 st.envMs, st.aerialMs);
            line("tick %u%s   camera %d   chase %.1f m%s", snap.tick, paused ? " PAUSED" : "", cameraMode, chaseDistance,
                 o.fixedStep > 0.0 ? "   fixed step" : "");
            line("K ray tracing %s   L shadows %s   O AO %s   T TAA %s   quality %s",
                 !st.rtAvailable ? "n/a" : !rt ? "off" : st.rtActive ? "ON" : "wanted", st.shadows ? "on" : "off",
                 st.ambientOcclusion ? "on" : "off", st.taa ? "on" : "off", kQuality[st.quality > 2 ? 1 : st.quality]);
            if (st.rtActive)
                line("   rt %.2f ms (ao %.2f, denoise %.2f)   %u px, %.1f%% shadowed", st.rtMs, st.aoMs, st.denoiseMs, st.rtPixels,
                     st.rtPixels ? 100.0 * st.rtShadowed / st.rtPixels : 0.0);
            else if (st.rtFallback[0])
                line("   shadows from cascades: %s", st.rtFallback);
            if (st.autoExposure)
                line("exposure x%.2f   metered %+.2f EV   adapted %+.2f EV", st.exposure, st.meteredEv, st.adaptedEv);
            else
                line("exposure x%.2f (manual)", st.exposure);
            line("V view %u: %s", st.debugView, renderer.DebugViewName(static_cast<int32_t>(st.debugView)));
            line("F1 readout  P pause  F2 shot  C camera  [ ] chase  , . orbit");
        }
        ui::DrawReadout(ui, debugHud, lines, o.stageWarning, shownHeight);

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
                quit = true;
            }
        }
        // PHYS_DRIVE_SHOTS=<n>: a capture every n ticks as well, for a replay looked at afterwards - the renderer's own
        // frame (captureFrame), which a covered or minimised window does not spoil. F2: the window as shown.
        const bool timedShot = shotEvery && !paused && snap.tick >= shotTick + shotEvery;
        if (timedShot) shotTick = snap.tick;
        const bool keyShot = shotKey(focused && Down(VK_F2));
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
        if (!paused || redraw || timedShot) {
            iface::FrameDesc fd{};
            fd.scene = scene;
            std::memcpy(fd.camera.view, viewM, sizeof viewM);
            std::memcpy(fd.camera.proj, proj, sizeof proj);
            fd.camera.nearZ = 0.25f;
            fd.camera.farZ = 8000.0f;
            fd.camera.fovY = view.fovY;
            fd.weatherPreset = 0;   // the stage's own weather, as the .ngg has it
            fd.instances = car.Ready() ? &carUpdate : nullptr;
            fd.instanceCount = car.Ready() ? 1 : 0;
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
    return 0;
}

} // namespace wrally::app
