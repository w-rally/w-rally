#include "app/game.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "app/content.h"
#include "app/paths.h"
#include "app/settings.h"
#include "app/window.h"
#include "assets/car_lights.h"
#include "assets/lamp_lab.h"
#include "assets/showroom.h"
#include "assets/car_render.h"
#include "assets/fences.h"
#include "assets/catalog.h"
#include "assets/legacy_stage.h"
#include "assets/splash.h"
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
#include "signals/car.h"
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
    // The upscaling: the settings', or PHYS_DRIVE_UPSCALE's (an index or a name of kUpscaleModes) over them.
    uint32_t mode = s.upscaling;
    if (const char* e = std::getenv("PHYS_DRIVE_UPSCALE")) {
        mode = static_cast<uint32_t>(std::strtoul(e, nullptr, 10));
        for (uint32_t k = 0; k < kUpscaleModeCount; ++k)
            if (std::strcmp(e, kUpscaleModes[k].name) == 0) mode = k;
    }
    const UpscaleMode& m = kUpscaleModes[mode < kUpscaleModeCount ? mode : 0];
    r.upscaler = m.upscaler;
    r.renderScale = m.scale;
    return r;
}

// The window, the renderer (module `rendererId`) on it and ImGui. Returns "" or what failed.
std::string OpenShell(Shell& sh, const std::string& rendererId)
{
    if (std::string e = sh.window.Create(L"W-Rally"); !e.empty()) return e;
    sh.window.SetFullscreen(sh.settings.fullscreen);
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

// The Garage's showroom (Quick rally -> Garage): the car it shows on a turntable in a synthetic scene
// (assets/showroom.h), lit by three spots in the night, the camera fixed and the car turning.
//
// The scene is loaded once - its load takes the renderer a second or two, so it is done where the menus stall anyway
// (Prepare: as they open, after a stage) - and kept while the menus run (a stage loads its own: one scene at a time).
// A car is read on a thread of its own once the Garage has rested on it a moment (stepping through the cars reads none
// of those passed), the menu running meanwhile, the turntable empty; read, its textures go to the renderer a few a
// frame, then its model, and it stands on the turntable. The car before it is taken off (its models released); its
// textures stay the scene's, so after a number of cars the scene is loaded afresh.
class Showroom {
public:
    ~Showroom()
    {
        if (m_reading.valid()) m_reading.wait();   // (a read in flight: done before its results go)
    }

    // The scene loaded, if it is not (the menus opening, back from a stage).
    void Prepare(render::Renderer& renderer)
    {
        if (m_scene) return;
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<uint8_t> bytes = assets::BuildShowroom();
        m_scene = renderer.LoadScene(bytes.data(), bytes.size());
        m_carsInScene = 0;
        if (!m_scene) return void(std::fprintf(stderr, "the showroom: %s\n", renderer.Error().c_str()));
        std::printf("the showroom: its scene loaded in %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }

    // The scene gone without unloading it: a stage loaded its own in its place (which released it).
    void Forget()
    {
        m_car.reset(), m_pending.reset();
        m_scene = 0, m_shown.clear();
    }

    // The world under the menu for `view` into `fd` (true), or nothing to draw (the Garage closed).
    bool Frame(render::Renderer& renderer, const assets::Catalog& catalog, const ui::ShowroomView& view, int width, int height,
               iface::FrameDesc& fd)
    {
        if (view.car.empty()) return false;
        Prepare(renderer);
        if (!m_scene) return false;
        const double now = GetTickCount64() / 1000.0;
        if (view.car != m_wanted) m_wanted = view.car, m_since = now;
        if (m_wanted != m_shown && now - m_since >= (m_car || m_pending || m_reading.valid() ? 0.25 : 0.0)) Load(renderer, catalog);
        // A read done: kept, if it is still the car shown, to be made the renderer's a few textures a frame (each an upload
        // the renderer waits for), the model last.
        if (m_reading.valid() && m_reading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            Read r = m_reading.get();
            if (r.folder == m_shown) m_pending = std::make_unique<Read>(std::move(r));
        }
        if (m_pending) {
            const auto t0 = std::chrono::steady_clock::now();
            auto spent = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
            bool all = true;
            for (assets::CarTexture& t : m_pending->model.materials) {
                // The texture, then its PBR maps (a normal or ORM map as data, assets::CarMap).
                for (int k = -1; k < assets::kCarMaps && all; ++k) {
                    std::vector<uint8_t>& dds = k < 0 ? t.dds : t.maps[k].dds;
                    uint32_t& uploaded = k < 0 ? t.uploaded : t.maps[k].uploaded;
                    if (uploaded || dds.empty()) continue;
                    if (spent() > 4.0) {   // (this frame's share spent: the rest next frame)
                        all = false;
                        break;
                    }
                    const double before = spent();
                    uploaded = k > assets::kMapBaseColour ? renderer.CreateTextureData(dds.data(), dds.size())
                                                          : renderer.CreateTexture(dds.data(), dds.size());
                    if (!uploaded && k < 0) dds.clear();   // (refused: the material's colour instead; a map is tried again,
                                                           // and named rejected, as the model is made)
                    if (std::getenv("WRALLY_SHOWROOM_TIMES"))
                        std::printf("the showroom: texture %s%s (%zu KB) in %.0f ms\n", t.material.c_str(), k < 0 ? "" : " (a map)",
                                    dds.size() / 1024, spent() - before);
                }
                if (!all) break;
            }
            if (all && spent() <= 4.0) {
                m_car = std::make_unique<assets::CarRender>();
                m_car->Create(renderer, m_pending->ok ? &m_pending->model : nullptr, {}, m_parts);
                m_height = 0.5f;
                if (!m_car->Stance(m_wheels, m_height)) std::printf("the showroom: %s has no wheel hubs in its model\n", m_pending->folder.c_str());
                std::printf("the showroom: %s read in %.0f ms (its thread), its model made in %.0f ms\n", m_pending->folder.c_str(),
                            m_pending->readMs, spent());
                m_pending.reset();
            }
        }

        // The car on the turntable, its wheels where the model's hubs are, turned about the up axis through the middle of
        // its hubs (the model's origin is often at the rear axle): that middle kept over the turntable's centre.
        physics::Pose p{};
        float mid[2] = {};
        for (const physics::Wheel& w : m_wheels) mid[0] += w.position[0] * 0.25f, mid[1] += w.position[1] * 0.25f;
        const float c = std::cos(view.turn), s = std::sin(view.turn);
        p.position[0] = -(c * mid[0] - s * mid[1]), p.position[1] = -(s * mid[0] + c * mid[1]);
        p.position[2] = m_height + assets::kShowroomDeck;
        p.rotation[2] = std::sin(view.turn * 0.5f), p.rotation[3] = std::cos(view.turn * 0.5f);
        m_instances.clear();
        const bool drawn = m_car && m_car->Ready();
        const bool cut = drawn != m_drawn;
        m_drawn = drawn;
        if (drawn) {
            const float cog[3] = {};
            m_instances.push_back(m_car->Place(p, m_wheels, cog, m_parts));
            const bool lit[assets::kLampGroups] = {};
            m_car->PlaceLamps(lit, false, m_parts, m_instances);
        }

        // The camera: three quarters from the front left, a little above, its picture's centre moved to `view`'s.
        constexpr float kFov = 0.52f;
        float viewM[16], proj[16];
        math::LookAt(V3{-6.9f, 2.1f, -7.8f}, V3{0.0f, 0.55f, 0.0f}, viewM);
        math::Perspective(kFov, height ? float(width) / float(height) : 16.0f / 9.0f, 0.2f, 400.0f, proj);
        proj[8] = view.centreX * 2 - 1, proj[9] = 1 - view.centreY * 2;   // (row vectors: x, y += shift * w)
        fd.flags = 0;
        fd.scene = m_scene;
        std::memcpy(fd.camera.view, viewM, sizeof viewM);
        std::memcpy(fd.camera.proj, proj, sizeof proj);
        fd.camera.nearZ = 0.2f, fd.camera.farZ = 400.0f, fd.camera.fovY = kFov;
        fd.skyTime = {1, 2026, 11, 9, 23.0f, 1.0f, 50.0f, 14.4f, 0.0f};   // a moonless late evening: the spots' light alone
        fd.lights = kLights, fd.lightCount = 3;
        fd.instances = m_instances.data(), fd.instanceCount = static_cast<uint32_t>(m_instances.size());
        fd.reflectionFocus = drawn ? m_instances[0].instance : 0;   // the world in its paint
        fd.time = static_cast<float>(now);
        // A car taken off or put on: TAA's history of the turntable dropped, as at a camera's cut. What stood still
        // there (the glass, a part turning slower than the renderer calls moving) is the background's now, and that
        // keeps its history unclipped - the car gone, a trail of it stayed a moment.
        fd.camera.cut = cut ? 1u : 0u;
        return true;
    }

    // The car is loaded and drawn.
    bool Ready() const { return m_scene && m_car && m_car->Ready(); }

    void Unload(render::Renderer& renderer)
    {
        m_car.reset();
        m_pending.reset();   // (its textures were the scene's: gone with it)
        if (m_scene) renderer.Api()->unloadScene(renderer.Instance(), m_scene);
        m_scene = 0, m_shown.clear();
    }

private:
    // The car before taken off, the new one's model read on a thread (one at a time: a later car waits for the read in
    // flight, which Frame then throws away). A car W-Rally cannot use stays off: the turntable empty.
    void Load(render::Renderer& renderer, const assets::Catalog& catalog)
    {
        if (m_reading.valid()) return;
        if (m_car) m_car->Release(renderer);
        m_car.reset(), m_pending.reset();
        if (++m_carsInScene > kCarsPerScene) Unload(renderer), Prepare(renderer);   // (their textures piled up: afresh)
        if (!m_scene) return;
        m_shown = m_wanted;
        const assets::Car* car = catalog.FindCar(m_wanted);
        const assets::CarInfo* info = car ? catalog.carRegister.Find(car->folder) : nullptr;
        if (!car || !info || !info->known || !assets::Catalog::Drivable(*car)) return;
        // (its files through its item: a downloaded patch's PBR maps over RBR's textures)
        m_reading = std::async(std::launch::async, [folder = car->folder, sgc = car->sgc,
                                                    files = assets::CarFilesOf(catalog.Content(), {vfs::Kind::Car, car->id, "", car->version})] {
            Read r;
            r.folder = folder;
            const auto t0 = std::chrono::steady_clock::now();
            try {
                r.model = assets::ReadCarModel(sgc, "gravel", &files);
                r.ok = true;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "the car's model: %s\n", e.what());
            }
            r.readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return r;
        });
    }

    static constexpr int kCarsPerScene = 8;   // cars shown before the scene is loaded afresh (their textures freed)
    int m_carsInScene = 0;

    struct Read {
        std::string folder;
        assets::CarModel model;
        bool ok = false;
        double readMs = 0;
    };
    std::future<Read> m_reading;
    std::unique_ptr<Read> m_pending;   // read, being made the renderer's

    // Three spots from above (render axes, y up): the key at the car's front left, a fill at its right, a rim behind.
    static iface::LightDesc Spot(V3 at, float candela)
    {
        iface::LightDesc l{};
        l.type = iface::kLightSpot;
        l.position[0] = at.x, l.position[1] = at.y, l.position[2] = at.z;
        const V3 d = math::Norm(V3{0, 0.4f, 0} - at);
        l.direction[0] = d.x, l.direction[1] = d.y, l.direction[2] = d.z;
        l.colour[0] = l.colour[1] = l.colour[2] = 1;
        l.intensity = candela, l.radius = 0.6f, l.innerDeg = 22, l.outerDeg = 40;
        return l;
    }
    inline static const iface::LightDesc kLights[3] = {Spot(V3{-3.5f, 6.0f, -4.0f}, 9000), Spot(V3{4.5f, 4.5f, -1.0f}, 3500),
                                                       Spot(V3{0.5f, 5.0f, 5.0f}, 6000)};

    iface::SceneHandle m_scene = 0;
    std::unique_ptr<assets::CarRender> m_car;
    bool m_drawn = false;   // the car on the turntable last frame (Frame: its coming and going drop TAA's history)
    std::string m_wanted, m_shown;
    double m_since = 0;
    physics::Wheel m_wheels[4] = {};
    float m_height = 0.5f;
    std::vector<physics::Part> m_parts;   // (none: the car whole)
    std::vector<iface::InstanceUpdate> m_instances;
};

// One frame of the menus without a stage: the window's messages, `draw`'s ImGui on the backdrop - over `world`'s scene
// when it fills one in (the Garage's showroom). False once the window was closed.
template <class F, class W> bool MenuFrame(Shell& sh, F&& draw, W&& world)
{
    if (!sh.window.Pump()) return false;
    sh.FollowSize();
    sh.ui.NewFrame();
    draw();
    iface::FrameDesc fd{};
    fd.flags = iface::kFrameNoWorld;
    world(fd);
    if (fd.flags & iface::kFrameNoWorld) ui::DrawBackdrop(sh.width, sh.height);   // (ImGui's background: behind all)
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
StageEnd RunStage(Shell& sh, const Options& given, bool fromMenu, std::string& error)
{
    auto fail = [&](const std::string& e) {
        std::fprintf(stderr, "%s\n", e.c_str());
        error = e;
        return StageEnd::Failed;
    };
    // The content layers the run reads through (docs/content-layers.md): the settings' downloads folder and W-Rally's
    // content, the run's RBR folder - the stage's files from its track item (--track), the modules' through them.
    const std::shared_ptr<const vfs::Vfs> content = MakeVfs(sh.settings, given.game);
    Options o = given;
    if (const std::string e = ApplyTrack(o, *content); !e.empty()) return fail(e);
    Window& window = sh.window;
    render::Renderer& renderer = sh.renderer;
    ui::ImGuiLayer& ui = sh.ui;
    const input::Bindings& keys = sh.settings.keys;   // (Settings -> Controls: changes apply at once)
    // The engine's rev limiter for the HUD's shift lights: its EngineInfo, at the car's load (from the physics' thread).
    std::atomic<float> rpmLimit{0};
    // The loading screen (ui::DrawLoading) up while the stage loads, a frame drawn between its steps: in from the left
    // first (~0.36 s), at the end the livery lifted out into the stage. False: the window was closed.
    ui::LoadingInfo loadingInfo;
    {
        auto caps = [](std::string s) {
            for (char& c : s)
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            return s;
        };
        static const char* const kDamage[4] = {"OFF", "SAFE", "REDUCED", "REALISTIC"};
        const uint32_t options = o.session.physics.settings;
        loadingInfo.stage = caps(!o.stageTitle.empty() ? o.stageTitle : o.stage);
        loadingInfo.facts = caps(!o.stageFacts.empty() ? o.stageFacts : o.session.physics.surface);
        loadingInfo.car = caps(!o.carName.empty() ? o.carName : std::filesystem::path(o.session.physics.sgc).stem().string());
        loadingInfo.weather = caps(!o.weatherName.empty() ? o.weatherName : !o.weather.empty() ? o.weather : "stage default");
        loadingInfo.damage = options & 0x10u ? kDamage[std::min(3u, options >> 8 & 0xffu)] : "";
        loadingInfo.setup = caps(!o.setupName.empty() ? o.setupName : o.session.physics.setup.empty() ? "default" : "its own");
    }
    // The stage's splash in the stripe: a UI texture (the scenes come and go under it), released with the run.
    struct Splash {
        render::Renderer& r;
        iface::TextureHandle t = 0;
        ~Splash() { if (t) r.Api()->releaseTexture(r.Instance(), t); }
    } splash{renderer};
    std::string splashPath = o.splash;
    if (splashPath.empty()) {   // a command line's: Tracks.ini's, by the stage's map
        char v[512] = {};
        const std::string sec = "Map" + std::to_string(rules::StageMap(o.stage));
        GetPrivateProfileStringA(sec.c_str(), "SplashScreen", "", v, sizeof v, (o.game + "\\Maps\\Tracks.ini").c_str());
        splashPath = v;
        if (splashPath.size() >= 2 && splashPath.front() == '"') splashPath = splashPath.substr(1, splashPath.find('"', 1) - 1);
    }
    if (!splashPath.empty()) {
        formats::Image img;
        float aspect = 1;
        if (const std::string e = assets::ReadSplash(o.game, splashPath, img, aspect); !e.empty()) {
            std::printf("the stage's splash: %s\n", e.c_str());
        } else if ((splash.t = renderer.CreateTextureRgba(img.rgba.data(), uint32_t(img.width), uint32_t(img.height)))) {
            loadingInfo.splash = {splash.t, int(std::lround(img.height * aspect)), img.height};   // (its shown shape)
        }
    }
    auto loadingFor = [&](int step, float seconds, float leaveFrom, float leaveTo) {
        const auto t0 = std::chrono::steady_clock::now();
        do {
            const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count(), u = seconds > 0 ? std::min(1.0f, t / seconds) : 1;
            const float leave = leaveFrom + (leaveTo - leaveFrom) * u * u;
            if (!MenuFrame(sh, [&] { ui::DrawLoading(ui, loadingInfo, step, leave, sh.width, sh.height); }, [](iface::FrameDesc&) {}))
                return false;
            if (u >= 1) return true;
        } while (true);
    };
    ui::StartMenuEntrance(false);
    if (!loadingFor(0, 0.36f, 0, 0)) return StageEnd::Quit;
    // The modules hear the physics on the bus (before what it connects, which goes first).
    signals::Bus bus;
    // The physics, through the session (a module DLL in modules/physics: --physics, "rbr" by default).
    session::Session sim;
    session::Config simConfig = o.session;
    simConfig.physics.content = content;
    if (const std::string e = sim.Open(simConfig); !e.empty()) return fail(e);
    if (!loadingFor(1, 0, 0, 0)) return StageEnd::Quit;
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
        std::string e = sound.Open(ExeDir() / "modules" / "audio", o.audioId, content);
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
    bus.Connect<signals::EngineInfo>([&rpmLimit](const signals::EngineInfo& e) { rpmLimit = e.rpmLimit; });
    sim.Connect(&bus);
    const std::vector<phys::data::RecMesh> collision =
        phys::data::CarCollisionMeshes(phys::data::ReadSgcCollision(o.session.physics.sgc));

    if (!loadingFor(2, 0, 0, 0)) return StageEnd::Quit;
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
    // --sky: that preset's weather with its clouds, fog and rain replaced - drawn from the WeatherDesc, not the preset.
    iface::WeatherDesc customSky{};
    bool customSkySet = false;
    if (o.skySet) {
        const iface::RendererApi* api = renderer.Api();
        char presetName[64];
        if (api->weatherPresetCount(renderer.Instance(), scene) > static_cast<uint32_t>(weatherPreset) &&
            api->weatherPreset(renderer.Instance(), scene, static_cast<uint32_t>(weatherPreset), &customSky, presetName,
                               sizeof presetName) == iface::Status::Ok) {
            customSky.cloudCover = o.skyClouds;
            customSky.fogVisibility = o.skyFog > 0 ? 6000.0f * std::pow(150.0f / 6000.0f, o.skyFog) : 0.0f;   // 6 km .. 150 m
            customSky.rain = o.skyRain;
            customSkySet = true;
            std::printf("sky: clouds %.2f, fog %.0f m, rain %.2f\n", customSky.cloudCover, customSky.fogVisibility, customSky.rain);
        } else {
            std::printf("sky: the .ngg has no weather presets to start from - its own weather\n");
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
    if (!loadingFor(3, 0, 0, 0)) return StageEnd::Quit;
    render::StageObjects stageObjects;
    assets::CarRender car;
    std::vector<physics::Part> parts;   // the car's parts that can come loose, as the physics loaded them
    sim.Physics().Parts(parts);
    // (its files through its item - --car-item - else beside the .sgc)
    assets::CarFiles carFiles;
    if (!o.carItem.empty()) {
        const size_t at = o.carItem.find('@');
        carFiles = assets::CarFilesOf(content, {vfs::Kind::Car, o.carItem.substr(0, at), "",
                                                at == std::string::npos ? "" : o.carItem.substr(at + 1)});
        if (!carFiles.read) std::printf("--car-item %s: not here - the model's files beside its .sgc\n", o.carItem.c_str());
    }
    car.Create(renderer, o.session.physics.sgc, o.session.physics.surface, collision, parts, &carFiles);
    // The stage's fences (its .fnc), knocked about by the car's hull (assets/fences.h).
    assets::Fences fences;
    if (const std::string e = fences.Create(renderer, o.session.physics.fnc, o.game); !e.empty())
        std::printf("fences: %s\n", e.c_str());
    else if (fences.Posts())
        std::printf("fences: %zu posts, %zu tapes\n", fences.Posts(), fences.Tapes());
    if (!collision.empty()) fences.SetCar(collision[0]);
    std::vector<iface::InstanceUpdate> instances;   // the frame's: the car's, the fences' that moved

    if (!loadingFor(4, 0, 0, 0)) return StageEnd::Quit;
    if (const std::string e = sim.Start(); !e.empty()) return fail(e);
    if (!loadingFor(ui::kLoadingSteps, 0, 0, 0)) return StageEnd::Quit;
    // Loaded: the loading screen stays over the stage's first frames (the first takes its time), then lifts off it -
    // the livery up and out, the rest faded (from the second frame, over 0.4 s).
    std::chrono::steady_clock::time_point leaveFrom{};
    int stageFrames = 0;

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
    // What the beside camera may not go through (camera/scenery.h): the stage's collision, a copy of the cameras' own.
    camera::Scenery scenery;
    if (const std::string e = scenery.Load(o.session.physics.col); !e.empty())
        std::fprintf(stderr, "the cameras' scenery: %s\n", e.c_str());
    const int cameras = tv ? 5 : 4;
    int cameraMode = (o.camera >= 0 ? o.camera : tv ? camera::kTv : camera::kChase) % cameras;
    float orbit = o.cameraAngle;   // the beside camera's place round the car, radians from its left side toward its front
    // The last drawn frame's camera, its TV camera and eye: another mode, another TV camera or an eye 10 m away (R back on
    // the road) is a cut, every history of the old view dropped (iface::FrameDesc camera.cut).
    int drawnMode = -1;
    int32_t drawnTv = -1;
    V3 drawnEye{};
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
        std::printf("test lights (PHYS_DRIVE_TEST_LIGHTS=%s): %s, always lit, in place of the car's own lamps\n", testLightsEnv,
                    testLampOnly ? "the lamp behind" : (std::to_string(testLamps.size()) + " lamps").c_str());
    // THE CAR'S LAMPS (NGG design 5.4 N3b): its own (assets::LoadCarLamps, at the first frame - the centre of gravity
    // is the physics'), switched by the driver (headlights, ramp, indicators, hazard) and by the car (the brake pedal,
    // reverse). Until the driver touches the headlights they follow the sun: lit after dusk - main beam and the ramp -,
    // off by day; and by day the small lamps' light is not given (it would show only on their glass, and costs the GPU
    // what the beams do).
    assets::CarLamps carLamps;
    bool carLampsLoaded = false;
    size_t lampLayout = 0;
    assets::LampSwitches lampSwitch;
    bool lampSwitchSet = false;   // the driver's (or PHYS_DRIVE_LAMPS'): no longer the sun's
    int lampFrames = 0;           // the frames drawn: from the third the stats are the stage's sun
    Press headlightsKey, rampKey, indicatorLeftKey, indicatorRightKey, hazardKey;
    static const char* const kBeams[] = {"off", "dipped", "main"};
    // PHYS_DRIVE_LAMPS=<0|1|2>[,noramp][,brake][,reverse][,left][,right][,hazard]: the switches from the start, the
    // brake and reverse lamps held lit - for captures, which have neither a pedal nor a gear.
    const char* const lampsEnv = std::getenv("PHYS_DRIVE_LAMPS");
    const bool lampsForced = lampsEnv && *lampsEnv;
    if (lampsForced) {
        lampSwitch.beam = std::clamp(std::atoi(lampsEnv), 0, 2);
        lampSwitch.ramp = !std::strstr(lampsEnv, "noramp");
        lampSwitch.hazard = std::strstr(lampsEnv, "hazard") != nullptr;
        lampSwitch.indicator = std::strstr(lampsEnv, "left") ? -1 : std::strstr(lampsEnv, "right") ? 1 : 0;
        lampSwitchSet = true;
    }
    float chaseDistance = o.session.chaseDistance;
    bool chaseChanged = false;     // reported once the key is let go
    const uint32_t jitterTo = std::getenv("PHYS_DRIVE_JITTER") ? std::strtoul(std::getenv("PHYS_DRIVE_JITTER"), nullptr, 0) : 0;
    const uint32_t shotEvery = std::getenv("PHYS_DRIVE_SHOTS") ? std::strtoul(std::getenv("PHYS_DRIVE_SHOTS"), nullptr, 0) : 0;
    uint32_t shotTick = 0;
    // PHYS_DRIVE_FRAMEMS: every drawn frame's whole GPU time (RenderStats::frameMs), its mean and median printed at the
    // end - how a renderer setting is judged (design 5.4 N2c), from a replay under --step.
    const bool frameMsLog = std::getenv("PHYS_DRIVE_FRAMEMS") != nullptr;
    std::vector<float> frameMsSeen;
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
    // What the pause menu tells of the run: Quick rally's names, else the command line's files.
    auto pauseInfo = [&] {
        ui::PauseInfo p;
        p.stage = !o.stageTitle.empty() ? o.stageTitle : o.stage;
        p.car = !o.carName.empty() ? o.carName : std::filesystem::path(o.session.physics.sgc).stem().string();
        p.surface = o.session.physics.surface;
        p.weather = !o.weatherName.empty() ? o.weatherName : !o.weather.empty() ? o.weather : "stage default";
        const uint32_t options = o.session.physics.settings;
        p.damage = options & 0x10u ? static_cast<int>(options >> 8 & 0xffu) : -1;
        p.time = snap.clock.Time(), p.splits = snap.clock.Splits(), p.falseStart = snap.clock.FalseStart();
        p.distance = snap.distance, p.finish = snap.clock.Finish();
        return p;
    };

    QueryPerformanceCounter(&last);
    bool quit = false;
    StageEnd end = StageEnd::Quit;
    // A quit (the close box, the menu, --exit-at, a fault) still finishes its frame - its steps are in the recording.
    while (!quit) {
        if (!window.Pump()) quit = true, end = StageEnd::Quit;
        // Esc: the menu up, down again, or a step back from its Settings / Debug (the settings kept as they are).
        const UINT keyDown = window.TakeKeyDown();   // (for Settings -> Controls)
        // (The pause menu's question takes Esc first: no.)
        if (window.TakeEscape() && !ui::CapturingKey() && !(menu == StageMenu::Pause && ui::PauseEscape())) {
            if (menu == StageMenu::Settings) SaveSettings(sh.settings);
            if (menu == StageMenu::None) ui::OpenPause();
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
        // PHYS_DRIVE_CAMERA_AT=<tick>:<camera>: that camera from that tick, as if C were pressed - a cut for captures.
        static const char* const cameraAt = std::getenv("PHYS_DRIVE_CAMERA_AT");
        static bool cameraAtDone = false;
        if (cameraAt && *cameraAt && !cameraAtDone && snap.tick >= std::strtoul(cameraAt, nullptr, 0)) {
            const char* const colon = std::strchr(cameraAt, ':');
            cameraMode = (colon ? std::atoi(colon + 1) : cameraMode + 1) % cameras;
            cameraAtDone = true;
            std::printf("camera %d at tick %u (PHYS_DRIVE_CAMERA_AT)\n", cameraMode, snap.tick);
        }
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
        // The lamps' switches (assets::LampSwitches).
        if (headlightsKey(focused && keys.Down(Action::Headlights))) {
            lampSwitch.beam = (lampSwitch.beam + 1) % 3;
            lampSwitchSet = true;
            std::printf("headlights: %s\n", kBeams[lampSwitch.beam]);
        }
        if (rampKey(focused && keys.Down(Action::Ramp))) {
            lampSwitch.ramp = !lampSwitch.ramp;
            std::printf("ramp %s\n", lampSwitch.ramp ? "on (with the main beam)" : "off");
        }
        if (indicatorLeftKey(focused && keys.Down(Action::IndicatorLeft))) lampSwitch.indicator = lampSwitch.indicator < 0 ? 0 : -1;
        if (indicatorRightKey(focused && keys.Down(Action::IndicatorRight))) lampSwitch.indicator = lampSwitch.indicator > 0 ? 0 : 1;
        if (hazardKey(focused && keys.Down(Action::Hazard))) lampSwitch.hazard = !lampSwitch.hazard;
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
        if (car.Ready()) {
            instances.push_back(carUpdate);
            // The lamps' glass, lit or not, by the switches (last frame's: the brake pedal and the gear are read below).
            const bool on = carLampsLoaded && !testLights_;
            bool lit[assets::kLampGroups] = {};
            lit[assets::kLampHead] = lit[assets::kLampTail] = on && lampSwitch.beam > 0;
            lit[assets::kLampBrake] = on && lampSwitch.brake;
            lit[assets::kLampReverse] = on && lampSwitch.reverse;
            lit[assets::kLampRamp] = on && lampSwitch.beam == 2 && lampSwitch.ramp;
            const bool housing = carLampsLoaded && !testLights_ && carLamps.layouts[lampLayout].name == "ramp";
            car.PlaceLamps(lit, housing, parts, instances);
        }
        for (const iface::InstanceUpdate& u : fences.Step(in.seconds, p, snap.centerOfGravity)) instances.push_back(u);
        const V3 chaseEye = snap.before.chase.eye + (snap.after.chase.eye - snap.before.chase.eye) * t;
        const camera::View view = camera::Look(cameraMode, p, chaseEye, orbit, tv.get(), &scenery);
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
        ui::DrawDashboard(ui, snap.dash, rpmLimit, shownWidth, shownHeight);
        ui::DrawStageClock(ui, snap.clock, shownWidth, shownHeight);
        if (live) {   // (the live game's): the call being spoken, and the next when close at the speed
            std::string current, next;
            codriver.Upcoming(snap.distance, std::fabs(snap.dash.speed), o.pacenoteNext, current, next);
            ui::DrawPacenotes(ui, current, next, shownWidth, shownHeight);
        }
        if (++stageFrames <= 2) leaveFrom = std::chrono::steady_clock::now();   // (the first frames: the screen held)
        if (const float leave = std::chrono::duration<float>(std::chrono::steady_clock::now() - leaveFrom).count() / 0.4f; leave < 1)
            ui::DrawLoading(ui, loadingInfo, ui::kLoadingSteps, stageFrames <= 2 ? 0.0f : leave * leave, shownWidth, shownHeight);
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
                 st.ambientOcclusion ? st.aoSource : "off", st.taa ? "on" : "off",
                 kQuality[st.quality > 2 ? 1 : st.quality]);
            const bool aoTraced = st.ambientOcclusion && std::strncmp(st.aoSource, "ray", 3) == 0;
            if (st.rtActive)
                line("   rt %.2f ms (ao %.2f, lights %.2f, denoise %.2f)   %u px, %.1f%% shadowed   %u lights", st.rtMs,
                     aoTraced ? st.aoMs : 0.0f, st.lightsMs, st.denoiseMs, st.rtPixels, st.rtPixels ? 100.0 * st.rtShadowed / st.rtPixels : 0.0,
                     st.lightCount);
            else if (st.rtFallback[0])
                line("   shadows from cascades: %s", st.rtFallback);
            if (st.ambientOcclusion && !aoTraced)
                line("   ao %.2f ms (%s)", st.aoMs, st.aoSource);
            if (st.upscaler[0])
                line("upscaled by %s from %ux%u (%.0f%%)", st.upscaler, st.renderWidth, st.renderHeight,
                     shownWidth ? 100.0 * st.renderWidth / shownWidth : 0.0);
            else
                line("native %ux%u, %s", st.renderWidth, st.renderHeight, st.taa ? "TAA" : "no TAA");
            if (st.autoExposure)
                line("exposure x%.2f   metered %+.2f EV   adapted %+.2f EV", st.exposure, st.meteredEv, st.adaptedEv);
            else
                line("exposure x%.2f (manual)", st.exposure);
            line("V view %u: %s", st.debugView, renderer.DebugViewName(static_cast<int32_t>(st.debugView)));
            if (carLampsLoaded)
                line("lamps: %s, the ramp %s (%s)%s%s%s   %s %s / %s ramp / %s %s indicators / %s hazard", kBeams[lampSwitch.beam],
                     lampSwitch.ramp ? "on" : "off", carLamps.layouts[lampLayout].name.c_str(), lampSwitch.brake ? ", brake" : "",
                     lampSwitch.reverse ? ", reverse" : "", lampSwitch.hazard ? ", hazard" : lampSwitch.indicator < 0 ? ", left"
                     : lampSwitch.indicator > 0 ? ", right" : "", input::KeyName(keys[Action::Headlights]).c_str(), "lights",
                     input::KeyName(keys[Action::Ramp]).c_str(), input::KeyName(keys[Action::IndicatorLeft]).c_str(),
                     input::KeyName(keys[Action::IndicatorRight]).c_str(), input::KeyName(keys[Action::Hazard]).c_str());
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
            switch (ui::PauseMenu(ui, pauseInfo(), shownWidth, shownHeight)) {
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
            const ui::SettingsChoice c = ui::SettingsUi(ui, sh.settings, true, settingsLog, keyDown, shownWidth, shownHeight, {},
                                                              renderer.Stats().upscalersAvailable);
            if (c == ui::SettingsChoice::Graphics) {
                sh.window.SetFullscreen(sh.settings.fullscreen);
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
        if (frameMsLog && !paused)
            if (const float ms = renderer.Stats().frameMs; ms > 0.0f) frameMsSeen.push_back(ms);
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
            {
                const V3 d = view.eye - drawnEye;
                const bool cut = cameraMode != drawnMode || view.tvCamera != drawnTv || d.x * d.x + d.y * d.y + d.z * d.z > 100.0f;
                fd.camera.cut = cut ? 1u : 0u;
                drawnMode = cameraMode, drawnTv = view.tvCamera, drawnEye = view.eye;
            }
            fd.weatherPreset = customSkySet ? -1 : weatherPreset;   // the .ngg's preset (0 its own weather), or --sky's
            if (customSkySet) fd.weather = customSky;
            fd.wetness = 0.5f * static_cast<float>(std::clamp(wet, 0, 2));   // the road's: dry 0, damp 0.5, wet 1
            if (o.skyTimeSet) {   // the time and place: the sun from them (NGG design 5.4)
                fd.skyTime = {1, o.skyYear, o.skyMonth, o.skyDay, static_cast<float>(o.skyHour), static_cast<float>(o.skyUtcOffset),
                              static_cast<float>(o.skyLatitude), static_cast<float>(o.skyLongitude), static_cast<float>(o.skyNorth)};
            }
            // PHYS_DRIVE_TEST_LIGHTS: the test lamps on the car at its pose (above), or the lamp above and behind it.
            std::vector<iface::LightDesc> testLights;
            for (const assets::Lamp& lamp : testLamps) testLights.push_back(assets::LampLight(lamp, p, !testNoShadow));
            if (!testLights_) {
                if (!carLampsLoaded) {
                    carLamps = assets::LoadCarLamps(o.session.physics.sgc, snap.centerOfGravity);
                    carLampsLoaded = true;
                    lampLayout = carLamps.defaultLayout;
                    for (size_t k = 0; k < carLamps.layouts.size(); ++k)
                        if (carLamps.layouts[k].name == o.lampLayout) lampLayout = k;
                    std::printf("the car's lamps: %s places; the ramp \"%s\", %zu lamps\n", carLamps.fromCar ? "its own" : "default",
                                carLamps.layouts[lampLayout].name.c_str(), carLamps.layouts[lampLayout].lamps.size());
                }
                // The sun the renderer drew last frame: lit after dusk (and the small lamps' light given) - every frame
                // until the driver switches them, so not latched from a frame that still had the menu's showroom sky
                // (a night). Its first two frames' stats may be the menu's: off until then.
                const float sun = renderer.Stats().sunElevationDeg;
                if (!lampSwitchSet) {
                    const int beam = ++lampFrames > 2 && sun < 3.0f ? 2 : 0;
                    if (beam != lampSwitch.beam) std::printf("headlights: %s (the sun %.1f degrees up)\n", kBeams[beam], sun);
                    lampSwitch.beam = beam;
                }
                lampSwitch.smallLamps = sun < 6.0f;
                lampSwitch.brake = in.brake || (lampsForced && std::strstr(lampsEnv, "brake"));
                lampSwitch.reverse = snap.dash.gear < 0 || (lampsForced && std::strstr(lampsEnv, "reverse"));
                lampSwitch.seconds += in.seconds;
                assets::LampMounts mounts;
                car.Mounts(parts, mounts);   // a lamp on a part that came off is dark; on one that hangs, it moves with it
                assets::CarLights(carLamps, lampLayout, o.lampSpread, o.lampPitch, lampSwitch, p, testLights, &mounts);
            }
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
            fd.reflectionFocus = car.Ready() ? carUpdate.instance : 0;   // the world in its paint
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
            std::swprintf(text, 128, L"W-Rally - %.0f km/h - tick %u - %.0f fps", speed * 3.6f, snap.tick, frame > 0 ? 1.0 / frame : 0.0);
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
    if (frameMsLog && frameMsSeen.size() > 30) {
        std::vector<float> v(frameMsSeen.begin() + 30, frameMsSeen.end());   // (the first second: loading, warming)
        double sum = 0;
        for (float ms : v) sum += ms;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        std::printf("gpu frame: mean %.2f ms, median %.2f ms over %zu frames\n", sum / v.size(), v[v.size() / 2], v.size());
    }
    window.SetTitle(L"W-Rally");
    return end;
}

// The physics modules installed, as the catalog takes them (the cars' physics variants: which ones can drive them).
std::vector<assets::PhysicsModule> CatalogModules()
{
    std::vector<assets::PhysicsModule> out;
    for (const physics::ModuleInfo& m : physics::ListModules(ExeDir() / "modules" / "physics")) {
        assets::PhysicsModule c;
        c.id = m.id, c.name = m.name, c.simVersion = m.simVersion;
        for (const physics::ModuleInfo::Format& f : m.formats) c.formats.push_back({f.name, f.minVersion, f.maxVersion});
        out.push_back(std::move(c));
    }
    return out;
}

// Quick rally's choice as a command line - a run from the menu is one too. Empty, and `error`, when it cannot be.
std::vector<std::string> QuickArgs(const Settings& s, const assets::Catalog& catalog, std::string& error)
{
    const assets::Stage* st = catalog.FindStage(s.stage);
    const assets::Car* car = catalog.FindCar(s.car);
    if (!st || !car) return error = "choose a stage and a car", std::vector<std::string>{};
    const assets::PhysicsVariant* physics = catalog.PhysicsFor(*car, s);
    if (!physics) return error = car->name + " has no physics a physics module drives", std::vector<std::string>{};
    // The stage from its track item, the car's model on disk (resolved through its versions), its physics (the variant
    // chosen, its base, the first module that drives it) and the setup as VFS paths (docs/content-layers.md).
    std::vector<std::string> a = {"W-Rally", s.rbrPath, st->ngg, "--stage", st->stage, "--track", st->track + "@" + st->version,
                                  "--sgc", car->sgc, "--car-item", car->id + "@" + car->version, "--car", physics->folder, "--physics-base", physics->base,
                                  "--physics", physics->modules.front(), "--damage", std::to_string(std::min(s.damage, 3u)),
                                  "--out", (UserDir() / "drive.phr").string()};
    if (!s.setup.empty()) a.insert(a.end(), {"--setup", s.setup});
    if (!s.weather.empty()) a.insert(a.end(), {"--weather", s.weather});
    // The road, the time of day and the sky: a preset's own, or the custom weather's over it.
    static const char* const kRoad[3] = {"dry", "damp", "wet"};
    const uint32_t road = s.weatherCustom ? std::min(s.road, 3u) : 0;
    a.insert(a.end(), {"--road", kRoad[road ? road - 1 : assets::RoadOfWeather(s.weather)]});
    const QuickDay day = QuickDate(s.season);
    if (s.weatherCustom && !s.time.empty()) a.insert(a.end(), {"--time", s.time, "--date", day.date, "--utc-offset", day.utcOffset});
    if (s.weatherCustom) {   // the preset's sky with these clouds, fog and rain
        char sky[96];
        std::snprintf(sky, sizeof sky, "%g,%g,%g", s.clouds, s.fog, s.rain);
        a.insert(a.end(), {"--sky", sky});
    }
    if (const auto it = s.lamps.find(car->folder); it != s.lamps.end()) {
        if (!it->second.layout.empty()) a.insert(a.end(), {"--lamps", it->second.layout});
        char aim[64];
        std::snprintf(aim, sizeof aim, "%g,%g", it->second.spread, it->second.pitch);
        a.insert(a.end(), {"--lamp-aim", aim});
    }
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

// The main menu and its screens until Exit or the window closes. Returns the exit code. `intro`: the game's start - the
// intro up while the content loads (a step a frame), then the menu's entrance.
int Menus(Shell& sh, std::string note, bool intro)
{
    assets::Catalog catalog;
    // The content layers (docs/content-layers.md), made again with every scan; the catalog is read through them.
    auto scan = [&] {
        std::shared_ptr<const vfs::Vfs> content = MakeVfs(sh.settings);
        std::printf("%s", DescribeVfs(*content).c_str());
        catalog.physicsModules = CatalogModules();
        const std::string e = catalog.Scan(sh.settings, std::move(content));
        std::printf("content: %zu stages, %zu cars%s%s\n", catalog.stages.size(), catalog.cars.size(), e.empty() ? "" : " - ", e.c_str());
        return e;
    };
    std::string scanned;
    Screen screen = Screen::Main;
    std::string settingsLog;
    Settings atOpen;   // the settings as the Settings screen opened (the content changed: scanned again)
    // The main menu's video loop (content/video/menu.mp4; none: the window dark).
    ui::MenuVideo video;
    Showroom showroom;
    struct Unload {   // (the showroom's scene gone before the renderer)
        Showroom& s;
        render::Renderer& r;
        ~Unload() { s.Unload(r); }
    } unloadShowroom{showroom, sh.renderer};
    // The loading: the content scanned, the video opened, the showroom's scene loaded (its load stalls once, now).
    static const char* const kDoing[] = {"READING THE STAGES AND THE CARS", "OPENING THE MENU'S VIDEO", "SETTING UP THE GARAGE"};
    constexpr int kSteps = int(std::size(kDoing));
    auto load = [&](int step) {
        if (step == 0) scanned = scan();
        else if (step == 1) {
            if (const std::string e = video.Open(WRallyFile("content/video/menu.mp4")); !e.empty()) std::printf("%s\n", e.c_str());
        } else showroom.Prepare(sh.renderer);
    };
    if (!intro) {
        for (int i = 0; i < kSteps; ++i) load(i);
    } else {
        // The intro drawn, then a step loaded - each one's frame up while it runs; held 4 s at least (Enter, Esc or a
        // click skip once it is loaded), then 0.4 s out to the ground, and the menu's entrance.
        const auto t0 = std::chrono::steady_clock::now();
        auto since = [](auto t) { return std::chrono::duration<float>(std::chrono::steady_clock::now() - t).count(); };
        int done = 0;
        float fadeAt = -1;   // when the fade out began (s since t0; -1 not yet)
        for (;;) {
            const float t = since(t0);
            const bool ready = done == kSteps;
            bool skip = false;
            const float alpha = fadeAt < 0 ? 1.0f : std::max(0.0f, 1 - (t - fadeAt) / 0.4f);
            if (!MenuFrame(sh, [&] { skip = ui::DrawIntro(sh.ui, sh.width, sh.height, float(done) / kSteps, ready ? "READY" : kDoing[done], ready, alpha); },
                           [](iface::FrameDesc&) {}))
                return 0;
            if (!ready) {
                load(done++);
                continue;
            }
            if (fadeAt < 0 && (skip || t >= 4.0f)) fadeAt = t;
            if (fadeAt >= 0 && t - fadeAt >= 0.4f) break;
        }
        sh.window.TakeEscape(), sh.window.TakeKeyDown();   // (a skip's key is not the menu's)
        ui::StartMenuEntrance();
    }
    if (note.empty()) note = scanned;
    for (;;) {
        video.Update(sh.renderer);   // (outside the frame: the upload is its own)
        bool start = false, exit = false;
        const UINT keyDown = sh.window.TakeKeyDown();   // (for Settings -> Controls)
        const bool capturing = ui::CapturingKey();      // (Esc then the binding's, not the menu's)
        const bool open = MenuFrame(sh, [&] {
            switch (screen) {
            case Screen::Main:
                switch (ui::MainMenu(sh.ui, sh.width, sh.height, note, video.Frame())) {
                case ui::MainChoice::QuickRally: screen = Screen::Quick; break;
                case ui::MainChoice::Garage: screen = Screen::Quick, ui::OpenGarage(); break;
                case ui::MainChoice::Settings: screen = Screen::Settings, atOpen = sh.settings, settingsLog.clear(); break;
                case ui::MainChoice::Exit: exit = true; break;
                case ui::MainChoice::None: break;
                }
                break;
            case Screen::Quick:
                switch (ui::QuickRally(sh.ui, catalog, sh.settings, sh.width, sh.height, video.Frame())) {
                case ui::QuickChoice::Start: start = true; break;
                case ui::QuickChoice::Back: screen = Screen::Main, SaveSettings(sh.settings); break;
                case ui::QuickChoice::None: break;
                }
                break;
            case Screen::Settings:
                switch (ui::SettingsUi(sh.ui, sh.settings, false, settingsLog, keyDown, sh.width, sh.height, video.Frame(),
                                   sh.renderer.Stats().upscalersAvailable)) {
                case ui::SettingsChoice::Graphics:
                    sh.window.SetFullscreen(sh.settings.fullscreen);
                    sh.renderer.ApplySettings(ToRender(sh.settings));
                    break;
                case ui::SettingsChoice::Rescan: {
                    const std::string e = scan();
                    settingsLog = std::to_string(catalog.stages.size()) + " stages, " + std::to_string(catalog.cars.size()) + " cars" +
                                  (e.empty() ? "" : "\n" + e);
                    break;
                }
                case ui::SettingsChoice::Back:
                    if (const std::string e = SaveSettings(sh.settings); !e.empty()) std::fprintf(stderr, "%s\n", e.c_str());
                    if (sh.settings.rbrPath != atOpen.rbrPath || sh.settings.downloadPath != atOpen.downloadPath ||
                        sh.settings.nggFolders != atOpen.nggFolders)
                        scanned = scan();
                    note = scanned;
                    screen = Screen::Main;
                    break;
                case ui::SettingsChoice::None: break;
                }
                break;
            }
            // Esc: back, the settings kept as they are (Quick rally's own: its pages step back to it first).
            if (sh.window.TakeEscape() && !capturing && screen == Screen::Settings) {
                SaveSettings(sh.settings);
                screen = Screen::Main;
            }
        }, [&](iface::FrameDesc& fd) {   // the Garage's car under it
            showroom.Frame(sh.renderer, catalog, screen == Screen::Quick ? ui::Showroom() : ui::ShowroomView{}, sh.width, sh.height, fd);
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
        o.stageTitle = catalog.FindStage(sh.settings.stage)->title;
        o.carName = catalog.FindCar(sh.settings.car)->name;
        o.weatherName = sh.settings.weatherCustom ? "custom weather" : sh.settings.weather.empty() ? "stage default" : sh.settings.weather;
        {   // the loading screen's line: the country, the surface, the length (as the register knows the track)
            const assets::Stage* st = catalog.FindStage(sh.settings.stage);
            std::string facts;
            if (const assets::TrackInfo* t = catalog.trackRegister.Find(st->map)) {
                o.splash = t->splash;
                for (const std::string& s : {t->country, t->surface}) facts += s.empty() ? "" : (facts.empty() ? "" : " \xC2\xB7 ") + s;
                if (t->lengthKm > 0) {
                    char km[32];
                    std::snprintf(km, sizeof km, "%.1f km", t->lengthKm);
                    facts += (facts.empty() ? "" : " \xC2\xB7 ") + std::string(km);
                }
            }
            o.stageFacts = facts;
            o.setupName = "default";
            const assets::Car& car = *catalog.FindCar(sh.settings.car);
            for (const assets::Setup& su : catalog.SetupsFor(car, catalog.PhysicsFor(car, sh.settings)))
                if (su.path == sh.settings.setup) o.setupName = su.name;
        }
        std::string line;
        for (const std::string& a : args) line += (line.empty() ? "" : " ") + a;
        std::printf("quick rally: %s\n", line.c_str());
        StageEnd end;
        showroom.Unload(sh.renderer);   // (the stage's scene in its place)
        while ((end = RunStage(sh, o, true, error)) == StageEnd::Restart) {}
        showroom.Prepare(sh.renderer);
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
    // --showroom <car folder> [--turn <degrees>]: the Garage's showroom with that car, one capture (showroom.bmp, in the
    // working folder) once it is loaded, then out - a look at a car's model without the menus.
    if (const auto it = std::find(args.begin(), args.end(), std::string("--showroom")); it != args.end() && it + 1 != args.end()) {
        if (const char* off = std::getenv("PHYS_DRIVE_OFF")) {   // as a stage's
            if (std::strstr(off, "taa")) sh.settings.taa = false;
            if (std::strstr(off, "ao")) sh.settings.quality = 0;
            if (std::strstr(off, "shadows")) sh.settings.shadows = false;
            if (std::strstr(off, "rt")) sh.settings.rayTracing = false;
        }
        if (const std::string e = OpenShell(sh, "wrenderer"); !e.empty()) return std::fprintf(stderr, "%s\n", e.c_str()), 3;
        assets::Catalog catalog;
        catalog.physicsModules = CatalogModules();
        catalog.Scan(sh.settings);
        ui::ShowroomView view;
        view.car = *(it + 1);
        if (const auto t = std::find(args.begin(), args.end(), std::string("--turn")); t != args.end() && t + 1 != args.end())
            view.turn = static_cast<float>(std::atof((t + 1)->c_str()) * 3.14159265 / 180);
        // --spin <degrees / s>: the turntable turning, a 60th of that a frame (the Garage turns at 12.6); --shots <n>:
        // n captures, frame after frame (showroom-00.bmp ...) - the denoisers' grain around a moving car, measured.
        float spin = 0;
        int shots = 1;
        if (const auto t = std::find(args.begin(), args.end(), std::string("--spin")); t != args.end() && t + 1 != args.end())
            spin = static_cast<float>(std::atof((t + 1)->c_str()) * 3.14159265 / 180 / 60);
        if (const auto t = std::find(args.begin(), args.end(), std::string("--shots")); t != args.end() && t + 1 != args.end())
            shots = std::clamp(std::atoi((t + 1)->c_str()), 1, 100);
        // --then <car folder>: the carousel stepping on to that car a quarter of the way through the shots, which go on
        // over the empty turntable - what the car taken off leaves behind.
        std::string then;
        if (const auto t = std::find(args.begin(), args.end(), std::string("--then")); t != args.end() && t + 1 != args.end())
            then = *(t + 1);
        // --stop <shot>: the spin stopped at that shot, as a mouse letting go (the edges as the car settles); --jerk <k>:
        // the spin given every k-th frame, k frames' worth at once, as a mouse's moves come in bursts.
        int stop = -1, jerk = 1;
        if (const auto t = std::find(args.begin(), args.end(), std::string("--stop")); t != args.end() && t + 1 != args.end())
            stop = std::atoi((t + 1)->c_str());
        if (const auto t = std::find(args.begin(), args.end(), std::string("--jerk")); t != args.end() && t + 1 != args.end())
            jerk = std::max(1, std::atoi((t + 1)->c_str()));
        Showroom showroom;
        int settle = 0;
        // PHYS_DRIVE_FRAMEMS: the captured frames' whole GPU time (RenderStats::frameMs), mean and median, as a stage's.
        const bool frameMsLog = std::getenv("PHYS_DRIVE_FRAMEMS") != nullptr;
        std::vector<float> frameMsSeen;
        for (int frame = 0; frame < 400 + shots && sh.window.Pump(); ++frame) {
            sh.FollowSize();
            sh.ui.NewFrame();
            iface::FrameDesc fd{};
            fd.flags = iface::kFrameNoWorld;
            const bool drawn = showroom.Frame(sh.renderer, catalog, view, sh.width, sh.height, fd);
            fd.ui = sh.ui.Render();
            if (drawn && (showroom.Ready() || settle >= 40)) {
                if (!then.empty() && settle == 40 + shots / 4) view.car = then;
                if (stop < 0 || settle < 40 + stop) view.turn += settle % jerk == 0 ? spin * jerk : 0.0f;
                if (++settle >= 40 && settle < 40 + shots) {
                    char name[32];
                    std::snprintf(name, sizeof name, "showroom-%02d.bmp", settle - 40);
                    sh.renderer.CaptureFrame(shots == 1 ? "showroom.bmp" : name);
                }
                if (settle == 41 + shots) break;
            }
            const auto r0 = std::chrono::steady_clock::now();
            sh.renderer.RenderFrame(fd);
            sh.renderer.Present();
            if (const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
                std::getenv("WRALLY_SHOWROOM_TIMES") && ms > 30)
                std::printf("the showroom: frame %d drawn in %.0f ms\n", frame, ms);
            if (frameMsLog && settle > 40)
                if (const float ms = sh.renderer.Stats().frameMs; ms > 0.0f) frameMsSeen.push_back(ms);
            if (const std::string log = sh.renderer.TakeLog(); sh.rendererLog && !log.empty()) std::printf("%s", log.c_str());
        }
        if (!frameMsSeen.empty()) {
            std::vector<float> v = frameMsSeen;
            double sum = 0;
            for (float ms : v) sum += ms;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            std::printf("gpu frame: mean %.2f ms, median %.2f ms over %zu frames\n", sum / v.size(), v[v.size() / 2], v.size());
        }
        showroom.Unload(sh.renderer);
        return 0;
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
        return Menus(sh, "", false);
    }
    if (const std::string e = OpenShell(sh, "wrenderer"); !e.empty()) return std::fprintf(stderr, "%s\n", e.c_str()), 3;
    return Menus(sh, "", true);
}

} // namespace wrally::app
