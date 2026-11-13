# NGG renderer — design

NGG (New Generation Graphics) is W-Rally's renderer: a D3D12 renderer with physical sky and lighting, ray-traced
shadows, AO and local lights, a raster fallback for GPUs without inline ray tracing, and the `.ngg` scene format.
It lives in `modules/renderer/` and ships as `WRenderer.dll`. The game reaches it only through
[`include/wrally/iface/renderer.h`](../include/wrally/iface/renderer.h), and the editor through
[`renderer_edit.h`](../include/wrally/iface/renderer_edit.h). The module ABI, the game's side and the phasing are
in [design.md](design.md). **This file is how the renderer works inside, and why.**

This document describes the renderer **as it is now**. Until 2026-11 it was a running log written while NGG was an
RBR plugin: a d3d9 proxy, a 64-bit host process, IPC, resource tagging. That history was dropped in the
2026-11-11 rewrite and stays in git (`git log -- docs/ngg-design.md`). Older commit messages and plugin-rbr still cite the old section numbers
("design 5.0e", "9b item 30"); [Appendix A](#appendix-a-old-section-numbers) maps them to the new ones.

**The decisions everything else rests on:**

| decision | consequence |
|---|---|
| **The renderer knows nothing about RBR** | RBR content reaches it as `.ngg` bytes or as models through the interface. The importers (Wallaby, the game's `.lbs` import) own every RBR fact (§2, §1.3) |
| **No baked lighting** | RBR's vertex colours and shadow textures are discarded. All light is computed: physical sky, sun and local lights, ray-traced or raster visibility (§3, §4, §5) |
| **Ray tracing first, raster as the fallback** | inline RayQuery (DXR 1.1, SM 6.5) gives sun shadows, AO and local-light shadows. Without it, cascades + XeGTAO + spot shadow maps fill the same slots (§4.1) |
| **Physical units** | the sky, sun and lamps are in physical radiometric units, and auto exposure meters the result. There are no per-scene fudge factors (§3.5) |
| **One frame for every client** | `core::Renderer::RecordFrame` owns pass order and correctness for the game, the Garage and the editor (§1.4) |
| **Shaders in files, compiled at build** | `modules/renderer/shaders/*.hlsl` are compiled into `WRenderer.dll`, and the shipped game runs no shader compiler (§1.6) |

Sections:

1. Baseline, structure and the frame
2. Assets: the `.ngg` format and where scenes come from
3. Sky, atmosphere, exposure and post
4. Shadows, ambient occlusion and ray tracing
5. Weather, surfaces and night
6. The car
7. Upscaling
8. Editor integration (Wallaby)
9. Native and generated tracks
10. Measuring and debugging
11. Open work
- Appendix A: old section numbers

---

## 1. Baseline, structure and the frame

### 1.1 Feature floor: what the code actually requires

The design target is DX12 Ultimate hardware (RTX 20 / RDNA2 and newer). **What the code requires is much less, and
everything above it is probed and degrades:** `Device::Create` makes the device at `D3D_FEATURE_LEVEL_11_0` and logs
the ray-tracing tier and highest shader model at start-up, rather than letting a feature discover them by failing.

| capability | used now | where / fallback |
|---|---|---|
| inline ray tracing (RayQuery, DXR tier 1.1 + SM 6.5) | **yes** - sun shadows, AO, local lights' shadows (`rt_*.hlsl`, `cs_6_5`), the car's reflection rays (`scene.hlsl` `ps_6_5`) | `Rt::Create` declines on an adapter without it; the frame takes the cascades + XeGTAO. `Device::SupportsInlineRaytracing` |
| full DXR pipelines (ray-gen/hit/miss, shader tables) | no | inline queries only: one PSO per pass |
| SM 6.0 DXIL | yes - scene pass, XeGTAO | precompiled; FXC 5.1 fallback for the scene in the run-time compile only |
| mesh / amplification shaders | **no** | vertex + index draws, `DrawIndexedInstanced` with an instance stream (`models.cpp`) |
| sampler feedback, tiled / reserved resources | **no** | every texture is uploaded whole at load (`textures.inl`, `mipgen.cpp`) |
| temporal upscalers | optional | FSR 3.1 (FSR 4 on RDNA 4) via ffx-api DLLs, DLSS SR via NGX on RTX; absent DLLs = our TAA |
| VR / stereo | not built | the quality tiers (1.5) are the hook |

**The old baseline's promises that are not in the code:** meshlet clusters as the primary geometry path with GPU
cluster culling, sampler-feedback texture streaming, RT global illumination. None is started; they remain the
direction, not the design.

**RT cost was measured before anything was built on it** (dev iGPU, Radeon, tier 1.1, SM 6.7): one shadow ray per
pixel is **~2 ns per ray**, linear from 194k to 967k rays (0.47-2.0 ms); the BLAS for a 594k-triangle stage is
92 MB. That is why RT shadows are on in every tier and the cascades are the fallback, not the other way round.
Alpha-tested geometry is cut out in the ray by the rasteriser's own test (texture alpha x vertex alpha against the
material cutoff, alpha sampled at fixed LOD 2) at no measurable cost.

### 1.2 Code map: `modules/renderer`

| file | holds |
|---|---|
| `src/core/device.*` | `Device`: D3D12 device, queue, ONE allocator + list + fence, synchronous `Begin`/`EndAndWait`; `CreateSecondary` (a second context on the same queue, for BVH builds mid-frame); DRED, `CheckRemoved`, debug-layer message drain, `NGG_PIX` |
| `src/core/renderer.*` | `core::Renderer`: the frame (`RecordFrame`), quality tiers, upscaler choice and render size, `KeepRtCurrent` (BVH follows the scene), whole-frame GPU timer |
| `src/core/scene.*` | `Scene`: `.ngg` load (`Load`, `LoadFromMemory`, variants), materials, the 41 scene pipelines, prepass + main pass (`RecordPass`), cascades and local-light shadow maps' draws, deferred local lights, glass, cube-probe faces, editor overlays / labels / selection, pixel probe, debug and light modes, RT input description |
| `src/core/textures.inl` | DDS loading for the NGGT chunk (BC1/BC2, BC4/5/7 behind a DX10 header) - included by `scene.cpp` |
| `src/core/models.cpp` | instanced models (NGGD/NGGE): model library, placements, culling, LOD, per-frame instance stream |
| `src/core/visibility.*` | `VisibilityProvider`: `RtVisibility` (ray tracer) or `CascadeVisibility` (cascades + XeGTAO) - the sun's visibility and AO for the main pass |
| `src/core/rt.*` | `Rt`: BLAS per mesh key + TLAS, `Trace` (sun, AO, local lights), denoiser, reflection TLAS |
| `src/core/shadow.*` | `ShadowMaps`: cascaded sun maps (2048) and the local lights' maps (1024) |
| `src/core/gtao.*` | `Gtao`: Intel XeGTAO (`third_party/xegtao`, unchanged) on the prepass's depth and normals |
| `src/core/lights.*` | local lights: records, clusters, shadow-slot choice, analytic shading (ratio estimator) |
| `src/core/sky.*`, `atmosphere_ms.*`, `aerial.*`, `env.*`, `astronomy.*` | physical sky; multiple-scattering LUT; aerial-perspective froxel table; sky prefiltered by roughness; sun / moon from date, time, place |
| `src/core/probe.*` | `ReflectionProbe`: dynamic cube probe around the car (reflection focus) |
| `src/core/weather.*` | weather presets (our format) |
| `src/core/post.*`, `upscaler.h`, `fsr.cpp`, `dlss.cpp` | `PostFx`: HDR target, TAA, exposure meter, tonemap + sharpen; the `Upscaler` interface and its FSR / DLSS backends |
| `src/core/mipgen.*` | mip chains for BC textures shipped without them |
| `src/core/shaders.*`, `shaders_embedded.h`, `shader_compiler.*` | `LoadShader` (precompiled table, or run-time compile under `NGG_SHADER_DIR`); FXC + DXC wrappers shared with the build tool |
| `src/core/pipeline_cache.*` | `CreatePipeline`, `CreateRootSignature`, `PipelineBatch`, the opt-in pipeline library |
| `src/core/log.*` | the one log sink; the host points it somewhere |
| `src/format/ngg_format.h` | the `.ngg` container: chunks, records, vertex layout |
| `src/format/ngg_scene.*` | `.ngg` writer + strict reader (the legacy importer writes through it; `tools/ngg-check` round-trips it against the renderer's own loader) |
| `src/module/module.cpp` | `WRenderer.dll`: `RendererApi` / `RendererEditApi` over the core - swap chain on the game's window, settings, models / instances, the frame, capture, stats |
| `src/module/ui.*` | the game's UI pass: alpha-blended, scissored draw lists onto the back buffer after the frame |
| `src/shaderc/main.cpp` | `ngg-shaderc`: compiles `shaders.txt` at build time into `generated/ngg_shaders.cpp` |
| `shaders/scene.hlsl` | the scene pass: prepass, main (opaque / alpha-tested / blended / G-buffer / reflection-ray variants), shadow and lamp maps, deferred local lights |
| `shaders/rt_trace, rt_ao, rt_ao_wide, rt_lights, rt_denoise.hlsl` | ray-traced visibility and its denoiser (`cs_6_5`) |
| `shaders/gtao.hlsl` | XeGTAO entry points with our registers (`cs_6_0`) |
| `shaders/sky, env, aerial, atmosphere_ms, probe.hlsl` | sky, environment, aerial table, MS LUT, reflection probe (FXC 5.1) |
| `shaders/post.hlsl`, `meter.hlsl` | TAA, tonemap, upscaler inputs; exposure (FXC 5.1) |
| `shaders/select.hlsl`, `ui.hlsl` | editor selection / labels / lines (FXC 5.1); the UI (FXC 5.0) |
| `shaders/atmosphere, lights, rt_common, brdf.hlsli` | shared: the one atmosphere model, light records + clusters, RT common part (alpha test), GGX / specular AA / roughness packing |
| `shaders/shaders.txt` | every shader the renderer loads, one line each (1.6) |

The ABI the game sees (`include/wrally/iface/renderer.h`, `renderer_edit.h`) is in [design.md §3](design.md).

### 1.3 Rules

- **`src/core` and `src/format` include nothing of the game, of the module ABI or of RBR.** Only `src/module`
  includes `wrally/iface`. The core is what `ngg-check`, the game and the editor share; the moment it knows the game,
  a second renderer appears.
- **The camera contract** (`scene.h`): view and proj are row-major, row-vector (`v * M`); the client hands a
  conventional near->0 / far->1 projection and **`Scene` applies reverse-Z itself** - clients must not pre-flip.
  Depth is `R32_TYPELESS` (D32 view for the DSV, R32 for SRVs), cleared to 0.
- **Jitter:** `Scene::LastViewProj` is reverse-Z included, jitter EXCLUDED; `LastViewProjJittered` includes it.
  **TAA reprojects into a stable grid and must not see the jitter; anything reconstructing a surface from depth
  (ray origins, AO, deferred lights) must** - a half-pixel of world space at a grazing road is the size of the
  shadow-ray bias.
- **The client owns** its window, input, overlays and presentation; **the renderer owns** order and correctness from
  "here is a camera" to "the tonemapped image is in your target". Settings are the client's; the pass order is not.
- **The frame is synchronous:** one list, `EndAndWait` at the end of every frame. Readbacks (timers, probe, overlay
  stats, exposure) are therefore last frame's and are read after the wait. Frame overlap needs per-frame allocators
  and a constant ring; it comes with the change that needs it.
- **Never key a descriptor on a resource pointer.** A recreated texture can land at the freed one's address; the
  RT visibility / AO descriptors are rewritten on every set (`Scene::SetRtVisibility`, `SetRtAo`).
- **Work that must happen while the client's list is open** (BVH builds) goes on `Device::CreateSecondary`'s context:
  `Rt::Rebuild` is blocking on the context it gets and completes on the queue before the frame's list is submitted.

### 1.4 One frame: `core::Renderer::RecordFrame`

`WRenderer`'s `renderFrame` (`module.cpp`) first applies the frame's description - stage-object moves, instance
placements, reflection focus, weather, wetness, local lights, overlay lines, `camera.cut` -> `ResetHistory` - then
opens the list, sets sky time, and records. The order is the code's:

| # | step | code |
|---|---|---|
| 0 | PIX capture trigger; follow the output size; camera planes for the upscaler; read last frame's GPU time, start this one's | `PixFrame`, `OnResize`, `PostFx::SetCamera` |
| 1 | tonemap rule: debug view -> raw, baked light mode -> sRGB, else the client's curve (the module: ACES); exposure; TAA on unless a debug view | `PostFx::SetCurve` |
| 2 | one atmosphere for every consumer; the MS switch mirrored to the sky | `Scene::SetAtmosphere`, `Sky::SetMultipleScattering` |
| 3 | **jitter set before the prepass** (every rasterising pass the same sub-pixel offset; the main pass tests `GREATER_EQUAL` against the prepass's depth) | `PostFx::Jitter` -> `Scene`/`Sky::SetJitter` |
| 4 | the instance stream starts over | `Scene::BeginFrame` |
| 5 | BVH follows the scene: finished BLAS batches land; same mesh set -> instances + TLAS only; new meshes -> their BLASes queued for the build thread and queue (never waited for), dropped ones leave; TLAS on the secondary context | `Renderer::KeepRtCurrent` |
| 6 | local lights' clusters for this view | `Scene::PrepareLights` |
| 7 | **prepass: depth + normal + motion + plant** at the render size | `Scene::RecordPrepass` |
| 8 | shadow source: RT if preferred, prepass drawn, BVH valid and shadows on; reflection TLAS set wherever RT exists (cascades too) | `m_rtActive`, `Scene::SetReflectionTlas` |
| 9 | sun visibility + AO (+ local lights' shadows): `RtVisibility` (trace + denoise; AO traced or XeGTAO) or `CascadeVisibility` (XeGTAO, cascades, local-light maps) | `VisibilityProvider::Record` |
| 10 | HDR target; sky, or clear | `PostFx::Begin`, `Sky::Record` |
| 11 | main pass: MS LUT + environment for this sun; last frame's probe faces resolved; aerial table for this camera; dynamic pool flush; this frame's cube-probe faces (2 of 6); opaque world front to back (solid, then alpha-tested); instanced models; deferred local lights; glass far to near | `Scene::Record` -> `RecordPass` |
| 12 | resolve: upscaler or TAA; exposure meter on what is about to be tonemapped; tonemap + sharpen into the output | `PostFx::Resolve` |
| 13 | end GPU timestamp | `LastFrameMs` |

After step 12 the frame starts the threads of the BLAS batches step 5 queued (`Rt::StartBatches`), so their
allocations overlap the frame's wait for the GPU (4.2). Then the module draws the editor overlay (only at native render size: it depth-tests against the render-size depth),
the UI pass, the optional capture copy, `EndAndWait`, the probe / overlay readbacks, `DrainMessages` and
`CheckRemoved`. `present` is a separate call (`vsync` from the settings).

**Upscaling:** everything up to step 12 runs at the render size (`RenderWidth/Height`); the materials' mip bias is
`log2(render/output)`. When the render size has at most half the output's pixels, AO traces twice the tier's rays
(`AoRaysTraced`) so it keeps roughly native rays per output pixel instead of blotches 1.5-2x the size.

### 1.5 Quality tiers and `RenderSettings`

**One lighting path scaled by tier, never a second renderer** - VR at 90 Hz is why. A tier changes only what was
measured to matter: AO is 75-80 % of the ray tracing, the RT sun shadow is cheap (cheaper than the cascades on real
geometry), so every tier keeps RT shadows and the tiers differ in AO (`Renderer::Tier`, `QualityTier`):

| tier | AO | XeGTAO (where AO is screen-space) | sharpen | RT ms, plugin-era lab forest / track-108 start (dev iGPU) |
|---|---|---|---|---|
| **low** | none (the VR baseline) | - | 0.25 | 1.55 / 0.59 |
| **medium** (default) | RT, 1 ray | medium (2 slices x 2 steps) | 0.50 | 3.0 / 1.22 |
| **high** | RT, 2 rays | high (3 x 3) | 0.50 | 4.27 / 1.56 |

New techniques join by adding a field to `QualityTier`. The individual setters (`SetAoRays`, `SetAoSource`,
`ScreenSpaceAo()`...) stay for A/B; changing one takes the client off its tier.

`RenderSettings` (game -> module, `Apply` in `module.cpp`): `quality` -> `SetQuality`; `rayTracing` ->
`SetPreferRayTracedShadows`; `shadows` -> `Scene::SetShadowsOn`; `autoExposure`; `upscaler` + `renderScale` ->
`SetUpscaling` (the game's modes: off/TAA, FSR native/quality 0.67/balanced 0.59/performance 0.5, DLAA, DLSS
quality/balanced 0.58/performance, `game/src/app/settings.h`); `taa` per frame; `vsync` at present; `mode` windowed
only. The module always draws the sky and uses ACES. `UpscalerAvailable` (probed once at `Create`) is what a
settings menu may offer.

### 1.6 Shaders and pipelines

**The HLSL lives in `shaders/*.hlsl` and is compiled at build time into `WRenderer.dll`.** `shaders/shaders.txt`
lists every shader the renderer loads, `<file> <entry> <profile> [args]` (58 entries), and that line is the key
the C++ asks by: `LoadShader("scene.hlsl", "PSMain", "ps_6_0", { "-HV", "2018", "-DALPHA_TEST=1" })`. Profiles
`*_5_x` compile with FXC (`d3dcompiler_47`, DXBC), `*_6_x` with the vendored DXC (`third_party/dxc`, DXIL, signed by
its `dxil.dll`; the build fails on unsigned DXIL). The 5.1 shaders stay FXC: porting changes their code. `ngg-shaderc`
compiles the list in parallel (~1 s) with `core/shader_compiler.cpp` - the same code and arguments as the run-time
path - into byte arrays + a sorted table + a hash of the set (~750 KB); `WRENDERER_BUILD_HASH` covers the shaders.
**The shipped game compiles nothing and needs neither `dxcompiler.dll` nor `dxil.dll`** (RT included).

- Pitfall: `dxcompiler.dll` loads `dxil.dll` by bare name, and under MSBuild the Windows SDK's came first on PATH -
  `dxil.dll` is now loaded first, from our folder.
- **Development:** `NGG_SHADER_DIR=<repo>/modules/renderer/shaders` compiles from the files at run time (restart, no
  rebuild; the log names a shader `shaders.txt` does not list). No hot reload.

**Every pipeline goes through `CreatePipeline` / `CreateRootSignature`** (`pipeline_cache.h`): counted, timed
(`LogPipelineStats`), named by a hash of the whole description including the root signature's bytes. 72 pipelines:
24 at start-up; at the first scene load the scene's 41 (`Scene::EnsurePipelines` -> one `PipelineBatch`, made again
only when the HDR format or the mip bias changes) plus 7 per load (the environment is 1.35 s of their 1.5 s cold).

**The pipeline library is OFF** (`NGG_PIPELINE_CACHE=library` turns it on): an `ID3D12PipelineLibrary` in
`UserDir()/cache/wrenderer/pipelines.bin`, keyed by vendor/device/subsystem/revision (not the LUID, new every boot),
driver version and the shader-set hash; any mismatch or refusal deletes and rebuilds it; not used under
`NGG_SHADER_DIR` or PIX. **On AMD it saves nothing:** the driver keeps 181 KB for 72 pipelines (no GPU code) and a
"hit" compiles again when the driver's cache is cold. Kept to measure on NVIDIA.

**Pipelines are created one at a time.** Parallel creation (8 threads, and the scene's batch ahead on its own thread
from start-up) cut a cold start by ~5 s, but AMD's compiler (`amdxc64.dll`, driver 32.0.31021.5001, Radeon 780M)
crashed or hung in ~1 run of 12-28, warm or cold, whenever two compiles ran at once; sequential never did. A crash on
4-8 % of starts is worse than 5 s on the first start after an install or driver update. `NGG_PIPELINE_THREADS=<n>`
re-enables threads for testing another driver or vendor. No "compiling shaders" screen: 72 pipelines and the driver's
own cache make it a first-start-only cost.

Measured (Radeon 780M, 1080p, track 42, RT on; cold = `NGG_DRIVER_SHADER_CACHE=clear`):

| | run-time compile (before) | precompiled (now) |
|---|---|---|
| shaders at start-up / per scene load | ~1.6 s / ~1.8 s | 0 / 0 |
| renderer "ready" | ~1.9 s | **0.29 s** |
| pipelines, driver cache warm | 16-32 ms | 10-15 ms |
| pipelines, driver cache cold | ~8 s | ~8 s (2.2 s start-up + ~6 s first load) |


## 2. Assets: the .ngg format and where scenes come from

### 2.1 Two producers, one renderer path

**The renderer draws only `.ngg`.** The game hands it file bytes (`iface::RendererApi::loadScene`, or
`loadSceneVariant` with the stage's name, interface 19; 20 added the weather's wind); where those bytes came from is the game's business
([design.md §6.3](design.md)). Two producers write them today:

| producer | path | what it guarantees |
|---|---|---|
| **Wallaby** (separate repo, `NggWriter`) | `LBS -> glTF project -> (Blender / Wallaby edit) -> wallaby-cli import -> .ngg`; the direct `wallaby-cli convert` survives as a development shortcut | v3 materials; de-atlased ground textures; welded normals (exact positions, area weighting, per layer); tagged plants (`NGGG`/`NGGP`); collapsed stages with variants and aliases (`NGGR`/`NGGQ`/`NGGA`, its `StagePlan`); `NGGK` keys; `NGGS` start; weather from the project's JSON; models and placements (`NGGD`/`NGGE`) as the vegetation work lands. A human may have corrected any of it. |
| **W-Rally's legacy importer** (`game/src/assets/legacy_stage.cpp`, `ImportLegacyStage`) | `Maps/<stage>.lbs` + `<stage>_textures.rbz` + `TrackSettings.ini` -> `.ngg` in memory at stage load, no cache (~0.5 s on stage 41) | automatic, nobody needed: layers 0 ground / 1 vegetation / 2 scenery / 3 movable; the ground's two-texture blend; `DefaultMaterial()` everywhere; untagged plants by topology; `NGGK` for movable placements; `NGGW` from `TrackSettings.ini`; `kSceneFlagLegacyImport` set. **Not:** de-atlas, weld, tags, variants, `NGGS`. |

**The Wallaby project is three files, `.glb` + `.scene.json` + `.textures.zip`, and the order is automation first,
human second.** glTF has nowhere for a layer id, a material flag or a detail-texture pairing, so those live in the
sidecar only our tools write, and survive a DCC tool rewriting the `.glb`. The export already runs de-atlas, shading
overrides and weld, so the author starts from the best automatic result. A converter request from the renderer side
is therefore only real if it survives the glTF hop: a pass before export, or a sidecar field. `.ngg` itself is
export-only, never edited or round-tripped.

**Which one draws a stage, best first** (`game/src/assets/catalog.cpp`): the track's own `.ngg` (by stage, map
or track name), then an `.ngg` in the development folders, then the `.lbs` imported (`StageTime::imported`). A
hand-finished `.ngg` from the stage server is planned ahead of all of them; the catalog has the seam. In the stock
RBR folder: 163 stages in the menu, 4 from `.ngg`, 159 imported.

**The source stays authoritative for gameplay.** For an RBR stage the physics and the game keep reading `.trk .col
.mat .fnc .lbs .dls`; the `.ngg` owns pixels only. **No producer moves a vertex** that collision depends on: visual
and collision divergence is a gameplay change. `NGGK` is the one link: the physics moves the `.trk` solid with
instance id *k*, the renderer moves every object keyed *k*.

### 2.2 Container and chunks

Little-endian, every payload 4-byte aligned, an unknown chunk skipped by size (`Scene::Load`). `FileHeader` is
`'NGG0'`, version, chunk count. **Only `kFormatCurrent` (3) is read**; an older file is refused with "reconvert
the asset", never upgraded. `ngg_format.h` is the truth; `modules/renderer/src/format/ngg_scene.*` is the
strict reader and the writer (the legacy importer writes through it; tools and tests read through it), and the
renderer keeps its own loader in `core/scene.cpp`. `tools/ngg-check` and CTest `ngg-roundtrip` keep writer, reader
and loader in step; `ngg-producer-wallaby` does the same for a Wallaby file.

| chunk | content | required | record versioning |
|---|---|---|---|
| `NGGH` | `SceneHeader` 40 B: bbox, vertex/index counts, flags (`kSceneFlagAnchorToCamera` test scene only, `kSceneFlagLegacyImport`), `bakedScale` | yes | file version |
| `NGGV` | `Vertex` 44 B: pos, normal, uv0, uv1, colour `0xAABBGGRR` | yes | file version |
| `NGGI` | uint32 indices | yes | - |
| `NGGM` | `Material` 64 B, v3 (§2.3) | no | file version |
| `NGGO` | `Object` 16 B: layerId, materialId, firstIndex, indexCount | no | - |
| `NGGT` | embedded DDS: count, then name + bytes per texture | no | - |
| `NGGS` | `SpawnPoint`: position, facing | no | - |
| `NGGK` | per object int32 source key (`.lbs` (1,6) instance id = `.trk` solid), -1 none | no | - |
| `NGGR` | `VariantChunkHeader` + `VariantRecord` 68 B (32 B records still read as the prefix) | no | `version` 1, `recordSize` |
| `NGGQ` | per object uint32 variant mask | no | - |
| `NGGA` | `VariantAlias` 36 B: variant index, name | no | `version` 1, `recordSize` |
| `NGGG` | tag names (UTF-8) | no | - |
| `NGGP` | plant table: `PlantTableHeader`, `PlantRecord` 32 B, `PlantPart` 16 B, tag refs | no | - |
| `NGGW` | `WeatherChunkHeader` + `WeatherRecord` 172 B (§2.5) | no | `version` 1, `recordSize` >= 124 |
| `NGGD` | model library (§2.6) | no | `kModelVersion` 3 (reads 1-3) |
| `NGGE` | placements, `Placement` 40 B | no | `kPlacementVersion` 1, `recordSize` >= 40 |

**Textures are embedded, not referenced, so a stage is one file and one hash** (§2.8). **Records grow by
appending:** a reader takes min(`recordSize`, its own size) over defaults and skips a longer tail; a chunk
`version` changes only when an existing field changes meaning, and an unknown `NGGD`/`NGGE` version is refused
(the cards are drawn instead), never guessed.

**`bakedScale` is measured per asset, not a constant.** Kangaroo-authored stages bake vertex colour at half scale
(Joux Verte p99 = 128), Wallaby-authored ones at full (track-108 p99 = 255); a constant tuned on one blows the other
out. Producers write 255 / p99 of the brightest channel (percentile, not max: a few stray vertices read a dim bake
as full). Only light mode 0, the A/B reference, uses the bake; the default lighting ignores it.

**`NGGS` is written but nothing reads it** (neither the renderer nor the game); nor does anything read a variant's
own start.

### 2.3 Materials: metallic-roughness, v3

**The material is glTF metallic-roughness near enough 1:1**, so an authored material crosses the glTF spine without
a translation layer inventing values. It replaced v2's RBR-shaped two-stage material outright (v2 reading removed
2026-11-07, once Wallaby wrote v3).

```
baseColour detail normal orm emissive  (NGGT indices, kNoTexture = 0xFFFFFFFF when empty)   flags
baseColourFactor[4]  roughness  metallic  normalScale  occlusionStrength  emissiveStrength  alphaCutoff
```

- **Slots are sRGB or linear by role**: `baseColour`, `detail`, `emissive` sRGB; `normal` and `orm` (glTF packing:
  R occlusion, G roughness, B metallic) linear. Nothing in a DDS says which, so `Scene::Load` collects the textures
  bound to normal/ORM slots (`m_linearTextures`) and the upload picks `*_UNORM` for them, `*_UNORM_SRGB` otherwise;
  the first binding wins if a DDS is used both ways.
- **Factors multiply their map; an empty slot binds 1x1 white**, so `map * factor == factor` and an untextured
  material is complete. `DefaultMaterial()` is a rough dielectric (roughness 0.75, metallic 0, opaque, cutoff
  170/255 = RBR's AlphaRef) - what the legacy importer emits for every RBR texture.
- **Kept from RBR because measured, not inherited:** `detail` blended by vertex alpha (live on 25-100 % of ground
  area on both test tracks; it hides chunk seams), and `baseColour.a` as coverage on alpha-tested geometry. The baked
  vertex colour stays in the vertex as the A/B reference, not as a material property.
- **Derived at load, never stored:** `kDrawSingleStage` when `detail == kNoTexture` (the shader then never reads
  uv1), `kDrawNormalMap` when `normal` is filled. The normal map is gated on that flag, not on the sample: the
  white fallback decodes to (1,1,1), and no single fallback is neutral for every slot.
- **No tangents in the vertex:** RBR's geometry has none and the 44-byte vertex has no room, so the tangent frame
  comes from screen-space derivatives in `scene.hlsl`.

| flag | bit | meaning |
|---|---|---|
| `kMaterialAlphaTest` | 0 | `clip()` against `alphaCutoff` |
| `kMaterialTwoSided` | 1 | no back-face culling |
| (was `kMaterialSingleStage`) | 2 | unused, so an old bit can never read as something else |
| `kMaterialBlend` | 3 | glass: drawn after opaque, far to near; no depth write, no shadow, not ray traced |
| `kMaterialFoliage` | 4 | implies AlphaTest + TwoSided (the loader ORs them in); lit with wrap and transmission; hashed alpha edge |

**Live editing goes through `renderer_edit.h`** (`getMaterial` / `setMaterial` / `materialName` /
`objectMaterial`, version 2): the whole descriptor in and out, the derived flags recomputed on every set, only a
texture-slot change touches the descriptor heap. A scalar edit shows on the next frame because the material
reaches the shader as root constants. The measured single-stage decision (§2.7) is stored per object and survives
a material edit. Persisting edits is Wallaby's, keyed by material (object indices move when the converter regroups).

### 2.4 Objects, layers, variants

**The object is the unit of drawing and of addressing:** one index range, one material, one layer. Layer ids are
the producer's (the legacy importer: 0 ground, 1 vegetation, 2 scenery, 3 movable); there is no layer table chunk.

**A variant is a STAGE, and the file says which names it answers to; the renderer guesses nothing.** RBR shipped each
time of day as its own `.lbs` with the light baked in. W-Rally lights live and treats time of day as weather, so
Wallaby collapses siblings that draw the same and are the same physics into one stage; only a stage that really
differs is a variant (a reversed stage always is). Measured over 57 RBR sibling pairs: the drawn terrain is the same
triangles (0 of 86,510 differ on 42, 0.45 % on 108), the objects mostly match, and what differs is the bake and
darkened overcast copies of textures - both things W-Rally now does live. So **a variant is only which objects it
has**, plus its own start and origin:

- `NGGR`: name (map number, `"42"`, `"45"`; `"125_O"` for a sibling kept apart), `kVariantHasSpawn`, spawn
  position/facing, `offset`. The first record is the main variant; its vertex colours and textures are the file's.
  At most 32.
- `NGGQ`: bit *v* = object in variant *v*. Without it every object is in every variant.
- `NGGA`: the other names a variant answers to (RBR's `track-42_M`, `track-42_O`).
- **Matching is exact, ignoring case**, names first then aliases (`Scene::SetVariant`); no match or no request draws
  the main variant and logs it. The game passes its stage name.
- **Objects outside the variant are loaded and marked removed, not dropped**, so object indices - which `NGGK`,
  `NGGP` and editor handles all are - stay the file's; `SetObjectVisible` cannot show a removed object.
- **`offset` moves the world:** a reversed stage can be the same terrain built elsewhere (RBR's 55 is 51 moved by
  (-1193, +664, +1793) m; 66 is 63 moved by (-51, 0, +3350)). Everything is stored in the main variant's frame; the
  loader moves a copy of the vertices, the bounds and the plant boxes before anything reads them.

Tests: `editor-api-41` (variant counts, a stage name, a reversed stage, an alias, a name only like one, masks of the
wrong length), `ngg-roundtrip` (all three chunks through `ngg_scene`).

### 2.5 Weather records

**`NGGW` is binary records; the JSON form is for projects and the editor.** The preset is inside the file, not
beside it, because it is part of an event and must be covered by the same hash as the geometry (a thinner fog is
a competitive change). `WeatherRecord` (172 B): a 64-byte name, sun direction, intensity, turbidity, ground albedo
and colour, cloud cover, fog visibility (Koschmieder, 0 = none) and height, rain, snow, exposure; appended
2026-10-30: `timeFlags` (`kWeatherTimeSet`), date, local hour, UTC offset, latitude, longitude, north - with the
time set, sun and moon follow astronomically and `sunDirection` is unused. A 124-byte record reads as no time set.
Appended 2026-11-13: the wind - `windDirectionDeg` (where it blows towards, degrees from +z towards +x), `windSpeed`
(m/s at 10 m), `gustiness` (0..1); a 160-byte record reads as calm (§2.6). JSON: `"wind": {direction, speed, gustiness}`.
The first record is the default. `DefaultWeatherRecord()` lives in the format so a producer outside the renderer can
start from it; `core/weather.cpp` checks at compile time that it matches `core::SkyParams`.

- **Runtime:** `core::ParseWeatherChunk` / `PackWeatherChunk` (`core/weather.h`); the game selects by name
  (`findWeatherPreset`), and the catalog lists a file's presets from its chunk headers alone (`NggWeathers`).
- **Project form:** `core::WeatherToJson` / `WeatherFromJson`, the `ngg-weather` schema; Wallaby's sidecar holds it
  and its writer packs it.
- **The name is the identity:** for a preset the game selects, RBR's `TrackSettings.ini` section name
  (`41M_crisp_clear`, `04E_hazy_partcloud`).

**`TrackSettings.ini` is an import source, not a preset system** (`game/src/assets/legacy_weather.cpp`,
`LegacyWeather`). Its scattering numbers are art knobs for RBR's 2004 shader (turbidity -2.8..9, Mie g = 1.0), so they
are not copied. What translates: the section name; `SunDir` and ground reflectance; the condition in the name (hazy
-> turbidity 3; part/light/heavy cloud -> cover 0.35/0.6/0.95; light/heavy fog -> visibility from `FogEnd`, else
1500/300 m, under a 150 m layer; rain and snow 0.4/1). Two repairs: decimal commas, and 48 suns at or near the horizon
raised to ~10 degrees on their bearing (else a physical sky draws night). RBR has no night stages: `M` morning, `N`
noon, `E` evening. The default is the stage's own time of day, crisp-clear if it has one. 32 stock stages have no
section and draw the default sky.

### 2.6 Plants and instanced models

**Plants (`NGGG`/`NGGP`)** are the alpha-tested geometry grouped into individual trees, bushes and tufts: a plant is
a list of parts, each a contiguous run of one object's indices, with a box and tags (`tree-card`, `bush-card`,
`grass-card` for the old crossed cards; the tagger's classes otherwise; unknown tags carried). The producer reorders an
object's triangles by plant so draws are unchanged. **Tags come from the producer, never from renderer heuristics.**
The legacy importer writes plants without tags: connected pieces over 1 cm-quantised positions (separate plants do not
touch), pieces wider than 50 m (`kPlantMaxWidth`) left out.

**Instanced models replace the cards** (2026-11-09). `NGGD` holds each model once, `NGGE` places it.

- **`NGGD` layout:** `ModelChunkHeader` (counts), `ModelRecord` 80 B (name, LOD range, model-space box, `fadeWidth`,
  `rtLod`), `ModelLod` 16 B (submesh range, `maxDistance`, `kModelLodBillboard`), `ModelSubmesh` 20 B, v3
  `Material`s (texture slots index the file's `NGGT`), 44-byte vertices in model space, indices relative to the
  submesh. Every array is its struct's exact size, so the chunk is checked whole (`ngg::CheckModels`). Model space:
  +y up, origin at the base, metres.
- **`Placement` 40 B:** model, the `NGGP` plant it replaces (`kNoPlant` none), base position, yaw, `scaleXZ`,
  `scaleY`, tint, variant mask. **Upright on purpose:** a plant on a slope grows up, and the fit from a card plant is
  exactly these numbers; a full matrix waits for a producer that needs one (`recordSize` grows then).
- **The replaced cards are made degenerate in the index buffer at load**, so object ranges, plant parts and
  triangle numbering stay the file's; `BuildRtMesh` leaves them out; an object left with no triangle is removed.
  Only against the file's own plant table (without one, the cards stay and the load says so).
- LODs switch by distance from the camera scaled by the placement's height and `NGG_MODEL_LOD_SCALE`; over the last
  `fadeWidth` metres two LODs are drawn dithered to complementary pixels; the last LOD dithers out. The billboard LOD
  turns about the placement's up axis and is never ray traced. One `rtLod` per model keeps the TLAS fixed as the
  camera moves. Drawing, culling and ray tracing: `modules/renderer/src/core/models.cpp`, §4.2.
- **A ray-tracing-only LOD** (`kModelLodRayOnly`, `ModelLod` flag bit 1; NGGD **version 2**, 2026-11-10): the mesh
  the BLAS is built from and nothing else - never drawn (prepass, main pass, shadow maps), never picked by distance.
  For a coarse shell with few alpha layers (the generator's `<id>_RT`: a ray crosses 2 alpha surfaces of it against
  18-24 of LOD0's). Rules, each refusing the chunk (the cards drawn instead): at most one per model and it is the
  model's LAST LOD; at least one drawn LOD before it; `rtLod` names it; not also a billboard; `maxDistance` 0. The
  drawn LODs keep their rules (`maxDistance` above 0 and increasing - the renderer's loader checks that too now).
  `ModelRecord`'s box covers the drawn LODs only. Its materials are alpha-tested and two-sided, not foliage. A
  version-1 chunk (same layout) still loads; one carrying the flag is refused. In the renderer `ModelDef::drawLods`
  counts the drawn LODs: the distance pick, the crossfade's next LOD and a cell's `reach` stop there (with the shell
  in place, `lods.back()` had made every reach 0 and faded the billboard into the shell). The editor table is
  unchanged (version 2).

**Measured on stage 41** (dev iGPU, RBR's run replayed in lock-step, TAA and vsync off, GPU frame), with
`ngg-check --trees` placing a code-generated conifer (LOD0 142 tris to 50 m, LOD1 40 to 150 m, billboard to 900 m)
in place of all 3,743 tree-card plants:

| stage 41 | GPU frame | ray tracing |
|---|---|---|
| RBR's tree cards | 26.9-28.8 ms | 8.8-9.1 ms |
| placeholder trees, LOD1 ray traced (default) | 31.5-32.9 ms | 12.6-12.7 ms |
| placeholder trees, LOD0 ray traced | 36.3-37.9 ms | 15.4-16.3 ms |
| LOD0, needles opaque (experiment) | 28.0-28.2 ms | 11.0 ms |

**Drawing the trees is cheap, ray tracing them is not:** a typical view is ~20 instanced draws, ~8k triangles, CPU
cull 0.03 ms (0.34 ms with four cascades). The cost is any-hit tests through overlapping alpha-tested needle tiers
(+2.2 ms opaque vs +7.2 ms alpha-tested), hence `rtLod` 1. A file without models costs nothing (28.8 / 28.8 ms
before and after). Tests: `instancing-41`, `editor-api-41` (placements through `renderer_edit.h` version 2).

**The ray-only shell, measured** (2026-11-10; the same replay, three interleaved rounds; the machine read ~40% slower
than on 11-09, so against the cards of the same round). `ngg-check --trees --rt-lod ray` adds the placeholder's shell:
its trunk and a closed hull round the needle tiers, 178 triangles, alpha-tested with holes (~60% covered).

| stage 41, ray tracing median | round 1 | round 2 | round 3 | over the cards |
|---|---|---|---|---|
| RBR's tree cards | 12.33 ms | 13.03 ms | 13.47 ms | - |
| placeholder trees, LOD1 traced (4 tiers) | 16.11 | 17.31 | 18.32 | +3.8 / +4.3 / +4.9 |
| placeholder trees, the ray-only shell | 16.14 | 16.95 | 17.28 | +3.8 / +3.9 / +3.8 |
| placeholder trees, LOD0 traced (7 tiers) | 20.18 | 20.59 | - | +7.9 / +7.6 |
| the shell made opaque (experiment) | - | - | 15.73 | +2.3 |
| the shell without its trunk submesh (experiment) | - | - | 16.77 | +3.3 |

Against LOD0 the shell saves 3.3-3.6 ms; against the placeholder's coarse LOD1, 0-1.0 ms. What is left over the cards
is mostly the TLAS traversal of 7,486 instances (+2.3 ms opaque), the shell's alpha ~1.5 ms, and a trunk as a second
submesh ~0.5 ms (so a producer puts the trunk into the shell's one submesh). Tests: `instancing-41` runs on the shell
(frames through every LOD band draw exactly what the trees without it draw, the BLAS is the shell's, a version-1 NGGD
of the same trees loads); `ngg-roundtrip` breaks each rule in turn and both readers refuse it.

**Wind** (2026-11-13; Wallaby's vegetation plan, "Planned: wind", step 2). Trees and bushes sway: the vertex shader
bends each vertex of a model that carries wind by weights its producer baked. Visual only (collision is the `.col`).
- **Format, NGGD version 3:** `ModelRecord::flags` `kModelWind` (bit 0), and `ModelRecord::reserved` became `float
  windBend` - how far the top leans at 10 m/s, metres at scale 1 (spruce ~0.3, poplar ~0.5, a shrub ~0.1). A wind
  model's vertices carry the weights in `uv1` (model materials are single-layer, so a model's `uv1` is never sampled):
  `x` the branch weight 0..1 (0 where a branch leaves the trunk), `y` the branch's phase in the fraction and the leaf
  flutter weight in hundredths in the integer part. Vertex alpha is not free (every pixel shader cuts by it). Rules,
  in both readers: no other flag; `windBend` in (0, 10] with the flag and 0 without; every drawn (not billboard) LOD's
  `uv1` in range; a version 1-2 chunk carrying either refused (version 2 loads, without wind). The writer writes 3.
- **The weather:** `WeatherRecord` appends `windDirectionDeg`, `windSpeed`, `gustiness` (§2.5; a shorter record is
  calm), `WeatherDesc` the same (interface 20). The renderer takes the wind of the weather in force (the preset, or
  `FrameDesc::weather`) and its clock from `FrameDesc::time`, which it reads from here on (`Scene::SetWind`,
  `SetWindTime`: wrapped at 3600 s; a jump over a second or backwards has no last frame). The game's stage frame sets
  it from the stage's own clock, which stops in a pause (`game.cpp` `stageClock`; only the garage set it before).
- **The shader, `shaders/wind.hlsli`:** `WindBend`, a float32 port of Wallaby's `tools/vegetation/plantgen/wind.py`
  (its docstring states the formula): the main bend (the whole plant leans by `(h / height)^2`, each vertex keeping its
  distance from the base, with the speed squared, 0.2 Hz at 11 m), the branch sway (0.3-0.42 Hz, along the wind and
  up and down) and the leaf flutter (2.5-3.75 Hz, along the normal); gusts roll across the stage, and a placement's
  phase is a PCG hash of its base to the centimetre. Precision far out and late: cycle counts are reduced to their
  fraction before they are summed (the time split into whole seconds, the frequencies into 12-bit halves, `2.2 /
  height` carried as hi + lo), and the hash's centimetres come from the float's bits, exactly Python's `round`.
- **`VSMain`:** an instance with `kInstWind` (ix.z bit 2) in a wind above 0 is bent; its constants ride in
  `InstanceRec::pad` (`f16 windBend | f16 height << 16`, both times the vertical scale). `prevWpos` is the vertex bent
  at the last frame's time, and `VSOut::swayed` makes the prepass write its motion (`NggMotion`, which also gives the
  new debug view 16, "MOTION") - otherwise TAA would smear every leaf. The billboard's quad leans by the main bend
  alone. The prepass, the main pass and the cascades share `VSMain`, so the shadow maps sway too (`gWindPrevOn` 0
  there: no last-frame bend). In calm air nothing is computed: the frame is the one without wind, bit for bit.
- **C++:** the loader keeps `wind`, `windBend` and the box's height per model; `emit` sets the flag and `pad`;
  `PlacementTransform` widens a wind model's culling box by what it can lean at the capped wind (20 m/s, gustiness 1:
  wind.py's `max_lean` and the flutter), fixed at load so the cells never rebuild with the weather - winds above
  20 m/s sway as 20. Its LOD distances still come from the box at rest. **Ray tracing is static:** the BLAS is the
  model's `rtLod` unbent, so with RT on (which draws no cascades) the shadows of moving leaves stand still.
- **Tests:** `wind-reference` (a compute shader with `wind.hlsli` against a C++ double port of wind.py, which must
  reproduce Wallaby's golden table, `tests/data/wind_golden.json`, to 1e-13 m): on the same float32 inputs 0.06 mm on
  the 64 golden rows and 0.63 mm on 512 worst-case rows (the hour's end, 20 m/s, gustiness 1, short trees, full
  weights), the hash bit for bit; a straight `frac(f t)` or `2.2 / height` in float give 1.2-1.3 mm there, and
  `round(x * 100)` in float hashed ~2% of far placements to another centimetre. The golden outputs themselves are 2.4
  mm off, and nothing can do better: rounding wind.py's double inputs to float32 moves its answer that much (the
  packed phase holds ~1e-5, and the phase also sets a branch's frequency). `instancing-41` (`--wind`, the placeholder
  with weights baked the generator's way): calm air draws exactly the frames of the trees without wind; at 15 m/s two
  frames 0.1 s apart differ on 22k pixels and the same frames without trees on none; 21k of 23k tree pixels carry
  motion of their own; the cascades' visibility changes on 8k ground pixels no tree covers; a lone billboard's top
  moves 5.2 px in a second and its bottom 5% 0.46 px (the hashed alpha edge of a 2-3 px trunk; its geometry moves
  ~0.01 px). `ngg-roundtrip`: the wind round-trips into the renderer, version 2 loads, a 160-byte NGGW is calm, nine
  broken rules are refused.

**The wind, measured** (2026-11-13; dev iGPU, RBR's run replayed, `--step 0.0333`, 8,000 ticks, 1,618 frames,
`PHYS_DRIVE_OFF=taa,vsync`, a worktree build without `W-Rally.ini`; the `instancing-41` file - the placeholder with
its ray-only shell and the wind - and `NGG_WIND=10,0.5,90` for the wind; three interleaved rounds, medians):

| stage 41 | round 1 | round 2 | round 3 | main pass | ray tracing |
|---|---|---|---|---|---|
| RBR's tree cards | 46.01 ms | 44.52 ms | 44.96 ms | 5.77-6.30 ms | 15.0-16.2 ms |
| placeholder trees, calm | 52.11 | 51.94 | 51.89 | 6.20-6.97 | 20.7-20.9 |
| placeholder trees, 10 m/s, gustiness 0.5 | 51.76 | 52.70 | 52.08 | 6.78-6.97 | 20.7-21.2 |

**The wind costs nothing measurable here:** -0.35 / +0.76 / +0.19 ms against calm, within the round-to-round noise (the
cards moved 1.5 ms between rounds); the target was under 0.2 ms. The placeholder is light (LOD0 142 triangles, ~8k
triangles in view), so the real models (LOD0 ~25-30k) are the run that bounds it; the vertex work per placement is
two `WindBend`s in the camera's passes and one in each cascade. These runs had the clock standing still (the stage
frame set no `FrameDesc::time` yet, fixed since) - the bend is computed the same either way. By eye, and whether the
static RT shadows under moving leaves show while driving (phase 2: K bent RT shells), waits on the real models.

### 2.7 Load-time fallbacks

**Every repair the renderer does at load follows one pattern: measure first, act only where the data is
unambiguous, log it, and stand down by itself once the producer does the job.** They exist for the legacy import
and for older or community files.

| fallback | where | rule |
|---|---|---|
| per-object AABB | `Scene::Load`, one pass over the indices | always; there is no bounds chunk, so culling never depends on a converter version |
| single-stage | `Scene::MeasureSingleStage` | an object whose uv1 is unusable (\|uv1\| > 1e6) on **every** vertex draws single-stage; per object, not per material (track-42's material 81 is 23 % garbage, 77 % fine); mixed objects counted, not touched. track-42: 323 flagged, 126 mixed; track-108: none |
| solids among the cards | `Scene::MeasureSolidCards` | an alpha-tested object that samples only opaque texels (15 barycentric points per triangle, 2x2 min, x vertex alpha, vs the cutoff) is drawn as a solid, not foliage; `NGG_NO_SOLIDS` turns it off. The legacy importer does the same test itself (`Builder::Solid`) |
| normal weld | `Scene::WeldNormals` | exact-position weld with hard edges kept; detects an already-welded file and does nothing (re-welding would undo the converter's forced splits). On by default |
| foliage volume normals | `Scene::SphericalFoliageNormals` | a plant's cards shaded as one volume; defers to the file |
| plants | `Scene::FindPlants` | only for a file with no plant table **and** not a legacy import (RBR's vegetation block also holds people, cars, houses, fences) |
| mip chains | `GenerateDdsMips` (`core/mipgen.*`) | below |

**Mip chains for textures that shipped without one:** track-108 ships 76 of 88 textures single-level. A DDS with mips
is left as it is. Mip 0's blocks are copied verbatim; each level is built from the previous one in linear light;
alpha-tested base colours keep mip 0's coverage at their material's `alphaCutoff` (Castano 2010, alpha rescaled by
bisection, colour alpha-weighted); levels 1+ are re-encoded in the source format (DXT1/3/5 only). Up to 8 threads.
Measured on track-108: 76 textures in 203 ms, memory 21.2 -> 28.3 MB (+33 %, against 113 MB uncompressed), re-encode
error 3.55/255; horizon-band shimmer at rest with TAA on 0.79 -> 0.47, aliasing energy with TAA off 27.1 -> 15.6. A box
filter errs either way on coverage (`FIELD01_FENCE` level 4: 36.5 % -> 26.6 %; `FENCE_WOOD03` 73.4 % -> 81.2 %);
preservation holds it within the alpha resolution. The mode (`Scene::SetMipFallback`: 0 off, 1 box, 2 coverage) is
2 and nothing sets it now.

**The vertex shader's `isfinite(uv1)` guard never fires** (RBR's bad uv1 is finite garbage up to ~3.1e36); the
magnitude test in `MeasureSingleStage` is what actually protects. Past ~1e6 the sampler collapses to the top mip, so
stage 1 only blends in the atlas average.

### 2.8 RBR asset rules the importer and the fallbacks rely on

- **Vertex colour is two quantities:** RGB = baked light at the editor's scale (§2.2 `bakedScale`), A = the ground's
  blend weight, or coverage on objects. RBR's actual blend weight is `tex1.a * vertexAlpha`.
- **Baked lighting is discarded for light**, together with RBR's baked shadow and specular maps, fog and in-scatter:
  the sun, the sky, RT shadows and AO replace them. What is owed to the original is geometry, UVs, texture assignment
  and the blend weight. A difference from vanilla that traces to a missing bake is out of scope; one that traces to
  our pipeline is a bug.
- **Object meshes are single-texture** (RBR's `RLSingleTexture`): their uv1 was never read by RBR and is
  unvalidated. The importer gives alpha-tested materials no detail layer.
- **Ground textures named `...Plate...` are atlases** (Joux Verte: 2048^2, 18 cells of 256^2, a 2.25-texel gutter
  sized for mip 0). Anisotropic taps cross the cell edge (dominant), bilinear taps at high mips cross the sub-texel
  gutter (residual): road seams. **The fix is de-atlasing in the converter** - per texture, not globally (some ground
  textures genuinely tile), wrap/clamp per cell decided from its UV range. Wallaby does it; the legacy importer does
  not, so imported stages keep the seams.
- **Hidden ground chunks:** RBR draws ground chunks of visibility 1 and 2 only; the importer drops the rest (darker
  patches over the road if drawn).
- **Vegetation block (1,1) is not only plants:** walls, rocks, houses, people and cars sit there under the same
  flags; only the texture's alpha tells a solid from a card (§2.7).
- **RBR's own visibility data is not used:** block (2,10)'s ground boxes are `(origin, size)`, one per ground region
  (158 on Joux Verte), ~10x coarser than our objects; block (1,4), named "clipping planes" in Wallaby, is not planes.
  Per-object bounds are computed at load instead.
- **Normals in RBR files were never used for shading** (no `dcl_normal` in its shaders) and are split per face; the
  importer takes them as they are (face normals where the layout has none) and the renderer welds.
- **Protected stages** (geometry block version 88/89, ciphertext) are never imported, not even in part
  (`formats::LbsEncryptedVersion`; the catalog leaves them out): decrypting is a licensed install's job, and a plain
  `.ngg` of one would leak it. Stock RBR: 31 files (maps 10 overcast, 95, 96, 105, 488-499).
- **Textures by surface:** the importer takes `<stage>_textures.rbz`'s dry/damp/wet folder matching the physics'
  wetness first, then the others.

### 2.9 Fairness and encryption

**Rendering is competitive:** view distance, fog, foliage density, night brightness are information. So everything
that changes what a player sees is inside the one `.ngg` (embedded textures, weather records, models) and covered by
the stage hash the game reports ([design.md §7](design.md)); a sanctioned render profile locked per event is not built.
**Encryption is deferred:** the format has no envelope field today; chunk framing keeps payloads independently
encryptable, and protected sources are refused rather than converted.



## 3. Sky, atmosphere, exposure and post

The sky is evaluated, not painted, and **one atmosphere model feeds everything that needs light from the sky**: the sky
pass, the environment and irradiance maps, the aerial-perspective table and the multiple-scattering table. Everything
writes linear HDR radiance into one RGBA16F target. Exposure, the tonemap and the sRGB encode happen once, at the end.

Frame order (`Renderer::RecordFrame` in `modules/renderer/src/core/renderer.cpp`):

| step | where | |
|---|---|---|
| atmosphere for the frame | `Scene::SetAtmosphere(sky)`, `Sky::SetMultipleScattering` mirrored from the Scene | one `SkyParams` for every consumer |
| TAA jitter | `PostFx::Jitter` → `Scene::SetJitter`, `Sky::SetJitter` | set **before** the prepass (see 3.7) |
| prepass, visibility | `Scene::RecordPrepass`, RT or cascades | depth, normals, motion |
| sky | `PostFx::Begin`, `Sky::Record` (or a clear to `PostFx::kHdrClear` with the sky off) | covers every pixel, no depth |
| scene main pass | `Scene::Record`: `AtmosphereMs::Update` (only when dirty), `Environment::Record`, reflection probe resolve, `AerialLut::Record`, draws | |
| post | `PostFx::Resolve`: TAA or upscaler, meter, tonemap + sharpen + encode | into the client's target |

In W-Rally the module (`modules/renderer/src/module/module.cpp`) builds the frame's `SkyParams` from the weather
(`weather.h`, preset or `WeatherDesc`) and, when `FrameDesc::skyTime` is set, puts the sun and moon there from the
date, time and place (`ApplySkyTime`, `astronomy.h`, Meeus). It always draws with the sky on, ACES, and TAA from
`RenderSettings::taa`. Weather and night are their own sections; here they only appear as inputs to the model.

### 3.1 Atmosphere and sky

**The model lives once, in `shaders/atmosphere.hlsli`, `#include`d by every consumer.** The sky and the aerial
perspective are the same integral over different segments of one ray; the horizon, where they meet, is exactly where
two implementations would visibly disagree. `AtmScatter(ray, sunDir, maxDist, ...)` returns in-scatter and the
transmittance of what lies beyond, so distant geometry is **both dimmed and tinted** (tint alone reads as paint, not
air).

| constant | value | |
|---|---|---|
| `kRg`, `kRa` | 6360 km, 6420 km | ground, top of atmosphere |
| `kHr`, `kHm` | 8000 m, 1200 m | Rayleigh, Mie scale heights |
| `kBetaR` | (5.8, 13.5, 33.1)e-6 /m | |
| `kBetaM` | 21e-6 /m × `turbidity` (extinction ×1.1) | `turbidity` is OUR haze dial |
| `kMieG` | 0.76 | Cornette-Shanks phase |
| `kCamHeight` | 500 m | the camera's fixed altitude on the sphere |

**The camera sits at a fixed 500 m on a planet-sized sphere**, so the atmosphere depends on view direction and the sun,
never on where on the stage the car is. That is what lets every table below be rebuilt from scratch each frame with no
history.

- `AtmSunTransmittance(sunDir, steps)`: one march along the sun ray, then the cloud deck and the fog. Low-sun reddening
  falls out of the model; it returns black with the sun below the horizon (no light through the earth).
- `AtmSkyRadiance`: scatter plus the below-horizon ground blend (the sky mirrored, times `groundAlbedo`), plus the
  second body's sky (`light2*`) and the night glow. **The sun disc is deliberately not in it**: the environment map
  feeds a specular term that has its own sun lobe, and a disc in both counts the sun twice.
- The sky pass (`shaders/sky.hlsl`, `Sky::Record`) marches **16×6** per pixel, adds the sun disc (0.00465 rad ×
  `sunDiscSize`, dimmed by cloud and fog), the moon and the stars. It builds its ray from the camera basis and the
  projection's tangents (no inverse, unaffected by reverse-Z) and **subtracts the jitter**, so the horizon does not crawl.

**Taken from RBR, and not.** `SunDir` (direction the light travels, RBR's sign convention kept in `SkyParams::sunDir`)
and `Terrain_Reflectance_R/G/B` (ground bounce colour) are used. `Turbidity`, `Rayleigh_Multiplier`, `Mie_Multiplier`,
`Greenstein_Value` are **not**: Joux Verte ships Turbidity 0 and Greenstein 1.0, degenerate for any real model, i.e.
internal scales for RBR's ps_1_1 constants. RBR's skybox mesh is not converted: a painted sky cannot follow a moving sun.

### 3.2 Multiple scattering

`AtmosphereMs` (`src/core/atmosphere_ms.*`, `shaders/atmosphere_ms.hlsl`) is Hillaire 2020 §5.5: a **32×32 table
Ψ_ms(μ_s, altitude)** of second-order isotropic scattering (64 directions × 20 steps, ground as a Lambertian
reflector) times `1/(1 − f_ms)` for all higher orders. `AtmScatter` adds `σ_s · Ψ_ms · T_view` per view step, not gated
on the sun ray being lit (a point in the planet's shadow still sees the lit atmosphere: most of twilight).

- **The table does not depend on the sun**: μ_s is a lookup coordinate. It changes only with turbidity and ground
  albedo, and `Update` records nothing otherwise. So the Sky and the Scene each own an instance with no ordering
  between them; the switch must match (`Renderer` mirrors it).
- Bound at `t0/s0 space1` in every root signature that compiles `AtmScatter` (`MsSrvRange`, `MsSampler`), so it can
  never collide with a consumer's own registers.
- Off = an all-zero table = exactly the single-scattering model; the A/B costs no shader branch.

**Verified**: GPU texel (937.5 m, μ_s 0.781) `0.02319 0.02943 0.04071` against a numpy port `0.02319 0.02944
0.04073` (fp16). **Cost** (dev iGPU, track-108): switching it on costs nothing over off; the per-step lookup raised the
aerial fill from 0.29 to 0.48 ms whether the table is zero or not.

### 3.3 Aerial perspective

`AerialLut` (`src/core/aerial.*`, `shaders/aerial.hlsl`): two RGBA16F volumes, **64×64 screen cells × 32 distance
slices**, in-scatter and transmittance, filled by one compute dispatch per frame (one thread per froxel, each an
independent `AtmScatter` over [0, d] at **8×3** steps). The scene pass reads them with two trilinear fetches
(`AerialAt` in `shaders/scene.hlsl`). Owned by `Scene`, filled inside `Scene::Record` before the scene binds its heap,
main pass only.

Each rule below would be a bug the other way round:

- **Radial distance, not view depth**: the integral is along the ray. Quadratic slices, `w = sqrt(d / maxDist)`, so the
  near field gets the resolution.
- **Below the first slice centre** in-scatter and `1 − T` scale by `(w/w0)²` (linear in distance, exact to first
  order); clamping to slice 0 would put metres of air between the camera and its bonnet.
- **Reach = 1.5 × the far plane** (radial distance at the frame corners runs ~1.4× view depth), clamped 500 m..60 km;
  12 km when the projection has no readable far plane.
- **Unjittered `Scene::LastViewProj`** for the cell rays: a cell spans ~30 px, following the jitter would only make the
  table shimmer.
- **CLAMP sampler (s4)**: wrapping in w blends the farthest slice into the nearest.
- **Same 8×3 quadrature as the per-pixel march**, which survives as an exact reference (`Scene::SetAerialMarch`), so a
  difference between them is the table's interpolation error alone. 16×4 cost 0.60 ms of fill for nothing visible
  after the tonemap.

Measured (dev iGPU, track-108 start camera, before multiple scattering):

| aerial | main pass | aerial's share |
|---|---|---|
| off | 1.65 ms | — |
| per-pixel march 8×3 | 3.55 ms | 1.9 ms |
| table, 8×3 fill | 2.14 ms | 0.50 ms (0.29 fill + ~0.2 lookup) |

Table vs march: max 1/255 on the HDR buffer, 99.4 % of pixels identical. The march costs ~10 ns per geometry pixel
and the fill is fixed, so the table's advantage grows with geometry coverage. A 32×32 grid would cut the fill ~4×;
not taken, and the Mie lobe towards a low sun (the case that would test it) has not been looked at.

### 3.4 Sky lighting: environment, irradiance, ambient specular

`Environment` (`src/core/env.*`, `shaders/env.hlsl`), **owned by `Scene` and rebuilt at the start of every main pass**
from the same `SkyParams` the sky uses (when it was the caller's, only the old viewer built one and in-game there was
no ambient specular). It is the frame's sky LUT: sky radiance depends only on direction and the sun, both uniform for
the frame, so 4,096 marches replace one per pixel.

| map | size | content |
|---|---|---|
| radiance | 64×64 octahedral, 7 mips, RGBA16F (< 40 KB) | mip 0 = `AtmSkyRadiance` at 16×4, each mip a 2×2 box of the last |
| irradiance | 16×16 octahedral | cosine-weighted upper-hemisphere integral of mip 0, 16 rings × 32 directions, exact per-ring solid angle |

- **Octahedral, not a cube**: ordinary mips and bilinear sampling, no seam policy. The small error along the
  octahedron's edge is invisible on a sky; revisit for sharp probes.
- **The mip chain is the roughness axis** (`level = roughness × (mips − 1)`); a box chain instead of a GGX prefilter is
  what most engines ship.
- **Rebuilt every frame**, no staleness rule: cheaper than any rule, and a rule can be wrong.
- **Irradiance, not `π·L(N+up)`**: one sample looks at the zenith, the darkest part of a clear sky; against the true
  integral it was 0.65× at a 50° sun and 0.43× at 10°. GPU vs numpy integral for one direction: `0.6221 / 1.0234 /
  1.6709` vs `0.6220 / 1.0247 / 1.6755` (within 0.3 %). Light from below is not in it: the scene adds the ground
  analytically (3.5).
- **Ambient specular** = `EnvRadiance(R, N, roughness) × EnvBRDFApprox` (Karis' analytic split-sum fit) × AO, with a
  clear-coat layer sampled separately. For reflection-focus objects (the car) `EnvRadiance` reads the cube probe
  (`probe.h`) in place of the sky; that is the car-paint section's.
- `gEnvReady` (the map exists) is separate from `gEnvOn` (ambient specular wanted), so turning specular off for an A/B
  does not change the diffuse. Without a map the diffuse falls back to a 6×2 per-pixel march, never to nothing.

### 3.5 Surface lighting balance and physical units

**The physical balance is the default and the only one any client reaches** (`Scene::m_physical = true`;
`SetPhysicalLighting` is never called). In `shaders/scene.hlsl`:

```
sunE     = sunIntensity * AtmSunTransmittance(sunTowards, 8)          // per pixel, 8 steps
diffuse  = albedo * (1 - metallic) / PI                               // Lambert as written
E_sky    = irradiance(N)
E_ground = groundColour * (sunE * sinElev + irradiance(up)) * groundBounce   // uniform Lambertian plane
ambient  = E_sky + E_ground * saturate(0.5 - 0.5 * N.y)
lit      = diffuse * (sunE * ndl * vis + ambient * ao) + GGX * sunE * ndl * vis + ambSpec
```

No π factors on specular, no dials: a glTF material means what it means elsewhere. `E_ground` needs no explicit 1/π
(the one turning irradiance into bounced radiance and the π turning it back cancel). **Ambient is an irradiance, the
same units as `sunE`; `AtmScatter` returns radiance**, a factor π apart. That one units error, made three ways, produced
navy shadows (π too dark, sky-only), red shadows (bounce without 1/π) and black ones (both).

**Why physical won** (units audit, then multiple scattering). The established balance was diffuse `albedo·E`,
`sunDiffuse` 0.18, `skyAmbient` 1.0, specular ×π. Relative to physical it was internally consistent but put ambient
**5.5× too strong against the sun**. The model's own numbers (numpy port, cosine-weighted 24×48 quadrature, single
scattering):

| sun elevation | E_sun horiz. | E_sky true | old `π·L(N+up)` | model sun:sky | established sun:sky |
|---|---|---|---|---|---|
| 50° | 14.79 | 0.973 | 0.635 | 15.2 : 1 | 4.2 : 1 |
| 30° | 9.10 | 0.905 | 0.446 | 10.1 : 1 | 3.7 : 1 |
| 10° | 2.21 | 0.699 | 0.300 | 3.2 : 1 | 1.3 : 1 |

The dials were hiding two deficits, the one-sample sky estimate and single scattering (sky ~2× too dim), and then
overshot. Lab measurement (18 % grey upward ground, 45° sun, pixel probe):

| | sun : sky, flat ground | lit : shadow |
|---|---|---|
| established, single scattering | 4.2 : 1 | 4.95 : 1 |
| established + MS | 3.2 : 1 | 3.94 : 1 |
| physical, single scattering | 14.0 : 1 | 15.5 : 1 |
| **physical + MS (default)** | **10.6 : 1** | **11.8 : 1** |

MS adds 32 % sky light (E_sky 0.97 → 1.28). 10.6 : 1 is clean clear air; real clear skies run ~5–10 : 1, and
`turbidity` is the dial for a hazier day. The established balance looked overcast. Auto exposure absorbs the absolute
level change (physical measured 1.5× brighter in sun at the track-108 start).

**Still in the code:** `SkyParams::sunDiffuse`/`skyAmbient`, `gSunDiffuse`/`gSkyAmbient`, the lerp ambient and
`kSpecScale = physical ? 1 : π` remain as the established path, reached only when `gEnvReady` is false (no environment
map). The deletion planned after "one round of comparison" has not happened.

Other rules in the same shader:

- **Back faces flip the normal** (`SV_IsFrontFace`): everything is drawn `CullMode NONE`, and a back face's normal points
  away from the viewer, giving `N·L ≤ 0` on a sunlit side.
- **Foliage (ALPHA_TEST) is wrap diffuse 0.4 plus transmission 0.5 × `saturate(dot(-N, L))`, no specular lobe**: a
  backlit leaf is not unlit, and a card's normal is a shading fiction. A shaded plant loses 75 % of the sunlit-ground
  bounce (`kFoliageGroundShade`), since the ground around it is shaded too. Card normals themselves (welding, volume
  normals) are the geometry/foliage section's.
- `Scene::SetLightMode`: **0 = RBR's bake × `bakedScale`, kept as the A/B reference** (display-referred, so it gets the
  sRGB-only curve), 1 = dynamic (default). The old "dynamic + bake as AO" mode is gone; the legacy profile
  (`SetLegacyProfile`, `NGG_LEGACY_PROFILE`) does the bake-as-ambient-occlusion variant for imported .lbs cards only.
- **The camera constant buffer is grouped in whole float4s from `gSunDiffuse` on**, with a `static_assert` that it is
  a whole number of registers. HLSL will not let a float3 straddle a register and a packed C++ struct does; that
  mismatch once fed every later field the wrong value, and it showed up as a plausible-looking wrong picture.

### 3.6 HDR, exposure and tonemap

`PostFx` (`src/core/post.*`, `shaders/post.hlsl`, `shaders/meter.hlsl`). Sky and scene draw into
`PostFx::kHdrFormat` = **RGBA16F** at the render size; the only allowed clear colour is `kHdrClear` (it is the
resource's optimised clear value). The sky and scene shaders never apply exposure: when one did and the other did not,
the exposure control worked on the sky and not on the ground.

**Curves** (`PostFx::Curve`); the curve is a rule, set in `RecordFrame`, not a free choice:

| curve | used for | why |
|---|---|---|
| raw | any debug view | false colour, not radiance: a curve would change the numbers, TAA would blend them (TAA is off there too) |
| sRGB only | light mode 0 (bake) | RBR's vertex colour is display-referred; the A/B stays honest |
| `1−exp(−x)` | reference | what dynamic lighting first shipped with |
| **ACES** (Narkowicz) | **default; the module always sets it** | |
| AgX | option | a clipping channel loses saturation, not hue (ACES turns a blown sun disc yellow first) |

The encode is in `PSTonemap` (`LinearToSrgbEnc`), because the swap chain is `R8G8B8A8_UNORM`. Textures are created
`BC*_UNORM_SRGB` (or `_UNORM` when the data is linear) so samples decode to linear. **Verify the colour pipeline end to
end before tuning any light**: linear values written to a display buffer look exactly like "the ambient is too weak".
Before the curve, `NightVision` desaturates and shifts towards blue-green below ~8 EV under the day's grey (rods alone
from ~17 EV); that and the night's exposure are the night section's.

**Auto exposure, on by default (`RenderSettings::autoExposure`), metered from the image and adapted on the GPU**, two
compute passes, no CPU in the loop:

1. `CSHistogram`: 128 bins of log2 luminance over [2^-32, 2^8] (the floor went down from 2^-12 for moonlit and moonless
   nights), one sample per 2×2 block, **centre-weighted 4:1** aspect-corrected (the driver looks down the road; sky at
   the top, verge at the bottom corners). It meters what is about to be tonemapped: the TAA output, the upscaler's
   output, or the raw HDR.
2. `CSAdapt` (one group): mean log2 luminance between the **40th and 90th percentiles**, straddling bins counted
   fractionally. A histogram, not a log-average, because the sun disc and deep shadow dominate a mean. At night the
   metered level is kept no lower than 3 EV under the 97th percentile, so headlamp light in view sets a floor and does
   not clip. Adaptation is **exponential in log space** (an N EV step settles in the same time for any N),
   **asymmetric: 3/s towards bright, 1.2/s towards dark** (forest exit into sun). It clears the histogram.

`exposure = dial × 2^ev`, `ev = clamp(made-up(ref − adapted), −3, +24)`, where up to 3 EV darker than the reference is
made up in full and 0.75 of the rest beyond it, so night stays darker than day. **The dial keeps one meaning**: it is
the exposure at the reference brightness, `kReferenceLog2 = −2.025`, **measured** as what the track-108 start camera
(the view the 0.8 dial was tuned on) meters at. Switching auto on leaves that view as it was: measured 0.798 auto vs
0.800 manual. Step response with the sky turned off (frame 1.58 EV darker): 0.80 → 1.54 → 2.02 → 2.24 → 2.38 at
~30-frame intervals, settling at exactly +1.58 EV with no overshoot. A static camera meters within ±0.001 EV
frame to frame, so the jitter does not make the meter hunt.

Details that would be bugs the other way round:

- **Buffers decay to COMMON after every `ExecuteCommandLists`.** The histogram and the state buffer start each frame
  from COMMON, and `m_exposureStateNow` tracks the state buffer through the frame (TAA reads it, the meter writes it,
  the readback copy and the tonemap read it).
- `dt` is measured inside `PostFx` (QPC, clamped to 0.1 s), so no client passes a frame time and a breakpoint is not
  seconds of adaptation.
- Only the exposing curves meter. Otherwise, and on `ResetHistory` (load, `FrameDesc::camera.cut`), the next metered
  value is **snapped** to, not adapted from a different world.
- `LastExposure`/`LastMeteredEv`/`LastAdaptedEv` are read back one frame late for `RenderStats`, never fed back.
- A meter partly undoes a brightness edit (a brightened material that fills the view brings exposure down to meet
  it), so a material editor must turn `RenderSettings::autoExposure` off while judging brightness.

### 3.7 TAA, sharpening and upscalers

**TAA exists because the noisy techniques depend on it**: RT shadows and AO, PCSS, stochastic alpha are only
affordable with a temporal filter to accumulate them. `PSTaa` runs at the render size, writes the history (RGBA16F,
alpha = depth with identity flags) and a `settled` R8 companion, ping-ponged.

**Jitter**: Halton(2,3), 8 phases (an upscaler's own sequence and phase count when one runs), applied to scene and sky.
**It is set before the prepass** (`RecordFrame`): the main pass tests `GREATER_EQUAL` against the prepass's depth, and
with the jitter set after it, half the terrain failed the test and the sky flickered through.

**The jitter rule.** `Scene::LastViewProj` is reverse-Z **included**, jitter **excluded**; the reprojection
`inverse(VP) · prevVP` and the prepass's motion vectors are both unjittered. TAA accumulates into a fixed pixel grid;
reprojecting through the jitter makes the history follow the Halton pattern and the whole image shakes by up to a pixel.
Reprojecting an unjittered ray against jittered depth is wrong by at most half a pixel of parallax. (Corollary: anything
that reconstructs a surface point from depth must use the jittered matrix, `LastViewProjJittered`; the aerial cells
need neither.)

**Reprojection stays homogeneous**: `float4(ndc, depth, 1) · reproj`, divided after. A sky pixel (reverse-Z depth 0) is
a point at infinity; reconstructing a world position first would divide by w and give NaN. This way the sky
reprojects as pure camera rotation with no special case.

**Static world by the camera, moving objects by the prepass.** The world is rigid, so camera reprojection from depth is
exact there. Moving objects (an instance that has moved since placed) write motion in the prepass
(`Scene::MotionBuffer`, RGBA16F: xy NDC offset to last frame, z last frame's depth, w = 1); their glass, which depth
does not hold, writes its own (`GlassMotionBuffer`, RG16F). The motion vectors exist since moving cars arrived; the
"no motion vectors" assumption only holds for the static world.

The per-pixel decisions in `PSTaa`, each against a trail seen in practice:

| case | rule |
|---|---|
| neighbourhood | **variance clip**, mean ± 1.25σ of the 3×3, intersected with its min/max (a box alone lets one specular pixel widen it into ghosting) |
| history fetch | **Catmull-Rom, 5 bilinear taps** (corners dropped, renormalised), clamped at 0. Bilinear compounds a blur under every non-integer camera move at a 0.9 weight |
| blend | 0.9 history, **1/(1 + luma × exposure)** weights against fireflies, on last frame's exposure |
| standing still | from 1/4 px of motion down to none, the clip fades out and the weight goes to **0.97**, scaled by `settled` (0→1 over 16 still frames): fence flicker −71 %, car edges −65 % at a stationary view |
| disocclusion | decided **by identity, not depth** (history alpha negated on moving objects): a static pixel whose 4×4 history touches a moving object that has gone drops its history (a tyre and the road meet at the same depth) |
| moved pixel | keeps history only if a texel of its 2×2 is a moving object at its depth (±5 %); a **still occluder in front** in the 4×4 → only its own texels, bilinear (no fence bars printed on a passing car) |
| object standing still | within 0.1 px of the camera's reprojection it is treated as the world, so its outline keeps history |
| moving fast | above 8 px/frame the weight falls to 0.6 at 40 px/frame |
| moved shadow / AO / local-light shadow (RT distrust or the local-shadow mask) | never "still"; history **relit** by the ratio of this frame's 3×3 mean to its own local mean (0.25..4; for AO bounded by the AO's own change less 6 %). Dropping it instead cost 2.5× the grain |
| history with no history of its own | at most 0.25 weight (the contact-shadow line one frame behind the car) |

**Sharpen, after the tonemap, on display values**: an unsharp mask on the 4-neighbour cross, **clamped to the cross's
min/max**, so it can steepen an edge but never create a value that was not there: no halos to tune. In HDR the sun
edge would dominate it. Only on a TAA-resolved image. Amount by quality tier: low 0.25, medium/high 0.5.

Measured (track-108 start camera, mean |Laplacian| of luma, % of the aliased TAA-off frame; `--pan` turned the camera
a fixed amount per frame, the old viewer's instrument; dev iGPU):

| | TAA off | bilinear | Catmull-Rom | CR + sharpen 0.5 |
|---|---|---|---|---|
| panning 0.5°/frame (~10 px) | 4.97 | 1.63 (33 %) | 2.12 (43 %) | **2.55 (51 %)** |
| static | 7.96 | = CR | 4.20 (53 %) | **5.10 (64 %)** |

The TAA-off column includes aliasing energy, so it is a ceiling, not the target. Sharpen 0.5 raises static
frame-to-frame change 0.206 → 0.238 (out of 255) by re-amplifying jitter residue, which is why it is a dial.

**Upscalers replace `PSTaa`, nothing else** (`upscaler.h`: FSR 3.1 in `fsr.cpp`, DLSS SR in `dlss.cpp`;
`RenderSettings::upscaler`, `renderScale`). With one, everything up to the resolve runs at the render size and the
tonemap at the output size. `PSUpscaleIn` writes the inputs from the same reprojection `PSTaa` makes: motion as
(prev uv − uv) of the unjittered centre, and a reactive mask of 0.25 on moving glass. The upscaler gets the colour
before exposure plus the 1×1 exposure. Debug views and TAA off bypass both and show the render-size image stretched.


## 4. Shadows, ambient occlusion and ray tracing

All numbers in this section are GPU times on the dev machine's integrated AMD Radeon (780M) at 1080p unless a line
says otherwise.

### 4.1 The visibility provider and the two paths

**One renderer, a pluggable source of the sun's visibility and the AO - not two renderers.**
`modules/renderer/src/core/visibility.h` defines `VisibilityProvider`: it runs between the prepass and the main
pass, takes a `VisibilityFrame` (the prepass's depth, normal, motion and plant targets, the jittered and unjittered
view-projection, the camera, the sun's direction and angular radius, AO wanted / rays, whether a temporal filter
follows, the lights) and fills a `Visibility` (sun visibility or "sample the cascades", an AO texture for the AO slot,
the local lights' shadowed fraction, the denoisers' outputs TAA reads, `aoSource` for the readout). What the main
pass, TAA and the upscalers do with it does not depend on which provider filled it.

| provider | sun | AO | local lights |
|---|---|---|---|
| `RtVisibility` | RT, denoised (`Rt::Filtered`) | RT (`AoSource::Traced`, default) or XeGTAO (`AoSource::ScreenSpace`) | RT, ratio estimator |
| `CascadeVisibility` | cascaded maps, sampled in the main pass | XeGTAO | spot/beam shadow maps (PCSS) |

`Renderer::RecordFrame` (`renderer.cpp`) picks RT when `m_preferRt && prepass && m_rt.HasScene() && m_rtValid &&
ShadowsOn()`; otherwise the cascades. `Renderer::RtFallbackReason` says why not ("cascades selected", "no ray tracing
on this adapter", "shadows off", "the acceleration structure failed to build"). The game's setting is `ray_tracing`
(`RenderSettings::rayTracing`); `kRendererNoRayTracing` at creation skips `Rt::Create` altogether. A third provider
(NRD SIGMA/REBLUR, DLSS Ray Reconstruction) is meant to fill the same struct.

**The depth + normal + motion prepass runs on both paths** (`Scene::RecordPrepass`): the main pass shades every pixel
once (`GREATER_EQUAL` against the prepass depth), both AOs read its depth and normals, TAA and the upscalers its
motion. The prepass normal is the GEOMETRIC normal (`R8G8B8A8_SNORM`, world space, no normal map - a ray origin
wants the surface, not the paint); `w` marks a plant's card, whose normal is then the card's plane.

**Rules the code relies on:**
- **TAA reprojects into a stable grid and must not see the jitter; anything reconstructing a surface from depth
  must.** Ray origins come from `Scene::LastViewProjJittered`; the denoisers' reprojection from `Scene::LastViewProj`.
  With the unjittered matrix the origin was half a pixel of world space off and the shadowed fraction cycled
  33-69 % on a still frame.
- **Every pass that rasterises a frame uses the same jitter, set before the first of them** (`RecordFrame` sets it
  before `RecordPrepass`); otherwise `GREATER_EQUAL` fails and the sky shows through, flickering with the sequence.
- The RT outputs are traced and read **in the same frame** (no lag).
- Glass (`kMaterialBlend`) is not in the ray-traced scene: it neither shadows nor occludes, and takes the sun whole.
  Far scenery (`kLayerFarScenery`) is neither traced nor shadowed.
- Quality tiers (`Renderer::Tier`): low = RT sun only, no AO; medium = AO, 1 RT ray, XeGTAO medium; high = AO,
  2 RT rays, XeGTAO high.

### 4.2 Acceleration structures

Inline `RayQuery` in compute shaders (`cs_6_5`), no DXR pipeline or shader tables; needs raytracing tier 1.1 and
SM 6.5 (`Rt::Create` logs the adapter's and declines otherwise). `modules/renderer/src/core/rt.*`.

**The BVH is organised like the draw list: one BLAS per mesh, one TLAS instance per placement.**
`Scene::DescribeRt` lists mesh keys and instances:

| key | BLAS | instances |
|---|---|---|
| `kRtWorld` | every static object at its authored position, in `InRtWorld`; key = FNV over the load and the objects it LEAVES OUT | one, identity |
| `kRtMoved \| i` | a static object that moved, was hidden/shown, or is keyed (NGGK, the physics moves it), at its authored position | one, its transform |
| `kRtDynamic \| mesh` | a dynamic-pool mesh (the car's parts, editor meshes, a model's ray-traced LOD submesh) | one per use; material per instance, `FORCE_OPAQUE` when drawn opaque |

`Rt` caches BLASes by key (`std::map`). `Renderer::KeepRtCurrent` runs when `Scene::GeometryRevision` changes, when a
batch of BLASes has landed, or the frame after one did. **Two speeds: a change in which meshes exist builds BLASes
(load, first move of a non-keyed object, hide/show, added car, prop or model); a change in where things are - a
driving car, a drag - is TLAS-only.** The TLAS (`Rt::UpdateInstances`, `PREFER_FAST_BUILD`, with the instance table)
is built on the **secondary device context** (`m_buildDev`, blocking: the frame's list is open). `Rt::Rebuild` drops
dead BLASes and rewrites the shared any-hit buffers and the descriptor heap, then the TLAS.

**New BLASes never stall a frame.** They are built off the frame and join the TLAS when done:

1. **Queue** (`Rt::SubmitBlases`, the frame a mesh appears in): `Scene::BuildRtMesh` copies each missing key's
   indices out of the scene (1-5 ms for a car or a stage), and the batch is queued. Nothing else - that frame
   allocates plenty of its own (the car's materials, the scene's heap), and a thread allocating beside it made it
   35 ms longer.
2. **Record and submit** (`Rt::StartBatches` after the frame is recorded, then `RecordBatch` on the batch's own
   thread): one index upload, one 8 MB scratch reused past UAV barriers, 4 MB result arenas (a BLAS of 4 MB or more
   gets its own buffer, so a replaced world never pins a car's parts), the builds (`PREFER_FAST_TRACE`) on **the
   ray tracer's own COMPUTE queue**, then a fence. The thread does not log (the sink is the main thread's).
3. **Land** (`Rt::Land`, step 5 of a later frame): a finished batch's BLASes go into the cache and the TLAS is
   published with their instances. Until then they are **simply not in the TLAS**: no RT shadow, AO or reflection
   rays from them. On a load that means no TLAS at all: the cascades draw those frames
   (`rtFallback` "the acceleration structure is being built").

Why each step is shaped so (measured, dev iGPU 780M):
- **One committed buffer per BLAS** (BLAS, scratch, index upload) cost ~1.2 ms each to create and as much again to
  free: a stage's 922 BLASes took 1.15 s to record and 0.38 s to release - the old first stage frame was 1.68 s.
  Sub-allocated, the same batch records in 40-50 ms, on its thread.
- **Small allocations on purpose**: the driver creates a buffer under a lock that the frame's own allocations (the
  TLAS's instance upload, every frame the car moves) wait on. With 32 MB arenas the frame after a new car waited
  143 ms for the batch's thread; at 4 MB each creation holds it about a millisecond.
- The GPU part is small: the i20's 71 BLASes (202k triangles, 31 MB) 8 ms, a stage's 922 (439k, 69 MB) 39 ms.
- **The renderer's RT targets** (outputs and histories, 11 textures at the render size) are made with the renderer
  and on a resize (`Rt::PrepareTargets`), not by the first frame that traces: 95 ms at 1080p, most of the frame a
  stage's first BVH landed in. A new descriptor heap only rewrites their views (`WriteTargetViews`) - recreating
  them dropped every history on the screen whenever a mesh appeared.
- The any-hit buffers are rewritten in place when big enough (made with a third to spare), and a pool upload for a
  mesh still building only moves the dynamic-vertex view (`FollowDynamicPool`), not the heap.
- **A superseded world is kept** while its successor builds (a hide or first move rebuilds the world's BLAS): its
  identity instance stays, or the whole stage would leave the rays for those frames. A new scene empties the cache
  first (`Rt::ForgetScene` in `Renderer::Load`), so an old stage is never kept.
- **History when it lands**: a landed mesh's instances go into that frame's instance table as occluders that were
  10 km away (`kRtAppearedFrom`), so `Allow` gives the pixels whose rays hit them no history (4.5) - the new shadow
  and AO come in at once instead of fading in over the old "unoccluded"; the next frame's table is the ordinary
  one (`InstancesStale`). And whenever the RT path turns on again (the first BVH of a load, `K`, shadows on), its
  histories are reset: they are from whenever it last ran.

Measured (dev iGPU, 1080p, whole-frame wall time / GPU `frameMs` of the frame before; machine noise +-10 ms):

| event | before | after |
|---|---|---|
| Garage, i20 R5 put on (71 BLASes) | 218-230 ms (record 176: BLAS 92 CPU + 8 GPU, release and the rest 38) | 59-71 ms; with RT off the same frame is 54-57 |
| Garage, Fabia RS Rally2 after the i20 (92 BLASes) | 328 ms | 98-124; RT off 104-105 |
| the frame the car's BLASes land (1-2 frames later) | - | publish 7-12 ms CPU (any-hit buffers, heap, TLAS) |
| stage 41 load, first frames (922 + 2 BLASes) | 1676, 103, 51 ms | 87, 42, 74 (the world lands: publish 13-18 ms), 53, 50; RT off 97, 112, 88 |

- **Keyed objects get their own BLAS from the load** (`InRtWorld` excludes them): a car going through a row of
  barriers used to rebuild the 92 MB world BLAS once per barrier - worst frame 292 ms -> 89 ms.
- Models (`.ngg` NGGD/NGGE): a BLAS per submesh of the model's `rtLod`, a TLAS instance per placement and submesh,
  cached until a placement edit (`Scene::DescribeModelsRt`). Replaced plant cards are degenerate and left out.
- Every instance carries its last-frame transform and `moved` = travelled more than `kRtMovedMin` (2 mm) this frame
  (`Travel` over the box corners) - a car idling is re-posed every tick by a hundredth of a millimetre, and treating
  that as motion cut the AO history around it.
- Instance masks: `kRtMaskAll` on all, `kRtMaskFocus` on the reflection focus (the car) - its reflection rays
  (`ReflOpen`, scene pass) trace only the car. So the TLAS is kept current **whenever the adapter ray traces**, on
  the cascades path too (`Scene::SetReflectionTlas`).
- An unchanged reflection focus is not a scene revision (an idle editor refitted its TLAS every frame).
- Scratch buffers are created in `COMMON`; asking for UAV earned a validation warning per rebuild. The world BLAS is
  built from `Scene::m_staticIndices` (CPU copy), never read back from an upload heap.

**Foliage: the any-hit test is the rasteriser's own.** A BLAS has two geometries: opaque (`GEOMETRY_FLAG_OPAQUE`)
and alpha-tested (no flag, every hit is a candidate). `Occluded` (`shaders/rt_common.hlsli`) looks the triangle up
through `gInst[CandidateInstanceID()]` (alpha-triangle base, vertex base, static or dynamic vertex buffer, material),
rebuilds uv0 and vertex alpha from the barycentrics and tests texture alpha x vertex alpha against the material's
cutoff - so a leaf blocks exactly the pixels it was drawn with. Texture alpha at a **fixed LOD 2** (a ray has no
footprint; mip 0 gives a shadow sharper than the leaf on screen). On top:
- **A plant's rays skip its own plant** (by id, `gTriPlant` vs the prepass plant id): crossed cards of one tree
  shadowed each other card by card. Other plants and everything opaque still count; the plant's own volume shading
  is the pixel shader's (`FoliageSelfShadow`).
- **Plant kind decides what it casts** (packed in uv1.y): grass casts no sun shadow and no AO, a bush half its hits
  (lighter, softer after the denoise), trees in full.
- **Legacy cards** (an import's remaining alpha cards) and **any alpha card that is not a plant** take the card's
  plane towards the camera as normal and skip cards within `kLegacyCardSkip` (1.5 m) - RBR's 75 m forest walls shadowed
  themselves along a straight line.
- The leaf pass/fail counters (`Rt::LeafHits/LeafMisses`) stay: "cutting leaves out" and "rejecting everything" look
  the same from a distance.

### 4.3 RT sun shadows

`shaders/rt_trace.hlsl`, one ray per pixel to a point on the sun's disc, `tMax` 4000 m.
- **Surface**: `SurfaceAt` - depth through the inverted jittered view-projection, the prepass normal. Shared by
  every ray type, so their origins never differ.
- **Origin**: normal offset, the normal flipped towards the sun (a two-sided card is pushed to the side the ray
  leaves), `0.02 m x (1 + 0.002 x distance)` - depth precision falls with distance. The old 5 cm step along the ray
  skipped exactly the occluders within 5 cm, which is where grass shades the ground (shadowed pixels 1.3 % -> 5.1 %).
- **Soft shadows**: the sun is a disc of angular radius `0.00465 x sunDiscSize` - the same `sunDiscSize` the sky draws,
  so the visible disc and the penumbra agree by construction. Uniform over the disc (`sqrt` on the radius).
- **Stratified over 4x4 pixel tiles** (`StratDisc`; the lamps' pick `StratU`), the pattern shifted every frame: with
  white noise a 5x5 window drew its samples unevenly and the denoiser kept that luck as blotches.
- The raw result is `R16_FLOAT`; `EncodeRaw` stores a hit on a moved object as a negative value carrying the history
  weight its rays allow (4.5).

### 4.4 RT ambient occlusion

`shaders/rt_ao.hlsl`: **cosine-weighted** hemisphere rays (the plain hit/miss mean IS the occluded fraction of the
diffuse ambient), radius 3 m, the same surface, origin and any-hit test. It scales the **ambient terms only** (sky,
ground bounce, ambient specular), never the sun. **The radius is where occlusion stops and GI starts**: a far wall
blocks sky but also reflects light nobody simulates, so beyond 3 m the answer is "unoccluded".
- **One ray per pixel** by default (tier medium; `NGG_AO_RAYS`), the denoiser accumulates. Measured (plugin viewer,
  lab forest): RT 7.87 -> 4.28 ms, mean AO identical, +17 % frame-to-frame noise at rest.
- Stratified over 4x4 tiles (`AoSample`), n rays split their cell along the azimuth; the denoise window spans exactly
  one period (outer ring weighted 0.5), so on a flat surface its mean is a 16-sample stratified estimate.
- A plant's card looks **up**, not along its plane: half its hemisphere was ground, which the ambient already lights
  from below; grass came out ~7 % darker than without AO.
- AO is traced only when applied; a frame without it drops the AO history.
- **Under an upscaler at <= half the output's pixels the AO traces twice the tier's rays** (`Renderer::AoRaysTraced`),
  so its grain is not blown up into blotches 1.5-2x the size.

**The wide pass** (`shaders/rt_ao_wide.hlsl`, AO and local lights): where the history was cut (moved occluder,
disocclusion, a slow occluder that did not allow it all, a moved object's young history) the denoised result is
averaged again over 13x13 (5x5 taps 3 px apart, the same depth and identity rules). Spatial only, so it cannot trail;
not fed back into the history.

### 4.5 Denoising and history rules

`shaders/rt_denoise.hlsl`, one shader for the sun, the AO and the local lights (`Rt::Denoise`); histories are RGBA16F
pairs: x visibility, y NDC depth, z distrust, w identity / allowed weight / youth. **The visibility is filtered before
the lighting sees it**: the colour TAA's variance clip exists to reject history that disagrees with the neighbourhood,
which is what a converging penumbra looks like, so it never converged.

1. **Spatial**: depth-aware 5x5 mean (relative linear depth within 3 % of what the pixel's **plane** predicts -
   `Slope`; a flat test rejected most of a grazing road), and **identity-aware**: a moved object's pixels and static
   ones never mix (the road under a bumper is within 3 % of it). Loaded once per group into a 12x12 shared window.
2. **Temporal**: reproject by the unjittered camera, or by the prepass motion for a moved object's pixel; accept
   only the same surface (3 % + half a pixel's slope) and the same identity.
3. **Neighbourhood clamp**: the history may not leave this frame's mean +- (sigma + 0.5/sqrt(n)).

| weight | sun / lights | AO |
|---|---|---|
| history (`blend`) | 0.85 | 0.85 (0.92 lagged once the car drove) |
| pixel still on screen (< 1/4 px, `blendStill`) | 0.95 | 0.95 |
| a ray hit a moved object (`blendMoved`, floor) | 0.25 | 0 |

**History rejection - what the past may not carry:**
- **Moved occluders**: a ray that hits a moved instance says how much history it allows (`Allow`): the hit point's own
  travel this frame against the softness of the shadow there (hit distance x the light's angular size; for AO x
  `kAoPenumbra` 0.15), keeping the lag within `kLagShare` 0.3 of the penumbra. A driving car allows nothing (no trail);
  the Garage's turntable car, millimetres a frame, keeps most (no grain round it). The distrust fades over four
  frames (1 then -1/8 a frame), and TAA reads it.
- **No ghost behind the car (AO)**: AO's `blendMoved` 0 and `kAoPenumbra` 0.15 (was the whole distance) - the road
  behind a driving car kept the car's occlusion where it had been; bias against no-history 18 km/h -10.29 -> -0.39
  grey.
- **The road a car has left takes no history**: a moving object counts as "standing" (its pixels may swap with the
  world's at a jittered outline) only if one of its pixels is in the 5x5 and none moves against the world. Where none
  is there any more, the object has gone, and what it uncovered starts over (golden replay, TV camera: copies of the
  car on the shaded road, blurred error 5.04 -> 2.40).
- Ground uncovered this frame (last frame's pixel was a moved object's) counts as touched, allow 0.
- **A mesh whose BLAS just landed** (4.2: a car, a prop, a stage's world) is an occluder that was 10 km away last
  frame, for one frame: its hits allow nothing, so its shadow and AO are in at once, not faded in over "unoccluded"
  (Garage shots: the Fabia's contact shadow complete in its second frame, no trail). The RT path turning on again
  resets all three histories.
- **A moved object's own pixels keep their history** (reprojected by their motion; their occluder is mostly
  themselves). Its young history (just out from behind a panel) is a running mean of its frames, youth counted in
  32nds in w, and widened while young.
- **Camera cuts**: `FrameDesc::camera.cut` -> `Renderer::ResetHistory` -> `Rt::ResetHistory` (all three histories),
  XeGTAO's sequence, TAA and exposure. The stage flags a cut on another camera mode, another TV camera, or an eye 10 m
  from the last frame's. Without it a still camera kept the old view for its long still history (car side +1 frame
  39.1/61.8 -> 0.7/13.5, x1e-3).
- `NGG_RT_NO_HISTORY=1`: no history and no wide pass - grainy but unbiased, the reference `tools/car-ghost`
  (`check-car-ghost.sh`, `ghost.py`) measures ghosts against (fails a mean bias over 1 grey).

**Pitfall:** the denoiser ping-pongs its histories, so `Rt::Filtered()` changes pointer every frame; a "ready" flag
reset on a new resource turned RT shadows off silently. The flag now gates only the descriptor write.

### 4.6 Cascaded shadow maps

The non-RT path's sun (`modules/renderer/src/core/shadow.*`, `Scene::RenderShadows`), drawn only by
`CascadeVisibility`. **Not drawn at all on the RT path** - four full-scene passes were the most expensive thing in
the frame. Four cascades at 2048^2 (`R32` typeless, conventional depth), split 1-1500 m with a log/uniform blend
(lambda 0.85), depth-only through the scene's own `VSMain`.

| choice | why |
|---|---|
| bounding-sphere fit per cascade, radius rounded up | rotation-invariant: the projection does not breathe as the camera turns |
| texel snapping of the light-space origin | sub-texel camera motion otherwise moves edges (shimmer that reads as a filtering bug) |
| normal-offset bias by the texel's world size, x(1 + 2 slope), x3 for alpha-tested receivers | scale-correct per cascade; depth bias trades acne for peter-panning |
| 3x3 hardware PCF (`SampleCmpLevelZero`) | 36 samples' worth for 9 fetches |

Casters culled per cascade volume; glass and far scenery cast nothing; alpha-tested casters through the shadow pixel
shader, two-sided; models instanced, one LOD, billboards turned to the sun. A sun below the horizon skips the pass.
**No penumbra** (no PCSS on the sun), no cross-fade between cascades; edge softness varies with cascade texel size
(~4 cm to ~2 m).

**Pitfall: an unwritten D3D12 descriptor reads as zero, silently.** A shadow SRV never written made every
`SampleCmp` fail - the whole world shadowed. Stored depth 1.0 = empty map, 0.0 = descriptor never written, ~receiver
depth = bias too small. Empty material slots are bound to a white fallback for the same reason.

### 4.7 XeGTAO

`modules/renderer/src/core/gtao.*`, `shaders/gtao.hlsl`: Intel's XeGTAO (`third_party/xegtao`, MIT, unchanged) at the
render size - the AO wherever the renderer runs (needs only the prepass), and the cheap AO a ray-traced frame can take
(`NGG_AO_SOURCE=screen`). It writes the same AO slot (`Scene::SetRtAo`, R8), ambient terms only.
- FP32 view-space depth MIPs (5): a stage's depth runs to kilometres, where FP16 has half a metre.
- Reverse-Z and jittered depth: the constants come from the reverse-Z projection and the reconstruction is shifted
  by the jitter.
- **Normals from the depth by default** (`NGG_GTAO_NORMALS=prepass|depth`): the prepass's smooth shading normals put
  dark bands along the terrain's triangle edges.
- Radius 1.5 m (x1.457 inside XeGTAO: ~2.2 m; 2.5 blotched near leaf cards, 1.0 hardly reached the car), thin-occluder
  compensation 0.5, three edge-aware denoise passes (fence bars before a passing door: grain 5.22 -> 2.47; RT 2.99).
- **No temporal accumulation of its own**: the noise walks with the frame (64-frame sequence) when TAA/FSR/DLSS
  follows, and stands still otherwise. A cut restarts the sequence.
- Quality by tier (medium 2 slices x 2 steps, high 3 x 3; `NGG_GTAO_QUALITY=0..3` overrides); each quality's main
  pass is compiled when first used. Other switches: `NGG_GTAO_RADIUS`, `NGG_GTAO_THIN`, `NGG_GTAO_DENOISE=1..3`.

**AO selection**: `AoSource` on `RtVisibility` only (`Renderer::SetAoSource`); the cascades always take XeGTAO. The
readout (`RenderStats::aoSource`, interface 14) says "ray traced, 1 ray" or "XeGTAO medium"; `aoMs` is the chosen
one's time.

### 4.8 Local-light shadows

The lights themselves (clusters, beams, units) are described with the lighting; this is how they are shadowed.

**With RT: every shadowed light, one ray per pixel however many lights** - the ratio estimator (Heitz et al. 2018,
`shaders/rt_lights.hlsl`). The scene pass sums every light's unshadowed light; the tracer picks one light of the
pixel's cluster in proportion to what it gives there (stratified `StratU`), traces one ray to a point on its emitter,
and the denoised 0/1 is the shadowed fraction of the sum. Denoised as the sun's, widened as the AO's (debug view 15).
`NGG_LIGHTS_REF=1|2`: a ray to every lamp, or 16 picks averaged - the references.

**Without RT: spot/beam shadow maps with PCSS** (`Scene::RenderLocalShadows`, `CascadeVisibility` only).
`Lights::Build` scores the shadowed spots and beams by peak / camera distance^2; lamps of one mounting share a map;
at most `kShadowSlots` 4 perspective maps at 1024^2 (near 0.05 m, far <= 150 m, half-angle <= 70 deg); point lights
stay unshadowed. `LocalShadow` (`scene.hlsl`): blocker search of 4 gathers, penumbra from the emitter's radius, 12
rotated comparison taps (one tap when the penumbra is under 1.5 texels); one lookup per mounting per pixel.
- A **moved-shadow mask** for TAA (the casters' kind beside each map; RGBA16F, four frames) stands in for the RT
  lights' distrust: without it the car's spotlit shadow left copies on the turntable.
- **XeGTAO takes a share of the local lights' diffuse at night** (0.5, faded in as the sun sets, none by day): at night
  the ambient it scales is almost nothing, and the car floated on the turntable. `NGG_LOCAL_SHADOWS=0`,
  `NGG_LOCAL_AO=<share>`, `NGG_LOCAL_SHADOW_TURN=0` for A/B.

### 4.9 Costs measured

**Why RT is the default path** (plugin-era viewer, `track-108`, earlier hardware state; the shape still holds):

| | |
|---|---|
| one sun ray per pixel | ~2 ns per ray, linear: 0.47 ms / 194k px, 1.8-2.0 ms / 967k px |
| frame: cascades, no prepass -> prepass + RT shadows | 53 -> 34 ms |
| track start view: RT shadow / RT shadow + 2-ray AO / cascades | 0.86 / 2.25 / **2.93** ms - on real geometry the four cascade passes cost more than the rays |
| lab forest: shadow without -> with foliage any-hit | 1.29 -> 1.89 ms (+46 %): any-hit is not the problem |
| AO share of the ray tracing (2 rays) | 75-80 % - hence one ray |
| world BLAS, `track-108` (594k tris) | 92 MB, built in 65-94 ms |

**W-Rally, 1080p:**

| | ms |
|---|---|
| RT AO (1 ray) + denoise / XeGTAO medium (/ under FSR Quality) | 5.9 + denoise / 2.2 (1.3) |
| stage 41 ray tracing, without -> with model trees (LOD1 traced; opaque trees 11.0) | 8.9 -> 12.6 (the alpha tests, not the triangles) |
| three local lights over the screen: light rays + denoise | 3.2 + 0.8 |
| local shadow maps, cascades path: Garage 12.7 -> 14.4 frame (maps 0.37); night stage, 3 maps | ~0.9 |
| keyed objects' own BLAS: worst frame driving through barriers | 292 -> 89 |
| `RenderStats` | `rtMs` (all traces), `aoMs`, `denoiseMs`; per-pass stamps in `Rt::ReadStats` |


## 5. Weather, surfaces and night

### 5.1 Weather presets and what the renderer does with them

**A preset is ours and physical; RBR's `TrackSettings.ini` is only an import source.** Its numbers are art knobs for a
2004 shader, not physics (measured over its ~760 sections: `Turbidity` −2.8…9 with a median of 0, the Mie phase
`Greenstein_Value` 1.0 - a delta function - in most, track-108's clear sky at `Rayleigh_Multiplier` 0.025), so they
are not copied. What does translate is the **condition in the section name** (`<map><variant>_<haze>_<cloud>`, e.g.
`41O_M_lightrain_heavycloud`), the fog distances (visibilities), `SunDir` and the terrain reflectance.

**Format** (`modules/renderer/src/core/weather.*`, `WeatherPreset`): sun (direction the light travels, intensity),
turbidity, ground albedo and colour, cloud cover 0..1, fog visibility (Koschmieder, metres: contrast 2 % there; 0 = no
fog) and fog layer height, rain and snow 0..1, exposure, optional time and place (5.4), `source` provenance. Two forms:
versioned `ngg-weather` JSON (`WeatherToJson` / `WeatherFromJson`, for projects and the editor; unknown keys ignored,
missing ones keep defaults) and binary records in the `.ngg`'s **NGGW** chunk, which is all the runtime reads
(`ParseWeatherChunk`, `Scene` at load). Presets live inside the file - covered by its CRC, part of an event's
definition; a loose file beside the game would let a competitor thin the fog. The first record is the default.

**NGGW grows by appending** (`format/ngg_format.h`): `version` stays 1, `WeatherRecord` is 172 bytes (124 before the
time and place, 160 before the wind); a reader takes `min(recordSize, sizeof)` bytes over `DefaultWeatherRecord()`, so an older record reads
as "no time set". `static_assert`s tie the format default to `SkyParams` and `SkyTime` defaults.

**Import** (`game/src/assets/legacy_weather.*` over `game/src/formats/track_settings.*`): the `.lbs` importer
(`assets/legacy_stage.cpp`) writes every section of the stage's map into the imported scene, the default first (the
stage's own time of day, `crisp_clear` if present). The translation, made once:

| name word | becomes |
|---|---|
| `hazy` | turbidity 3 |
| `partcloud` / `lightcloud` (and misspelt `ligthcloud`) / `heavycloud` | cloud cover 0.35 / 0.6 / 0.95 |
| `lightfog` / `heavyfog` | visibility = `FogEnd` when `UseFog` (else 1500 / 300 m), 150 m layer, turbidity 2 |
| `lightrain` / `heavyrain` | rain 0.4 / 1, turbidity 3 / 4, visibility 6000 / 2500 m under a 600 m layer |
| `lightsnow` / `heavysnow` | snow 0.4 / 1, turbidity 3 / 4, visibility 3000 / 800 m under a 600 m layer |

Repairs RBR's file needs: decimal commas in `SunDir` (`0,64`) read as points; a sun at or within ~3° of the horizon
(48 presets, overcast mostly - RBR's hand-set ambient made it irrelevant) is raised to ≥ ~10° on the same bearing, or
a physical sky draws night. **Every RBR stage is daytime: M morning, N NOON (suns ~50° up), E evening** - `N` is not
night. Map numbers are zero-padded (`04M_...`, `formats::TrackSettingsMap`).

**Selection.** The preset's **name is its identity**: the game asks `findWeatherPreset(name)` and sends
`FrameDesc::weatherPreset` (index; 0 the default) or `-1` with an explicit `FrameDesc::weather` (`WeatherDesc`, 60
bytes). A name the file lacks falls back to the default, logged by the game. Quick rally picks a stage file by the
preset's time of day (`assets::Stage::TimeOf`) and may override the preset's sky (`--sky clouds,fog,rain`: fog 0..1
maps to 6 km..150 m) and the road (`--road`). So a preset the game should select must carry the section-style name of
the condition it stands for; free names work only by explicit `--weather`.

**Rendering, all in the one atmosphere** (`shaders/atmosphere.hlsli`, `AtmSetWeather`), so sky, environment,
irradiance and the aerial table agree:
- **Fog**: an exponential height layer on the camera's ground, composited **analytically** (closed-form optical depth
  - the view march's kilometre steps would step over a 150 m layer), lit by the sun into it (Mie phase) and a diffuse
  field (half sky, half ground bounce); the direct sun at a surface inside it is attenuated through the layer.
- **Clouds**: a uniform deck. Direct sun × `1 − 0.98·smoothstep(0.3, 1, cover)` (`AtmCloudSun`); the sky turns toward a
  CIE overcast dome at 30 % of the clear global irradiance; the air is lit by the deck's diffuse light. Clear-sky
  diffuse uses the fit `0.064 · I · sin(elev)^¼` (within ~5 % from 10° to 50°).
- A clear, fog-free preset leaves the weather path untouched.
- `rain` / `snow` are carried but draw nothing themselves (no particles); wetness is separate (5.2).

Pitfall: HLSL starts a `float2` on a new register rather than straddle one - the sky's constant buffer once read its
jitter and weather one slot off. A CB struct mirrored in C++ needs explicit pads and `static_assert`ed offsets.

### 5.2 Wet surfaces and puddles

**The wetness shown is the road's, not the sky's**: rain that has stopped still leaves the road wet. The game sends
its road state as `FrameDesc::wetness` (dry 0, damp 0.5, wet 1; `game/src/app/game.cpp` from the physics' wetness,
which also picks an import's wet textures); the shader reads it as `gWetness`. The weather's `rain` does not set it.

**Shading** (`shaders/scene.hlsl`, opaque non-alpha-tested, non-glass variants only; after Lagarde, "Water drop 2b"):

| term | rule |
|---|---|
| exposure to rain | `(0.5 + 0.5·N.y)` (up full, walls half) × `lerp(0.35, 1, AO)` (sheltered stays drier) |
| porosity | from roughness, `saturate((r − 0.3)/0.5)`; albedo × 0.85 (smooth) .. 0.45 (porous) when wet |
| gloss | roughness capped at 0.25 when fully wet; the environment's split sum then gives the sky's sheen down the road |
| grass | green-dominant albedo: no puddles, wet roughness only down to 0.6 (else lawns became blue-grey mirrors) |
| puddles | ground layer only, wetness > 0.5: world-fixed two-octave value noise (~3 m and ~1 m cells), threshold 0.74 (damp) → 0.62 (wet), flat ground only (N.y > 0.93); albedo × 0.35, roughness 0.02, normal turned level |

RBR's materials carry no grass tag; the albedo colour test stands in until materials are tagged (plants come from
Wallaby's tags, not heuristics - this is a surface heuristic, kept until the format has a surface class).

### 5.3 Glass and blended materials

**`kMaterialBlend` is drawn blended, in its own pass after everything opaque** (`Scene`, `m_psoBlend`): sorted far to
near by box centre, depth tested (reverse-Z), no depth write, over the sky too. Format bit: `kMaterialBlend = 1 << 3`
in `.ngg` (bit 2 stays unused - it was v2's single-stage, and a reused bit is a misread waiting to happen); the
interface's `iface::kMaterialBlend` is bit 2 and the module maps it. Blend wins over the alpha test.

- **Premultiplied** (ONE, INV_SRC_ALPHA). The `BLEND` shader variant scales the tint's light (diffuse, baked colour,
  local lights' diffuse, aerial in-scatter) by coverage = base alpha × vertex alpha × factor; the **reflection stays
  whole** (sun GGX, environment / probe). A clear pane reflects as much sky as a tinted one. glTF's `BLEND` scales
  everything and reads as a matt film.
- **No screen buffers of its own**: RT sun visibility, AO and the local lights' shadowed fraction are the opaque
  surface's behind the pane, so glass takes AO 1, the sun unshadowed under RT (the cascades it looks up as anything
  does) and local-light visibility 1.
- **Casts no shadow, not in the BVH** (cascades, local shadow maps, `DescribeRt` skip it): the sun reaches the seats.
- **Not in the depth prepass**, so the ray tracer and TAA see what is behind it. A moving object's glass writes only
  its own motion in the prepass (`PSGlassMotion` into `kGlassMotionFormat`), so TAA never treats moving glass as still
  (no headlamp-lens smear).
- **The car** (`game/src/assets/car_render.cpp`): only real glass is glass - shader class `CarGlass` / `CarIntGlass` /
  `CarLight` or a mesh named for it (`e_lightglass_*`, `e_window_*`) goes in as `kMaterialBlend | kMaterialTwoSided`,
  roughness 0.05; other transparent textures (grilles, vents) are alpha-tested holes. Rules in
  [car-pbr.md](car-pbr.md).

Measured on the i20 (stage 41, chase): the rear window went from 0–5/255 (black cabin through a hole) to ~85 - the
sky's reflection, ~10 % at that angle, as Fresnel gives. Physically right; lighter than RBR's dark windows.

### 5.4 Time of day: date + time + place

**Night is W-Rally's own** (RBR has none) **and any stage can be driven at any hour**: a date, a local time and the
stage's latitude/longitude give the sun, the moon, its phase and the stars. Decided 2026-10-30 with three more (5.5).

- **Astronomy** (`core/astronomy.*`, Meeus): the sun's low-accuracy method, the moon's main terms, its lit fraction,
  sidereal time; dynamical time = UT, no refraction. World axes x east, y up, z north, turned by `northDeg`. CTest
  `astronomy-check` against Meeus's worked examples (sun 0.00001°, moon longitude 0.0007°, distance 29 km, lit
  fraction 0.002) and the solstice noon sun at 50 N (63.44°, due south, 13:04 CEST).
- **Interface**: `FrameDesc::skyTime` (`SkyTimeDesc`, `set` = 1) puts the sun there over any weather; a preset may carry
  its own time (`WeatherPreset::timeSet`, JSON `"time"` / `"place"`). `RenderStats::sunElevationDeg` reports the
  frame's sun back (the game's lamps switch by it). Game: `--time HH:MM [--date] [--utc-offset] [--latitude]
  [--longitude] [--north]`; defaults 21 June, UTC+1, 50 N 14.4 E, north 0. Quick rally: `quick_time` on a season's
  date (`app::QuickDate`; autumn 2026-10-15, UTC+2: dusk ~18:30). **No stage carries its own place yet**: every stage
  uses the default latitude and north 0.
- **The light passes to the moon at the end of civil twilight** (sun −6°, `ApplySkyTime`): the one directional light -
  direct light, its RT shadows, the aerial table - becomes the moon's, at a full moon's 2.3e-6 of the sun's
  illuminance × its phase brightness (Krisciunas-Schaefer, 0.026a + 4e-9a⁴ mag over phase angle a). The direct sun is
  long gone by −6°, so the ground shows no handover.
- **The sky is both bodies'** (`SkyParams::light2`): the moon's sky through the dusk, the sun's twilight into the night,
  only while it shows. The first handover at −11° jumped (a full moon's sky outshines the twilight there); either side
  of −6° the picture is now the same.
- **Moon disc** (0.52°, by day too): a sphere lit by the real sun, so the phase draws itself; albedo 0.12, earthshine,
  atmospheric extinction.
- **Stars** (`shaders/sky.hlsl` `Stars`): procedural on the equatorial sphere (cube-face cells, ~9,000 to magnitude 6,
  N(<m) ~ 10^(0.5m)), rotated by local sidereal time; each a pixel-wide gaussian holding its flux (resolution
  independent); magnitude 0 = 2.3e-11 of the sun's irradiance.
- **Night glow** (airglow + starlight, ~0.001 lux) in `AtmSkyRadiance`, brighter to the horizon, dimmed by cloud: what
  lights a moonless night.
- **Exposure** (`core/post.cpp`, `shaders/meter.hlsl`): the meter reaches 2^-32 (the day's 2^-12 floor froze it at
  night); beyond `kNightKnee` 3 EV under the day's grey only `kNightSlope` 0.75 of the rest is made up - full moon ~4 EV,
  moonless ~6 EV under the day: dark, readable. **At night the lit patch sets a floor**: the metered level is at least
  3 EV under the 97th percentile, blended in over 2 EV past the knee (day unchanged) - else the black sky and verge
  open the exposure and the headlamp patch and everything reflecting it clip white.
- **Night vision** (`post.hlsl` `NightVision`): per pixel, on scene luminance before exposure; full colour to 8 EV
  under the day's grey, rods only (desaturated, Purkinje blue shift) from 17 EV. A surface in a beam keeps its colour
  beside a colourless moonlit verge.

Without a time set (or by day) the frame is unchanged: the day's mean difference against pre-night builds 0.03/255.
`PHYS_DRIVE_LOOK_UP=<deg>` tilts the view to the sky for checks.

### 5.5 Local lights: car lamps, the light loop, limits

**The decisions (2026-10-30, the user's):** (1) scope = car lights, the night sky and stage lights; (2) time = date +
time + place (5.4); (3) **every light is shadowed**, by ray tracing at one ray per pixel however many lights, not a
"few shadowed, the rest fill" split; (4) car lights are the **car's**, per lamp, each with its own character.

**Interface** (`iface::LightDesc`, 176 bytes; `FrameDesc::lights` / `lightCount`): point, spot or **beam**; world
position, axis and up; linear tint; intensity in **candela**; range (0: from intensity to 0.1 lux, at most 1 km - a
pencil beam's reach); emitter radius (shadow and highlight softness); a spot's inner/outer half-angles;
`kLightNoShadows`. A beam (`kLightBeam`) is the sum of up to 4 `LightLobe`s in the lamp's own frame: an elliptical peak
in candela that halves at the half-width across and half-height up/down, centred a yaw and pitch off the axis, with a
`sharpness` (2 gaussian, higher = flatter top, harder edge - a dipped beam's cut-off); `I = Σ cd · 2^-(q^(s/2))`.
Units: 1 cd at 1 m = 1 lux = 2e-4 of the sun's irradiance unit (`Lights::kCandelaToUnits`; 22 = ~110,000 lux).
`RenderStats::lightsMs`, `lightCount`.

**Clusters** (`core/lights.*`, `Lights::Build`): the view in 16 × 9 tiles × 24 exponential depth slices (0.25–1000 m),
built on the CPU per frame (conservative screen box and slice span per light; a spot or beam culled by its cone against
each cluster's bounding sphere), uploaded and copied to default-heap buffers that the scene pass and the ray tracer
both read - **both see exactly the same lights**. Limits: 512 lights, 65,536 index entries. Per-light constants that a
light alone decides (`invRange2`, `spotScale`, lobe tangents) are computed on the CPU (`GpuLight`, 208 bytes, mirrored
in `shaders/lights.hlsli`).

**Shading = the ratio estimator** (Heitz, Hill, McGuire 2018). The scene sums every light's **unshadowed** light
exactly (Lambert + GGX; foliage cards the wrapped cosine, no highlight); `shaders/rt_lights.hlsl` picks one shadowed
light per pixel in proportion to its luminance × cosine (stratified over 4 × 4 tiles, inverse CDF, `StratU`;
`StratDisc` for the point on the emitter's disc) and traces one ray. The 0/1 is an unbiased estimate of the shadowed
fraction, a smooth scalar like the sun's visibility, denoised the same way; the light is sum × fraction. The highlight
is the emitter's: the lobe widened by the disc's angular size (Karis 2013) - a point on a wet road (roughness floor
0.002) would peak near 1e5 × irradiance.

**Deferred** (`PSDeferredLights`): while there are local lights, opaque and alpha-tested draws use GBUF pipeline
variants that leave two 32-bit targets (`kGbufFormats`: R10G10B10A2 octahedral normal + roughness + kind, R8G8B8A8 sRGB
albedo + metallic); one full-screen pass reads them and the depth (read-only view), rebuilds the position with the
**jittered** view-projection's inverse, calls the same `LocalLights` and adds (ONE, ONE) × the aerial transmittance
(`AerialAt`, recomputed). Glass stays forward, after it. Day with no lights: the plain pipelines, nothing extra runs.
A third target and half-float normals cost +3.8 ms; packed, +0.3. `NGG_FORWARD_LIGHTS` shades in the main pass (A/B).

**Rules the cost depends on:** the scene shader is `[earlydepthstencil]` (its debug-probe UAV write would otherwise
test depth after the shader: 6.4 fragments per pixel on stage 41), the alpha-tested variant draws over the prepass depth
without writing it so the driver tests early too; opaque draws sorted front to back. The lights' cost is per-fragment
arithmetic (~130 VALU per light in one dependent chain), not memory - hence deferred. Lessons: buffers shaders read
belong in the default heap, not UPLOAD; a struct copied from a buffer and indexed by a variable becomes scratch memory
(~1.5 ms per light) - read fields by index.

**Without ray tracing** (cascades + XeGTAO path): up to 4 shadow maps (`Lights::Build` picks shadowed spots/beams by
peak / camera distance²; `Scene::RenderLocalShadows`), 1024² D32, perspective along the lamp's axis, half-angle ≤ 70°,
near 5 cm, far ≤ 150 m. Lights of one **mounting** share a map (within 0.8 m, 0.12 m of height, axes within ~45°: a
ramp's pods). Point lights stay unshadowed. Slot in the record's flags (bits 8–11). Sampling `LocalShadow`: PCSS, 4
blocker gathers, penumbra from emitter radius, 12 comparison taps rotated per pixel by interleaved gradient noise.
At night (sun 0 → −6°) the AO takes a 0.5 share of the local diffuse (`NGG_LOCAL_AO=<share>`; 0 by day). TAA would keep
the shadow a moving car just left: maps carry a casters' kind target (`PSShadowFlag`), `PSDeferredLights` writes a
**moved-shadow mask** (four frames of decay) that TAA takes in the RT lights' slot (`Scene::LocalShadowMask`).
Switches: `NGG_LOCAL_SHADOWS=0`, `NGG_LOCAL_SHADOW_TURN=0` (taps unrotated), `NGG_LOCAL_SHADOW_LOG`.

**Cost** (1080p, dev Radeon 780M iGPU, GPU frame `RenderStats::frameMs` - per-pass timestamps do not isolate a pass on
this GPU; vsync quantises fps):

| case | GPU frame |
|---|---|
| 3 test lights over the screen, RT, full clock | 15.2–16.8 → 19.2–20.7 ms (rays 3.0, denoise +0.7, shading ~0.5) |
| ramp layout, 6 lamps, RT | +5.5–8 ms (rays 3.9: the pick weighs every lamp of the cluster; denoise +1.0; shading ~1.2) |
| night stage, 6 lamps, cascades path (3 maps ~0.9 ms) | 18.2 → 20.0–21.2 ms (RT on ~27.5) |
| Garage, cascades path | 12.4–13.0 → 15.5–16.1 ms (maps 0.4 ms) |

Debug: `NGG_LIGHTS_REF=1` (a ray to every lamp) / `=2` (the estimator averaged over 16 picks) as the exact reference;
debug view "RT local lights" (the lamps' shadowed fraction). Fanned lighter shadows need several lamps per patch:
narrow, apart-aimed pods give one lamp's hard shadow, which the reference confirms.

**Car lamps** (`game/src/assets/car_lights.*`). A lamp is a **beam preset** (`LampKind`) at a place in the car frame
(x LEFT, −y forward, z up, from the CoG), aimed yaw right / pitch up; `LampLight` makes the `LightDesc` for the pose.
Colour temperature → linear sRGB of luminance 1 (`KelvinToRgb`, Planckian locus), so candela stay candela.

| preset | character (round figures, tuned on the wall) |
|---|---|
| `halogen-low` / `halogen-high` | 3200 / 3300 K headlamps; low with a cut-off under the axis |
| `halogen-pencil`, `halogen-driving` | 3300 K pods: narrow hard-edged spot / wider driving beam, each with a dim wide spill (~7 % of peak) |
| `led-bar` | combo: 6 × 3.5° spot 150 kcd, 18 × 6° 30 kcd, 40 × 10° flood 10 kcd, all soft (no cut-off) |
| `ditch` | cornering lamp, aimed to the side |
| tail, brake, reverse, indicator | coloured, no shadows; indicators amber 1.5 Hz (no RBR car has them) |

- **Places from RBR's car `.ini`** (`LoadCarLamps`: `Point_light_lf/rf/lb/rb/brake_*/reverse_*/bon<n>`, y/z swapped,
  less the CoG), defaults where missing. **Aims are W-Rally's** - RBR's lamp normals are not aims (bonnet lamps point
  sideways); RBR modelled ramps (`e_ramp`) in a few cars and never switched them on.
- **Layouts** the setup chooses: the car's own "ramp", "2 pods", "4 pods", "6 pods", "LED bars", "no ramp"; default its
  own ramp, else four pods. Aims as tuned on the wall (`tools/lamp-wall/wall.py`, the renderer's beam formula on a
  10 m wall): with pods the main beams turn 10° out; 2 pods 5° out 3° up; outer pair 20° out, inner 5° out 4° up,
  six-pod middle pair 13° out 2° up; four or more all pencils; with LED bars the main beams straight on. Setup aim
  `--lamps <layout> --lamp-aim <spread>,<pitch>`, remembered per car (`lamps_<folder>` in W-Rally.ini).
- **Switches** (rebindable actions, defaults H off/dipped/main, J ramp - it shines with the main beam -, Q/E
  indicators, G hazard; brake pedal and reverse gear by the car). Until the driver touches them, the lamps follow the
  renderer's sun every frame: main beam below 3°, the small lamps' light below 6° (by day only their glass shows).
  `PHYS_DRIVE_LAMPS=<0|1|2>[,noramp][,brake]...` for captures; `PHYS_DRIVE_TEST_LIGHTS=ramp|bars|low|lamp|noshadow`
  (`assets::TestLayout`) replaces the car's own, always lit.
- **Glass**: RBR's lamp meshes come in pairs (`e_light_lf` / `e_light_lf_lit`); the car draws the unlit or the lit twin
  by the switches, the lit one glowing flat in its lamp's colour at the lens's brightness (~1600 nits per unit: headlamp
  ~100,000 nits, brake ~10,000, tail ~200) - not its texture (RBR's CarLight shader used the tyre sheet).
- **Lamps on loose parts** (`CarRender::Mounts`, `assets::LampMounts`): a part that comes off takes its lamps dark;
  one hanging on (a bonnet on its hinge) moves its beams with it.
- **The lamp lab** (`W-Rally --lab`, `assets::BuildLampLab`): the car on a floor, a wall 10 m ahead of the ramp, white
  poles at 5 and 8 m, a person-sized block; no stage or physics; `--camera`, `--time`, `--exit-at`,
  `PHYS_DRIVE_SHOTS` as on a stage.

**Stage lights: not built.** There is no light chunk in the `.ngg` and no derivation. The plan stands: a light chunk
with `provenance=auto|authored` (reconversion never clobbers `authored`), automatic candidates from emissive-looking
textures, and Wallaby's light-placement mode for the tracks people race. RBR's `_N` variant is noon, not a night light
map, so it gives nothing.

### 5.6 What is still open for night and weather

The four decisions of 5.5: **(1)** car lights and the night sky built, stage lights not; **(2)** built, but no stage
carries its place, so all use 50 N and north 0; **(3)** built - every light ray traced at one ray per pixel, and a
4-map cascades fallback for the non-RT path; the VR tier and min-spec costs are unmeasured; **(4)** built from RBR's
`.ini` with W-Rally presets and layouts, but not from the car's own `car.yaml` lamp spec, and no generic size-based
default for a car without an `.ini`.

- Weather: cloud shapes (partly cloudy is a uniform blend; RBR's cloud textures vs volumetric undecided); rain and snow
  themselves (drops, spray, splashes, rain on glass); snow cover; fog and cloud in the multiple-scattering table.
- Wetness: a step (dry/damp/wet), no drying over time; puddles and glass reflect only the sky/probe - under a canopy
  that is wrong, RT reflections for mirror-like pixels are the cure, against the RT budget.
- Glass: faces within one object are not sorted (fine for RBR's single-sheet panes, wrong for a closed shell); tint is
  grey (coloured transmission needs dual-source blending); Wallaby's `NggWriter` does not write bit 3 yet.
- Night sky: the moon's phase unreadable at its true size; no twinkling, no Milky Way.
- Car lamps: N3c tuning against the reference shots (intensities, spreads, aim, exposure); `car.yaml` lamp schema
  (draft: `beams`, `lamps` with `role`, `layouts`); a setup editor for layout and aim; beams visible in dust, mist and
  rain (haze), retroreflective materials, glare, bloom on lit glass; replays switch brake/reverse lamps by live input
  only; the light pick weighs every lamp in the cluster (the rays' main cost with 6+ lamps).
- Stage lights (N4): format chunk, auto-derivation, Wallaby placement.
- Culling: per-object frustum only; stage 41's objects are large (299 of 2987 culled).


## 6. The car

The renderer knows no cars. **A car is a model on a node tree that the game builds, places and animates each frame;
the renderer never interprets node names.** Everything specific to RBR's car files lives in the game
(`game/src/assets/car_model.*`, `car_render.*`); the renderer contributes the clear coat, the reflection probe and
the rules that keep a moving object clean in TAA and the RT denoisers. Finish rules and the PBR map format are in
[car-pbr.md](car-pbr.md); this section is the renderer's view and the invariants.

### 6.1 How the game hands a car to the renderer

- **Reading** (`assets::ReadCarModel`, any thread): the `.sgc` (HedgeHog3D's scene file, read as `HedgeHog3D.dll`
  reads it), the exterior only (`e_` meshes, the lamps' lit twins, the wheels and spare wheel of the requested
  surface - tarmac, gravel or snow), textures through `<car>_shaders.ini`, each mesh group's material and RBR shader
  class from `<car>.ini` (`FaceBlock<g>_Material`, `FaceBlock<g>_Shader`), and the optional PBR maps beside each
  texture (6.3). Files come through the car's content item (`CarFiles`, [content-layers.md](content-layers.md)), so a
  downloaded patch's maps lie over RBR's textures.
- **Building** (`assets::CarRender::Create`): one `createModel` with a fixed node layout, then `createInstance`.

  | node | what | placed by |
  |---|---|---|
  | 0 | body | the model's frame to the car's: `-cog` |
  | 1 + k | wheel k's hub (lf, rf, lb, rb) - calipers, what does not spin | the physics' knuckle and hub |
  | 5 + k | wheel k's spin, child of its hub - tyre, brake disc | spin about the knuckle's x |
  | 9 + i | loose part i (`physics::Part`: door, bonnet, bumper) | the physics' part pose |

  A mesh goes on its wheel's node, else on a part's (`e_<name>`, `e_int_<name>`, or a part above it in the `.sgc`
  tree: the grille with the bumper, mirrors with the doors), else on the body. **A part at rest gets exactly the
  body's matrix** (`PartNode`), so an undamaged car draws as a rigid one; a torn-off part's node is its world pose
  turned back into the car's frame. `PHYS_DRIVE_PARTS=1` prints the assignment.
- **Material 0 is always a red lacquered paint**, used by the fallback hull (the collision mesh, flat-shaded) when
  the model yields nothing, so material slots never shift.
- **Lamps are separate instances on the same node tree** (`PlaceLamps`): per lamp group and side an unlit and a lit
  model, one visible at a time; the lit twin glows in a solid lamp colour (RBR draws lit twins on the tyre sheet,
  which as an emission is pink). Lamp light itself is the night lighting's (§5.5).
- **Per frame** (`CarRender::Place`): `InstanceUpdate` with the pose matrix and all node matrices.
  `FrameDesc::reflectionFocus` names the body instance (6.2). The module (`module/module.cpp` `Place`) multiplies
  the tree out and calls `Scene::SetObjectTransform` per mesh; each mesh is its own dynamic `DrawItem` with its own
  BLAS.
- Creating a model rebuilds the material SRV heap once (`Scene::BeginMaterialBatch`), not twice per material:
  a car's creation went 57-204 -> 16-32 ms.
- `CarRegister` (`car_register.*`) is metadata only (name, class, physics, whether W-Rally can use the car); the
  renderer never sees it.

### 6.2 Paint: clear coat and the reflection probe

**Clear coat** (`MaterialDesc::clearCoat`, interface 15; times the ORM alpha, interface 17): a smooth dielectric
lacquer over the base layer - f0 0.04, roughness 0.02, its own GGX lobe for sun, moon and local lights and its own
sharp environment sample; the base's light is scaled by `1 - coat * F`. Forward and deferred (G-buffer kind 3:
coated pixels carry base roughness and the specular-AA widening, 5 bits each, in the roughness channel).
RBR's textures carry no gloss mask (DXT3, alpha opaque), so without maps the shader class is the mask (6.3).

**Reflection probe** (`core/probe.h` `ReflectionProbe`, interface 16). The environment map is the sky only, and
screen-space reflection cannot serve a car's side (what it reflects is off screen), so **the stage is drawn again
around the car**:

| parameter | value |
|---|---|
| faces | 6 x 256², RGBA16F (the HDR target's format, so the scene's PSOs draw them) |
| faces per frame | 2 (`NGG_PROBE_FACES` 1..6); all six at first, on a focus change and after a jump > 25 m |
| resolve | next frame's start: faces -> 512² octahedral, 10 mips (2x2 box down), sky behind wherever a face saw nothing - env.h's mapping and roughness axis |
| read by | the focus's objects only (paint coat and base, its glass), in place of the sky (`EnvRadiance`, t19) |
| cost, dev iGPU 1080p, track 42 | ~1.0 ms at 2 faces (0.6 at 1, 2.9 at 6); 128² faces were 0.8 ms - the cost is the scene, not its pixels |

- **The faces are lit as the main pass, less what is per main-camera pixel**: no RT sun visibility or AO, no aerial
  table (the probe marches its air), every local light unclustered and unshadowed; the cascades where drawn.
- **Left out of the faces: the car, any model within 3 m of the centre (its lamps, a helmet), the glass.** A probe
  at the car's centre would see its inside.
- **The car is kept as a box in the faces** (`CarBoxVis`, `gCarBoxC/X/Y/Z`: axes of its biggest mesh, extents over
  all): a sun ray through it is shadowed and the ground under it keeps a quarter of the ambient - else the paint's
  grazing edges mirror a sunlit road where the car's own shadow lies.
- The centre is the instance origin (the CoG); faces drawn on different frames lag by a frame or two of travel -
  accepted, it is a blurred reflection on a curved panel.
- **The car occludes its own reflections** (`ReflOpen`): the focus's opaque draws trace one inline ray (DXR 1.1)
  along the reflection, 5 m, against the car's own TLAS instances only (`Scene::kRtMaskFocus`); blocked = no probe
  term. Separate pipelines (`m_psoRefl`) for those draws only - a ray query in every pipeline cost the main pass
  ~1 ms. Active whenever the TLAS exists, cascades included. Off with `NGG_NO_REFL_RAY=1` or without tier 1.1.
  Cost +0.7 ms main pass with the car close. Without it a lacquered roll cage mirrored a lamp-lit barrier through
  the roof (night glints).
- **Rough lobes read the probe in taps**: a lobe much wider than a texel of its level is read half a lobe-width
  finer, the reflection plus six taps around it, each above the surface; narrow lobes and the sky take one sample.
  A 2x2-box chain read at level ~7.6 for a tyre (roughness 0.85) let the far side's lit road through (tyre glow).
- **Specular AA** (Tokuyoshi-Kaplanyan) widens base, coat, sun/moon and local-light lobes and the environment's
  roughness from the shading normal's screen derivatives. The sun's and moon's discs widen highlights as an emitter
  does.
- `Scene::SetReflectionFocus` is called every frame but bumps the geometry revision only on a change (it moves TLAS
  instance masks); counting every call refitted the TLAS every frame.

### 6.3 PBR maps and the class fallback

**Two layers: W-Rally's PBR maps per texel where a texture has them, else RBR's shader class per mesh group.** A map
overrides the rules for its texture only; a car without maps looks exactly as the rules make it. Names, channels,
formats, texconv lines and the draft tool are [car-pbr.md](car-pbr.md); the decisions:

- Maps beside the texture, by name only: `<tex>_basecolor.dds` (sRGB, replaces the texture), `_normal.dds`
  (tangent space, two channels, z rebuilt), `_orm.dds` (linear: r occlusion, g roughness, b metallic, **a clear
  coat**). Data maps load through `createTextureData` (interface 17: no sRGB decode); the DDS loader takes BC1-3 and,
  behind a DX10 header, BC4/5/7 (TYPELESS too).
- **With an ORM map the map is the finish** (roughness, metallic, coat factors 1). The class still decides glass
  or holes.
- Class rules without maps (`car_render.cpp` `finishOf`):

  | class / case | finish |
  |---|---|
  | `CarPaint`, `CarPaintInt` | clear coat, base roughness 0.2 (livery decals under the same lacquer) |
  | `CarTire` | roughness 0.85 |
  | other opaque | roughness 0.45 |
  | transparent texture on glass (`CarGlass`, `CarIntGlass`, `CarLight`, or a mesh named `*glass*` / `*window*`) | `kMaterialBlend`, two-sided, roughness 0.05 |
  | transparent texture elsewhere | holes: alpha test 0.25 (the game's 170/255 cut thin meshes away), two-sided, roughness 0.6; lacquered on `CarPaint` |

- **A material drawn with two finishes becomes two materials.**
- **The body is one-sided**, as the game draws it: its inner shell faces (black) sit just behind the paint and
  would show through.
- Over the 42 cars at hand the glass rule moves ~90 groups (vents, logos, bolts) off the glass and no lens or pane;
  most cars draw their lamp lenses as `CarPlastic`, hence the name rule.
- `tools/car-pbr` drafts ORM maps (`car-uv-dump` + `make_pbr.py`) into a new version of the car in the downloads
  layer; `PHYS_DRIVE_MATERIALS=1` lists the maps found and any the renderer refused.

### 6.4 Frames, wheels, pitfalls

**Frames.** The physics pose's origin is the **centre of gravity**, not the model's origin: a model point draws at
`v - cog` (body node). The car frame (`physics::Pose`): **+x is the car's LEFT, -y forward, z up**. `math::ToRender`
swaps y and z (render: y up, the car facing -z), which mirrors handedness - anything with a "right" (a lamp lobe's
yaw) takes its sign from where the car's right lands in render space. The `.sgc`'s root frame is already the render
frame (y up, forward -z).

**Wheels.** On a stage they stand where the physics has them: hub and knuckle rotation per wheel, the right
knuckles turned half about z, spin about the knuckle's x (`PartMatrix`, `SpinNode`). **A wheel mesh's `.sgc` node
matrix is a leftover: only its scale is kept.** Every wheel mesh has its rim towards +x; the reader turns the right
wheels half about y (i20 R5's tarmac and snow wheels are all four stored as right ones: the left rims faced inward).
Off the physics (the Garage) `CarRender::Stance` places them at the wheel meshes' own nodes when all four exist,
else at the `Point_e_wheel_*` helpers (5-15 cm off where the maker drew the wheel), and the origin at hub height
plus the tyre radius.

Pitfalls:
- A face group's first index is its record's fourth field, not its second (else groups draw another's triangles).
- A mesh's object offset (pivot, rotation, scale) must be applied; mirrored meshes are rewound.
- Glass has no depth and is not in the ray tracer's scene: it writes its own motion (`Scene::GlassMotionBuffer`,
  RG16F) for TAA and the upscalers, and reflection rays pass through windows.
- **A moving object is one that has moved since it was placed** (`DrawItem::dynamic`), standing or not: it keeps its
  motion and identity in TAA and the denoisers frame after frame. A per-frame threshold (2 mm) made a dragged car
  flip between world and object - copies behind it.
- **The road a car has left takes none of the car's RT history**: "standing" means a moving object's pixel in the
  5x5 and none moving against the world (none at all = gone). AO's history allowance under a moving hit uses
  `kAoPenumbra` 0.15 and no `gBlendMoved` floor. Checked by `tools/car-ghost/check-car-ghost.sh` (replay, TAA off,
  each stretch against `NGG_RT_NO_HISTORY=1`; fails a mean bias over 1 grey).

### 6.5 The Garage showroom

The Garage (Quick rally -> Garage) draws the chosen car on a turntable in a **synthetic stage built in memory as an
`.ngg`** (`assets::BuildShowroom`: a 120 m tessellated dark floor, a turntable disc r 3.2 m, 4 cm high, and its rim).
Lit by three spot lights (key 9000, fill 3500, rim 6000 cd) under a moonless 23:00 sky, so the spots alone light it.
Fixed camera; the projection's centre shifted to where the menu wants the car (`proj[8]`, `proj[9]`).

- **The scene is loaded once** where the menus stall anyway (as they open, after a stage) and kept; a stage
  replaces it (one scene at a time).
- **A car is read on its own thread** after the carousel has rested on it 0.25 s; its textures and maps are uploaded
  at most ~4 ms a frame, the model last. The turntable stays empty meanwhile, and for a car W-Rally cannot use.
- Textures belong to the scene, so after 8 cars (`kCarsPerScene`) the scene is reloaded to free them.
- **A car put on or taken off cuts TAA's history** (`Camera::cut`): what stood still there (glass, slow parts)
  otherwise stayed as a trail on the empty turntable.
- The turning car (12.6 deg/s, dragged by the mouse in bursts) is what the moving-object rules above were built
  against. TAA keeps an R8 `settled` count per pixel: the still weight 0.97 and the clip's fade grow over 16 frames,
  so a car stopping does not jump; at speed it keeps less (0.9 at 8 px/frame to 0.6 at 40). The RT denoiser lets
  each ray keep as much history as the hit point's travel is small against the shadow's penumbra - the turntable's
  millimetres a frame keep most, a driving car none.
- `PHYS_DRIVE_OFF` (taa, ao, shadows, rt) applies as on a stage.
- Measuring: `--showroom <car> [--turn deg] [--spin deg/s] [--shots n] [--stop shot] [--jerk k] [--then car]`
  captures `showroom.png` / `showroom-NN.png`; `WRALLY_SHOWROOM_TIMES=1` logs upload and slow-frame times.
- **A new car's BLASes are built off the frame** (4.2): its first frame is as long with RT as without (55-70 ms on
  the iGPU, from the car's materials and model), and the car casts no RT shadow or AO for the 1-2 frames until they
  land. It was 160-280 ms, 110 of them the BLAS build inside the frame.

---

## 7. Upscaling

**The scene, prepass, ray tracing and denoisers run at a render size; a temporal upscaler resolves the jittered
frames to the output in place of the TAA (`PSTaa`); tonemap, HUD and UI stay at the output.** Off (the default) is
the TAA at the output, unchanged.

### 7.1 The interface

`core/upscaler.h` `Upscaler`: `Resize` (render and output size; the history goes with it), `JitterPhases` /
`JitterOffset` (the backend's own sequence), `Dispatch(UpscaleDispatch)` (with `reset` on a cut or load).
`UpscaleRenderSize`: even, at least 64 px. **One convention, converted once per backend** and written at the top of
each backend file:

| input | convention |
|---|---|
| colour | linear HDR before exposure and tonemap (RGBA16F); last frame's exposure as a 1x1 R32F beside it, so the backend's tonemapped heuristics see the screen's brightness |
| depth | reverse-Z R32, infinite far |
| motion | RG16F, render size, previous uv - current uv, unjittered: camera reprojection for still pixels, the prepass's motion for moved ones (`PostFx` `PSUpscaleIn`) |
| reactive | R8, how little history to keep: moving glass |
| jitter | render pixels, +x right, +y down - the projection's offset |
| states | inputs PIXEL\|NON_PIXEL_SHADER_RESOURCE, output UNORDERED_ACCESS, on entry and return |

`Renderer::SetUpscaling(kind, scale)` switches backends between frames; a backend that cannot run leaves `None`
(logged). Interface: `RenderSettings::upscaler` / `renderScale` (11), `kUpscalerDlss` (12),
`RenderStats::renderWidth/Height`, `upscaler` name and `upscalersAvailable` (13) - see [design.md](design.md).

**What changes with a render size below the output:**
- the scene's samplers take a mip bias `log2(render / output)` (`Scene::SetMipBias`; static samplers, so the
  pipelines are rebuilt when it changes);
- **the AO traces twice the tier's rays at half the output's pixels or fewer** (`Renderer::AoRaysTraced`): its
  one-ray grain is otherwise denoised at render size and blown up into blotches 1.5-2x larger;
- the jitter is the backend's sequence, not our 8-phase Halton.
- TAA off (or a debug view) skips the upscaler too: the render-size frame goes straight to the tonemap.
- The coverage-preserving mip fallback for single-level textures (§2.7) is what made upscaling safe to ship on
  community stages (it was the old precondition).

### 7.2 Backends

| | FSR (`core/fsr.cpp`) | DLSS SR (`core/dlss.cpp`) |
|---|---|---|
| version | ffx-api, FSR SDK v2.4.0: FSR 3.1 on any DX12 GPU, FSR 4 on RDNA 4 (the signed loader picks) | NGX, DLSS SDK v310.9.16 |
| where from | headers in `third_party/fidelityfx` (MIT); AMD-signed DLLs fetched at configure, SHA-256 pinned, copied beside `WRenderer.dll` | nothing in git (NVIDIA-proprietary headers): headers, `nvsdk_ngx_s.lib`, `nvngx_dlss.dll` fetched at configure, pinned (`third_party/nvidia-dlss/README.md`) |
| linking | none: loaded by name from the module's folder; no DLL = no FSR | static lib, /MT: Release builds only; Debug has a stub |
| gate | the loader succeeding | **vendor 0x10DE first** (a non-NVIDIA adapter never loads NGX), then NGX and `SuperSampling_Available` |
| notes | jitter phases from ffx | Halton(2,3), 8 x ratio² phases; reactive = bias-current-colour mask; **preset K for every mode** - M (Performance) lifted the Garage's dark floor ~6% and doubled its flicker; `NGG_DLSS_PRESET=<letter>` to compare |

The renderer probes both once at create (`renderer.cpp`, logged "upscalers available") and reports them in
`RenderStats::upscalersAvailable`; the game's Settings -> Graphics -> UPSCALING steps past and dims modes this
machine cannot run. The main menu credits NVIDIA DLSS and AMD FidelityFX Super Resolution, as the licences ask.

**Game modes** (`game/src/app/settings.h` `kUpscaleModes`; `upscaling` in `W-Rally.ini`; `PHYS_DRIVE_UPSCALE=<index
or name>` overrides): `off` (TAA), `fsr-native` (1.0), `fsr-quality` (1/1.5), `fsr-balanced` (1/1.7),
`fsr-performance` (0.5), `dlaa` (1.0), `dlss-quality` (1/1.5), `dlss-balanced` (0.58), `dlss-performance` (0.5). The
readout shows "upscaled by <name> from WxH".

**Measured** (GPU frame median):

| machine, case | native TAA | FSR Q | FSR B | DLSS Q | DLSS B | DLSS P | DLAA |
|---|---|---|---|---|---|---|---|
| dev iGPU, track-42 replay, 1080p, medium | 27.9-31.6 ms | 21.9 | 20.8 | - | - | - | - |
| dev iGPU, same, high | 34.8 | 24.1 | 21.1 | - | - | - | - |
| RTX 5070 Ti, 2160p, medium | 9.21 | 7.12 | - | 7.74 | 7.04 | 6.50 | 10.56 |

FSR Quality also reduced the door-panel grain (fine 2.58 -> 1.67, blot 8.65 -> 6.66), the AO's doubled rays being
paid out of the smaller frame.

### 7.3 What remains

The plan was FSR 3.1 first (runs anywhere), DLSS SR behind the same interface, then NRD, then DLSS Ray
Reconstruction. **FSR and DLSS SR are built; NRD and DLSS RR are not.** The visibility layer is shaped for them:
a third `Visibility` source (NRD's SIGMA/REBLUR over the RT output, or DLSS RR) takes the same frame and fills the
same outputs (`core/visibility.h`). NVIDIA-only features stay optional and are checked by eye on the RTX machine.


## 8. Editor integration (Wallaby)

**Wallaby draws through `WRenderer.dll`, the renderer the game ships** ([design.md decision 11](design.md)). One
renderer means the editor's preview cannot disagree with the game. That was the reason for the whole integration: a
second, differently-wrong renderer (Wallaby's old fixed-function SlimDX preview, then plugin-rbr's frozen `ngg-api.dll`)
keeps producing "looks right in the editor, wrong in the game" bugs.

### 8.1 Shape: in-process, flat C ABI

- **In-process, through the module's flat C tables**, not C++/CLI and not a second process. The old out-of-process
  argument was RBR's (32-bit game). Wallaby is x64 and P/Invokes a plain DLL. Wallaby generates its bindings from the
  headers, so it follows `kRendererInterfaceVer`.
- **The cost accepted:** a device removal or driver hang takes the editor down with it. WRenderer has DRED
  breadcrumbs (`core/device.cpp`) but **no device-lost recovery** (re-create, reload). That is open (§11).
- **Two tables.** The editor opens the renderer and loads a scene like the game does (`renderer.h`: `loadScene`
  from memory, `createModel`/`createInstance` for added content, `FrameDesc` per frame). It then asks
  `RendererApi::editApi(r, kRendererEditVer, &t)` for
  [`renderer_edit.h`](../include/wrally/iface/renderer_edit.h). The edit table has its own version (now 2), so
  editor additions do not move the game's interface. The rules are as in `renderer.h`: one thread, POD, pack 4,
  nothing thrown across. Edits take effect from the next `renderFrame`. Readouts describe the last frame, which is
  complete when `renderFrame` returns (synchronous).
- **The `.ngg` is the runtime format, not the editing format.** Wallaby authors in its own model and exports `.ngg`.
  **The renderer never loads glTF or any other editing format:** to show the truth it would have to repeat the
  converter's passes (normal welding, shading overrides, de-atlasing), which would make two content pipelines instead
  of two renderers. The converter stays the only content pipeline, and the renderer draws the buffers it is handed.
- Implementation: `modules/renderer/src/module/module.cpp` (`Edit*`, `kEditApi`) over `core::Scene`
  (`src/core/scene.h`, the "editor:" blocks). Test: CTest `editor-api-41` (`tests/editor_api.cpp`). It fails
  against a stub, and planted breaks of the move, the selection and the layer mapping each turn it red.

### 8.2 The live scene: objects, placements, the dynamic pool

**The scene is edited in place, never rebuilt per edit.** The editor's costs fall into three tiers:

| what changed | cost | mechanism |
|---|---|---|
| camera, sun/weather, debug view, light mode, culling | free | `FrameDesc` / `RenderSettings` fields, `setLightMode`, `setCull` |
| move/hide an object, move/add/remove placements, select, material scalar | ~free | per-object delta and flags, placement batches, root constants |
| geometry edit, weld angle, re-converted texture | scoped rebuild | Wallaby re-runs its pass and re-pushes (today: the whole `.ngg` through `loadScene`) |

- **Objects by index.** An object is one of the scene's draw objects: the `.ngg`'s `NGGO` order, then whatever
  `createInstance` added (`instanceObjects` maps an instance to its objects). `setObjectTransform` takes a
  **world-space delta** from where the file put it, because the `.ngg` bakes object placements into the vertices.
  Identity restores the object. Turning about its own centre is the caller's job, composed from `objectBounds`. An
  object can also be moved by `FrameDesc::stageObjects` or an `InstanceUpdate`. The last call wins, so an editor uses
  one of them per object.
- **Deletion is hiding** (`setObjectVisible`): a draw-list edit with no GPU work. The geometry stays resident, so undo
  costs nothing. Freeing storage on delete would make undo the expensive direction.
- **Placements (edit table v2)** are the instanced models of `NGGD`/`NGGE` (§2.6): a handle per placement, the
  file's in order and then the added ones, **never renumbered**. A removed placement becomes a tombstone, so the
  handles in an editor's undo stack stay valid. Edits come in **batches** (`addPlacements`, `setPlacements`,
  `removePlacements`), so a brush stroke or a dragged selection rebuilds the culling cells once. A batch is checked
  whole and changes nothing if any handle, model or scale is bad. `modelStats` reports the last frame's counts.
  This answers the old "instanced like glTF, not baked" requirement for placed content: fifty cones are one model and
  fifty placements.
- **Added geometry goes to the dynamic pool** beside the static scene (`Scene::AddMesh/AddInstance`, reached through
  `createModel`/`createInstance`). Placing one prop does not re-upload the track. The pool is uploaded lazily at the
  next frame and grows like a `vector`. Mesh/texture/material handles are 1-based; instance handles are object
  indices. `Scene::Compact` (reclaim without renumbering) exists but nothing calls it: released meshes stay until the
  scene goes ([design.md §3.1a](design.md)).
- **Materials live** (`getMaterial`/`setMaterial`, handles 1-based): a scalar shows on the next frame at no cost, and
  a texture slot rebuilds the descriptor heap. Only `kMaterialAlphaTest`, `kMaterialTwoSided` and `kMaterialBlend`
  are exposed. What the renderer derives from the slots stays inside it. `materialName` is the base-colour texture's
  name (RBR's naming).
- `focusPoint` gives a point **on the ground** mid-stage. On a bent stage the box centre is in the air.

### 8.3 Overlays: what needs depth is drawn by the renderer

A child window with a swap chain wins the airspace fight, so host-painted (GDI/WinForms) overlays flicker or never
appear. **Decision: the renderer draws everything in-world that needs the depth buffer** (the selection wire, line
layers, labels). Wallaby draws its 2D chrome outside the viewport.

- **Selection** (`setSelection`): a wire round each object. Hidden objects draw none, because hiding is deletion.
- **Overlay lines** (`setOverlayLines`): segments (vertex pairs, per-vertex colour `0xAABBGGRR`), one layer replaced
  per call and kept until changed. *Described, not commanded*: there is no render-state API, which is what made the
  D3D9 immediate-mode gizmos replaceable at all. An odd vertex count is refused. `kEditNoDepth` is for manipulators,
  which must stay grabbable. **Editor layer L is core layer L + 1**: `FrameDesc::lines` owns layer 0 and clears it
  when a frame drops its lines. On a shared layer, the editor's gizmos would vanish with the game's debug lines.
- **Labels** (`setFont`, `setLabels`): the host supplies a monospace RGBA atlas (no glyph data or size frozen into the
  renderer). Text sits at world anchors, `pixelHeight` tall at any distance. `kEditScreenSpace` is for HUD readouts
  and `kEditTextShadow` keeps text readable over sunlit road.
- **Where in the frame** (`Scene::RenderEditorOverlay`, `shaders/select.hlsl`): **after the tonemap and TAA,
  straight onto the back buffer, depth-tested against the scene's depth.** Drawn into the HDR target, the overlay
  would be tonemapped (not the colour asked for), and a one-pixel wire fed to TAA ghosts. Upscaling breaks this: the
  overlay is **not drawn while upscaling** (FSR/DLSS), because the depth buffer is then not the output's size.
- **Unjittered view-projection**, unlike the scene: a jittered wire would crawl against the TAA-stable image. The
  up-to-half-pixel mismatch with depth is absorbed by the selection PSO's depth bias, which is **positive under
  reverse-Z**. The wrong sign fails silently: the wire sinks into its surface and never appears. Overlay lines have
  no bias, so they sit against geometry.
- **The occluded part is drawn too, dimmed**, on the exact complement of the visible pass's depth function. A
  selection that vanishes behind a hill is worse than none.
- **Pixel counters** (`overlayStats`): visible and occluded pixels per kind, the only way to tell "drawn" from "drawn
  and rejected" from outside. **`[earlydepthstencil]` is load-bearing:** a UAV-writing pixel shader otherwise runs
  before the depth test, and the counters then count rasterised pixels, not surviving ones. The symptom: identical
  visible/occluded counts from two complementary depth functions. *Rule: any instrument written from a pixel shader
  measures invocations unless early-Z is forced.*
- **The pixel probe** (`probePixel`): the next frame logs that pixel's lighting terms by name. The buffer is zeroed
  before the armed frame and the shader sets a FIRED marker, so "nothing covered the pixel" reads as a miss, not as a
  stale pass.

Measured through the table on stage 41 (`editor-api-41`):

| check | result |
|---|---|
| translate +10 m | moved exactly (10, 0, 0) |
| turn 90 deg about its centre / about the origin | 0.0000 m / 2706 m |
| scene + camera moved 64 m together / scene alone | max delta 0.0002 / 60.5 |
| selection, occluders hidden | 35,835 / 46,430 -> 36,036 / 46,229, total conserved |
| lines, scene hidden | 4,179 / 2,489 -> 6,668 / 0 |
| labels, twice the text | 9,580 = 2 x 4,790 |

The transform comparison runs **without sun shadows**. The cascades snap to a world-fixed texel grid, so moving the
camera 64 m moves the snap. That is the maps' stability design, not a transform error.

### 8.4 Picking and the rest

- **Picking is the editor's, on the CPU** (Möller–Trumbore against Wallaby's own meshes). The table has no pick
  call, which keeps the renderer out of the editor's object model.
- Other editor-only switches: `setLightMode` (0 = RBR's bake as the A/B reference, 1 = dynamic; `lightModeName`),
  `setCull`.
- **Open:** device-lost recovery; scoped re-push of changed objects instead of a whole `loadScene` (build it when
  the full reload hurts); a transform for the file's own `NGGO` objects (they are baked; moving is a delta).

---


## 9. Native and generated tracks

Future design. What exists today is marked **exists**; everything else is planned.

### 9.1 What exists

- **exists:** the runtime treats every `.ngg` alike; nothing in the format is RBR-only. Every field that comes from
  un-baking (`bakedScale`, the vertex colour) is optional in effect: a native file sets `bakedScale` 1 and white
  colour, and the default lighting never reads them.
- **exists:** instanced models and placements (`NGGD`/`NGGE`, §2.6) - the mechanism for added content on converted
  stages (better trees, props) and for generated scatter. A placement with `kNoPlant` adds; one naming a plant replaces.
- **exists:** glTF as the interchange (Wallaby's conversion spine, §2.1) and v3 materials that map onto glTF
  metallic-roughness.
- **not yet:** W-Rally stages (`stage.yaml`): the catalog reads none. Native gameplay in `.ngg` chunks
  ([design.md decision 10](design.md)). Any generator.

### 9.2 Gameplay first, as a stub, then native

A generated stage still needs everything to *drive* on - collision, surfaces, drive line, splits, cameras, fences,
pacenotes - but nothing from RBR to *draw* it. **The plan in [design.md §6.3](design.md):** a W-Rally stage ships an
`.ngg` plus gameplay files in RBR's formats (`.trk .col .mat .fnc .dls`), written by our tools, so both physics modules
run it from day one; later those move into `.ngg` chunks and a stage is one file. Since W-Rally draws the stage
itself, the old "minimum-viable `.lbs` render stub" question no longer exists: no render geometry has to satisfy RBR's
engine.

**The collision rule inverts for generated content:** collision is authored first and the visual mesh conforms to it.
Wallaby's existing tools (`DriveLineCreator`, `colgen`, `FenceBuilder`, `CamCreator`, `PacenotesEditor`) are the
starting point, but were built for interactive use; a generator needs them as passes: a drive line with splits and
start/finish from a centreline, collision from the road surface, `.mat` from the generator's own material assignment,
pacenotes from curvature, cameras and triggers templated from the drive line. That is Wallaby's project; it does not
touch the renderer.

### 9.3 Content path

```
generator (AI / procedural / DCC) --glTF--> Wallaby --+--> .ngg            (renderer)
                                                     +--> gameplay files   (physics, game)
```

**glTF, not USD** (decided): reach and weight - glTF is native in Blender and every real-time tool, USD's tooling is
thin outside Houdini/Maya and heavy to embed. USD's loss is its layer composition and variant sets; the `.scene.json`
sidecar carries that instead (stage layout, layers, material classes, weather, drive line). Converted stages enter the
same spine further down, so this is one pipeline. glTF does not carry UV scrolling without `KHR_animation_pointer`
(thinly supported), which matters little: RBR's `...Anim` shaders are a single UV-scroll instruction and its `...Sway`
moves nothing. glTF is a transport format; Wallaby's working form is its own mesh structure.

### 9.4 Replacing the card vegetation

**De-merging works with no help from the file:** separate plants do not touch, so connected components over quantised
positions split them. Joux Verte: 337,944 alpha-tested triangles -> 38,033 components (~33 per object), median 6
triangles and 1.3 m footprint (a bush of 2-3 crossed cards), 38 % tree-like, 0.3 % sprawling > 40 m. **Shape alone does
not classify:** the largest components are the distant backdrop (`MB_SUPERBOWL_4`, 7 km wide) and fences; tagging
needs texture name and shape, with fences and backdrop excluded explicitly - Wallaby's tagger (§2.6). The legacy
importer already does the de-merge (untagged).

| part | state |
|---|---|
| detection / de-merge | **exists** (importer; Wallaby's plant table) |
| classification | Wallaby's tags; per-track review |
| rendering instances with LOD | **exists** (§2.6); ray-tracing cost of alpha-tested foliage open |
| replacement models, bushes, grass, converter placements | open (vegetation plan workstreams 3-4, Wallaby) |

Collision comes from `.col`/`.trk`, not from these meshes, so replacing them is visual only - as long as collision is
left untouched.

### 9.5 Nearer-term uses of the same path

- **Material synthesis:** normal/ORM/emissive maps for RBR's 256^2 diffuse textures, offline, into v3 slots.
- **Light placement** from RBR's evidence of where light was intended; machine output must stay distinguishable from
  authored (a provenance field when the lights chunk exists).
- **Detail scatter** over converted stages, masked by the `.mat` surface classes: additive placements, no original
  vertex touched.

### 9.6 Fairness

Generated and augmented content changes what players see, so it is in the same hash chain (§2.9). **A generated stage
must be reproducible from a recorded generator id, seed and input hash**; the format has no field for it yet.


## 10. Measuring and debugging

### 10.1 Judging a change: whole-frame GPU time and A/B captures

**A change of what the GPU does is judged by the whole frame's GPU time** (`Renderer::LastFrameMs`,
`RenderStats::frameMs`: timestamps around `RecordFrame`, first command to last), not by a pass's own timestamps,
which shift work between passes. Per-pass numbers (`passMs`, `rtMs`, `aoMs`, `lightsMs`...) say where a difference
is, once the total says there is one.

**A/B runs are deterministic replays** (game side, `game/src/app`):

| switch | effect |
|---|---|
| `--replay run.phr`, `--step <s>` / `PHYS_DRIVE_STEP` | fixed time per frame, simulation in lockstep: the same frames at the same ticks run to run |
| `--pause-at`, `--exit-at` / `PHYS_DRIVE_PAUSE_AT`, `_EXIT_AT` | stop or quit at a tick |
| `PHYS_DRIVE_SHOTS=<n>` | `CaptureFrame` (the swap chain's image, PNG) every n ticks, `shot-NNN.png` beside `--out` |
| `PHYS_DRIVE_OFF=taa,ao,shadows,rt,vsync` | those off from the start (`ao` = quality low). **`taa` for image A/B** (history makes frames differ); **`vsync` for timing** (a capped iGPU clocks down into the slack) |
| `PHYS_DRIVE_FRAMEMS` | mean + median of `frameMs` (and `passMs`, `rtMs` medians) printed at the end |
| `PHYS_DRIVE_UPSCALE=<mode>` | upscaling mode by index or name (`fsr-quality`...) over the settings |
| `PHYS_DRIVE_READOUT` | the readout on from the start |
| `PHYS_DRIVE_NGGLOG` | the renderer's log on stdout |
| `PHYS_DRIVE_CAMERA_AT=<tick>:<cam>` | a camera cut at a tick |
| `PHYS_DRIVE_JITTER=<tick>` | RMS of the car's frame-to-frame screen motion up to that tick, then quit |
| `PHYS_DRIVE_LOOK_UP=<deg>`, `PHYS_DRIVE_TEST_LIGHTS`, `PHYS_DRIVE_LAMPS` | views tilted up (sky); test lamps on the car; lamp switches forced |
| `--showroom <car> [--shots n]` | the Garage turntable, captured and quit (`WRALLY_SHOWROOM_TIMES` logs load times) |

Keep runs short. Renders are compared within one compiler / build only. `tools/car-ghost` is a scripted A/B of this
kind (RT history vs `NGG_RT_NO_HISTORY=1`, view 11/13).

### 10.2 The readout and `RenderStats`

The renderer fills `RenderStats` (`renderer.h` iface, `stats`) after each frame: GPU ms (frame, main pass, env,
aerial, RT, AO, denoise, local lights), RT available / active and **why not** (`rtFallback`: "cascades selected",
"no ray tracing on this adapter", "shadows off", "the acceleration structure failed to build"), traced and shadowed
pixels, quality, AO source name, exposure (applied, metered and adapted EV), render size and upscaler, light count,
sun elevation. **The game draws it** (`F1`, `ui::DrawReadout` in `game.cpp`) through the UI pass - fps, gpu frame,
switches (`K` RT, `L` shadows, `O` AO, `T` TAA), RT breakdown or the fallback reason, upscaling, exposure, debug view.

Rule from the readout's history: **when an instrument reports "nothing happened", check that it is connected**
before believing it - readbacks are valid only after the GPU wait (`Scene::LogProbe`, `ReadOverlayStats` run after
`EndAndWait`).

### 10.3 Debug views, light modes, the pixel probe

`setDebugView` / `V` / `NGG_DEBUG_VIEW=<n>` (`Scene::DebugModeName`): 0 normal, 1-2 tex0/tex1, 3 blend weight,
4 baked vertex colour, 5 uv0, 6 flat grey, 7 sun visibility, 8 shadow acne, 9 ambient specular x8,
10 roughness/metallic, 11 RT sun visibility, 12 RT vs shadow map (red RT-shadow/map-lit, blue reverse), 13 RT AO,
14 plants, 15 RT local lights. Debug views get the raw curve and no TAA. Light mode (`NGG_LIGHT_MODE`): 0 baked RBR
vertex colour (sRGB curve, the A/B reference), 1 dynamic. The editor's `probePixel` (`Scene::SetProbe`) logs every
lighting term of one pixel in linear HDR the next frame.

### 10.4 The lab

`W-Rally --lab` (`RunLab`, `game/src/assets/lamp_lab.cpp`) builds a synthetic `.ngg` in memory and loads it through
the ordinary `loadScene`, so the real path is measured: the car at the origin, a flat floor (albedo 0.18), a wall
11.9 m ahead (0.5, an aiming screen), thin poles (0.9) and a person-sized block; untextured, exact linear albedos;
night by default (`--time`), test lamps from `PHYS_DRIVE_TEST_LIGHTS`. **Measuring on known albedos and fixed
geometry, judging on real tracks.** The old general lab (spheres, albedo ladder, 12 km plane, TAA poles, `--probe`)
is not ported. `tools/lamp-wall/wall.py` draws the same beam model offline.

### 10.5 Device removal: DRED

`Device::Create` enables DRED before the device exists (auto-breadcrumbs and page-fault data, forced on: the cost is
small and a GPU reset without a record is a dead end). Command lists are named (`ngg frame`,
`ngg secondary (BVH builds)`). **`CheckRemoved` runs after every submit, on a failed `Begin` and at the end of
`renderFrame`**, and logs once per device: the removal reason, for each unfinished list the breadcrumb window around
the last completed command, and the page fault (GPU VA and up to 6 recently freed allocations near it, by name and
type). Recently-freed allocations near a fault mean use-after-free - the one such hang found was a descriptor keyed on a reused pointer
(1.3). The D3D12 debug layer exists behind `kRendererDebugLayer`; `DrainMessages` logs its messages.

### 10.6 PIX and RGA

- **`NGG_PIX=<frame>`** loads the newest PIX GPU capturer from `Program Files\Microsoft PIX` before the device is
  made, and captures renderer frame `<frame>` (>= 2) to `ngg-frame-<frame>.wpix` in the working directory - for PIX,
  or `pixtool open-capture <file> save-event-list <csv> --counters=*` for per-draw times. The pipeline library is off
  under PIX.
- **`NGG_DUMP_PS=<path>`** writes the scene's opaque pixel and vertex shader blobs, the root signature and
  `scene.hlsl` with its includes flattened (`.ps.obj`, `.vs.obj`, `.rs.bin`, `.hlsl`) - what Radeon GPU Analyzer needs
  for registers, occupancy and ISA. How the local lights' cost was taken apart.
- `NGG_SHADER_DUMP=<dir>` writes every shader as handed out (proved the move to files byte-identical).

### 10.7 Environment variables (renderer)

All read once, at start-up or first use; development only unless noted.

| variable | effect | read in |
|---|---|---|
| `NGG_SHADER_DIR=<dir>` | compile shaders from the files at run time; disables the pipeline library | `shaders.cpp`, `pipeline_cache.cpp` |
| `NGG_SHADER_DUMP=<dir>` | write each shader blob as handed out | `shaders.cpp` |
| `NGG_SHADERC_THREADS=<n>` | build-time compiler's threads (default cores, max 16) | `shaderc/main.cpp` |
| `NGG_PIPELINE_CACHE=library` | turn the pipeline library on | `pipeline_cache.cpp` |
| `NGG_PIPELINE_THREADS=<n>` | create a batch's pipelines on n threads (crashes AMD) | `pipeline_cache.cpp` |
| `NGG_PIPELINE_TIMES` | log every pipeline's creation time | `pipeline_cache.cpp` |
| `NGG_DRIVER_SHADER_CACHE=off\|clear` | driver's shader caches disabled / emptied (developer mode) - `clear` is the honest cold start; `off` makes everything slow | `device.cpp` |
| `NGG_PIX=<frame>` | PIX capture of that frame | `device.cpp` |
| `NGG_DUMP_PS=<path>` | scene shader + root signature + flattened source for RGA | `scene.cpp` |
| `NGG_DEBUG_VIEW=<n>` | start in that debug view (module and game) | `module.cpp`, `game.cpp` |
| `NGG_LIGHT_MODE=<n>` | 0 baked, 1 dynamic | `module.cpp` |
| `NGG_LEGACY_PROFILE=<n>` | legacy cards' lighting: 0 off, 1 A (bake), 2 B (bake as occlusion, the game's default for an imported `.lbs`) | `module.cpp`, `game.cpp` |
| `NGG_AO_RAYS=<n>` | RT AO rays per pixel over the tier | `module.cpp` |
| `NGG_AO_SOURCE=screen` | a ray-traced frame takes XeGTAO's AO | `module.cpp` |
| `NGG_GTAO_QUALITY=0..3`, `_RADIUS=<m>`, `_NORMALS=prepass\|depth`, `_THIN=0..0.7`, `_DENOISE=1..3` | XeGTAO knobs over the tier | `module.cpp` |
| `NGG_RT_NO_HISTORY=1` | RT sun denoiser without history: the unbiased reference for ghosting | `rt.cpp` |
| `NGG_LIGHTS_REF=1\|2` | local lights' shadows by a reference shader (checking only) | `rt.cpp` |
| `NGG_NO_REFL_RAY` | no ray-traced reflections on the car (no `ps_6_5` variant) | `scene.cpp` |
| `NGG_FORWARD_LIGHTS` | local lights shaded in the main pass instead of deferred (A/B) | `scene.cpp` |
| `NGG_LOCAL_SHADOWS=0` | no local-light shadow maps without RT (A/B) | `visibility.cpp` |
| `NGG_LOCAL_AO=<share>` | night share of XeGTAO on local lights' diffuse (default 0.5) | `visibility.cpp` |
| `NGG_LOCAL_SHADOW_TURN=<k>` | scale of the per-pixel rotation of the local shadow-map filter (default 1) | `scene.cpp` |
| `NGG_LOCAL_SHADOW_LOG` | log the shadow-slot candidates every 300 frames | `lights.cpp` |
| `NGG_PROBE_FACES=1..6` | cube-probe faces drawn per frame (default 2) | `scene.cpp` |
| `NGG_MODEL_LOD_SCALE=<k>` | instanced models' LOD distances scaled (min 0.05) | `models.cpp` |
| `NGG_WIND=<speed>[,<gust>[,<deg>]]` | that wind over the weather's (m/s, 0..1, towards; default gust 0.5, 90 deg): the wind's A/B | `module.cpp` |
| `NGG_NO_SOLIDS` | don't measure solid cards: every alpha-tested object treated as foliage (A/B) | `scene.cpp` |
| `NGG_DLSS_PRESET=<A..O>` | force a DLSS render preset for every mode (default K) | `dlss.cpp` |


## 11. Open work

This is the renderer's running TODO, written in W-Rally terms. It keeps what is still open from the plugin era's
list and phase plan, plus the open points of §§1-10. Game-side render features already listed in
[design.md §3.2](design.md) are not repeated here: particles, skid marks, decals, damage deformation, rain on
glass, and VR behind `kRendererCapVr`.

**Frame and start-up**

| # | item | state / why | blocked on |
|---|---|---|---|
| 1 | **Build a new mesh's BLAS off the frame** | done (4.2): queued, recorded on a thread, built on the ray tracer's own compute queue, fenced, landed 1-2 frames later. Left: the landing frame's publish, 7-18 ms CPU (the any-hit buffers rebuilt whole - appending the new BLASes' share would make it ~1 ms - and the blocking TLAS); dropping a car frees its BLASes on the frame (3-8 ms); a frame's other allocations can still wait a millisecond or so behind the thread's | nothing |
| 2 | **TLAS refit instead of a full rebuild** | the TLAS is rebuilt whole (`FAST_BUILD`), blocking on the secondary context, every frame anything moves (the car). No update path. Measured once, stage 41 (922 instances): 3.5-5 ms of the frame's CPU, the upload, the build and the wait together - so worth it, and the async queue of 4.2 would take the wait out | nothing |
| 3 | **Pipelines in parallel** | built and reverted (§1.6). A cold start was ~5 s shorter, but AMD driver 32.0.31021.5001 crashed in `amdxc64.dll` or hung in about 1 run of 12-28 whenever two threads compiled. `NGG_PIPELINE_THREADS=<n>` retries it. Run it 60+ times with the crash/hang harness before trusting it | a driver that survives it (newer AMD, or NVIDIA) |
| 4 | **Measure the pipeline library on NVIDIA** | it saves nothing on the AMD iGPU, so it is opt-in (`NGG_PIPELINE_CACHE=library`). On the RTX machine, run with `NGG_DRIVER_SHADER_CACHE=clear`, with and without it, and compare the log's `pipelines:` lines. If there is no win there either, delete `pipeline_cache`'s library and keep the wrappers | the RTX machine |
| 5 | **Shader hot reload** | `NGG_SHADER_DIR` compiles from the files at start only. Recreating the pipelines without a restart is not built | nothing (optional) |
| 6 | **D3D12 debug layer reachable** | `kRendererDebugLayer` exists, but nothing in `game/` or `tools/` sets it, so there is no way to run the game under validation | nothing |
| 7 | **Device-lost recovery** | the game and the in-process editor (§8.1) die on a device removal. Re-create the device and reload the scene | nothing |

**Visibility, shadows and AO**

| # | item | state / why | blocked on |
|---|---|---|---|
| 8 | **A third visibility provider: NRD (SIGMA/REBLUR) or DLSS Ray Reconstruction** | named in `visibility.h`, not built. Most of the remaining RT grain is what it would remove (§4.5) | the RTX machine (DLSS RR), NRD integration |
| 9 | **Ray-traced foliage cost** | instanced trees add +2.2 ms with opaque RT and +7.2 ms alpha-tested (§2.6), since any-hit runs through the needle tiers. Done: a ray-tracing-only LOD (`kModelLodRayOnly`, §2.6). Open: distant placements opaque or out of the TLAS, one submesh per shell; the real models decide how far the shell goes | the real tree models (Wallaby) |
| 10 | **Cascades: penumbra and cross-fade** | the non-RT sun shadow is hard-edged (no PCSS) and switches between cascades with no blend | nothing |
| 11 | **Cheaper distant cascades** | cascade 3 covers kilometres and changes slowly: update it every 2nd-3rd frame. 2048² for 0.7 m texels is wasted bandwidth | nothing |
| 12 | **Re-measure RT vs the raster path in W-Rally** | the "cascades cost more than RT" number is from the plugin (§4.9). Env, aerial and multiple scattering were last timed before the probe and weather were added | nothing |
| 13 | **Debug view 12 on the RT path** | it compares RT with the cascade map, but `RenderShadows` is not called on the RT path, so it may read a stale map. Unverified | nothing |

**Content and the format**

| # | item | state / why | blocked on |
|---|---|---|---|
| 14 | **The legacy `.lbs` import, finished** | no de-atlas (road seams stay on imported stages), no weld in the importer (the renderer's fallback welds), no variants, no `NGGS`, no cache. The planned order is a hand-finished server `.ngg` before the import (§2.1) | the stage server |
| 15 | **Instanced models, the rest** | not in the lamps' shadow maps or the car's reflection probe. A model's own `kMaterialBlend` is drawn opaque. One RT LOD only (no per-LOD BLAS), no billboard normal map. The wind: ray tracing static (its shadows stand still under moving leaves with RT on) | nothing; phase 2 of the wind if it shows |
| 16 | **GPU culling of placements** (`ExecuteIndirect`) | the CPU cull takes 0.03 ms (camera) and 0.34 ms (with cascades) at 3.7k placements, so it is not worth it yet. Measure again at ~25k (bushes, grass) | bush and grass placements |
| 17 | **Finer culling of stage objects** | every pass frustum-culls per object, but stage objects are large: stage 41 culls 299 of 2,987. Converter-chosen bounds and grouping, distance culling and occlusion culling are open | the converters |
| 18 | **PBR maps for stages** | roughness, metallic, normal and emissive are read but fall back to white, so stage surfaces are uniform rough dielectrics. Generating maps from albedo is converter work. The catch: RBR textures carry baked shading, which generated normals would read as geometry | converter (Wallaby), importer |
| 19 | **Mip chains and the single-stage flag from the converters** | the renderer generates missing mip chains at load (§2.7; 203 ms / +7 MB on track-108) and measures single-stage materials. Both fallbacks stand down once the converters write the data | BC encoder in the converter |
| 20 | **`NGGS` and the variant spawn points** | written by the converters, read by nobody: the car starts from the physics' data | a use for them |
| 21 | **RBR animated objects** (`.dls`) | drawn static. `NativeSim::Open` refuses animation objects | physics side |
| 22 | **Native gameplay chunks in `.ngg`** | collision, surfaces, fences, drive line, splits and pacenotes into the `.ngg` ([design.md decision 10](design.md)); generated stages after that (§9) | the game running end to end |
| 23 | **Texture streaming above the VRAM budget** | not built: every texture is resident | a stage that needs it |
| 24 | **`.ngg` content protection** | deliberately deferred; no envelope field yet (§2.9) | competition use |

**Lighting, weather, night**

| # | item | state / why | blocked on |
|---|---|---|---|
| 25 | **Delete the established lighting balance** | physical lighting is the default and `Scene::SetPhysicalLighting` has no caller. `sunDiffuse`, `skyAmbient`, `kSpecScale`, the albedo*E convention and the lerp ambient run only when there is no environment map | nothing |
| 26 | **Weather v2** | v1 does fog, haze and cloud cover, and the wet road is done (§5.2). Open: cloud shapes (RBR's textures or volumetric, undecided), falling rain and snow, spray, drying, snow cover, fog and cloud in the multiple-scattering table, RT reflections for puddles and glass | user's choice on clouds |
| 27 | **Stage lights (N4)** | a light chunk in the `.ngg` (`provenance=auto|authored`). Emissive-texture detection for the automatic part, Wallaby's light placement for the authored part | Wallaby light editor |
| 28 | **Car lamps, the rest (N3)** | `car.yaml` lamp spec and a size-based default; tuning on the user's reference shots (N3c); bloom on lit glass; beams visible in dust, mist and rain; glare; retroreflectors. Replays switch brake and reverse lamps from live input only | references; weather/dust work for beams |
| 29 | **A stage's own place** | every stage is lit as 50 N 14.4 E, north 0, because no stage metadata gives a latitude, longitude or north | `stage.yaml` / stage metadata |
| 30 | **Light pick cost** | `rt_lights.hlsl` weighs every lamp of the cluster per pixel. The 6-lamp layout's rays take 3.9 ms | nothing |
| 31 | **TAA under camera motion** | only moving objects get the speed-based history weight. Under a pan the static world keeps ~51 % of its detail. Judge at driving speed before changing it | an in-game look |

**The car**

| # | item | state / why | blocked on |
|---|---|---|---|
| 32 | **Car normal and base-colour maps** | the format and loader support them, but no tool drafts them: from colour alone they emboss decals. `make_pbr.py` drafts ORM only, and loses a black-painted car's lacquer | a drafting tool |
| 33 | **Lacquer seen through glass** | glass is not in the RT scene, so lacquered parts seen through a window still reflect the stage. Some roll-cage lines remain at night | nothing |

**Housekeeping**

| # | item | state / why | blocked on |
|---|---|---|---|
| 34 | **RT vs VR budget** | which techniques fit a two-eye 90 Hz tier can only be measured on real hardware, once VR exists | VR ([design.md §3.2](design.md)) |

## Appendix A: old section numbers

Commit messages before 2026-11-11 and plugin-rbr cite the plugin-era numbering. Where the topic still exists:

| old | new | old | new |
|---|---|---|---|
| 1.8, 1.8.1-1.8.4 | 1.1, 1.5 | 5.0k (lab) | 10.4 |
| 1.8.5 (RT probe) | 1.1, 4.2, 4.3 | 5.0l (mip fallback) | 2.7 |
| 3.0, 3.1 (conversion spine, sidecar) | 2.1 | 5.0m (RT cost) | 4.9 |
| 3.2, 3.2b (un-baking, RBR's lighting) | 2.8 | 5.0n (one frame) | 1.4 |
| 3.3-3.3d, 3.3g (materials, atlases) | 2.3, 2.8 | 5.0o (BVH instancing) | 4.2 |
| 3.3e, 3.3f (bounds, single stage) | 2.7 | 5.0p (quality tiers) | 1.5 |
| 3.4a, 3.4 (weather presets) | 5.1 | 5.0q, 5.0s (weather) | 5.1 |
| 3.4b (layers, variants) | 2.4 | 5.0r (TDR, DRED) | 10.5 |
| 3.5 (format) | 2.2 | 5.0t (weather records) | 2.5 |
| 3.6, 3.7 (fairness, encryption) | 2.9 | 5.0u (glass) | 5.3 |
| 4.x (animation harvesting) | gone; 11 #21 | 5.0v (wet surfaces) | 5.2 |
| 5.0 (sky) | 3.1 | 5.0w (instanced models) | 2.6 |
| 5.0b (cascades) | 4.6 | 5.0x (shaders, pipelines) | 1.6 |
| 5.0c (HDR, TAA, tonemap) | 3.6, 3.7 | 5.1-5.3 (lights) | 5.5 |
| 5.0d (ambient specular) | 3.4 | 5.4 N1 (time of day) | 5.4 |
| 5.0e (RT shadows, RT AO) | 4.3, 4.4 | 5.4 N2, N3 (light loop, car lamps) | 5.5 |
| 5.0f (aerial perspective) | 3.3 | 5.4 N4 (stage lights) | 11 #27 |
| 5.0g (auto exposure) | 3.6 | 6 (car) | 6 |
| 5.0h (TAA filter, sharpen) | 3.7 | 7, 7.x (native tracks) | 9 |
| 5.0i, 5.0j (units, multiple scattering) | 3.2, 3.5 | 8, 8.0 (module layout) | 1.2, 1.3 |
| 8.0b (readout) | 10.2 | 8.1, 8.2, 8.2a (editor) | 8 |
| 9 (phasing) | gone; design.md §9 | 9b, 10 (TODO, open questions) | 11 |

The rest of the old numbering described the plugin and is gone: §0, §1.1-1.7, §1.9, §2 (bridge, IPC, tagging,
camera interception).
