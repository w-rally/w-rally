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

## Third-party code

| here | what | from |
|---|---|---|
| `third_party/imgui/` | Dear ImGui 1.91.5 (MIT): core + `backends/imgui_impl_win32.*`, unchanged | github.com/ocornut/imgui, tag v1.91.5, downloaded 2026-10-25 |
| `third_party/imgui_config/` | WRally's ImGui configuration (32-bit indices) | ours |
| `third_party/dxc/` | DirectX Shader Compiler redistributables | see the table above |

**Checked on import:** WRally.exe and plugin-rbr's `phys-drive.exe` with the same arguments (`C:\Brb`, track-108_M,
`--no-sound --step 0.0166 --quick-start --exit-at 2000 --overrides none`) write byte-identical `.phr` recordings.
