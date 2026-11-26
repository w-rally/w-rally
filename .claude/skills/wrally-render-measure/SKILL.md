---
name: wrally-render-measure
description: Load when you run W-Rally to measure, capture or A/B the NGG renderer: replay a drive headless-ish, take shot-NNN.png captures, compare images (noise, ghosting, before/after), time a change by GPU frame ms, use debug views, PIX/RGA, the showroom or the lamp lab. Gives the command lines, every PHYS_DRIVE_*/NGG_* switch, the standard replays and the traps that silently change what a run measures.
---
# W-Rally: measuring and comparing rendering

Depth: `docs/ngg-design.md §10` (10.1 A/B, 10.3 debug views, 10.6 device loss, 10.7 PIX/RGA, 10.8 all `NGG_*`).
Game switches live in `game/src/app/game.cpp` (`ToRender`, `RunStage`, `RunLab`, `RunGame`) and `game/src/app/options.cpp`.
The renderer reads its own in `modules/renderer/src/module/module.cpp` (`ApplyEnvironment`) and `src/core/*.cpp`.

## 1. A stage run from a replay

Run from a folder of its own: shots and the recording land beside `--out` (default `drive.phr` in the cwd).
Use Git Bash, `timeout`, and always redirect output to a file.

```bash
W=<repo>/build/bin/W-Rally.exe
d=<scratch>/run-a; rm -rf $d; mkdir -p $d; cd $d
PHYS_DRIVE_STEP=0.0333333 PHYS_DRIVE_SHOTS=30 PHYS_DRIVE_OFF=taa,vsync PHYS_DRIVE_FRAMEMS=1 \
  timeout 600 $W <RBR> <RBR>/Ngg/fr_cote_darbroz.ngg --stage track-41_M --replay <drive>.phr \
  --camera 0 --skip-to 11050 --exit-at 11250 --no-sound --no-codriver --overrides none \
  --out $d/drive.phr > log.txt 2>&1; echo rc=$?
```

- Positional: `<game dir>` (RBR folder) and the stage file, `.ngg` or a Maps `.lbs` (the legacy importer).
- `--stage track-NN_X` is needed when the file name holds no `track-<n>` (renamed stages: `fr_joux_verte.ngg`).
  A `--stage` of another map than the `.ngg` is only a WARNING: the car drives on ground that is not drawn.
- `--skip-to <tick>` runs the physics to that tick without drawing. `--exit-at`/`--pause-at` stop at a tick.
  A replay also quits at its own end and prints `replay check: N poses, all bit-exact ...`.
- `--step <s>` (or `PHYS_DRIVE_STEP`): fixed time per frame, simulation in lockstep. Same frames at the same ticks run to run.
- `--camera`: 0 chase, 1 bonnet, 2 above, 3 beside (`--camera-angle <deg>`), 4 TV (a replay's default when the `.dls` has cameras).
- Other useful: `--time HH:MM [--date]`, `--weather`, `--sky c,f,r`, `--road`, `--sgc`, `--car`, `--car-item`, `--track`,
  `--damage`, `--hold`, `--renderer`, `--debug-layer`. Full list: header of `game/src/app/options.h`.
- The car physics and setup must match the recording, or the replay check fails (a physics problem, not a render one).

### Re-running what the user drove
A Quick rally from the menus prints `quick rally: <the full argument list>` and writes `drive.phr` to the user folder
(beside the exe for a build, `%LOCALAPPDATA%\W-Rally` when installed). Started from Explorer, stdout goes to
`W-Rally.log` there. So: `grep "quick rally:" <user folder>/W-Rally.log`, copy `drive.phr` to your scratch folder
at once (the next drive overwrites it), then rerun those arguments with `--replay <copy>`.

### Showroom and lab
- `W-Rally.exe --showroom <car folder> [--turn deg] [--spin deg/s] [--shots n] [--then car] [--stop k] [--jerk k]`:
  the Garage turntable, captured after loading, then quit. One shot: `showroom.png`; n: `showroom-00.png`...
  in the cwd. Honours `PHYS_DRIVE_OFF` and `PHYS_DRIVE_FRAMEMS`. `--spin 12.6` = the Garage's speed (moving car: RT grain).
- `W-Rally.exe --lab [--camera 1] [--time 23:00] [--exit-at frame]`: lamp lab, synthetic scene, known albedos.
  `PHYS_DRIVE_SHOTS=n` = a shot every n FRAMES (not ticks). `PHYS_DRIVE_TEST_LIGHTS` picks the lamps.

## 2. Switches

### Game (`PHYS_DRIVE_*`, all verified in `game/src/app`)
| switch | effect |
|---|---|
| `PHYS_DRIVE_STEP=<s>` | = `--step`. 0.0333333 for 30 fps frames; 0.1 for the golden run |
| `PHYS_DRIVE_EXIT_AT`, `_PAUSE_AT` | = `--exit-at`, `--pause-at` |
| `PHYS_DRIVE_SHOTS=<n>` | `shot-NNN.png` every n ticks, beside `--out`. Swap-chain capture: a covered window is fine |
| `PHYS_DRIVE_OFF=taa,ao,shadows,rt,vsync` | off from the start. `taa` for image A/B; `vsync` for timing. `ao` = AO off (tier kept) |
| `PHYS_DRIVE_FRAMEMS=1` | at the end: `gpu frame: mean/median`, `gpu parts:` (main pass, RT medians), `gpu frame: p95 p99 slowest at tick`, `cpu phases`, `wall frame` (first 30 frames dropped) |
| `PHYS_DRIVE_UPSCALE=<idx or name>` | `off`, `fsr-native`, `fsr-quality`, `fsr-balanced`, `fsr-performance`, `dlaa`, `dlss-*`, `rr-dlaa`, `rr-*` (`kUpscaleModes`, `settings.h`) |
| `PHYS_DRIVE_UPSCALE_AT=<tick>:<mode>` | switch upscaling mid-run |
| `PHYS_DRIVE_DENOISER=ours\|nrd\|<idx>` | RT denoiser over the settings (also `NGG_DENOISER`, renderer side) |
| `PHYS_DRIVE_VEGETATION=low\|medium\|high` | vegetation detail over the settings |
| `PHYS_DRIVE_CAMERA_AT=<tick>:<cam>` | camera cut at a tick |
| `PHYS_DRIVE_NGGLOG=1` | the renderer's log on stdout (otherwise it is dropped) |
| `PHYS_DRIVE_READOUT=1` | F1 readout on from the start (shows in shots) |
| `PHYS_DRIVE_JITTER=<tick>` | RMS of the car's screen motion up to that tick, then quit |
| `PHYS_DRIVE_LOOK_UP=<deg>`, `_TEST_LIGHTS=ramp\|bars\|low\|lamp\|noshadow`, `_LAMPS=<0-2>[,noramp,brake,...]` | sky look; test lamps; lamp switches forced |
| `PHYS_DRIVE_PARTS`, `_MATERIALS` | print the car's part / material assignment |
| `PHYS_DRIVE_DEBUG_LAYER=1` | = `--debug-layer` (D3D12 debug layer; needs Graphics Tools) |
| `WRALLY_SHOWROOM_TIMES=1` | showroom: log slow frames |

### Renderer (`NGG_*`, the ones A/B sessions use; full table `ngg-design.md §10.8`)
| switch | effect |
|---|---|
| `NGG_DEBUG_VIEW=<n>` | start in a debug view (`V` cycles). 0 normal, 1 tex0, 2 tex1, 3 blend weight, 4 baked vertex colour, 5 uv0, 6 flat grey, 7 sun visibility, 8 shadow acne, 9 ambient specular x8, 10 roughness/metallic, 11 RT sun visibility, 12 RT vs shadow map, 13 RT AO, 14 plants, 15 RT local lights, 16 motion vectors. Names: `kDebugModeNames`, `core/scene.cpp` (the doc lists up to 15) |
| `NGG_RT_NO_HISTORY=1` | sun/AO denoise without history: grainy but unbiased reference for ghosting |
| `NGG_DENOISER=nrd\|ours` | RT denoiser |
| `NGG_AO_RAYS=<n>` | RT AO rays/pixel (32 = near noise-free reference) |
| `NGG_MODEL_LOD_SCALE=<k>` | instanced models' LOD distances (min 0.05); wins over the vegetation setting |
| `NGG_CLIP_WALLS=0\|back\|front` | clip walls off / mesh culled one side. Any other value = on (older `=2`,`=3` no longer mean anything) |
| `NGG_CLIP_CULL=0` | clip walls hide pixels only, no object culling |
| `NGG_WIND=<speed>[,<gust>[,<deg>]]` | fixed wind (ctest uses `1,0.2,90`) |
| `NGG_LIGHT_MODE=0\|1`, `NGG_LEGACY_PROFILE=0..2` | baked vs dynamic lighting; legacy card lighting |
| `NGG_FOLIAGE_*`, `NGG_GTAO_*`, `NGG_NRD_*` | per-feature A/B knobs, see §10.8 |
| `NGG_CPU_TIMES=1` | renderer CPU phase times in its log |
| `NGG_TEST_DEVICE_REMOVED=<frame>[,...]` | removes the device in that renderFrame (recovery test; log must say `device 1 ready`) |
| `NGG_PIX=<frame>` | PIX capturer loaded, frame (>=2) to `ngg-frame-<frame>.wpix` in the cwd |
| `NGG_DUMP_PS=<path>` | scene PS/VS blobs, root sig, flattened HLSL: `<path>.ps.obj .vs.obj .rs.bin .hlsl` for RGA |
| `NGG_DEBUG_LAYER=1` | D3D12 debug layer (as `--debug-layer`) |
| `NGG_SHADER_DIR=<dir>` | compile shaders from files at start (disables pipeline library) |

## 3. Comparing images
- Python with Pillow + numpy. Typical: crop the same box from `runA/shot-NNN.png` and `runB/shot-NNN.png`,
  mean / signed mean / max of the difference, side-by-side sheets with `Image.paste`. Write scripts to your scratch folder.
- `tools/car-ghost/ghost.py <run dir> <ref dir> [--max b] [--box x0,y0,x1,y1] [--blur px --max-abs g] [--lit pct]`:
  signed bias behind the car vs the no-history reference (boxes at 1920x1080, scaled).
  It says "the reference IS the run" when the build ignores `NGG_RT_NO_HISTORY`.
- `tools/car-ghost/check-car-ghost.sh [W-Rally.exe] [out dir]`: needs `RBR=<RBR>`, optional `GOLDEN_DIR=<phys-data>`.
  Shoots three track-42 stretches (views 13/11), the golden run (view 11) and the forest stretch (ours and NRD),
  each against `NGG_RT_NO_HISTORY=1 NGG_DENOISER=ours`. About 7 min on the iGPU. Non-zero exit = ghost over limit.
- RT noise: judge with **TAA off** (`PHYS_DRIVE_OFF=taa`) and on **moving** frames (driving, or `--showroom --spin`).
  A settled still frame hides the grain the user sees. Untextured views (11, 13) show it best.

## 4. Timing a change
- Judge by the **whole frame's GPU time** (`gpu frame` from `PHYS_DRIVE_FRAMEMS`, `RenderStats::frameMs`).
  Per-pass stamps (`passMs`, `rtMs`, NGG `*_TIMES` logs) only say *where*, once the total moved. On the iGPU they absorb other passes' work.
- Always `PHYS_DRIVE_OFF=vsync` (with vsync the iGPU clocks down into the slack: every pass reads slower).
- Run-to-run drift on the iGPU is ~1 ms. Do back-to-back rounds: off/A/B interleaved, 2-3 rounds, compare best or median of each.
  Check the machine is quiet (no ctest, no second W-Rally, no build).
- Compare two renderers by swapping `modules/renderer/WRenderer.dll` into ONE `bin` copy (same exe, same W-Rally.ini).
  Compare renders only within one compiler/build config.
- Keep runs short: ~200-300 ticks for images, a few thousand for timing. Stop on the expected shot count, not only a timeout.
- NVIDIA-only (DLSS, RR) or real GPU cost: run the same commands on an NVIDIA machine.

## 5. Standard stretches
| name | stage file | replay | ticks / notes |
|---|---|---|---|
| golden run-41 | `<phys-data>/m5/track-41.ngg` (or the shipped stage) | `<phys-data>/m2/run-41-20261011-150633.phr` | `--stage track-41_M --step 0.1 --overrides none`; ctest `golden-replay-41`; ghost: 4400-4830, view 11 |
| track-42 Impreza | `<RBR>/Ngg/fr_joux_verte.ngg` (old `track-42.ngg`) | `tools/car-ghost/track42-impreza.phr` | needs the Impreza args from `check-car-ghost.sh`; 7000-7210, 9400-9606 (view 13), 1100-1290 (view 11) |
| forest (Cote d'Arbroz) | `<RBR>/Ngg/fr_cote_darbroz.ngg` | `tools/car-ghost/track41-i20.phr` | `--stage track-41_M --camera 0`, 11050-11250, low sun behind the car |
| Joux hairpins (clip walls) | `<RBR>/Maps/track-42_M.lbs` | a user Quick-rally drive (`joux.phr`, in `<handoff>/clip-walls/in/`) | 24150-24300 hairpin; whole stage `PHYS_DRIVE_SHOTS=800` |

## Pitfalls
| symptom | cause | fix |
|---|---|---|
| Two builds differ in fps or look; worktree build 28 ms vs 20 ms | `W-Rally.ini` sits in the user folder = `build/bin` of each build. A worktree has none: defaults are `fullscreen=1`, `quality=1`, `vegetation_detail=1`, RT/TAA/vsync on, `upscaling=0` | Read `build/bin/W-Rally.ini` (`ray_tracing`, `taa`, `upscaling`, `denoiser`, `vegetation_detail`, `quality`, `vsync`) before trusting a run. Copy one ini into both, or force with `PHYS_DRIVE_*` |
| User says "it's still there" but your shots are clean | user's ini has e.g. `upscaling` (FSR) or `denoiser=1` (NRD), your run didn't | Match their ini or set `PHYS_DRIVE_UPSCALE` / `_DENOISER` explicitly |
| Frames differ at every edge between identical builds | TAA history | `PHYS_DRIVE_OFF=taa` plus `--step` |
| No output in the terminal | windowed app: with no redirect and no console to attach to, stdout goes to `W-Rally.log` in the user folder | Always `> log.txt 2>&1` |
| No renderer lines in the log | renderer log is only printed with `PHYS_DRIVE_NGGLOG=1` (otherwise only `OutputDebugString`) | set it |
| `renderer: no renderer module "wrenderer" ... InitWInterface: ErrVersion` | `WRenderer.dll` older than the exe (built one target only) | build `W-Rally` and `WRenderer` together, or the whole solution |
| `<x>.ngg: --stage needed (no track-<n> in its name)` | renamed stage files | pass `--stage track-NN_M` |
| Menu video, track names or catalog missing in a worktree build | `content/` submodule empty in a new worktree (the exe falls back to the source tree's `content/`) | `git submodule update --init` in the worktree |
| Timing noisy, golden test slow or flaky | `ctest` runs `golden-replay-41` (opens the game window) in parallel with other tests or your run | run ctest alone, `-j 1` when measuring; never measure while ctest runs |
| `ModuleNotFoundError: numpy` | `python -I` ignores the user site-packages where numpy is installed | use plain `python` (or `-P`) for your own scratch scripts |
| `FileNotFoundError` for `shot-000.bmp` | shots were BMP before; now `.png` (and showroom `.png`) | use `.png` |
| Shots missing or in the wrong folder | stage shots go beside `--out`; showroom, lab and PIX files go to the cwd | `cd` into the run dir and pass `--out $d/drive.phr` |
| Old run still holding the GPU / files | a W-Rally left running after a killed shell | check `tasklist | grep -i W-Rally`; close it before the next run |
| PIX replay hangs the GPU (driver reset) | AMD counter plugin | `pixtool ... --disable-gpu-plugins` |
| `NGG_DUMP_PS` files not `.dxbc` | they are `.ps.obj/.vs.obj/.rs.bin/.hlsl` now | feed those to `rga -s dx12` |
