---
name: wrally-game-dev
description: Load before changing or debugging W-Rally's game executable (game/src - app, session, camera, input, assets, render, ui, formats, sound, rules), its menus/ImGui UI, Settings rows, W-Rally.ini, the user folder, install.ini, the VFS and content layers, the module loading/ABI, the Garage/showroom, replays (.phr, drive.phr), the co-driver or the sound mix. Gives the code map, the run modes and command lines, the settings/paths rules, recipes for common changes, and the traps earlier sessions hit.
---
# W-Rally: the game executable

W-Rally (`<repo>`) is a standalone rally game. `W-Rally.exe` (namespace `wrally::app`) owns the window, input,
menus, rules, replays, VFS and config. Renderer, physics and audio are DLLs under `bin/modules/` loaded through
the iface ABI. Read first: `docs/design.md` §1 (who owns what), §2 (ABI), §6 (game), `docs/content-layers.md`
(where a file comes from; it overrides design.md §6.1 where they differ). Building and ctest: skill
`wrally-build-test`. Running to capture/measure frames: skill `wrally-render-measure`. Cars: skill `wrally-car`.

## Code map (game/src)

| folder | what | start at |
|---|---|---|
| `app/` | `main.cpp` (output, exit box), `game.cpp` (RunGame: lab / showroom / command line / menus; RunStage; the Showroom class; QuickArgs), `options.*` (command line, **the env-var list in options.h's header**), `settings.*` (W-Rally.ini), `paths.h` (user folder, install.ini, downloads), `content.*` (MakeVfs), `window.*` | `game.cpp` RunGame (end of file) |
| `session/` | one stage run: `session.*`, `sim_thread.*` (physics thread, `--step` lockstep), `launch_control.h`, `anti_lag.h` | |
| `assets/` | `catalog.*` (stages, cars, physics variants), `car_register.*` / `track_register.*` (names, car.yaml/track.yaml), car model/render/lights, `legacy_stage.*` (.lbs import), `legacy_weather.*`, `fences.*`, `showroom.*`, `lamp_lab.*`, `splash.*` | |
| `ui/` | `menus.*` (MainMenu, SettingsUi, PauseMenu, DebugMenu, MixPanel), `quick_rally.cpp` (Quick rally pages, Garage), `menu_look.*` (design system "Factory black": colours, plates, chevrons), `menu_video.*` (menu loop `content/video/menu.mp4`), `hud.*`, `imgui_layer.*` | `menus.h` header comment |
| `input/` | `bindings.cpp` (`kInfo`: every action, default scan code), `keyboard.h` | |
| `camera/` | chase/bonnet/beside cameras, `replay_camera.*` (TV cameras from .dls), `scenery.*` | |
| `render/` | `renderer.*` (C++ wrapper over the renderer iface), `stage_objects.*` | |
| `physics/`, `sound/` | wrappers over the physics/audio DLLs; `sound/codriver.*` (pacenotes), `sound/mix_edit.*` | |
| `formats/` | readers: `lbs_geometry`, `fnc`, `dds_image`, `track_settings` | |
| `rules/` | `stage.*`: StageClock, penalties | |

Elsewhere: `include/wrally/iface/*.h` is the ABI (common, renderer, renderer_edit, physics, audio, records). It is at
the repo root, **not** under `game/` or `modules/renderer/include`. `libs/` holds vfs, json (YAML subset), modules
(ModuleHost, FileHost), signals (record bus), png. `game/config/sound-mix.ini` is the default mix.

## Module ABI in one paragraph

Each DLL exports `GetWInterfaceVer(kind)` and `InitWInterface(gameVersion, in, out)`. The game uses a DLL only if
its version equals the game's counter: `kRendererInterfaceVer` (renderer.h), `kPhysicsInterfaceVer` (physics.h),
`kAudioInterfaceVer` (audio.h). **Any** change to a struct or function table of a kind bumps its counter by one,
including a function appended at the end. Boundary rules: POD, fixed-width types, `#pragma pack(push, 4)`,
`static_assert` sizes, no STL/exceptions/`bool`, files only through `HostServices::files`. `renderer_edit.h`
additions do not move the counter. `tools/module-info` lists a DLL's kinds and versions.

## Run modes (app/game.cpp RunGame)

| command | what |
|---|---|
| `W-Rally.exe` | intro, then menus |
| `W-Rally.exe <RBR> <stage .ngg or .lbs> --stage track-41_M ...` | that stage at once; Esc menu's "Exit to main menu" goes on to menus |
| `W-Rally.exe --showroom <car folder> [--turn deg] [--spin deg/s] [--shots n] [--then car] [--stop n] [--jerk k]` | Garage showroom, writes `showroom.png` (or `showroom-NN.png`) to the **cwd**, then quits |
| `W-Rally.exe --lab [--camera 1] [--time HH:MM] [--exit-at frame]` | lamp lab: test lamps in a synthetic scene, no physics |

A menu run is a command line too: `QuickArgs` builds it from Settings, then `Parse`. The full list is in
`app/options.h`. A run as Quick rally makes it today (from `tools/car-ghost/check-car-ghost.sh`):

```
W-Rally.exe <RBR> <RBR>/Ngg/fr_joux_verte.ngg --stage track-42_M --track fr_joux_verte@rbr \
  --sgc <RBR>/Cars/IMPREZA05/impreza05.sgc --car-item impreza05@rbr --car cars/impreza05/physics/rbr/rbr/ \
  --physics-base physics/rbr/rbr/ --physics rbr --damage 2 --road dry \
  --replay <drive.phr> --camera 0 --skip-to <tick> --exit-at <tick> --no-sound --no-codriver --out <dir>/drive.phr
```

The short form the golden test uses also still works: `<RBR> <ngg> --stage track-41_M --step 0.1 --no-sound
--overrides none --replay <golden .phr>`.

Keys in a stage (input/bindings.cpp): F1 readout, F2 screenshot (`shot-NNN.png` next to `--out`), P pause, T TAA,
O AO, L shadows, K ray tracing, V debug view, M mute, C camera, R back on road, H/J lamps, Esc pause menu.

## Settings, folders, logs

- **User folder** (`app/paths.h` UserDir): `%LOCALAPPDATA%\W-Rally` **only if `install.ini` sits beside the exe**
  (an installed game). A build run from its tree uses the **exe's folder**, i.e. `<repo>/build/bin`.
- It holds `W-Rally.ini`, `W-Rally.log`, the last drive `drive.phr` (Quick rally's `--out`) and `cache\`.
  A command-line run writes `--out` (default `drive.phr`) **relative to the cwd**.
- `install.ini` (installer) is read first (`rbr_path`, `download_path`), then the user's `W-Rally.ini`.
- `W-Rally.ini`: `name = value` lines. The keys are in `settings.cpp` `Fields()`: `rbr_path`, `download_path`,
  `ngg_folders` (dev only, default `<rbr>\Ngg`), `fullscreen`, `quality`, `ray_tracing`, `taa`, `upscaling`
  (`kUpscaleModes` index), `denoiser`, `vegetation_detail`, `shadows`, `vsync`, `sound`, `codriver*`,
  `hud_pacenote_next`, `dev_engine`, `dev_als_bangs`, `dev_als_idle_bangs`, `net_server`, `quick_*` (last Quick rally
  choice), plus `key_<action>` (hex scan code), `lamps_<car> = layout|spread|pitch`, `physics_<car id> = <variant>`.
  Saving rewrites the whole file from `Fields()`: hand-edited values of known keys survive, **unknown keys and
  comments are dropped**. A new key must be in `Fields()` or it is lost at the next save. An old `wrally.ini` is
  still read when there is no `W-Rally.ini`.
- Output: a redirect wins (`> log.txt 2>&1`, as CTest's), else the parent console, else `W-Rally.log` in the user
  folder. `PHYS_DRIVE_NGGLOG=1` adds the renderer's log.
- Without RBR the game is standalone; RBR's content is a read-only layer (`rbr_path`, or the exe's folder if it has
  `RichardBurnsRally_SSE.exe`). **Nothing ever writes into the RBR folder.**

## Content layers and VFS (docs/content-layers.md)

Layers, later wins: RBR (mapped into W-Rally's layout, plus raw under `rbr/<RBR path>`), W-Rally `content/`
(`<exe>/content`, then the source tree's `content/` in a dev build), downloaded (`download_path`; in a dev build
`<bin>\Downloads`). Item paths: `cars/<id>/<version>/...`, `cars/<id>/physics/<variant>/<version>/...`,
`tracks/<id>/<version>/...`, `physics/<base>/<version>/...`; RBR items have the version `rbr`.
`content/rbr-tracks.yaml` maps RBR map numbers to track ids (written by `tools/rbr-tracks/make_table.py`).
Manifests are YAML (`car.yaml`, `track.yaml`). Not done yet: version fingerprints, cleaning (steps 7, 8).
`content/` is a **git submodule** (`w-rally/content`).

Game files of its own (`content/...`, `game/config/...`): `app::WRallyFile(rel)` looks beside the exe, then in the
source tree (`WRALLY_SOURCE_DIR`), so editing `game/config/sound-mix.ini` shows at once in a dev build.

## Sound in brief (no separate skill: too little recurring work)

- `modules/audio` builds WAudio.dll (`src/audio`: grains, bank, mix, voices, device; `src/module`). Game side:
  `game/src/sound` (`sound.*`, `codriver.*`, `mix_edit.*`). Design: `docs/design.md` §5. The grains plan
  (S0-S4) is in plugin-rbr's `doc/sound-engine-plan.md`.
- Engine loops: RBR's `audio/Cars/<eng>.eng` (`--eng`, default `Focus03-08`). Mix: `--mix` or
  `game/config/sound-mix.ini`, re-read when saved; Debug menu / MixPanel edit it.
- Checks: `audio-check` renders offline and compares WAudio.dll with the linked engine **bit for bit**
  (`audio-offline-*` tests); `mix-edit`, `codriver-41` tests. Run `audio-check` with no args for the synthetic case.

## Recipes

**A new setting (W-Rally.ini + Settings screen):**
1. Field with default in `app/settings.h` `Settings`.
2. One `f("ini_key", s.field)` line in `settings.cpp` `Fields()` (load and save both use it).
3. A row in `ui/menus.cpp` SettingsUi: `add(SetRow::Toggle|Choice|Slider|Text|Action, "LABEL", "help").on = &s.field;`
   Labels are upper case; the help says when it applies ("Applies at once." / "From the next stage.").
4. If a stage needs it: pass it through `QuickArgs` (a flag in `options.cpp`) or read `sh.settings` in RunStage.
   A command-line run must still work without the menus.
5. Env-var overrides go in the `options.h` header comment list (`PHYS_DRIVE_*`).

**A new key:** add to `input::Action` and `kInfo` in `input/bindings.cpp` (id, label, group, default scan code).
It shows in Settings > Controls and saves as `key_<id>`.

**A new menu screen:** follow `menus.h`: a function that draws one screen between `ImGuiLayer::NewFrame` and
`Render` and returns the choice. It holds no state of its own. Draw with `menu_look.h` pieces; sizes are px at 1080p
times `k`. Fonts are Segoe UI (standing in for Saira).

**A change across the ABI:** edit `include/wrally/iface/<kind>.h`, bump the counter, update `static_assert`s and the
module's `module.cpp`. Then rebuild the game and the module together (the `W-Rally` target depends on all three
modules).

**Reproduce what the user saw:** their last drive is `<user folder>/drive.phr` (`build/bin/drive.phr` in a dev
build). **Copy it to the scratchpad first**: the next run overwrites it. Replay it with `--replay` and the same stage
and car (see the run line above).

## Workflow conventions

- Work in a worktree on a branch `claude/<name>` (`<repo>/.claude/worktrees/<name>`). Merge into `main` with
  `git merge --no-ff claude/<name> -m "Merge: <what>"`. Commit subject `<Area>: <what changed>`, a body that says why,
  numbers where measured.
- Every change to docs/code keeps `golden-replay-41` bit-exact (physics path) and the audio bit-exact tests green.
- Public repos: no local paths in code, comments, CMake, commit messages (use `<RBR folder>`-style words or a
  CMake/env variable).
- GPU-heavy runs, DLSS/NVIDIA checks, long ctest: on an NVIDIA machine (a local hand-off skill, if installed).

## Pitfalls

| symptom | cause | fix |
|---|---|---|
| `cmake` / `ctest` "not recognized" / "command not found" | not on PATH | use VS 18's bundled `<cmake>` / `<ctest>` by full path (skill `wrally-build-test`) |
| `LNK1104: cannot open ...\W-Rally.exe` (Czech: "se nedá otevřít") | the game is still running | `Stop-Process -Name W-Rally` (PowerShell) or `taskkill //F //IM W-Rally.exe` (Git Bash). The process is `W-Rally`, not `WRally` |
| `renderer: no renderer module "wrenderer" in ...: WRenderer.dll: InitWInterface: ErrVersion` | the DLL and the exe were built from different iface headers (a half-failed build, a DLL locked while linking, an old DLL copied in) | close the game, rebuild `W-Rally` (builds the modules too), check the build log for errors |
| Grep/Read on `game/include/wrally/iface/...` or `modules/renderer/include` fails | wrong place | the ABI is `include/wrally/iface/` at the repo root |
| A new worktree builds but finds no content / `git submodule status` shows `-<hash> content` | submodule not checked out | `git submodule update --init content` in the worktree |
| `W-Rally.log` looks days old | you read `%LOCALAPPDATA%\W-Rally` (an installed copy's) | a dev build logs to `build/bin/W-Rally.log`; better: redirect stdout to a file |
| `Get-Content: Cesta ...\C se nenašla` when reading the ini | a path list collapsed to its first char (`$ini[0]` on a string) | test `Test-Path` on one path; dev ini is `build/bin/W-Rally.ini` |
| Python heredoc edit in Bash: `unexpected EOF while looking for matching '` (33 times) | quotes in the heredoc body | write the script with Write to the scratchpad, run `python -I <script>`; or use Edit |
| Python script: `SyntaxError: unterminated string literal` on `'\'` | backslash in a heredoc/inline string | `os.path.basename`, raw strings, or a script file |
| `ModuleNotFoundError: numpy` / `scipy` | system Python has Pillow only | PIL-only code, or ask the user before installing numpy |
| PowerShell: "Remove-Item on system path '"C:\Program' is blocked" | a `rm`/`del` alias in the same command as a quoted `Program Files` path | run the delete as its own command with a plain path |
| Edit: "String to replace not found" / "File has been modified since read" | file changed (CRLF, another session, a script edit) | Read again, then Edit; watch CRLF in `menus.cpp`-style files |
| A capture or ctest behaves oddly | a `PHYS_DRIVE_*` / `NGG_*` env var still set in the shell | clear them (`$env:PHYS_DRIVE_OFF=$null`, ...) before ctest |
| Keys sent to the game do nothing | the window is not in front | bring it to front first, or use `--exit-at`, env switches and `PHYS_DRIVE_SHOTS` instead of keys |
| `drive.phr` appears in the repo root | `--out` defaults to the cwd | run from the scratchpad or pass `--out`; `*.phr` is git-ignored anyway |
