# WRally — architecture design

Status: draft 3, 2026-10-25. Interfaces: [`include/wrally/iface/`](../include/wrally/iface/).
Decisions taken so far are in §9.

## 0. Goal

A standalone rally game that:

- **runs on its own content**, with no RBR install needed: WRally stages, cars and sounds in WRally's own formats;
- **also runs everything RBR has**, when WRally is placed in an RBR folder (or pointed at one). It then reads RBR's
  stages, cars, physics data, sounds and replays directly, alongside its own content;
- is **modular**: the game executable plus swappable DLLs for the renderer, the physics and the audio. Several
  physics modules can sit side by side (original RBR, WPhysics, others later), and the player or an event picks one.

C++20, x64 only, Windows first. The top namespace is `wrally`.

### What we start from

**The game grows out of `phys-drive`** (`plugin-rbr/phys/drive/`). It is already a working, minimal version of the
game loop: it loads a stage and a car from RBR's files, drives the car from the keyboard with the native physics,
draws through the NGG renderer DLL (`ngg-api.dll`), plays the engine sound (`audio/`) from the physics' records,
runs RBR's stage clock (countdown, false start, splits, finish), switches cameras (including the stage's TV
cameras), records `.phr` runs and replays them. WRally keeps that working at every step while it is split into the
exe and modules (§8).

| existing (plugin-rbr) | becomes |
|---|---|
| **`phys/drive/drive.cpp`** — the phys-drive main loop | **WRally.exe**: `wrally::app` / `session`, split by §5.5 |
| `phys/drive/car_model` — `.sgc` → render parts | `wrally::assets` car model → `iface::ModelDesc` |
| `phys/drive/replay_camera` | `wrally::camera` |
| `phys/drive/launch_control.h`, `anti_lag.h` | game's driver layer (`wrally::session`), emits `DriverTick` |
| `phys/drive/sound-mix.ini` | the audio module's per-car mix (`CarSoundDesc::mix`) |
| `audio/` (game-audio: grains, bank, mix, XAudio2 device) | **WAudio.dll** core |
| `ngg/core` — D3D12 renderer: PBR, sky/atmosphere, RT shadows + AO, TAA, weather | **WRenderer.dll** core |
| `ngg/api` — flat C API, already loaded by phys-drive as a DLL | template for the renderer interface (game-shaped) |
| `ngg/format` — `.ngg` container | shared `wrally::formats::ngg` |
| `phys/native` + `phys/num` + `phys/data` — native RBR physics, bit-exact with the game | **WPhysicsRBR.dll** |
| `phys/sim/sim.h` `Physics` class, `NativeSim`, `StageClock` | physics interface template; `StageClock` → `wrally::rules` |
| `phys/data` — readers for `.lsp .rbz .trk .col .dls .lbs .sgc .rpl .phr` | shared `wrally::formats::rbr` |
| `signals/` — record bus | `wrally::signals` + the pinned record layout in `iface/records.h` |

**WRally is self-contained.** The plugin-rbr code it uses is copied into this repository, not referenced from
it. [imported.md](imported.md) lists what came from where.

`ngg/bridge`, `ngg/ipc` and the d3d9 hooking are **not** carried over. They exist only to render inside
RichardBurnsRally_SSE.exe, and WRally owns its own window. The classic `phys::sim::Sim` (the exe image in a 4 GB
arena) is not carried over either. It stays in plugin-rbr as the oracle that the native port is checked against.

---

## 1. Process and modules

```
 WRally.exe  (wrally::app)  ─────────────────────────────────────────────────────────────
 │  window, input, GUI (ImGui), game flow, rules, timing, replays, VFS, assets, config
 │
 │  ModuleHost: scans modules/, GetWInterfaceVer → InitWInterface → function tables
 │
 ├── modules/renderer/WRenderer.dll         RendererApi   (ngg-core inside)
 │                     dxcompiler.dll, dxil.dll  (WRenderer's own dependencies, next to it)
 ├── modules/physics/WPhysicsRBR.dll        PhysicsApi    id "rbr"       (phys/native)
 ├── modules/physics/WPhysics.dll           PhysicsApi    id "wphysics"  (fork of rbr, extended, JSON config)
 ├── modules/physics/<future>.dll           PhysicsApi
 └── modules/audio/WAudio.dll               AudioApi      (audio/ inside, XAudio2)
```

All modules run in one 64-bit process that the game owns. No IPC is needed.

### Responsibilities

| | owns | never does |
|---|---|---|
| **Game** | window and message loop, input devices and FFB, menus/HUD, game flow, stage timing and rules, the driver layer (launch control, anti-lag), career/events, replays, the VFS, reading **all** shared file formats, config | GPU work, integrating the car, mixing sound |
| **Renderer** | the device and swap chain on the game's window, `.ngg` scenes, models, frame recording, post, present, the UI pass | reading RBR files, knowing what a car is |
| **Physics** | the world (stage collision, solids, cars), stepping, the car's state and records, its own config and car files | rendering, timing the stage, reading files except through the VFS |
| **Audio** | the output device and mixer thread, the car's sound from the records, one-shots, buses, listener | deciding *when* game sounds play (the game does), reading files except through the VFS |

Two rules carry over from NGG design §8.0. The renderer knows nothing about RBR. A module includes only
`wrally/iface/*`, never the game's headers or another module's.

---

## 2. Module ABI

The full contract is in [`common.h`](../include/wrally/iface/common.h). In short:

### 2.1 The two exports

```cpp
WRALLY_EXPORT uint32_t       WRALLY_CALL GetWInterfaceVer(ModuleKind kind);
WRALLY_EXPORT iface::Status  WRALLY_CALL InitWInterface(uint32_t gameVersion,
                                                       const StructHeader* in, StructHeader* out);
```

- **`GetWInterfaceVer(kind)`** returns the interface version the DLL implements for `kind`, or 0 if it does not
  implement that kind. It has no side effects, so it is safe to call on any DLL in the folder. It takes the kind so
  that the game can tell renderer, physics and audio DLLs apart without relying on folders, and so that one DLL could
  provide two kinds.
- **`InitWInterface(gameVersion, in, out)`**: `in` is the kind's input struct (`RendererInput` / `PhysicsInput` /
  `AudioInput`): the game's version, `HostServices` (log, allocator, VFS, clock), and the kind-specific data (the
  HWND for the renderer, the config file for the physics, the device settings for the audio). `out` is the kind's
  output struct: `ModuleInfo` (id, name, version, build hash, capability bits), the function table, and the instance.

### 2.2 Versioning

- One counter per kind: `kRendererInterfaceVer`, `kPhysicsInterfaceVer`, `kAudioInterfaceVer`. **Any** change to a
  struct or function table of that kind increases it by one. Adding a function to the end of a table counts too,
  because tables are not extensible in place by design.
- The game uses a DLL **only if its version equals** the game's counter. Mismatched DLLs show up in the module list
  as "incompatible (interface N, game needs M)" and are never initialised.
- A second check catches build mistakes: every struct starts with a `StructHeader {size, kind, interfaceVer}`, and
  `InitWInterface` rejects any mismatch (`ErrStructSize`, `ErrKind`, `ErrVersion`) before it reads anything else.
- `gameVersion` is for the module's own refusal (`ErrGameVersion` with `minGameVersion`) and for logs. It is not a
  compatibility mechanism: the interface version is.
- Growth that does **not** need a bump: new record kinds (the game and audio ignore unknown kinds), new capability
  bits that are only ever read when set, and anything inside a module's own config or car files.

### 2.3 Rules for everything across the boundary

- POD structs, fixed-width types, `#pragma pack(push, 4)`, `static_assert` on every size. No STL, exceptions,
  `bool`, or `enum` without an underlying type.
- Opaque handles. The side that created a handle destroys it.
- Strings are UTF-8 and NUL-terminated. They are valid for the duration of the call, unless the function says
  otherwise (`lastError`: until the next call).
- Memory the game frees comes from `HostServices::alloc`. Memory a module returns (`Blob`) goes back through that
  module's matching release call.
- No exceptions cross the boundary. Every entry point catches everything, sets `lastError`, and returns a `Status`.
  `ErrFault` means the instance is broken and must be destroyed.
- **Files only through `HostServices::files`.** That way RBR's loose files, `physics.rbz` members, WRally's content
  and later mod packages all resolve one way (§5.1).

### 2.4 Loading (game side)

```cpp
namespace wrally::modules {

struct LoadedModule {
    std::filesystem::path path;
    HMODULE               dll = nullptr;
    iface::ModuleKind     kind = iface::ModuleKind::None;
    uint32_t              interfaceVer = 0;   // what the DLL reported
    bool                  compatible = false;
    iface::ModuleInfo     info{};
    std::string           error;              // why not usable
};

class ModuleHost {
public:
    void Scan(const std::filesystem::path& dir, iface::ModuleKind kind);   // fills Available(kind)
    const std::vector<LoadedModule>& Available(iface::ModuleKind) const;
    Renderer* OpenRenderer(const std::string& id, HWND, const RenderSettings&);
    Physics*  OpenPhysics(const std::string& id);      // per session
    Audio*    OpenAudio(const std::string& id, const AudioSettings&);
};
}
```

`Scan` does the following for each `*.dll`:

1. `LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`. A module's
   dependencies (WRenderer's `dxcompiler.dll`) load from its own folder.
2. `GetProcAddress("GetWInterfaceVer")`. If it is missing, this is not a module: unload.
3. `ver = GetWInterfaceVer(kind)`. If it is 0, unload. If it differs from the game's counter, record the DLL as
   incompatible and unload.
4. `InitWInterface` is called only when the module is actually used. Scanning must not create a D3D12 device or
   open an audio endpoint.

The game wraps each function table in a small C++ class (`wrally::render::Renderer`, `wrally::physics::Physics`,
`wrally::sound::Audio`) that turns `Status` into `std::expected` and handles into RAII. Nothing else in the game
touches `iface::` types.

---

## 3. Renderer module — WRenderer.dll

Interface: [`renderer.h`](../include/wrally/iface/renderer.h). Implementation: `ngg/core` moved in, `ngg/api` as
the reference for semantics.

### 3.1 From the editor API to a game API

`ngg_api.h` is an *editor's* interface: many small setters that edit a retained scene. phys-drive uses it the same
way today (`ngg_add_mesh`/`ngg_add_instance` per car part, `ngg_set_transform` per part per frame, `ngg_set_labels`
for the HUD). A game sets the whole frame every frame, so the interface keeps NGG's resources and replaces its
setters with one **`FrameDesc`** per frame:

| ngg_api (as phys-drive uses it) | WRenderer |
|---|---|
| `ngg_create(parent HWND)` makes a child window | swap chain on the **game's** window (no airspace problem; the game draws its UI *through* the renderer) |
| `ngg_load_memory` | `loadScene(bytes)`: the game's VFS or the legacy importer produced the bytes |
| `ngg_set_camera`, `ngg_set_weather`, `ngg_set_transform`, `ngg_set_visible` | fields of `FrameDesc` |
| `ngg_add_mesh/material/instance` per car part | `createModel` (node tree + meshes + PBR materials) / `createInstance`; per-frame node transforms |
| `ngg_object_key` + `ngg_set_transform` world delta (phys-drive's body follower) | `FrameDesc::stageObjects` keyed by the `.trk` instance id: the physics' `BodyState::id` |
| `ngg_set_font` + `ngg_set_labels` for the HUD | **`FrameDesc::ui`**: Dear ImGui's draw data, for all menus and HUD |
| overlay lines (editor gizmos) | `FrameDesc::lines` (debug) |
| quality / TAA / RT setters | `RenderSettings` applied as a set |

The renderer still owns frame **order and correctness**. That is the `core::Renderer::RecordFrame` rule:
environment → prepass → sun visibility (RT or cascades) → AO → sky → scene → models → TAA → exposure → tonemap → UI
→ present.

### 3.1a What WRenderer does today (phase 4)

- One scene at a time. Scene textures and models belong to the loaded scene, and loading another releases them.
  UI textures (`createTextureRgba`) are the renderer's and stay.
- Windowed only (`ErrNotSupported` for borderless and fullscreen). `renderScale` is ignored, and `kMaterialBlend`
  is not drawn blended.
- UI textures carry a high bit in their handle (`UiPass::kTextureBit`). A UI command naming any other texture draws
  white.
- Released models and destroyed instances leave their meshes in ngg-core's dynamic pool until the scene goes.
- One renderer per process: ngg-core's log sink is global.

### 3.2 New work inside the renderer

- **Models**: node-hierarchy rendering (no skinning), blended materials (glass), per-instance node transforms.
  `ngg-core` today draws only static scenes plus the dynamic pool.
- **Dynamic objects in RT**: already handled by ngg-core (found in phase 6). `Renderer::KeepRtCurrent` keeps a
  BLAS per mesh and, when only transforms change, updates just the instances and the TLAS. The moving car and stage
  solids keep their ray-traced shadows ("instances only" in the renderer's log).
- **UI pass**: ImGui's `ImDrawData` maps 1:1 onto `UiDrawList` (vertex/index buffers, commands with texture and clip
  rect). Textured 2D triangles after the tonemap, scissored, alpha-blended.
- **Display modes**: borderless/fullscreen, vsync, render scale. VR (OpenXR) later behind `kRendererCapVr`.
- Later, each as an interface bump: particles (dust, gravel, spray), skid marks, decals, damage deformation (vertex
  offsets on model meshes), rain on glass, headlights (NGG §5 night lighting).

### 3.3 Frame flow and threads

```
 main thread    : window messages, input polling, ImGui frame          ──▶ UiDrawList
 sim thread     : fixed-step physics + driver layer + rules            ──▶ snapshot N (poses, bodies, clocks)
                  records ──▶ game's signals::Bus ──▶ audio.submitRecord, HUD, FFB, recorder
 render thread  : interpolate snapshots N-1..N at render time           ──▶ FrameDesc ──▶ renderFrame + present
 audio thread   : the module's own (device pull)
```

The physics runs at its own `dt`. The game accumulates real time and steps whole ticks, with a cap on ticks per frame
so that a hitch cannot spiral (a 1 kHz WPhysics after a 100 ms stall is 100 steps, not unbounded). Each tick's
car/body poses go into a triple-buffered snapshot, and the render thread interpolates between the last two by the
remainder. Only the render thread calls `RendererApi`, and only the sim thread calls a given `World`.

**As built (phase 5).** The sim thread exists: `session::SimThread` runs the `Session`, so physics, driver layer,
stage clock and recording step there, and the physics' records reach the bus (and the audio module) on that thread.
It differs from the sketch above in two ways:
- **Rendering stays on the main thread**, with the window and ImGui. A separate render thread is a later step.
- **The sim thread steps on request, not on its own clock.** Each frame posts its time and keys, and requests not
  yet stepped merge. In real time a frame draws the newest snapshot, usually the previous frame's steps while this
  frame's run alongside: at most one frame of extra latency. Under `--step` (lockstep) a frame waits for its own
  steps, so a run is the same frame for frame and tick for tick.

---

## 4. Physics modules

Interface: [`physics.h`](../include/wrally/iface/physics.h), records: [`records.h`](../include/wrally/iface/records.h).

### 4.1 The interface

This is `phys::sim::Physics` as a function table. `createWorld(WorldDesc)` loads the stage and cars (the slow part).
Then, per tick: `setControls` per car, `step(1)`, then read `carState`/`bodies`. Records (engine, drivetrain,
wheels, body, gear changes, collisions) go to the game's `RecordSink` during `step`. The game re-emits them on its
own `signals::Bus` for audio, HUD, FFB, particles and replays, exactly as phys-drive wires them today.

Things that are deliberately the game's, not the physics': the **stage clock** (`StageClock`), the **driver layer**
(launch control, anti-lag: `phys/drive`), the camera, and recovery decisions. The physics only executes `resetCar`.

**Own configuration.** Each module reads its own config file, named by `PhysicsInput::config`. By convention that is
`config/physics/<id>.json` through the VFS, in whatever format the module likes. Per car, `CarDesc::carData` names
the module's own car file. The game passes both through without parsing them, so a physics module can change what it
configures without an interface bump.

**Tick rate.** `dt` is per world and the module's to choose. Nothing in the game assumes RBR's rate.

### 4.2 WPhysicsRBR.dll — id `"rbr"`

- Built from `phys/native` + `phys/num` + `phys/data` (`NativeSim` without the classic arena).
- Must stay **bit-exact** with RBR. That is what makes RBR replays and fair online times possible. The checks
  against RBR's own code (`phys-native-check`, `phys-sim-check`) need the classic physics, so they stay in
  plugin-rbr. WRally gates this module with golden recordings: `.phr` runs that must replay to the same poses,
  bit for bit.
- Caps: `RbrCars | RbrStages | CarOverrides`. Not `MultiCar` (one car per world today) or `Spectators` (not ported:
  `NativeSim::Open` refuses animation objects).
- Its config is RBR's own data (`physics.rbz`, the cars' `.lsp`). It needs no file of its own, so `config` is "".
  It runs RBR cars only (decision 8), so it is offered only when the RBR layer is present. It can drive those cars on
  WRally stages too, since those use RBR's gameplay formats (§6.3).
- File access: every read the physics makes goes through `phys::data::ReadWholeFile` (`data/files.h`). The module
  points it at `HostServices::files`, so `WorldDesc` paths are VFS paths. Car overrides arrive as text
  (`CarDesc::overrides`), because WRally's car data is not in the RBR folder.
- Not yet read out of the native car: `CarState::steeringTorque` and `damaged` (0), and `resetCar`
  (`ErrNotSupported`).

### 4.3 WPhysics.dll — id `"wphysics"`

- Starts as a source fork of the rbr module, so it is identical on day one, with the same tests passing.
- **Own configuration in JSON**: `config/physics/wphysics.json` for the module (tick rate, e.g. 1 kHz, solver
  iterations, tyre model choice, debug) and `cars/<car>/wphysics.json` per car (`kPhysicsCapExtendedCars`). An RBR
  car still loads from `physics.rbz`, with the JSON able to override any value on top. That generalises today's
  `cars/<car>.ini` overrides.
- Then it diverges where RBR is known to be limited: higher tick rate, tyre model, multi-car worlds, `saveState`
  (restart/rewind), extended car data (more than 4 cylinders, modern drivetrains, aero).
- It never claims RBR compatibility for timing. Results and replays carry the module id + `buildHash` (§6).

### 4.4 Choosing the physics

- The config has a default physics. An event or online championship may *require* one (id + minimum module
  version, or an exact `buildHash` for competitive events).
- A car declares which physics it can run on. An RBR car runs on any module with `RbrCars`. A WRally car has only a
  `wphysics.json` and needs WPhysics (decision 8). The car menu hides combinations that cannot run.
- One physics module per session. Several can be *loaded* (for menu listing), but only one has a `World`.

---

## 5. Audio module — WAudio.dll

Interface: [`audio.h`](../include/wrally/iface/audio.h). Implementation: `plugin-rbr/audio` (game-audio) moved in.

- **The car's sound is driven by the record stream.** The game forwards every physics record and its own
  `DriverTick` to `submitRecord`, unchanged. This is what `audio::Audio::Connect(bus)` does inside phys-drive today,
  moved across the boundary. The grains engine needs per-firing records (`EngineTick` crank angle, `fired`), which is
  why the interface passes records rather than a few summarised parameters.
- `createCarVoice(CarSoundDesc)` per car: the engine sound (RBR `.eng`, or a WRally sound set), the surrounding bank,
  and the mix (today's `sound-mix.ini`, the module's format).
- Game sounds: `loadSound`/`play` on buses (World, CoDriver, Ui), positional or not. The co-driver's pacenotes are the
  game's logic (which call, when, from `.dls`). The audio module only plays them.
- `setListener` once per frame from the camera, with an `interior` flag for cockpit filtering.
- Offline mode (`kAudioOffline` + `renderOffline`): the same records give the same samples, for rendering a replay to
  WAV. It replaces `audio::Audio::Mode::Offline`.
- Device: XAudio2 as today. Settings are applied as a set (endpoint, per-bus volume, speakers).
- **Not in WAudio yet** (phase 3 moved what the game used): one car voice only; the default device only; no
  one-shots (`loadSound`/`play`: `ErrNotSupported`); `setListener` does nothing, because the mix is still mono with
  no placed sources (sound-engine plan S3). The gain is `volume[Master] × volume[Car]`; pause and mute both silence
  the car.

---

## 6. Game

### 6.1 Content roots and the VFS — `wrally::vfs`

WRally runs **standalone** on its own content. When it sits in an RBR folder, it **adds** RBR's content.

Layers, with later layers winning:

1. **RBR install**, read-only, *optional*. It is used when the WRally exe's folder (or `rbrPath` in `wrally.ini`)
   contains `RichardBurnsRally_SSE.exe` and `Physics/physics.rbz`. It provides `Maps/`, `Cars/`, `Physics/`,
   `Audio/` and `Replays/`.
2. **WRally content** (`<WRally>/content`): WRally's own stages, cars, sound sets, UI, default configs.
3. **Mod packages** (zip, later), one layer each.

Archive members use `archive.rbz:member/path` (`physics.rbz` is a zip of deflated members, `phys/data/inflate`).
Lookup is case-insensitive with forward slashes. The VFS serves the game and, through `HostServices::files`, every
module.

**Content catalog** (`wrally::assets::Catalog`). This is what the menus list: one list of stages and one of cars,
with a source tag on each.

- **WRally content** is listed by manifests: `content/stages/<id>/stage.json` and `content/cars/<id>/car.json`.
- **RBR content**, when the RBR layer is present, is listed from `Maps/Tracks.ini` and `Cars/Cars.ini`, and wrapped
  in the same catalog entries in memory. RBR's files are never rewritten.
- Without RBR, the RBR entries do not exist and nothing in the game refers to them. The game never assumes
  `physics.rbz` or `Maps/` exists.

### 6.2 Formats — `wrally::formats`

One static library used by the game and by modules that need it. Readers only, with one exception: the NGG writer the
legacy importer uses (decision 2), which sits next to the renderer's reader. Other writing belongs to Wallaby/tools.

| format | what | source today | used by |
|---|---|---|---|
| `stage.json`, `car.json` | **WRally manifests**: what a stage/car consists of, per-physics/per-audio data files | new | game catalog |
| `.lsp`, `.rbz` | physics defs, car/setup/tyre files | `phys/data/lsp`, `inflate` | rbr physics |
| `.trk` | collision objects, drive line | `phys/data/trk`, `trkworld` | physics, game (drive line) |
| `.col` `.mat` `.fnc` | static collision, surfaces, fences | `phys/data/col`, `phys/native` | physics |
| `.lbs` | **render geometry** (+ script objects) | script objects: `phys/data/lbs`; geometry: WRally's own reader (decision 2) | legacy importer, physics |
| `.dls` | cameras, pacenotes, splits/finish | `phys/data/dls` | game |
| `.sgc` + `<car>.ini` + `_shaders.ini` | car render model + collision meshes | `phys/drive/car_model`, `phys/data/sgc` | game → renderer model, physics |
| `.dds` | textures | (pass-through) | renderer |
| `Tracks.ini`, `TrackSettings.ini`, `Cars.ini` | stage list, surfaces, weather/lighting, car list | `sim::StageSurface`, NGG weather | game catalog |
| `.eng`, audio banks | engine sounds | `audio/` | audio module |
| `.rpl` | RBR replays | `phys/data/rpl` | game (playback by state) |
| `.phr` | WRally recordings (controls + checks) | `phys/data/rec` | game |
| **`.ngg`** | stage graphics (PBR, weather presets, object keys); later car graphics | `ngg/format` | renderer |
| module JSON | physics/audio module config and per-car data | new, owned by each module | that module only |

### 6.3 Stages

A catalog stage entry resolves to two halves: graphics for the renderer, gameplay for the physics and the game.

```
render : the entry has an .ngg?  ──yes──▶ loadScene(file bytes)
                 │ no (RBR stage without a converted .ngg)
                 ▼
         LegacyStageImporter: .lbs + .dds + TrackSettings.ini  ──▶ .ngg in memory ──▶ loadScene
         (cached to <cache>/stages/track-41_M.ngg, keyed by the source files' hashes)

gameplay: .trk .col .mat .fnc .lbs (physics) + .dls (cameras, pacenotes, splits) (game)
```

- **RBR stages** use RBR's own gameplay files. Their graphics come from a Wallaby-converted `.ngg` if there is one,
  otherwise from the on-the-fly import.
- **WRally stages** (`stage.json`) ship an `.ngg` and gameplay files *in RBR's formats*, written by our tools.
  That is what NGG design §7 already specifies for native tracks (".ngg + LBS/COL/DLS gameplay stub"). Reusing the
  formats does not need any RBR content, and it lets both physics modules run WRally stages from day one. Later the
  gameplay data moves into `.ngg` chunks (decision 10), so a WRally stage becomes one file; the manifest is what
  makes that change local.

So there is **one renderer path**. A legacy stage is "an NGG converted on the fly with default PBR materials", which
is what `Material`'s defaults are designed for (`DefaultMaterial()`, every factor standing alone).

**WRally imports `.lbs` itself** (decision 2): a C++ reader of the `.lbs` geometry and a simple automatic
importer, written fresh (no Wallaby code). It reads the geometry, the textures and the object keys. It drops RBR's
baked shading (the sun, ray tracing and AO light the stage), rebuilds the normals by an angle weld, cuts out
alpha-textured foliage, and takes weather presets from `TrackSettings.ini`. Its result is cached. Wallaby remains
the tool for a hand-finished `.ngg`, which wins whenever there is one. Until the importer lands, RBR stages without
an `.ngg` can be driven but not drawn.

Object keys tie the two halves together. The physics moves a solid with `.trk` instance id *k*, and the renderer
moves every scene object whose NGGK key is *k*. The importer writes those keys for legacy stages too.

### 6.4 Cars

- **RBR cars** (RBR layer present): `Cars/Cars.ini` lists them. Render model from `.sgc` (`ReadCarModel`) →
  `iface::ModelDesc`. Physics from `physics.rbz` `Physics/<car>/`. Sound from `.eng`.
- **WRally cars** (`car.json`): the render model (`.sgc` written by our tools at first, a car `.ngg` later),
  `wphysics.json` for the physics (WPhysics-only, decision 8: no `.lsp`), and a sound set for the audio.
- The game animates the model from `CarState`: body pose → instance `world`, `wheels[k]` position/rotation/spin →
  hub and spinning nodes, `damaged` bits → hide detached parts. This is phys-drive's car-part code, moved to node
  transforms.

### 6.5 Game modules (namespaces)

| namespace | contents | from phys-drive |
|---|---|---|
| `wrally::app` | `main`, window, threads, config (`wrally.ini`), crash handling | `drive.cpp` window/loop, args |
| `wrally::modules` | `ModuleHost`, the C++ wrappers over the iface tables | (new) |
| `wrally::vfs`, `wrally::formats` | §6.1, §6.2 | file loading in `drive.cpp` |
| `wrally::assets` | catalog, stage resolution, `LegacyStageImporter`, car model building, caches | `car_model`, stage/`.ngg` pick |
| `wrally::session` | one stage run: world + scene + car instances + clock + recorder; the driver layer (launch control, anti-lag) emitting `DriverTick`; start, run, finish, restart | the main loop body, `launch_control.h`, `anti_lag.h`, body follower |
| `wrally::rules` | `StageClock`, penalties, damage/repair, events, championships, career (later) | `sim::StageClock` |
| `wrally::input` | XInput, DirectInput wheels/pedals/shifters, keyboard, bindings, calibration, **FFB** from `CarState::steeringTorque` | keyboard handling |
| `wrally::camera` | chase, bonnet, above, beside, cockpit, TV cameras (`.dls`), replay cameras | camera code, `replay_camera` |
| `wrally::ui` | menus, HUD, settings, module selection, on **Dear ImGui** → `UiDrawList` | `DrawHud`, `DrawStageHud`, `DrawDebugHud` |
| `wrally::sound` | wrapper over the audio module: forwards records, co-driver pacenote scheduling, UI sounds | `audio::Audio` setup |
| `wrally::replay` | `.phr` record/play (controls + physics id/hash), `.rpl` playback (stored states, no physics) | `--out`/`--replay` |
| `wrally::signals` | the record bus (`signals/bus.h`) | as is |
| `wrally::net` | *later*: online results (onlineracing.cz/RBRTM), ghosts, multiplayer | |

### 6.6 Session lifecycle

```
Menu ──▶ pick stage/car/physics/weather (catalog)
     ──▶ Loading: resolve stage → renderer.loadScene (‖ in parallel) physics.createWorld
                  car model → createModel/createInstance; weather preset → WeatherDesc
                  audio.createCarVoice
     ──▶ Entry (car held) ──▶ Countdown (StageClock) ──▶ Running ──▶ Finished ──▶ Results
     ──▶ Restart: destroyWorld + createWorld (or loadState with SaveState)
     ──▶ Exit: destroyWorld, unloadScene, release models, destroyCarVoice
```

---

## 7. Replays, determinism, fairness

- A `.phr` recording stores the physics module **id + moduleVersion + buildHash**, the full `WorldDesc` (file
  hashes, including the module's config and car JSON, not just paths), and every tick's controls. Playback re-runs
  the physics, and plays only on a module with the same `buildHash` and the same config hash. Otherwise the game
  offers "watch as ghost" (poses only).
- RBR's `.rpl` plays from stored states, without physics, as RBR itself does.
- Online/competitive: the game reports the physics `buildHash` and the hashes of stage, car and config files. A
  server accepts times only from approved hashes. That is the fairness profile of NGG design §1.9, moved into a game
  we control. WPhysics' JSON config is part of what is hashed, because it changes the car.

---

## 8. Repository layout

```
wrally/
  CMakeLists.txt
  docs/design.md                      this
  docs/schemas/                       JSON Schemas: stage, car, wphysics, wphysics-car (decision 9)
  include/wrally/iface/               THE contract - the only headers modules and game share
      common.h  renderer.h  physics.h  audio.h  records.h
  libs/
      modules/        ModuleHost, FileHost                                   (phase 1)
      signals/        the record bus                                         (phase 1, imported)
      formats/        wrally::formats   (data readers move here from physics-rbr/src/data once the game needs them
                                         apart from the physics; ngg/format; car_model; LBS reader port)
      vfs/            wrally::vfs
      json/           nlohmann/json + schema validation, for manifests and module configs
  game/               WRally.exe        (phase 1: src/drive/ = phys-drive, imported)
      src/app  src/modules  src/assets  src/session  src/rules  src/input
      src/camera  src/ui  src/sound  src/replay
      config/         sound-mix.ini ...
  modules/
      renderer/       WRenderer.dll     (src/core, src/format: ngg-core; src/api: ngg-api, until phase 4)
      physics-rbr/    WPhysicsRBR.dll   (src/num, src/data, src/native, src/sim: the native RBR physics)
      physics-w/      WPhysics.dll
      audio/          WAudio.dll        (src/audio: plugin-rbr/audio)
  third_party/        dxc (dxcompiler.dll, dxil.dll); later imgui, nlohmann/json
  content/            WRally's own stages, cars, sound sets, default configs (standalone)
  tests/              iface layout tests, module conformance (any DLL of a kind through its Api),
                      rbr bit-exact suite (phys-native-check), renderer smoke, audio offline golden
  tools/              module-info (lists a DLL's kinds/versions/caps), phr-check
```

Output layout, mirroring the runtime:

```
WRally.exe
modules/renderer/WRenderer.dll, dxcompiler.dll, dxil.dll
modules/physics/WPhysicsRBR.dll, WPhysics.dll
modules/audio/WAudio.dll
content/   config/   cache/   wrally.ini
```

---

## 9. Phasing

Each phase ends with a game you can drive. phys-drive works on day one, and every step keeps it working.

1. **Skeleton + phys-drive moved in** — DONE 2026-10-25. CMake, `iface` headers, `ModuleHost` + `FileHost`,
   `tools/module-info`, a stub module and handshake tests. phys-drive and the libraries it uses were imported
   (docs/imported.md): WRally.exe is phys-drive, still drawing through `ngg-api.dll`, with `NativeSim` and
   `audio::Audio` linked in. Its recordings are byte-identical to plugin-rbr's phys-drive.
2. **WPhysicsRBR** — DONE 2026-10-25. `NativeSim` behind `PhysicsApi` (`modules/physics-rbr/src/module`), every
   file read through `HostServices::files` (`data/files.h`). The game loads the DLL through
   `wrally::physics::Physics` (`game/src/physics`) and turns its records back into bus records
   (`signals/convert.h`). The stage clock moved to the game (`game/src/rules`). A replay checks every pose against
   the recording. RBR's own stage-41 recording (49,913 poses) replays bit-exact: CTest `golden-replay-41`.
3. **WAudio** — DONE 2026-10-25. `audio/` behind `AudioApi` (`modules/audio/src/module`, interface v2: the mix and
   the bang files passed as data, `setCarMix`). Its files are read through the VFS (`audio/files.h`). The game
   forwards every bus record through `wrally::sound::Sound` (`game/src/sound`). `tools/audio-check` renders the same
   records offline through the linked library and through the DLL, and the samples are bit for bit the same: CTests
   `audio-offline-*`, from made-up records, with a mix, and from RBR's stage-41 run with RBR's sounds.
4. **WRenderer** — DONE 2026-10-26. `ngg-core` behind `RendererApi` (`modules/renderer/src/module`, interface v2:
   weather presets by index, a shadows setting, the full readout as `RenderStats`, `sceneKeyStats`). The module makes
   the same ngg-core calls in the same order as `ngg-api` did, so the frame is the same: compared against the phase-3
   build on identical inputs, every 3D pixel matches within the GPU's own run-to-run noise (±1); only the HUD
   differs. The game loads the DLL through `wrally::render::Renderer` (`game/src/render`). The car is a model (a node
   per part, posed per frame); moved stage objects go by key. The HUD is Dear ImGui 1.91.5 (`game/src/ui`,
   `third_party/imgui`), drawn by the module's UI pass. `ngg-api` is no longer built.
5. **Restructure the game** — DONE 2026-10-26. `drive.cpp` split along §6.5 into `app/` (main, options, window,
   frame loop), `session/` (one stage run, the driver layer), `camera/`, `input/`, `assets/`, `render/stage_objects`,
   `ui/hud` and `core/math`. The session runs on its own thread (`session/sim_thread.h`, §3.3), in lockstep with the
   frames under `--step`. Recordings and frames are unchanged in lockstep, a real-time threaded drive replays
   bit-exact, and a replay now quits at its end (`--hold` keeps it up).
6. **Cars as models** — DONE 2026-10-26.
   - **Node tree:** the car model has a body node, a hub and a spin node per wheel, and a node per loose part
     (`game/src/assets/car_render.*`).
   - **Parts from the physics:** physics interface v2 reports the car's parts that can come loose (`carParts`:
     doors, bonnet, exhaust, bumpers, wing), with state and pose, and they're drawn where the physics has them. A door
     swings on its hinge, a loose bumper hangs, and a torn-off part becomes a world object. At rest a part is placed
     exactly as the body. A mesh goes with a part when it is the part's mesh or hangs under it in the `.sgc`'s node
     tree (the grille with the front bumper, the mirrors with the doors; `PHYS_DRIVE_PARTS` lists them). In the stage-41
     run the front bumper comes loose at tick 8792 and is torn off at 45465.
   - **Shader compiler fix:** ngg-core now loads `dxcompiler.dll`/`dxil.dll` from the renderer module's folder. A
     clean build had silently lost ray tracing, because the old copies next to the exe were what made it work.
   - **Already there:** moved stage objects (phase 4) and RT instancing (ngg-core).
   - **Not done:** the body's dents (the `.sgc` morph channels, from the damage zones).
6a. **Menus** — 2026-10-26 (phase 7 moved later). With no command line WRally opens a main menu: Quick rally,
   Settings, Exit (`game/src/ui/menus.*`, Dear ImGui, mouse or keyboard). The window, the renderer and ImGui live for
   the whole run, and a stage loads and unloads its scene on them (`app/game.cpp`).
   - **Quick rally:** stage, car, setup (the default or a saved RBR `.lsp`), damage (off, safe, reduced, realistic),
     weather (the `.ngg`'s presets, named as `TrackSettings.ini`'s sections) and road (from the weather, dry, damp,
     wet). The choice becomes a command line (`--setup`, `--damage`, `--weather`, `--road`), so a menu run is the
     same as a CLI run. Custom weather (sun, rain, clouds...) comes later.
   - **All cars:** the catalog (`assets/catalog.*`) lists every `Cars/<folder>` that has an `.sgc`, not only the eight
     `Cars.ini` slots. A car's physics comes from its own `Cars/<folder>/physics/` if it has one, else its slot's
     `physics.rbz` folder, else the base physics (Settings). Settings → "Extract the slot cars' physics" copies the
     slot cars' physics out of `physics.rbz` into their folders, so each car's physics is a set of loose files.
   - **Esc in a stage:** the stage holds and a menu comes up: Resume, Restart stage, Settings, Debug (what the keys
     switch: renderer switches, debug view, camera, chase distance, mute, anti-lag) and Exit to main menu. A CLI run
     still quits at its end (the window closed, a replay's end, `--exit-at`). Its Esc menu leads to the main menu.
   - **Settings:** saved in `wrally.ini` beside the exe (`app/settings.*`). They cover the RBR folder, the `.ngg`
     folders (default `<RBR>\Ngg`), the base physics, graphics (applied at once) and sound.
7. **Content.** VFS layers, catalog, `stage.json`/`car.json` with their schemas, and standalone mode without RBR
   (tested with the RBR layer switched off; RBR content remains the development data, decision 7).
8. **Legacy stages.** WRally's own automatic importer (decision 2), so every RBR stage draws without Wallaby:
   1. a C++ NGG writer next to the renderer's reader, with a round-trip test; the `.lbs` geometry reader; a minimal
      importer (geometry, textures, object keys, baked shading dropped) — DONE 2026-10-26.
      - **Writer:** `modules/renderer/src/format/ngg_scene.*` (library `ngg-format`) writes v3 and reads v3 and v2.
      - **Tests:** `tools/ngg-check` runs three CTests. `ngg-roundtrip` writes a made-up scene, reads it back and
        has the renderer load it. `ngg-producer-wallaby` reads Wallaby's stage 41 `.ngg`, rewrites it identically
        and has the renderer load both. `legacy-import-41` imports RBR's `track-41_M.lbs`.
      - **Reader and importer:** `game/src/formats/lbs_geometry.*` and `game/src/assets/legacy_stage.*`. Given an
        `.lbs` in place of the `.ngg`, the game imports it. Stage 41 takes 0.5 s and gives 304,744 triangles (as
        many as Wallaby's `.ngg`) with all 34 textures found.
      - **Look:** it draws close to Wallaby's `.ngg` under the dynamic light. What step 2 is for still shows:
        normals facing away on some flat scenery (banners) and a hard edge on the distant backdrop.
   2. normals rebuilt, foliage cut out
   3. weather presets from `TrackSettings.ini`, the cache, every RBR stage in the menu catalog
9. **Game.** Menus, car/stage selection, settings, wheels + FFB, replays, results.
10. **WPhysics.** Fork, JSON config, higher tick rate, then extend. Multi-car and save-state come with their
    interface bump.
11. **Native gameplay in `.ngg`** (decision 10), then **first WRally stage and car** for standalone (§11).

---

## 10. Decisions (2026-10-25)

1. **GUI: Dear ImGui** for all game UI in v1, rendered through `FrameDesc::ui`.
2. **Two converters (revised 2026-10-26).** WRally has its own simple, automatic LBS → NGG importer in C++, and
   Wallaby stays the authoring converter. Wallaby does the sophisticated work: replacing low-poly objects, manual
   overrides, tagging, plants and weather import. The game's importer needs no human input and makes any RBR stage
   drawable and dynamically lit. No Wallaby code ships with WRally or is copied into it; the importer is written
   against RBR's formats. A Wallaby-made `.ngg` always wins over the automatic import. To keep the two NGG writers
   from drifting apart, WRally's writer sits next to the renderer's reader (one format header), a round-trip test
   writes, loads and compares, and a test loads Wallaby-made `.ngg` files as another producer of the format.
3. **Audio is a module** (`ModuleKind::Audio`, WAudio.dll), fed by the physics' record stream (§5).
4. **WPhysics** may run at a higher tick rate and has its own configuration, in JSON (`config/physics/wphysics.json`,
   `cars/<car>/wphysics.json`). The interface leaves config format and `dt` to each module.
5. **Standalone + RBR.** WRally works without RBR on its own content. Placed in an RBR folder (or pointed at one), it
   also uses RBR's content, read in place and never shipped.
6. **Starting point: `phys/drive`** (phys-drive), which already combines the physics, the NGG renderer DLL, the
   audio, the stage clock and replays.
7. **Development content: RBR's own stages and cars.** Until the game runs end to end, all development and testing
   uses an RBR install. Standalone mode is built and tested with the layer switched off, but WRally's own first
   stage and car are not needed before phase 9 (§11).
8. **WRally cars are WPhysics-only.** They carry `wphysics.json`, never `.lsp`. The rbr physics module runs RBR cars
   only, so standalone play (no RBR install) runs on WPhysics.
9. **JSON: nlohmann/json, with published schemas.** Every manifest and module config has a JSON Schema in
   `docs/schemas/` (`stage.schema.json`, `car.schema.json`, `wphysics.schema.json`, `wphysics-car.schema.json`). The
   game validates manifests against them at load and in `tools/`, and modders get the same files.
10. **Native gameplay format: later, inside the `.ngg`.** WRally stages use RBR's `.trk/.col/.mat/.fnc/.dls`
    formats until the game runs end to end. Then collision, surfaces, fences, the drive line, splits and pacenotes
    become `.ngg` chunks, so a WRally stage is one file. Physics modules read them through the same VFS. Readers
    still accept the RBR formats, because RBR stages keep using them.

## 11. Open questions

- **First own content for a standalone release.** Deferred by decision 7. Revisit after phase 9: Wallaby-authored,
  generated (NGG design §7), or licensed community content.
