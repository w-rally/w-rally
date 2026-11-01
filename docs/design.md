# W-Rally — architecture design

Status: draft 3, 2026-10-25. Interfaces: [`include/wrally/iface/`](../include/wrally/iface/).
Decisions taken so far are in §9.

## 0. Goal

A standalone rally game that:

- **runs on its own content**, with no RBR install needed: W-Rally stages, cars and sounds in W-Rally's own formats;
- **also runs everything RBR has**, when W-Rally is placed in an RBR folder (or pointed at one). It then reads RBR's
  stages, cars, physics data, sounds and replays directly, alongside its own content;
- is **modular**: the game executable plus swappable DLLs for the renderer, the physics and the audio. Several
  physics modules can sit side by side (original RBR, WPhysics, others later), and the player or an event picks one.

C++20, x64 only, Windows first. The top namespace is `wrally`.

### What we start from

**The game grows out of `phys-drive`** (`plugin-rbr/phys/drive/`). It is already a working, minimal version of the
game loop: it loads a stage and a car from RBR's files, drives the car from the keyboard with the native physics,
draws through the NGG renderer DLL (`ngg-api.dll`), plays the engine sound (`audio/`) from the physics' records,
runs RBR's stage clock (countdown, false start, splits, finish), switches cameras (including the stage's TV
cameras), records `.phr` runs and replays them. W-Rally keeps that working at every step while it is split into the
exe and modules (§8).

| existing (plugin-rbr) | becomes |
|---|---|
| **`phys/drive/drive.cpp`** — the phys-drive main loop | **W-Rally.exe**: `wrally::app` / `session`, split by §5.5 |
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

**W-Rally is self-contained.** The plugin-rbr code it uses is copied into this repository, not referenced from
it. [imported.md](imported.md) lists what came from where.

`ngg/bridge`, `ngg/ipc` and the d3d9 hooking are **not** carried over. They exist only to render inside
RichardBurnsRally_SSE.exe, and W-Rally owns its own window. The classic `phys::sim::Sim` (the exe image in a 4 GB
arena) is not carried over either. It stays in plugin-rbr as the oracle that the native port is checked against.

---

## 1. Process and modules

```
 W-Rally.exe  (wrally::app)  ────────────────────────────────────────────────────────────
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

Two rules carry over from NGG design §8.0 ([`ngg-design.md`](ngg-design.md), the renderer's own design record). The renderer knows nothing about RBR. A module includes only
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
- **Files only through `HostServices::files`.** That way RBR's loose files, `physics.rbz` members, W-Rally's content
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
  UI textures (`createTextureRgba`) are the renderer's and stay; `updateTextureRgba` (interface 10) replaces one's
  pixels, the same size - the main menu's video loop, a frame at a time.
- Windowed only (`ErrNotSupported` for borderless and fullscreen). `renderScale` is ignored.
- `kMaterialBlend` (glass) is drawn blended since 2026-10-30: after the opaque, far to near, premultiplied, no
  shadow and not ray traced (NGG design 5.0u). The car's glass uses it.
- UI textures carry a high bit in their handle (`UiPass::kTextureBit`). A UI command naming any other texture draws
  white.
- Released models and destroyed instances leave their meshes in ngg-core's dynamic pool until the scene goes.
- One renderer per process: ngg-core's log sink is global.

### 3.2 New work inside the renderer

- **Models**: node-hierarchy rendering (no skinning), blended materials (glass, done: §3.1a), per-instance node
  transforms.
  `ngg-core` today draws only static scenes plus the dynamic pool.
- **Dynamic objects in RT**: already handled by ngg-core (found in phase 6). `Renderer::KeepRtCurrent` keeps a
  BLAS per mesh and, when only transforms change, updates just the instances and the TLAS. The moving car and stage
  solids keep their ray-traced shadows ("instances only" in the renderer's log).
- **UI pass**: ImGui's `ImDrawData` maps 1:1 onto `UiDrawList` (vertex/index buffers, commands with texture and clip
  rect). Textured 2D triangles after the tonemap, scissored, alpha-blended.
- **Display modes**: borderless/fullscreen, vsync, render scale. VR (OpenXR) later behind `kRendererCapVr`.
- **Wet surfaces** (interface 4, 2026-10-30): `FrameDesc::wetness`, the road's state from the game (dry 0, damp 0.5,
  wet 1): darker porous surfaces, glossier everything, puddles on the ground from damp on (NGG design 5.0v).
- Later, each as an interface bump: particles (dust, gravel, spray), skid marks, decals, damage deformation (vertex
  offsets on model meshes), rain on glass.
- **Night** (decided 2026-10-30, NGG design 5.4): any stage at any hour from date + time + place (sun, moon,
  stars astronomically); every light ray traced through many-light sampling; car lights the car's own (`car.json`,
  else derived from RBR's car `.ini` light points, else a default), with brake, reverse, indicators and hazard;
  stage lights in the `.ngg`. Phases N1 sky and time, N2 light system, N3 car lights, N4 stage lights.

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
  plugin-rbr. W-Rally gates this module with golden recordings: `.phr` runs that must replay to the same poses,
  bit for bit.
- Caps: `RbrCars | RbrStages | CarOverrides`. Not `MultiCar` (one car per world today) or `Spectators` (not ported:
  `NativeSim::Open` refuses animation objects).
- Its config is RBR's own data (`physics.rbz`, the cars' `.lsp`). It needs no file of its own, so `config` is "".
  It runs RBR cars only (decision 8), so it is offered only when the RBR layer is present. It can drive those cars on
  W-Rally stages too, since those use RBR's gameplay formats (§6.3).
- File access: every read the physics makes goes through `phys::data::ReadWholeFile` (`data/files.h`). The module
  points it at `HostServices::files`, so `WorldDesc` paths are VFS paths. Car overrides arrive as text
  (`CarDesc::overrides`), because W-Rally's car data is not in the RBR folder.
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

### 4.3a Splitting the physics: libraries, not modules (decided 2026-10-28)

The physics stays **one module per world**, behind one interface. Its parts are split at the **source** level
instead, into libraries in `modules/physics-rbr` that a module links or replaces when it is built:

Each library depends only on the ones above it in this table:

| library | holds | headers |
|---|---|---|
| `rbr-base` | `Unreached`, the stop on a path no classic run takes | `unreached.h` |
| `rbr-collision` | static collision (the stage's `.col`, materials) and dynamic collision: the object model's base (frames, entities, bodies), collision meshes, events, the pair tests, brake walls | `entity.h`, `stage_collision.h`, `collision_mesh.h`, `event.h`, `contacts.h`, `brake_wall.h` |
| `rbr-tyre` | the tyre model and the spring-damper | `tyre.h`, `suspension.h` |
| `rbr-world` | rigid bodies (`Solid`), the contact solver, the world and its step | `solid.h`, `rigid_body.h`, `solver.h`, `world.h` |
| `rbr-car` | the car's models (drive, engine, VCU, driveline) and its solids (the car, its wheels, hinged and mounted parts, damage) | `car_solids.h`, `wheel.h`, `damage.h`, `drive.h`, `engine.h`, `vcu.h`, `driveline.h` |
| `rbr-load` | the world built from RBR's files (defs, setups, the stage and the car) and run | `defs.h`, `setup.h`, `load.h`, `run.h` |

- **WPhysicsRBR** links them all and stays bit-exact (golden replay).
- **WPhysics** (§4.3) links the parts it keeps and replaces the ones it rewrites. A new tyre model, for example,
  replaces `rbr-tyre` but keeps RBR's collision and car.
- **The seams are C++ interfaces** (for example a `TyreModel` the world step calls), not a C ABI.

**Why not separate DLLs:**
- **The parts interleave inside every tick.** The wheels probe the stage, the tyre forces feed the car, and the
  body, parts and wheels go through contacts, the solver and the penetration pass before integrating. A runtime
  boundary there would freeze those internals as an ABI, and they are what WPhysics exists to change.
- **Bit-exactness is easiest from one build of one source tree.** Several DLLs that must match each other add a
  way to break it for no gain.

**Status:**
- **Step 1, the separable layer: DONE 2026-10-29.** `modules/physics-rbr/CMakeLists.txt` now builds:
  - `rbr-base`: `Unreached`, moved out of `entity.*` into `native/unreached.*`
  - `rbr-collision`: `stage_collision`
  - `rbr-tyre`: `tyre`, `suspension`
  - `rbr-car`: `drive`, `engine`, `vcu`, `driveline`
  - `rbr-world`: everything else; `phys-native` is gone.

  The three model libraries include nothing above them. The golden replay is unchanged (49,913 poses bit-exact).
- **Step 2, the object model: DONE 2026-10-29.** The layout in the table above.
  - **Headers:** RBR's one object model in `entity.h` is split into `entity.h` (frames, `Entity`, `Body`),
    `solid.h` (`Solid`, what its damage handler hears) and `car_solids.h` (the car, its passengers, its hinged and
    mounted parts, its body). The declarations moved verbatim.
  - **Definitions:** the car's impulse code moved from `solver.cpp` into `car_parts.cpp`, and so did `CarBody`'s
    from `world.cpp` and `entity.cpp`. The base classes' notify and tick slots moved from `world.cpp` into
    `entity.cpp`.
  - **Seams** where RBR cast a car-flagged solid to `CCar`:
    - `Solid::DriveLineStep`: the car's drive-line parameter after the integration;
    - `Solid::SurfaceAt`: the car's surface cells under a contact;
    - `World::ReleasePart` takes a plain solid and its car slot; the car's `DetachHinged`, `ReleaseHinged` and
      `ReleaseStick` are free functions in `rbr-car`.

    The calls happen at the same points, so the arithmetic is unchanged: the golden replay is still bit-exact.
  - **Enforced:** CTests `physics-layer-rbr-collision`, `-rbr-tyre`, `-rbr-world` and `-rbr-car` link each
    library whole with only the ones below it. An upward call fails the link: a planted one in `world.cpp` did.
  - **Not yet a seam:** the car calls the world's concrete members (gravity, stage collision, the mesh and grid
    queries, the game's callbacks). A physics module replacing `rbr-world` but keeping RBR's car would need those
    behind an interface. Not done until one does.

**Queries for the rest of the game.** Other parts of the game need answers from the physics: the camera a ray
against the stage (so it does not clip into banks), AI or ghost cars the surface under a point, sound and particles
the surface type. They go into the physics interface as optional functions (`raycast`, `surfaceAt`), each behind a
capability bit (`kPhysicsCap...`). A module without them does not set the bit, and the game falls back.
Adding them is an interface version bump when the first caller needs them, not before.

### 4.4 Choosing the physics

- The config has a default physics. An event or online championship may *require* one (id + minimum module
  version, or an exact `buildHash` for competitive events).
- A car declares which physics it can run on. An RBR car runs on any module with `RbrCars`. A W-Rally car has only a
  `wphysics.json` and needs WPhysics (decision 8). The car menu hides combinations that cannot run.
- One physics module per session. Several can be *loaded* (for menu listing), but only one has a `World`.

---

## 5. Audio module — WAudio.dll

Interface: [`audio.h`](../include/wrally/iface/audio.h). Implementation: `plugin-rbr/audio` (game-audio) moved in.

- **The car's sound is driven by the record stream.** The game forwards every physics record and its own
  `DriverTick` to `submitRecord`, unchanged. This is what `audio::Audio::Connect(bus)` does inside phys-drive today,
  moved across the boundary. The grains engine needs per-firing records (`EngineTick` crank angle, `fired`), which is
  why the interface passes records rather than a few summarised parameters.
- `createCarVoice(CarSoundDesc)` per car: the engine sound (RBR `.eng`, or a W-Rally sound set), the surrounding bank,
  and the mix (today's `sound-mix.ini`, the module's format).
- Game sounds: `loadSound`/`play` on buses (World, CoDriver, Ui), positional or not. The co-driver's pacenotes are the
  game's logic (which call, when, from `.dls`). The audio module only plays them.
- `setListener` once per frame from the camera, with an `interior` flag for cockpit filtering.
- Offline mode (`kAudioOffline` + `renderOffline`): the same records give the same samples, for rendering a replay to
  WAV. It replaces `audio::Audio::Mode::Offline`.
- Device: XAudio2 as today. Settings are applied as a set (endpoint, per-bus volume, speakers).
- **One-shots (2026-10-30):** `loadSound` (`.ogg` through stb_vorbis, or `.wav`, by the file's first bytes) and
  `play` / `stop` / `isPlaying` work. `audio::Voices` holds the samples and their playing instances, resampled by
  linear interpolation. It applies each bus's gain times the master, and the world's buses hold while paused (the UI
  plays on). The car's `Audio::Render` adds them after its own samples, so the car's sound is unchanged: the
  offline checks still pass bit for bit. Offline renders the car alone.
- **The co-driver (2026-10-30)** is the game's (`game/src/sound/codriver.*`):
  - **Calls:** the stage's `.dls` pacenotes, using the types and flags as Wallaby's pacenotes editor names them,
    become phrases: the call, its modifiers, a note within 25 m chained with "into", and a distance note saying
    the gap to the next call (once per call).
  - **Voice:** RBR's: `audio/CoDriver.ini` names each call's recordings in `audio/Speech/PaceNote`, and one is
    picked at random each time.
  - **Timing:** a phrase is called when the car is the speed × the lead time from it (at least 60 m), and only
    while the stage time runs: from GO (the first call comes with it) to the finish, never in the countdown. Its
    sounds play one after another on the CoDriver bus, held with the stage.
  - **Settings:** Sound → Co-driver (on, volume, how far ahead), or `--no-codriver`, `--codriver-lead`,
    `--codriver-volume`. F1's readout shows the last call. Stage 41: 79 calls from 112 notes, 97 recordings.
  - **The live game's:** a replay plays without the co-driver unless asked (`--codriver-replay`, or Settings →
    Sound → "in replays too", `codriver_replay`).
  - **On the HUD (live only):** the calls ahead at the top centre (`ui::DrawPacenotes`, `CoDriver::Upcoming`):
    - **Current:** the first call ahead, from when the voice starts saying it until 5 m past it, so the HUD and the
      voice agree. Its timing is the call's (Call ahead, `--codriver-lead`); without a voice, when it is called.
    - **Next:** the one after it, when it is within 8 s at the car's speed (`--pacenote-next`, `hud_pacenote_next`),
      with a current only; centred under it, smaller and dimmer.
    - **Tuning:** the next call's time is in Settings → Sound → Pacenotes on the HUD.
    - **Text:** temporary plain text until the HUD's design. Corners are L1/R1 (flat) .. L7/R7 (hairpin) as
      Wallaby's editor names them, the rest short words (`crest`, `long`, `don't cut`, `>` for into), a distance
      its number.
    - **Without the voice:** with the sound or the co-driver off, it still shows. CTest `codriver-41` checks it on
      stage 41.
  - **Loaded on its own thread** (`CoDriver::LoadAsync`): `CoDriver.ini`, the recordings' decoding and the
    phrases, while the stage loads. Nothing is called until it is ready. Stage 41: 68 ms, ready before the scene.
  - **Not yet:** the Czech voice (`audio/Speech/Czh`, other file names); RBR's own timing rules beyond the lead;
    `PLUS` and `MINUS`, which `CoDriver.ini` names but this install has no recordings of.
- **Not in WAudio yet** (phase 3 moved what the game used): one car voice only; the default device only;
  `setListener` does nothing, because the mix is still mono with no placed sources (sound-engine plan S3). The car's
  gain is `volume[Master] × volume[Car]`; pause and mute both silence the car.
- **TODO, low priority** (the audio threads, 2026-10-30):
  - **A render thread of our own.** Today the car and the one-shots render inside XAudio2's callback on its
    thread, so a slow block would glitch. The fix is our thread rendering a few ms ahead into a ring the callback
    only copies, at a little latency. First measure one block's render time against its budget (0.67 ms per 32
    samples); only worth it near the limit, which positional sound (S3) or more cars may reach.
  - **Lock-free one-shot commands.** `play`/`stop` take the mutex `Voices::Mix` holds while mixing; a command ring
    like the records' would stop the game's thread ever waiting on the audio's.
  - **`lastError` across threads.** The module's error string is written by `loadSound` on the co-driver's loader
    thread and read by the game's: a benign race (a message, never a crash) to close with the command ring.

---

## 6. Game

### 6.1 Content roots and the VFS — `wrally::vfs`

W-Rally runs **standalone** on its own content. When it sits in an RBR folder, it **adds** RBR's content.

Layers, with later layers winning:

1. **RBR install**, read-only, *optional*. It is used when the W-Rally exe's folder (or `rbrPath` in `W-Rally.ini`)
   contains `RichardBurnsRally_SSE.exe` and `Physics/physics.rbz`. It provides `Maps/`, `Cars/`, `Physics/`,
   `Audio/` and `Replays/`.
2. **W-Rally content** (`<W-Rally>/content`): W-Rally's own stages, cars, sound sets, UI, default configs.
3. **Mod packages** (zip, later), one layer each.

Archive members use `archive.rbz:member/path` (`physics.rbz` is a zip of deflated members, `phys/data/inflate`).
Lookup is case-insensitive with forward slashes. The VFS serves the game and, through `HostServices::files`, every
module.

**Content catalog** (`wrally::assets::Catalog`). This is what the menus list: one list of stages and one of cars,
with a source tag on each.

- **W-Rally content** is listed by manifests: `content/stages/<id>/stage.json` and `content/cars/<id>/car.json`.
- **RBR content**, when the RBR layer is present, is listed from `Maps/Tracks.ini` and `Cars/Cars.ini`, and wrapped
  in the same catalog entries in memory. RBR's files are never rewritten.
- Without RBR, the RBR entries do not exist and nothing in the game refers to them. The game never assumes
  `physics.rbz` or `Maps/` exists.

**Registers** (`assets::CarRegister`, `assets::TrackRegister`): what W-Rally knows of each car and track in the
catalog, in layers, a later one winning field by field - RBR's files (`Cars.ini`'s names, `Tracks.ini`'s stage names
and particle sets, the surface rule), then the server's list (hardcoded until there is a server), then the item's own
manifest: `Cars/<folder>/car.json` (name, maker, year, class, drive, power, weight, physics) and `track-<n>.json`
beside the stage's `.ngg` or in `Maps` (name, country, surface, length). A car neither the server nor a `car.json`
knows is listed but not usable by W-Rally: nothing says what it is or how it drives.

### 6.2 Formats — `wrally::formats`

One static library used by the game and by modules that need it. Readers only, with one exception: the NGG writer the
legacy importer uses (decision 2), which sits next to the renderer's reader. Other writing belongs to Wallaby/tools.

| format | what | source today | used by |
|---|---|---|---|
| `stage.json`, `car.json` | **W-Rally manifests**: what a stage/car consists of, per-physics/per-audio data files | new | game catalog |
| `.lsp`, `.rbz` | physics defs, car/setup/tyre files | `phys/data/lsp`, `inflate` | rbr physics |
| `.trk` | collision objects, drive line | `phys/data/trk`, `trkworld` | physics, game (drive line) |
| `.col` `.mat` `.fnc` | static collision, surfaces, fences | `phys/data/col`, `phys/native` | physics |
| `.fnc` + `Textures/fence.dat` | the fences drawn: post points, tape/post textures | `formats/fnc` | game (`assets/fences`: drawn, knocked about by the car) |
| `.lbs` | **render geometry** (+ script objects) | script objects: `phys/data/lbs`; geometry: W-Rally's own reader (decision 2) | legacy importer, physics |
| `.dls` | cameras, pacenotes, splits/finish | `phys/data/dls` | game |
| `.sgc` + `<car>.ini` + `_shaders.ini` | car render model + collision meshes | `phys/drive/car_model`, `phys/data/sgc` | game → renderer model, physics |
| `.dds` | textures | (pass-through) | renderer |
| `Tracks.ini`, `TrackSettings.ini`, `Cars.ini` | stage list, surfaces, weather/lighting, car list | `sim::StageSurface`, NGG weather | game catalog |
| `.eng`, audio banks | engine sounds | `audio/` | audio module |
| `.rpl` | RBR replays | `phys/data/rpl` | game (playback by state) |
| `.phr` | W-Rally recordings (controls + checks) | `phys/data/rec` | game |
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
- **W-Rally stages** (`stage.json`) ship an `.ngg` and gameplay files *in RBR's formats*, written by our tools.
  That is what NGG design §7 already specifies for native tracks (".ngg + LBS/COL/DLS gameplay stub"). Reusing the
  formats does not need any RBR content, and it lets both physics modules run W-Rally stages from day one. Later the
  gameplay data moves into `.ngg` chunks (decision 10), so a W-Rally stage becomes one file; the manifest is what
  makes that change local.

So there is **one renderer path**. A legacy stage is "an NGG converted on the fly with default PBR materials", which
is what `Material`'s defaults are designed for (`DefaultMaterial()`, every factor standing alone).

**W-Rally imports `.lbs` itself** (decision 2): a C++ reader of the `.lbs` geometry and a simple automatic
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
- **W-Rally cars** (`car.json`): the render model (`.sgc` written by our tools at first, a car `.ngg` later),
  `wphysics.json` for the physics (WPhysics-only, decision 8: no `.lsp`), and a sound set for the audio.
- The game animates the model from `CarState`: body pose → instance `world`, `wheels[k]` position/rotation/spin →
  hub and spinning nodes, `damaged` bits → hide detached parts. This is phys-drive's car-part code, moved to node
  transforms.

### 6.5 Game modules (namespaces)

| namespace | contents | from phys-drive |
|---|---|---|
| `wrally::app` | `main`, window, threads, config (`W-Rally.ini`), crash handling | `drive.cpp` window/loop, args |
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
  game/               W-Rally.exe        (phase 1: src/drive/ = phys-drive, imported)
      src/app  src/modules  src/assets  src/session  src/rules  src/input
      src/camera  src/ui  src/sound  src/replay
      config/         sound-mix.ini ...
  modules/
      renderer/       WRenderer.dll     (src/core, src/format: ngg-core; src/api: ngg-api, until phase 4)
      physics-rbr/    WPhysicsRBR.dll   (src/num, src/data, src/native, src/sim: the native RBR physics)
      physics-w/      WPhysics.dll
      audio/          WAudio.dll        (src/audio: plugin-rbr/audio)
  third_party/        dxc (dxcompiler.dll, dxil.dll); later imgui, nlohmann/json
  content/            W-Rally's own stages, cars, sound sets, default configs (standalone)
  tests/              iface layout tests, module conformance (any DLL of a kind through its Api),
                      rbr bit-exact suite (phys-native-check), renderer smoke, audio offline golden
  tools/              module-info (lists a DLL's kinds/versions/caps), phr-check
```

Output layout, mirroring the runtime:

```
W-Rally.exe
modules/renderer/WRenderer.dll, dxcompiler.dll, dxil.dll
modules/physics/WPhysicsRBR.dll, WPhysics.dll
modules/audio/WAudio.dll
content/   config/   cache/   W-Rally.ini
```

---

## 9. Phasing

Each phase ends with a game you can drive. phys-drive works on day one, and every step keeps it working.

1. **Skeleton + phys-drive moved in** — DONE 2026-10-25. CMake, `iface` headers, `ModuleHost` + `FileHost`,
   `tools/module-info`, a stub module and handshake tests. phys-drive and the libraries it uses were imported
   (docs/imported.md): W-Rally.exe is phys-drive, still drawing through `ngg-api.dll`, with `NativeSim` and
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
6a. **Menus** — 2026-10-26 (phase 7 moved later). With no command line W-Rally opens a main menu: Quick rally,
   Settings, Exit (`game/src/ui/menus.*`, Dear ImGui, mouse or keyboard). The window, the renderer and ImGui live for
   the whole run, and a stage loads and unloads its scene on them (`app/game.cpp`).
   - **Quick rally:** stage, car, setup (the default or a saved RBR `.lsp`), damage (off, safe, reduced, realistic),
     weather (the `.ngg`'s presets, named as `TrackSettings.ini`'s sections) and road (from the weather, dry, damp,
     wet). The choice becomes a command line (`--setup`, `--damage`, `--weather`, `--road`), so a menu run is the
     same as a CLI run. Custom weather (sun, rain, clouds...) comes later.
   - **All cars:** the catalog (`assets/catalog.*`) lists every `Cars/<folder>` that has an `.sgc`, not only the eight
     `Cars.ini` slots. A car's physics comes from its own `Cars/<folder>/physics/` if it has one, else its slot's
     `physics.rbz` folder, else its `car.json`'s. There is no stand-in: a car without physics of its own is not usable.
     `--extract-slot-physics` (development) copies the slot cars' physics out of `physics.rbz` into their folders, so
     each car's physics is a set of loose files.
   - **Esc in a stage:** the stage holds and a menu comes up: Resume, Restart stage, Settings, Debug (what the keys
     switch: renderer switches, debug view, camera, chase distance, mute, anti-lag) and Exit to main menu. A CLI run
     still quits at its end (the window closed, a replay's end, `--exit-at`). Its Esc menu leads to the main menu.
   - **Settings:** saved in `W-Rally.ini` in the user folder (`app/settings.*`, `app/paths.h`). They cover the RBR
     folder, graphics (applied at once; the full screen, a borderless one, by default) and sound. The `.ngg` folders
     (`ngg_folders`, default `<RBR>\Ngg`) are the `.ini`'s alone, for development.
   - **Installed:** the MSI (`installer/`, built on every push by `.github/workflows/installer.yml`) installs to
     Program Files\W-Rally and asks for the RBR folder. It pre-fills the one it finds, and goes on only when the
     folder has `RichardBurnsRally_SSE.exe`, `Physics.rbz`, `Maps\Tracks.ini` and `Cars\Cars.ini`. It writes that
     folder to `install.ini` beside the exe, which the game reads before `W-Rally.ini`. With `install.ini` present,
     the user folder is `%LOCALAPPDATA%\W-Rally` (`W-Rally.ini`, `W-Rally.log`, `drive.phr`, `cache\`). Without it
     (a build run from its tree), the user folder is the exe's own.
   - **Controls:** the Settings tab that binds every action's key (`game/src/input/bindings.*`). A key is held by its
     place (scan code), named as the layout prints it. Click an action and press a key: Esc keeps the old one, Delete
     unbinds, and an action whose key was taken becomes unbound. The keys are saved as `key_<action>` in
     `W-Rally.ini`.
7. **Content.** VFS layers, catalog, `stage.json`/`car.json` with their schemas, and standalone mode without RBR
   (tested with the RBR layer switched off; RBR content remains the development data, decision 7).
8. **Legacy stages.** W-Rally's own automatic importer (decision 2), so every RBR stage draws without Wallaby:
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
   1b. vegetation without cardboard — DONE 2026-10-27, on imported and Wallaby stages:
      - **Import marker:** the importer sets `kSceneFlagLegacyImport` in the scene header. The renderer never guesses
        plants on such a scene (`FindPlants` does not run).
      - **Plant table from topology:** the importer writes `NGGP` itself. Each connected piece of a vegetation
        mesh is a plant, with no tags and nothing merged; pieces over 50 m are left out. The renderer gives an
        import's untagged plants the full plant path: volume normals, the own-plant ray skip and the canopy's
        inner darkening. Stage 41: 25,944 plants. RBR's vegetation block also holds people, cars and fences, which
        become plants too; Wallaby's tags are the exact answer.
      - **Grass and bush shadows (all stages):** `grass-card` plants cast no sun shadow and no AO. `bush-card`
        plants cast a lighter, softer one: half their ray hits, every other shadow-map texel. Trees are unchanged.
        An import goes by plant height instead: under 0.65 m grass, under 2.5 m bush, measured from Wallaby's tags
        on stage 41.
      - **Legacy profile:** alpha-tested cards outside the plant table (fences, banners, spectators in scenery)
        on imports. Mode 0 off, 1 A (RBR's bake times today's light), 2 B (dynamic, the bake as occlusion;
        default). Their rays skip cards within 1.5 m. Switched by Debug → Legacy cards, `NGG_LEGACY_PROFILE` or
        renderer interface 3's `setLegacyProfile`.
   2. normals rebuilt, foliage cut out
   3. weather presets from `TrackSettings.ini` (DONE 2026-10-29), every RBR stage in the menu catalog (DONE
      2026-10-30); the cache **deferred** (below)
      - **Weather:** the importer writes every `TrackSettings.ini` section of the stage's map as an `NGGW` preset
        (`game/src/assets/legacy_weather.*`, reading through `game/src/formats/track_settings.*`), named as the section,
        so `--weather` and the menu select it. The stage's own time of day, crisp and clear, is the default. The
        translation is plugin-rbr's (NGG design 5.0q): SunDir and the ground's reflectance kept, the condition in the
        name turned into haze, cloud cover, fog visibility, rain and snow, decimal commas read, and daytime suns at or
        below the horizon raised. Checked against plugin-rbr's own translation (`rbrtm --export-weather`): stages 108,
        41 and 4, 61 presets, every field the same to float precision. On stage 41, `--weather 41O_M_lightfog_heavycloud`
        draws an overcast, foggy sky; the default draws a sunny morning.
      - **Maps under 10:** `TrackSettings.ini` writes them in two digits (`04E_...`). The catalog's weather list missed
        them with `4E_`, and now uses `formats::TrackSettingsMap` too.
      - **Tests:** `legacy-import-41` also checks the weather line (7 presets, default `41M_crisp_clear`), and
        `legacy-import-weather-4` checks a map under 10 (43, `04E_crisp_clear`). `ngg-check` already compares the
        renderer's preset count with the file's.
      - **`N` is noon** (2026-10-30), not night: its suns stand ~50 degrees up, and RBR has no night stages.
        plugin-rbr's translation left 17 overcast noon presets with the sun 2.8 degrees up, drawn as dusk. Now every
        preset's sun within ~3 degrees of the horizon is raised to ~10 degrees on its bearing (48 in all; stage 11's
        `11O_N_hazy_partcloud`, stage 69's four).
      - **Every stage in the menu** (2026-10-30): the catalog offers, after the stages an `.ngg` draws, every other
        `Maps/<stage>.lbs` with its `.trk`, "imported from .lbs", its weathers `TrackSettings.ini`'s for its map and
        time of day, drawn (`Stage::imported`). In the stock RBR folder: 194 stages, 4 from `.ngg` files and 190
        imported. CTest `catalog-every-stage` (`tests/catalog_check.cpp`) checks that, and stage 4's evening weathers
        under `04E_`.
      - **Where a stage comes from, best first:** a hand-finished `.ngg` from the stage server (planned: tweaked by hand
        in Wallaby, better than any automatic import), then an `.ngg` in the folders, then the automatic import. The
        catalog has the seam for the first (`catalog.cpp`).
      - **The cache, deferred:** an import takes about 0.5 s (stage 41), and the server's copies will replace most
        imports, so caching the automatic import is not worth it now.
      - **Coverage** (sweep of every `.lbs` with a `.trk` in the stock RBR folder, `ngg-check --lbs`, 2026-10-30):
        every plain stage imports and loads (164); the 31 failures were all **protected stages**, encrypted (maps 10
        overcast, 95, 96, 105, 488-499: every geometry block version 88 or 89, ciphertext at 8.00 bits a byte). The
        reader skipped their encrypted ground, then read their encrypted scenery as garbage ("unknown vertex type").
        Now `formats::LbsEncryptedVersion` finds them from the block headers alone: the reader refuses such a file
        whole ("encrypted - a protected stage ..., not imported", CTest `legacy-import-protected`), and the catalog
        leaves them out (`Catalog::protectedStages`). They are never imported in part either: decrypting is a
        licensed install's job, and a plain `.ngg` of one would leak it (NGG design 3.7). The menu: 163 stages, 4 from
        `.ngg` files, 159 imported. (The other 2 sweep failures were `Clouds.lbs` and `skyboxes.lbs`, not stages.) 32
        stages have no `TrackSettings.ini` section at all and draw the default sky with no weathers to pick.
9. **Game.** Menus, car/stage selection, settings, wheels + FFB, replays, results.
10. **WPhysics.** Fork, JSON config, higher tick rate, then extend. Multi-car and save-state come with their
    interface bump.
11. **Native gameplay in `.ngg`** (decision 10), then **first W-Rally stage and car** for standalone (§11).

---

## 10. Decisions (2026-10-25)

1. **GUI: Dear ImGui** for all game UI in v1, rendered through `FrameDesc::ui`.
2. **Two converters (revised 2026-10-26).** W-Rally has its own simple, automatic LBS → NGG importer in C++, and
   Wallaby stays the authoring converter. Wallaby does the sophisticated work: replacing low-poly objects, manual
   overrides, tagging, plants and weather import. The game's importer needs no human input and makes any RBR stage
   drawable and dynamically lit. No Wallaby code ships with W-Rally or is copied into it; the importer is written
   against RBR's formats. A Wallaby-made `.ngg` always wins over the automatic import. To keep the two NGG writers
   from drifting apart, W-Rally's writer sits next to the renderer's reader (one format header), a round-trip test
   writes, loads and compares, and a test loads Wallaby-made `.ngg` files as another producer of the format.
3. **Audio is a module** (`ModuleKind::Audio`, WAudio.dll), fed by the physics' record stream (§5).
4. **WPhysics** may run at a higher tick rate and has its own configuration, in JSON (`config/physics/wphysics.json`,
   `cars/<car>/wphysics.json`). The interface leaves config format and `dt` to each module.
5. **Standalone + RBR.** W-Rally works without RBR on its own content. Placed in an RBR folder (or pointed at one), it
   also uses RBR's content, read in place and never shipped.
6. **Starting point: `phys/drive`** (phys-drive), which already combines the physics, the NGG renderer DLL, the
   audio, the stage clock and replays.
7. **Development content: RBR's own stages and cars.** Until the game runs end to end, all development and testing
   uses an RBR install. Standalone mode is built and tested with the layer switched off, but W-Rally's own first
   stage and car are not needed before phase 9 (§11).
8. **W-Rally cars are WPhysics-only.** They carry `wphysics.json`, never `.lsp`. The rbr physics module runs RBR cars
   only, so standalone play (no RBR install) runs on WPhysics.
9. **JSON: nlohmann/json, with published schemas.** Every manifest and module config has a JSON Schema in
   `docs/schemas/` (`stage.schema.json`, `car.schema.json`, `wphysics.schema.json`, `wphysics-car.schema.json`). The
   game validates manifests against them at load and in `tools/`, and modders get the same files.
10. **Native gameplay format: later, inside the `.ngg`.** W-Rally stages use RBR's `.trk/.col/.mat/.fnc/.dls`
    formats until the game runs end to end. Then collision, surfaces, fences, the drive line, splits and pacenotes
    become `.ngg` chunks, so a W-Rally stage is one file. Physics modules read them through the same VFS. Readers
    still accept the RBR formats, because RBR stages keep using them.

## 11. Open questions

- **First own content for a standalone release.** Deferred by decision 7. Revisit after phase 9: Wallaby-authored,
  generated (NGG design §7), or licensed community content.
