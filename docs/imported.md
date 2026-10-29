# Code imported from plugin-rbr

WRally is self-contained: it builds nothing from outside this repository. The code it shares with plugin-rbr
(`C:\Projects\onlineracing.cz\plugin-rbr`) was **copied** in on 2026-10-25 from plugin-rbr commit `e696c4f`
("M6 cleanup, step 2: the native physics self-contained"). From then on, the two copies evolve separately. A fix
that matters to both is ported by hand.

plugin-rbr keeps what WRally does not need: the classic physics (`phys/core`, `phys/model`, `phys/oracle`: RBR's
own code run in an arena), the in-game probe, the RE tooling, the d3d9 bridge and IPC, and the RBRTM plugin. The
classic physics remains the oracle that the native port is checked against (`phys-native-check`, `phys-sim-check`).

| here | from plugin-rbr | changes on import |
|---|---|---|
| `modules/physics-rbr/src/num/` | `phys/num/` | none |
| `modules/physics-rbr/src/data/` | `phys/data/*.h *.cpp` | none |
| `modules/physics-rbr/src/native/` | `phys/native/` | none |
| `modules/physics-rbr/src/sim/sim.h`, `native_sim.*` | `phys/sim/` | `sim.h`: the classic `Sim` class removed |
| `modules/physics-rbr/src/sim/stage.cpp` | `phys/sim/sim.cpp` (`StageClock`, `StageMap`, `StageSurface`, `SurfaceTyreType`, `kTyreTypeNames`) | split out of the classic `Sim`'s file |
| `modules/physics-rbr/src/sim/fault.cpp` | `phys/sim/sim.cpp` (`PrintStack`, `PrintFaultStack`) | split out likewise |
| `libs/signals/signals/` | `signals/bus.h`, `car.h` | none |
| `modules/audio/src/audio/` | `audio/` (audio, bank, device, grains, mix, sample, wav) | none; the bench and scan tools not imported |
| `modules/renderer/src/core/`, `api/`, `format/` | `ngg/core/`, `ngg/api/`, `ngg/format/` | none; the host, bridge, ipc and tools not imported |
| `docs/ngg-design.md` | `doc/rendering-overhaul-design.md` (commit `e756b11`) | **moved** 2026-10-29, not copied: the renderer's design record continues here, and plugin-rbr keeps only a pointer. A move note heads the file |
| `third_party/dxc/` | `ngg/api/build/bin/dxcompiler.dll`, `dxil.dll` | binaries (Microsoft DirectX Shader Compiler redistributables) |
| `game/src/drive/` (split in phase 5, below) | `phys/drive/` (drive, car_model, replay_camera, launch_control, anti_lag) | `drive.cpp`: `--classic` removed; WRally's own files found by `WRallyFile` (beside the exe, else the source tree) |
| `game/config/sound-mix.ini` | `phys/drive/sound-mix.ini` | none |
| `content/cars/s_i2003.ini` | `cars/s_i2003.ini` | none |

The physics libraries keep plugin-rbr's compile settings (C++17, `/fp:precise`, `PHYS_RSQRT=AMD`), because the
native physics is bit-exact with RBR only under them.

## Changed since the import

- Phase 2: every file read in `data/` and `native/run.cpp` goes through `data/files.h` (`ReadWholeFile`, with a
  reader the module sets). `sim::Config::overrides` is the overrides' text, no longer a path. The stage clock and
  stage helpers moved out of the physics into `game/src/rules/stage.*` (namespace `wrally::rules`). `drive.cpp`
  loads the physics as a module (`--physics <id>`) and checks a replay's poses against its recording.

- Phase 3: the sound reads its files through `audio/files.h` (`ReadWholeFile`, `FileExists`, with a reader the module
  sets). `Bank::LoadRbr` takes "" for VFS-relative paths. `Bank::LoadBangFiles` takes a list of files
  (`LoadBangs` lists a folder and calls it). `Mix::Parse` reads a mix from text (`Read` reads the file and calls
  it). `drive.cpp` plays through the audio module (`--audio <id>`).

- Phase 4: `modules/renderer/src/api/` (ngg-api) removed. Its swap-chain and capture code lives on in the WRenderer
  module (`src/module/module.cpp`). `drive.cpp` draws through the renderer module (`--renderer <id>`), and its HUD
  is Dear ImGui instead of ngg-api's labels.

- Phase 5: `game/src/drive/` is gone. `drive.cpp` was split into `game/src/app`, `session`, `camera`, `input`,
  `assets`, `render/stage_objects`, `ui/hud` and `core/math`. The imported helpers moved and took the game's
  namespaces: `car_model` to `assets/` (`wrally::assets`), `replay_camera` to `camera/` (`wrally::camera`),
  `launch_control` and `anti_lag` to `session/` (`wrally::session`).

- Phase 6:
  - `sim::Physics::Parts` / `NativeSim::Parts` report the car's hinged and mounted parts, named by their def's
    kind.
  - ngg-core's `dxc.cpp` loads `dxcompiler.dll` and `dxil.dll` from its own module's folder (`LoadBeside`); a bare
    `LoadLibraryW` searched the exe's folder.
  - ngg-core's `renderer.h` comment on RT fallbacks is corrected; the code already instanced moving objects.

- Legacy stages (phase 8, step 1): `data/lsp.*` gains `ReadZip`, every member of a zip from one read (a stage's
  texture archive). `ngg_format.h`'s note on who writes the layout now names WRally's writer, `ngg_scene.h`.

- Physics libraries (2026-10-29, design 4.3a step 1): `phys-native` split into `rbr-base`, `rbr-collision`,
  `rbr-tyre`, `rbr-car` and `rbr-world`. `Unreached` and its probe moved from `native/entity.*` to
  `native/unreached.*`, so `drive.cpp` and `driveline.cpp` include that instead of `entity.h`. No code changed.
  - Step 2: `entity.h` split into `entity.h`, `solid.h` and `car_solids.h`, and `rbr-load` added (design 4.3a).
    The car's impulse code and `CarBody`'s methods moved to `car_parts.cpp`, the base classes' tick slots to
    `entity.cpp`. Two virtuals replace the casts to `Car` (`Solid::DriveLineStep`, `Solid::SurfaceAt`).
    `World::ReleasePart` takes `(Solid&, Solid*& carSlot, bool)`; `DetachHinged`, `ReleaseHinged` and
    `ReleaseStick` left `World` for `car_solids.h`.

- Vegetation (phase 8, 2026-10-27), in ngg-core:
  - `ngg_format.h` gains `kSceneFlagLegacyImport`.
  - `Scene` gains the legacy profile (`SetLegacyProfile`; shader `gLegacyMode` in the former `gPadB` slot) and a
    per-plant shadow mode (`Plant::shadow`). The mode is packed into plant vertices' uv1.y above
    `kPlantShadowPack`; `PSShadow` and `rt.cpp`'s `Occluded` read it.
  - `ReadPlants` takes `untaggedCards`; `FindPlants` is skipped on a legacy import.
  - The prepass marks a legacy card `~0`, and `Occluded` skips cards within `kLegacyCardSkip` of its rays.

- Menus (after phase 6): `native/run.cpp` reads a car's `common.lsp`, `<surface>.lsp`, `r_<surface>.lsp` and default
  setup from a loose folder when `physicsCar` is a VFS path ending in '/' (`CarFile`), and from `physics.rbz` as
  before otherwise. A setup can be a VFS file as well as an rbz member. The tyres still come from `physics.rbz`.

## Third-party code

| here | what | from |
|---|---|---|
| `third_party/imgui/` | Dear ImGui 1.91.5 (MIT): core + `backends/imgui_impl_win32.*`, unchanged | github.com/ocornut/imgui, tag v1.91.5, downloaded 2026-10-25 |
| `third_party/imgui_config/` | WRally's ImGui configuration (32-bit indices) | ours |
| `third_party/dxc/` | DirectX Shader Compiler redistributables | see the table above |

**Checked on import:** WRally.exe and plugin-rbr's `phys-drive.exe` with the same arguments (`C:\Brb`, track-108_M,
`--no-sound --step 0.0166 --quick-start --exit-at 2000 --overrides none`) write byte-identical `.phr` recordings.
