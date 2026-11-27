---
name: wrally-renderer-dev
description: Load before changing code in W-Rally's NGG renderer (modules/renderer, WRenderer.dll) - a render feature, a shader, a renderer setting, a .ngg chunk, an NGG_* switch, the RT denoiser, TAA or motion vectors. Gives the code map, the frame order, the invariants that broke earlier sessions, and step-by-step recipes with every file to touch.
---
# W-Rally renderer (NGG): map, invariants, recipes

Depth lives in `docs/ngg-design.md` (rewritten 2026-11-11; describes the code **as it is**). Read §1 first, then the
section for your topic. Building and ctest: skill `wrally-build-test`. Judging a change (A/B, frameMs, shots):
skill `wrally-render-measure`. NVIDIA-only checks (DLSS, RR) need an NVIDIA GPU.

## Where things are

| path | owns |
|---|---|
| `include/wrally/iface/renderer.h` | the game ABI: `RenderSettings`, `FrameDesc`, `RenderStats`, `RendererApi`; `kRendererInterfaceVer` + the version list comment above it |
| `include/wrally/iface/renderer_edit.h` | the editor table (Wallaby), versioned on its own: `kRendererEditVer` |
| `modules/renderer/src/module/module.cpp` | `WRenderer.dll`: `Apply` (RenderSettings -> core), `ApplyEnvironment` (module-level `NGG_*`), `renderFrame`, `Gpu` (device-bound part), `Journal` (replay after device loss), `MakeGpu`/`Recover` |
| `src/core/renderer.*` | `core::Renderer::RecordFrame` (pass order), tiers, upscaler choice, `KeepRtCurrent`, `ResetHistory` |
| `src/core/scene.*` (~7k lines) | `.ngg` load (`Scene::Load`/`LoadFromMemory`, chunk switch ~l.2870), materials, scene pipelines (`EnsurePipelines`), prepass + main pass (`RecordPass`), `CameraCB`, debug views (`kDebugModeNames`), clip walls |
| `src/core/visibility.*`, `nrd_visibility.*`, `rr_visibility.*` | `VisibilityProvider`: `RtVisibility`, `CascadeVisibility`, `NrdVisibility`, `RrVisibility` |
| `src/core/rt.*` | BLAS/TLAS, `Trace`, our denoiser (`Rt::Denoise`, `Rt::ResetHistory`) |
| `src/core/models.cpp`, `grass.*` | instanced models (NGGD/NGGE), LOD, foliage map; grass (NGGF) |
| `src/core/post.*`, `fsr.cpp`, `dlss.cpp`, `upscaler.h` | TAA, exposure, tonemap; upscalers |
| `src/core/shaders.*`, `shader_compiler.*`, `pipeline_cache.*` | `LoadShader`, FXC/DXC wrappers, `CreatePipeline`/`CreateRootSignature`/`PipelineBatch` |
| `src/format/ngg_format.h` | the `.ngg` truth: chunks, records, `Check*Chunk` readers shared by both loaders |
| `src/format/ngg_scene.*` | strict reader + writer (`SceneData`, `Write`), used by the legacy importer and `tools/ngg-check` |
| `src/shaderc/main.cpp` | `ngg-shaderc`: compiles `shaders/shaders.txt` at build into `generated/ngg_shaders.cpp` |
| `shaders/*.hlsl`, `*.hlsli` | `scene.hlsl` (all raster passes), `rt_*.hlsl` (`cs_6_5`), `post`, `sky`, `gtao`, `grass`... |
| game side | `game/src/app/settings.h/.cpp`, `game/src/app/game.cpp` (`ToRender`, `PHYS_DRIVE_*`, keys), `game/src/ui/menus.cpp` (`SettingsUi` rows), `game/src/render/renderer.*` (the game's wrapper), `game/src/assets/legacy_stage.cpp` (`.lbs` -> `.ngg`) |

Rule (§1.3): `src/core` and `src/format` include nothing of the game, of `wrally/iface` or of RBR. Only `src/module`
includes the iface. RBR facts belong to the importers (Wallaby, `legacy_stage.cpp`), not renderer heuristics.

## The frame (`RecordFrame`, §1.4)

`renderFrame` applies the `FrameDesc` (moves, instances, weather, lights, `camera.cut` -> `ResetHistory`), then:
PIX/resize/GPU timer -> tonemap curve -> atmosphere -> **jitter** -> `Scene::BeginFrame` -> `KeepRtCurrent` (BLAS
land, TLAS build into the frame's list) -> `PrepareLights` -> **prepass** (depth, normal, motion, plant) -> RT or
cascades chosen -> `VisibilityProvider::Record` (sun vis + AO + local-light shadows) -> HDR + sky -> main pass (env,
probe, aerial, opaque, models, deferred lights, glass) -> resolve (upscaler or TAA, meter, tonemap) -> end timer.
Then `Rt::StartBatches`, editor overlay, UI pass, capture, `EndFrame`, `CheckRemoved`, readbacks.
**Two frames in flight** (default; `NGG_FRAMES=1` = synchronous): anything a frame hands the GPU lives per slot -
constants/CPU buffers via `FrameUpload`/`FrameAlloc`, freed or replaced resources via `DeferRelease`, timers as
`SlotTimer`, readbacks per slot range (read two frames late), rewritten descriptors per slot. Table calls between
frames that replace GPU state call `Settle` (module.cpp). Check a change with `NGG_FRAMES=1` vs default and
`NGG_DEBUG_LAYER=1 NGG_GPU_VALIDATION=1`.

Visibility provider (§4.1): RT when `m_preferRt && prepass && m_rt.HasScene() && m_rtValid && ShadowsOn()`; else
cascades. `RenderSettings::denoiser` picks NRD over ours; `UpscalerKind::DlssRr` forces `RrVisibility` (raw traces).
A switch of source starts without history. `Renderer::RtFallbackReason` / `RenderStats::rtFallback` say why not RT.

## Invariants that bit sessions

- **Jitter is set before the prepass**, the same for every rasterising pass. Main pass tests `GREATER_EQUAL`
  against prepass depth; jitter set later = sky flickers through.
- **TAA and denoiser reprojection use the unjittered `Scene::LastViewProj`; anything reconstructing a surface from
  depth (ray origins, AO, deferred lights) uses `LastViewProjJittered`.** Wrong one: shadow fraction cycled 33-69 %
  on a still frame. Reprojection stays homogeneous (sky = depth 0, no divide first).
- **Camera contract:** row-major, row-vector (`v * M`); client gives a conventional projection, `Scene` applies
  reverse-Z. Depth cleared to 0. Never pre-flip.
- **Moving objects need motion vectors:** an instance that moved since placed writes prepass motion
  (`MotionBuffer`, RGBA16F); its glass writes `GlassMotionBuffer`. Anything new that moves (wind sway, grass bend)
  must write its own motion, or TAA and the denoisers smear it (§3.7, §2.6).
- **History distrust for moved objects** (§4.5): a ray hitting a moved instance limits history (`Allow`); identity
  separates moved and static pixels in spatial/temporal filters; uncovered ground starts over. A new BLAS's hits
  allow nothing for a frame. Don't loosen the clamp where history is distrusted (the pale car copy, 2026-11-19).
- **Camera cut resets everything:** `FrameDesc::camera.cut` -> `Renderer::ResetHistory` -> RT histories, XeGTAO
  sequence, TAA, exposure. A new temporal thing must hook into `ResetHistory` and device-loss recovery.
- **Glass (`kMaterialBlend`) is not in the RT scene**; far scenery is neither traced nor shadowed.
- **Never key a descriptor on a resource pointer** (a recreated texture lands at the freed address; the DRED
  use-after-free). The denoiser ping-pongs: `Rt::Filtered()` changes pointer every frame.
- **GPU work the frame depends on goes in the frame's list**, ahead of its readers. No mid-frame submit+wait.
- **Device loss:** everything device-bound lives in `Gpu`; client state that lives on the GPU must be recorded in
  `Journal` (module.cpp) or set again by `Apply`/`ApplyEnvironment`, else it is gone after a TDR.
  Test: CTest `device-lost-41`, or `NGG_TEST_DEVICE_REMOVED=<frame>` (§10.6).
- **One lighting path scaled by tier**, never a second renderer. New techniques add a field to `QualityTier`.
- **The scene root signature is at its 64-DWORD limit** (scene.cpp ~l.3484): add SRVs into an existing table, not
  as root SRVs. Grass blades cannot read more textures for the same reason.

## Recipe A: a renderer setting, end to end

Model commits: `2507637` (vegetationDetail, v23), `7b82db1` (ambientOcclusionOff, v26), `0983b69` (denoiser, v21).
1. `include/wrally/iface/renderer.h`: field at the END of `RenderSettings` (+ `k*` constants with a comment);
   `kRendererInterfaceVer` +1; add the `// N: ...` line to the version list above it.
2. `modules/renderer/src/module/module.cpp` `Apply`: map it onto the core (`r.g->core` / `r.g->scene` / `r.g->post`;
   order matters: e.g. AO off goes after `SetQuality`). An `NGG_*` override that wins for A/B goes here too.
3. Core: the setter in `renderer.h/.cpp` or `scene.h/.cpp` (+ `models.cpp` etc.).
4. Game: `game/src/app/settings.h` field + default + comment; `settings.cpp` `Fields()` line (ini key, snake_case);
   `game/src/app/game.cpp` `ToRender` (+ the `PHYS_DRIVE_<NAME>` env override, names or index); if it has a key or
   debug toggle, the `RunStage` key handler and `PHYS_DRIVE_OFF` parsing (also in the lab/showroom copies, ~l.1685
   and ~l.1762); `game/src/app/options.h` comment lists env switches.
5. `game/src/ui/menus.cpp` `SettingsUi`: an `add(SetRow::Choice|Toggle, "NAME", "help. Applies at once.")` row in the
   graphics tab, and the copy-back that sets `graphics = true`.
6. Docs: `docs/design.md` §3.1a bullet "X (interface N): ..."; `docs/ngg-design.md` §1.5 RenderSettings line and the
   topic section; §10.8 if an `NGG_*` was added; §11 item state.
7. Wallaby: `python tools/gen_wrenderer_interop.py` regenerates `Wallaby.Render/WRendererInterop.g.cs` (the editor
   refuses any other version). Wallaby's `RenderSettingsForm.cs` if the editor exposes it.
8. Build `WRenderer` AND `W-Rally` (both carry the version), ctest.

## Recipe B: a shader

1. `.hlsl` in `modules/renderer/shaders/` (picked up by the CMake glob; reconfigure-free with `CONFIGURE_DEPENDS`).
2. One line per variant in `shaders/shaders.txt`: `<file> <entry> <profile> [args]`, args in the EXACT order the C++
   passes them - the line is the lookup key. `*_5_x` = FXC/DXBC, `*_6_x` = DXC/DXIL (RayQuery needs `*_6_5`);
   scene shaders use `-HV 2018`.
3. C++: `LoadShader("x.hlsl", "CSMain", "cs_6_0", { "-DFOO=1" })`; root signature via `CreateRootSignature`, PSO via
   `CreatePipeline` (never the raw device calls - they are counted, named, cached). Scene pipelines go into
   `Scene::EnsurePipelines`' `PipelineBatch`.
4. Constant buffers: C++ struct in whole float4 groups + `static_assert(sizeof(X) % 16 == 0)`; root constants:
   `static_assert(sizeof(X) == n * 4)`. Flags shared with HLSL: a `#define` copy in the shader + a `static_assert`
   in scene.cpp (see `kMaterialVertexTint`).
5. Iterate without rebuild: `NGG_SHADER_DIR=<repo>/modules/renderer/shaders` (restart; logs unlisted shaders).
   Build fails on unsigned DXIL; `ngg-shaderc` needs `third_party/dxc`.

## Recipe C: a .ngg chunk

Model commit: `fd7a231` (NGGC clip walls) - 13 files.
1. `src/format/ngg_format.h`: header struct with `version` + `recordSize`/`triangleSize`, record structs,
   `static_assert(sizeof...)` ("keep Wallaby's NggWriter in step"), `kXVersion`, `inline uint32_t ChunkX()`
   (`FourCC('N','G','G','?')`), and `inline const char* CheckXChunk(p, n, ...)` returning why it is refused.
2. Version rules (§2.2): records grow by appending (reader takes min(recordSize, own)); bump the chunk `version`
   only when a field changes meaning; unknown version = refused, never guessed. File version (`kFormatCurrent` 3)
   only for core layout changes - an old file is refused ("reconvert"), not upgraded.
3. Loader: `core/scene.cpp` chunk switch (`else if (ch->fourcc == ChunkX())`), call `CheckXChunk` first.
4. `src/format/ngg_scene.h/.cpp`: `SceneData` field, `Write`, strict `Read` (same `CheckXChunk`).
5. Producers: `game/src/assets/legacy_stage.cpp` (+ `game/src/formats/lbs_*`); Wallaby `Wallaby.Formats/NggWriter.cs`
   (+ its own file like `NggClipWalls.cs`), separate repo, branch `ng`.
6. `tools/ngg-check/main.cpp`: round-trip + each refusal rule broken and refused by both readers; a CTest in
   `tests/CMakeLists.txt` (`ngg-roundtrip`, `legacy-import-*`) with a `PASS_REGULAR_EXPRESSION`.
7. Docs: ngg-design §2.2 chunk table row, the topic section, §11.

## Recipe D: an env / debug switch

- Read once: `static const char* const e = std::getenv("NGG_X");` in the file that uses it, or in module.cpp
  `ApplyEnvironment` (re-applied after device loss). Log what it changed.
- Add a row to ngg-design §10.8: `| NGG_X=<values> | effect | file.cpp |`. Check none is missing:
  `grep -rhoE '"NGG_[A-Z0-9_]+"' modules/renderer/src game/src | sort -u` vs §10.8.
- A game switch is `PHYS_DRIVE_*` in `game.cpp` + `options.h` comment + ngg-design §10.1 table.
- A debug view: `kDebugModeNames` (scene.cpp, count static_asserted) + the shader branch + §10.3 list.

## Keeping docs/ngg-design.md current

Same commit as the code: §1.2 (new file), §1.4 (pass order), §1.5 (settings), §1.6 (shader/pipeline counts), §2.2
(chunks), the topic section with the measurement (GPU frame, machine, resolution), §10.8 (switches), §11 (open work:
mark done / left). Write the present state, not a log. Old numbers in commits ("5.0e", "9b item 30") map via
Appendix A. Interface notes go to `docs/design.md` §3.1a. Commit text: what changed + the measured number.

## Pitfalls

| symptom | cause | fix |
|---|---|---|
| `WRenderer.dll: InitWInterface: ErrVersion`, "no renderer module" | game and DLL built from different `kRendererInterfaceVer` | build both `W-Rally` and `WRenderer`; Wallaby: regenerate the interop |
| two branches both claim interface N on merge | parallel worktrees bumped together | renumber the later one N+1, keep both list lines in order |
| fields read their neighbour's value, no error | C++ CB not in float4 groups vs HLSL packing | whole float4 groups + `static_assert(% 16)` |
| `C2338 static assertion failed: 'WeatherDesc layout'` etc. | iface struct layout changed | update the struct + assert deliberately; bump version |
| GPU hang / `0x887A0005` | TDR; out-of-bounds or use-after-free | read DRED in the log (§10.5); `--debug-layer` / `NGG_DEBUG_LAYER=1`; PIX replay with `--disable-gpu-plugins` (AMD's plugin hangs the GPU) |
| `GetDimensions` on a root SRV gives 0 on NVIDIA | undefined for root descriptors | pass the count as a root constant |
| sky flickers through terrain | jitter set after the prepass | set it before (`RecordFrame` step 3) |
| trail / ghost of the car | missing motion or history not distrusted | motion in the prepass; `Allow`/identity rules; check with `NGG_RT_NO_HISTORY=1`, `tools/car-ghost` |
| RT shadows silently off | a "ready" flag reset on a new (ping-ponged) resource | gate only the descriptor write |
| shader "not listed" / fails in shipped build | missing or misordered `shaders.txt` line | line args in the C++ order |
| ctest regex test fails after a log-line change | `PASS_REGULAR_EXPRESSION` matches log text (instancing-41, ngg-roundtrip) | update the regex; in a quoted CMake string a literal `(` is `\\(` |
| `--target WRally` fails / stale | the game target is `W-Rally` (renamed 2026-10-31; a stale `WRally.vcxproj` may linger in old build dirs) | `--target W-Rally` |
| `build is not a directory` in a worktree | each worktree needs its own configure | see `wrally-build-test` |
| `C2001 newline in string literal`, CRLF warnings | a Python/heredoc edit turned `\n` into a newline or wrote CRLF | use the Edit tool; Python `open(..., newline='\n')` |
| crash/hang at pipeline creation on AMD | parallel pipeline creation | keep sequential; `NGG_PIPELINE_THREADS` only to test |
| doc says 58 shaders / 41 pipelines | counts drift (83 lines in 2026-11) | check the file, update §1.6 |
| A/B frames differ everywhere | TAA history, or different W-Rally.ini in a worktree bin | `PHYS_DRIVE_OFF=taa`; swap `WRenderer.dll` into one bin |
| `LNK1168` cannot open exe/dll | the game is running | close it first |
