# NGG renderer — design record

> **Moved here from plugin-rbr** (`doc/rendering-overhaul-design.md`, last changed there in commit `e756b11`) on
> 2026-10-29. The renderer now lives in WRally (`modules/renderer/`: ngg-core, the `.ngg` format, the WRenderer
> module), and new renderer work is recorded **here**. "NGG design §N" in WRally's docs and "design N" in
> ngg-core's comments mean the sections of this file.
>
> Everything up to the move is kept as written, so it describes the renderer **as an RBR plugin**: the d3d9 proxy
> (`ngg/bridge`), the IPC block, `rbrtm.exe` as launcher and host, the RBRTM plugin, and paths in plugin-rbr's tree
> (`ngg/...`, `tools/...`). Those parts stayed in plugin-rbr and are not in WRally (see `imported.md`).

*Original title: Modern renderer for RBR — design*

Status: **design draft, rev 4**. P0 in progress. `tools/interop-probe/` and `ngg/bridge/` are real, build, pass
their checks, and RBR itself now runs on D3D12 through the proxy.
Name: **NGG - New Generation Graphics**, following the RBR community's existing NGP
(New Generation Physics). Deliberately not tied to any site or organisation, and deliberately
not `RX`-adjacent - `rbr_rx` already exists and an earlier working title (`orx`) read as "o-RX".

**Build and verify what exists so far**

```
tools/interop-probe/build-probe.ps1     # 17 checks: cross-process D3D12<->D3D9 interop (1.2)
ngg/build-ngg.ps1                       # builds BOTH halves (d3d9.dll + ngg-host.exe) + proxycheck
ngg/build-ngg.ps1 -Deploy               # also installs both into the game dir
ngg/bridge/build-bridge.ps1             # bridge only
```

`proxycheck` is the end-to-end test and needs no game: with `EnableHost=1` it drives the full path — bridge
creates the shared texture on the 9on12 device, launches the 64-bit host, the host renders a known marker, the
bridge unwraps the D3D9 backbuffer and copies into it — then reads the pixel back through D3D9 and verifies it.

Deployed proxy is one file; remove `<gamedir>\d3d9.dll` to undo completely. It writes `ngg-bridge.log` beside
itself, and reads an optional `ngg.ini` (`[ngg] Enable9On12=1, Log=1`).

Goal: render the track environment (and later the car) ourselves with modern techniques, driven by a converted
asset format that stores geometry + materials and **no baked lighting**, while the game keeps drawing its UI and
menus. Target quality bar: current-generation, including believable night lighting, which the original renderer
never had.

Rev 2 incorporates these decisions:

| Decision | Consequence |
|---|---|
| 64-bit address space for textures | Renderer moves **out of process** into a 64-bit host. This is the single most structural decision in the document. |
| VR supported | Renderer owns stereo and OpenXR; the game must be driven to render the car twice per frame. |
| No compatibility burden for legacy D3D9 plugins | We may take the D3D9 device over aggressively. Detect and **warn**, don't accommodate. |
| Prepare for car rendering, implement later | Tagging, format and material system must cover the car from day one; only the renderer work is deferred. |
| Unlimited named weather/daylight presets, each mapping to a chosen `.MAT` | Weather becomes a first-class authored entity, decoupled from RBR's 4 time-of-day variants. |
| Night with realistic lights | Forces a many-light architecture (clustered), emissive materials, punctual light data in the format, and a light-authoring workflow. |
| Encryption postponed, not closed | Container gets an envelope + key-id field now; no crypto implemented. |
| Converter lives in Wallaby | Wallaby becomes the authoring tool: convert, override materials, place lights, preview, validate. |
| Be ready for AI-generated tracks | `.ngg` must be a **first-class authorable format**, not merely a conversion output. Conversion becomes one producer among several (§7). |

Rev 3 settles the remaining questions:

| Decision | Consequence |
|---|---|
| **DX12 Ultimate baseline** (mesh shaders, sampler feedback, hardware RT) | Deletes whole subsystems: CSM as the primary shadow path, SSAO, the CPU culling hierarchy, and most of the un-baking work (§1.8). Floor is ~RTX 20-series / RDNA2. Creates one real tension: RT vs. VR at 90 Hz (§1.8.4). |
| **Vanilla RBR + our plugin only; no RSF** | We own device creation outright → `d3d9.dll` proxy becomes the default hooking method, and the clean D3D9On12 path is available without negotiation. Implies our own distribution/launcher. |
| **Auto-derived lights + editor for key tracks** | P3 needs a real Wallaby light-placement mode with live preview, not just the extraction pass. |
| **Both AI-assisted asset upgrade *and* fully generated stages** | Full generation is in scope, so the minimum-viable-LBS experiment moves onto the **critical path in P0**, and Wallaby must generate gameplay data (drive line, collision, triggers, pacenotes), not only visuals. |

Rev 4 — first measurements and one scope relaxation:

| Change | Consequence |
|---|---|
| **Interop verified by experiment** (§1.2) | Rev 3's directional constraint was avoidable. Shared textures are created by *us* on the 9on12 device; the D3D9↔D3D12 hop stays in-process. Nothing the game creates needs to be shareable. P0's #1 risk is retired. |
| **The car is not rendered at all in early phases** | Much larger simplification than it appears: with no car in the frame there is nothing to depth-composite against, so **INTZ, the depth write-back pass, and stencil car-harmonization all leave the P1 critical path** (§2.4). P1 renders our world plus the game's UI, nothing else. |

**Answering the question directly: no, keeping a D3D9 path would not close the door to D3D12 later — but given the
decisions above, a D3D9 renderer backend is now throwaway work and should not be written at all.** D3D9 survives in
this design only as the *interception and hand-off surface*, never as a renderer. See §1.

---

## 0. What we already have

| Asset | Where | Why it matters |
|---|---|---|
| `ParseLBS` / `ParseCOL` detours | [RBRTMHelp.h:717](../RBRTMHelp.h#L717), [RBRTMPlugin.cpp:2414](../RBRTMPlugin.cpp#L2414) | Gives us the raw LBS stream and the track filename during load — useful for reading our sidecar. **Not** a place to tag rendering resources: the parse creates no D3D objects (§2.1). |
| `LoadFileMem` / `CreateFileA` / `fopen` hooks | RBRTMHelp.h | File redirection already exists — this is how a weather preset swaps the `.MAT` (§3.4) with no new plumbing. |
| Full LBS/COL/MAT/FNC/DLS/SGS parsers in C# | `wallaby/Fileparser.cs`, `wallaby/MapObjects/**` | The converter reuses these instead of re-deriving formats in C++. |
| Vertex-format table | `wallaby/MeshTools.cs:181` | Exact layouts for mesh types 3/4/6/7/8/9/11/14/17 — which slot is baked diffuse, which UV is the shadow map, where the foliage-sway parameters live. |
| `.MAT` surface maps | `wallaby/MapObjects/MAT/Materials.cs` | Per-texel surface classification per weather. Ground truth for automatic PBR material assignment. |
| TOD variants `_M` / `_N` / `_E` / `_O` | `RBRTMCheckers.h:139` | Same geometry baked under 4 lighting conditions → the un-baking solve (§3.2) **and** the night-light seeding trick (§5.3). |
| Tournament CRC/key infrastructure | `Tournament.*`, `RBRTMCheckers.h` | The fairness profile (§3.6) plugs into this rather than inventing a parallel mechanism. |

Known gap: `DLS/AnimationSetItem.cs` is a stub — DLS stores only animation *set names*; the curves live in SGS
files that `SGS/AnimMesh.cs` parses only partially. §4 is built around that fact rather than wishing it away.

---

## 1. Architecture: two processes

```
┌─ RBR.exe (32-bit) ────────────────────────┐      ┌─ ngg-host.exe (64-bit) ─────────────┐
│                                            │      │                                     │
│  game logic, physics, audio  (untouched)   │      │  D3D12 renderer                     │
│                                            │      │   · all track geometry + materials  │
│  ngg-bridge (our DLL, inside the plugin)   │      │   · shadows, clustered lights, GI   │
│   · d3d9 interception                      │◄────►│   · sky/atmosphere, night lighting  │
│   · resource tagging + draw suppression    │ IPC  │   · post/tonemap                    │
│   · camera + transform harvesting          │      │   · stereo + OpenXR                 │
│   · car/UI rendered into shared RTs        │      │   · texture streaming (>4 GB)       │
│   · composite + Present (flat screen)      │      │   · composite + submit (VR)         │
└────────────────────────────────────────────┘      └─────────────────────────────────────┘
              shared textures (colour, linear depth, car, UI)  +  shared-memory frame state
```

### 1.1 Why out-of-process

You asked for 64-bit address space for textures. RBR is a 32-bit executable and cannot be made otherwise, so the
renderer has to live somewhere else. Worth being precise about what this does and doesn't buy:

- The 32-bit ceiling limits **CPU-side working set**, not VRAM. A 32-bit process can technically drive a 10 GB
  texture set by streaming through small upload buffers.
- What it actually costs you in-process: no memory-mapping whole asset packages, constant fragmentation pressure,
  bounded decompression/staging pools, no room for CPU-side caches (BVH, probe data, LOD pools), and every
  third-party library you'd want is happier at 64-bit.
- The out-of-process split additionally gives: modern toolchain and debuggers for the renderer, a crash in the
  renderer that doesn't take the game down (fall back to vanilla and keep the session alive — valuable for a
  competition platform), and worker threads that cannot stall RBR's game loop.

So: it's the right call, and the incidental wins are worth more than the address space itself.

### 1.2 Interop — VERIFIED, and simpler than rev 3 assumed

> **Rev 4: measured, not assumed.** `tools/interop-probe/` implements this section as a headless test and it now
> passes end-to-end on AMD Radeon / Windows 11 26200. Re-run it with `tools/interop-probe/build-probe.ps1` on any
> new driver or GPU; it is the executable version of this section.

Rev 3 assumed a hard directional constraint — D3D9 creates, D3D12 opens, never the reverse — and built the whole
architecture around it. **That constraint turned out to be avoidable**, because unwrapping is an *in-process*
operation. The verified design is:

1. **The renderer's shared textures are created by us on the game process's 9on12 D3D12 device**, with
   `D3D12_HEAP_FLAG_SHARED`, and shared to the 64-bit host by NT handle. This is D3D12 → D3D12 cross-process and
   cross-bitness, which is fully supported. *(probe check C: passes.)*
2. **The D3D9 backbuffer is unwrapped locally, in the game process**, via
   `IDirect3DDevice9On12::UnwrapUnderlyingResource()`, and the shared texture is blitted into it with ordinary
   D3D12 commands, then handed back with `ReturnUnderlyingResource()`. *(probe check D: passes — D3D9
   `GetRenderTargetData` reads back exactly the pixels the 64-bit process wrote.)*

So the D3D9↔D3D12 boundary never crosses the process boundary, and the process boundary is D3D12 on both sides.
Nothing the *game* creates ever has to be shareable.

For the record, rev 3's assumed path is genuinely unavailable: unwrapping a D3D9 texture succeeds, but
`CreateSharedHandle` on the result **fails** — 9on12's internal allocations don't carry `D3D12_HEAP_FLAG_SHARED`.
*(probe check B: not available.)* Had we built on that assumption we'd have discovered it much later and much
more expensively. It stays in the probe as a regression check in case a future Windows build changes it.

**Mechanism: D3D9On12.** Proxy `Direct3DCreate9` → `Direct3DCreate9On12`. The game keeps plain D3D9 semantics
(including `D3DPOOL_MANAGED`, which RBR needs and which D3D9Ex would take away). The old objection to 9On12 —
"it slows the game's draw calls" — is void here, because after suppression the game only draws the UI.

**Fallback, and no longer merely theoretical:** D3D9Ex legacy shared surfaces (secondary D3D9Ex device, legacy
handle across the process boundary, `ID3D11Device::OpenSharedResource` in the renderer). Keep this path alive —
see the 9on12 defect below.

> **Measured defect: 9on12 progressively loses resources.** With `Enable9On12=1`, RBR **stops rendering trees
> after roughly a minute** of driving. With `Enable9On12=0` (same bridge, same wrapper, real D3D9 driver) the trees
> render correctly for the whole stage. So this is D3D9On12, not our wrapper and not the game.
>
> Most likely memory pressure: 9on12 backs `D3DPOOL_MANAGED` resources with its own D3D12 allocations, roughly
> doubling residency, and the test machine is a 2 GB integrated Radeon. That fits "works, then degrades".
>
> Why this probably does not sink the architecture: in the finished design the game's world rendering is
> suppressed, so what still flows through 9on12 collapses to the UI and (later) the car. The pressure that
> triggers this should largely disappear. **But that is a prediction, not a measurement** — it must be re-tested
> once suppression is permanently on, and on a discrete GPU. If it survives that, the D3D9Ex fallback above
> becomes the primary interop path, which is why it stays documented.

**Still required: same adapter.** Both processes must resolve to the same GPU. The 32-bit side reads its LUID from
`ID3D12Device::GetAdapterLuid()` and the 64-bit side binds with `IDXGIFactory4::EnumAdapterByLuid()` — "the default
adapter" is not the same device in both processes on a switchable-graphics laptop. Already implemented in the
probe; refuse to start and fall back to vanilla if it can't be satisfied.

### 1.3 Compositing and who presents

Because all shared textures are created game-side, the compositor's *location* is a switch rather than an
architectural commitment:

- **Flat screen:** the renderer writes the finished frame into a game-created shared texture; the bridge draws it
  as a full-screen quad and the game presents as normal. Window ownership, input, focus and frame pacing are all
  untouched. Lowest risk.
- **VR:** the renderer composites per eye and submits to OpenXR itself; the desktop window shows a mirror via the
  flat path. This is the natural VR shape and avoids the old approach of bolting VR onto a D3D9 device.

Keep both. They share everything except the final 5%.

**When in the frame we composite decides how much of §1.4 we need.** Compositing at `Present` puts our world over
everything the game drew, UI included — fine as a proof that the host path works, useless as a product.
Compositing at the game's **first draw call of the frame** lands us after its `Clear` but ahead of everything it
draws, so the UI paints over us through ordinary D3D9 draw order. No shared depth, no draw-order negotiation, no
cooperation from the game at all.

**This works for 2D and only for 2D.** Measured in-game: with the world suppressed and the UI composited this
way, the UI is correct — but every piece of *3D* content the game still draws (fences, 3D course markers) floats
on top of our world, because it depth-tests against a buffer that knows nothing about what we drew. Ordinary draw
order gets us layering, not occlusion.

So the rule is narrower than "anything drawn after us composites for free":

| game content drawn after our composite | works without shared depth? |
|---|---|
| 2D UI, HUD, menus (no depth test) | **yes** |
| 3D geometry that should be *occluded by* our world | **no — needs §1.4** |
| 3D geometry that should *occlude* our world | **no — needs §1.4** |

Two ways out, and they are not alternatives so much as a sequence:

1. **Suppress it too** — correct while the plan is that we eventually draw everything anyway. Costs nothing and
   is the right move for the content we are about to convert (scenery, movable meshes). This is what P1 does.
2. **Export depth (§1.4)** — unavoidable the moment the game keeps 3D content we do *not* intend to own. The car
   is the obvious case, and so is any object we have not converted yet but still want on screen.

The practical consequence: §1.4 is deferrable exactly as long as the suppression list is *complete*. Every piece
of game 3D content we leave drawing is a thing that will render wrongly until depth export exists.

### 1.4 Depth

> **Rev 4: not needed until the car comes back.** Depth exchange exists solely so the game's car can occlude and
> be occluded by our world. With the car absent from early phases there is nothing to composite against — P1 draws
> our world, then the UI on top, and needs no depth interop whatsoever. Everything below applies from the phase
> where the car returns (P3 harmonization, or P5 takeover), and is kept here so that phase isn't designed from
> scratch. **Do not build it in P1.**

#### What actually forces depth interop

§1.3 establishes that game 3D content drawn after our composite renders wrongly without shared depth. That is
not, however, an argument for building depth exchange — it is an argument for *not leaving that content to the
game*. Split what remains by a single question: **can we reproduce it from source data we can read?**

| game 3D content | source | verdict |
|---|---|---|
| 3D pacenote marks, direction signs, distance markers | generated at runtime from the **`.trk`** driveline + scripts | **re-implement** |
| fences, scenery, movable meshes | `.lbs` blocks 1/1, 1/3, 1/6 | **convert** (in progress) |
| the **car** | live physics state — suspension travel, wheel rotation, damage deformation, dirt | **needs depth interop** |

The marks are the instructive case. They are *not geometry in the track file* — the game builds them from the
driveline spline and script markers in the `.trk`, which uses the **same block framing this project's converter
already parses** (type 2 subtypes 10–15: `DrivePoints`, `DriveLine`, `Scripts`, `ScriptsObjects`; type 3/20).
Drawing them ourselves needs no interop, no asset conversion, and no cooperation from the game — just a reader
for data we can already frame.

So depth interop's scope collapses to **exactly one thing: the car.** Everything else is either convertible or
regenerable. That is a much stronger reason to defer §1.4 than "the car is absent" — it means §1.4 will never be
needed for anything *except* the car, and if we eventually own the car too (P5) it may never be needed at all.

**Fairness makes this the preferred answer, not merely the cheaper one (§3.6).** Pacenote marks are gameplay
information. Left as a game-drawn overlay we composite around, what the driver sees is outside our control and
outside any audit. Regenerated by us from the same source data under the sanctioned profile, their appearance is
deterministic and checkable. A renderer that could quietly emphasise or suppress course markers would be a
competitive-integrity problem; owning them removes the question.

**The cost, and it is real:** pacenote marks are safety- and gameplay-critical. Placement or orientation that
differs subtly from vanilla would mislead drivers who learned a stage. This needs the same
screenshot-diff-against-vanilla validation as the lighting solve (§3.2), and that validation is a requirement of
the work, not a refinement of it.

**Our own depth buffer is reverse-Z** (`D32_FLOAT`, near→1, far→0, clear 0, `GREATER`). RBR hands us a
conventional projection, which is the worst possible pairing with a float depth buffer: float precision is
densest near 0 and a conventional projection already crowds distant geometry there, so the far field ends up with
almost no usable bits. Reversing puts float's dense region where the projection is sparse and the two curves
cancel. The bridge does not need to know — the host flips the matrix it receives (`col2 = col3 - col2`, row-vector
convention) rather than asking the game to change anything.

> When depth interop below is eventually built, remember the game's depth is **not** reverse-Z; the resolve has to
> flip back.

Compositing the game's car against our world needs shared depth, and depth-stencil formats are not shareable.
Solution both directions:

- Ours → game: the renderer writes **linear depth into a shared `R32_FLOAT` colour texture**; the bridge runs a
  tiny full-screen pass that writes `oDepth` into the game's depth-stencil so subsequent game draws depth-test
  correctly against our world.
- Game → ours: force the game's depth-stencil to **INTZ** (readable depth, a long-standing FOURCC hack supported
  on all three vendors) via the proxy, then resolve it into a shared `R32_FLOAT` for the renderer's post passes.

### 1.5 Frame protocol

One shared-memory control block plus two fences per frame. Per frame the bridge sends: view/projection per view
(and per eye), time, weather preset state, car transform, and the harvested transform table (§4.1). The renderer
signals completion; the bridge composites. Synchronous by default — the GPU work is the same as in-process, only
the submission crosses a process boundary, and a shared D3D12 fence makes that cheap.

Non-negotiable rule: **the bridge never blocks on GPU readback in the frame path**, and a renderer that misses its
deadline gets the previous frame's image rather than stalling RBR's game loop.

### 1.6 One protocol, three clients

The same IPC surface serves the game bridge, **Wallaby's preview viewport**, and a standalone viewer. That means
the editor previews through the exact renderer that ships, and it sidesteps Wallaby's 32-bit SlimDX/WinForms
viewport entirely — Wallaby keeps its existing D3D9 editing view and launches `ngg-host` for preview. This is a
large practical payoff and is the reason to build the IPC boundary early rather than "extract it later".

### 1.7 Hooking method and legacy plugin detection

Because the target is vanilla RBR plus our own plugin — no RSF, no third-party D3D9 hookers — we take the
strongest option: **a `d3d9.dll` proxy**, which owns device creation and therefore makes the D3D9On12 path in §1.2
available without negotiation. (Under an RSF-compatibility requirement this would have had to be cooperative
vtable hooking instead; that constraint is gone.)

At init, enumerate loaded modules for known hookers (RBRVR, ReShade and other `d3d9.dll` proxies, RBRHUD,
NGPCarMenu) and check whether device vtable entries point outside `d3d9.dll`. On detection: log it, show an
on-screen warning naming the module, and offer to continue in vanilla rendering. We warn; we do not interoperate.

### 1.8 Renderer feature baseline: DX12 Ultimate

Baseline is mesh shaders, sampler feedback and hardware ray tracing — roughly RTX 20-series / RDNA2 and newer.
This is the decision that most shapes `ngg-core`, and it mostly *removes* work:

**1.8.1 Mesh shaders make our cluster format the primary geometry path.** `NGGC` meshlets stop being an
optimisation and become the representation: amplification shader does cluster culling (frustum, normal cone,
occlusion) on the GPU, mesh shader emits triangles. Consequences: the `NGGN` CPU BVH shrinks to coarse
streaming/residency decisions only, and per-draw CPU cost — the thing that makes 2004-era geometry slow — largely
stops mattering. The §5.9 re-batching work gets easier, not harder.

**1.8.2 Sampler feedback streaming is the answer to the 64-bit texture requirement.** Tiled resources plus
feedback maps means we stream only the mip tiles actually sampled, which is exactly what an open rally stage with
long sightlines wants. This is the mechanism that makes "more texture data than fits in VRAM" work properly rather
than by heuristic prefetch.

**1.8.3 Ray tracing deletes most of the un-baking problem.** With RT shadows, RT ambient occlusion and RT global
illumination, we no longer need to *recover* occlusion from the baked vertex colours — we compute it. That
substantially reprioritises §3.2 (see the note there) and removes CSM, SSAO and the irradiance probe grid from the
critical path. RT reflections also matter more here than in most games: wet tarmac and car paint are the two
surfaces players look at constantly.

**1.8.4 The one real tension: ray tracing versus VR at 90 Hz.** Stereo at 90 Hz with a full RT lighting pipeline
is demanding even on baseline-Ultimate hardware. Do **not** resolve this by writing two renderers. Resolve it with
**quality tiers over one technique-agnostic lighting interface**: RT-heavy for flat screen, RT-light for VR
(RT shadows only, screen-space or probe-based indirect), with the same material and light data feeding both. The
tiering must exist in the architecture from P1 even though VR lands in P4 — retrofitting it means rewriting the
lighting pass.

---

### 1.8.5 The ray-tracing probe — MEASURED, and the cost is not the problem

`ngg/core/rt.h/.cpp` plus `ngg/core/dxc.h/.cpp`. Built to answer one question before anything is designed on top
of it: **what does ray tracing actually cost on the floor hardware?** §1.8.3 already makes RT the baseline and
takes SSAO, CSM-as-primary and the probe grid off the critical path — but that argument was never checked
against a number.

**Measured on the development machine — an integrated AMD Radeon, which is at or below the intended floor:**

| | |
|---|---|
| adapter | raytracing **tier 1.1**, highest shader model **6.7** |
| BLAS | **92.2 MB** over 594,058 triangles (`track-108`), built **once** at load |
| TLAS | 1.8 KB, one identity instance |
| one shadow ray per pixel | **0.47 ms** for 194k rays, **1.8–2.0 ms** for 967k rays |

That is roughly **2 ns per ray**, linear across a 5× range, on an integrated GPU. Against a 44 ms frame it is
1–4%. **The cost objection to the RT path does not survive contact with a measurement.**

**Three prerequisites, and only the second was a surprise:**

1. **Tier 1.1 + SM 6.5** for inline RayQuery. Confirmed on the adapter, and `Device::Create` now logs both at
   startup rather than letting a feature discover it by failing to create.
2. **A SECOND SHADER COMPILER.** Everything here compiles with `D3DCompile` (FXC), which tops out at SM 5.1 and
   cannot emit DXIL at all. So the RT path does not begin with an acceleration structure — it begins with DXC,
   loaded at runtime so a machine without it gets one log line instead of an executable that will not start.
   `dxil.dll` has to travel with it: without the signing library DXC still produces DXIL, but leaves it
   *unsigned*, and the runtime rejects unsigned DXIL outside developer mode — a works-on-this-machine-only
   failure worth a file copy in `build-ngg.ps1` to avoid. FXC stays for every other shader; porting them
   wholesale would be a large diff with no visible change.
3. **Geometry in a shape a BLAS can take.** Free, and that is a property of the format rather than luck: `.ngg`
   bakes every transform into world space, position is the first 12 bytes of the vertex, and indices are
   `R32_UINT`. **One BLAS, built once, never refitted** — the same rigidity that let TAA skip motion vectors
   (§5.0c). When per-object transforms land (§9b item 7) the TLAS starts needing a per-frame rebuild, and that
   is the line to remember.

**The first ray is a SHADOW ray, not ambient occlusion, and deliberately.** Its answer can be checked against
the cascaded shadow map, which we already have. AO has nothing to be checked against, so it is the wrong thing
to write first.

> **The correctness check caught a real bug, and the counter was right.** First run: ray count stable at a
> static camera, but the shadowed fraction cycled 32.6% → 51.9% → 69.0% → 37.2% and back, with camera and sun
> both logged as unchanged. The obvious reading — after §8.0b — was a broken instrument. It was not.
>
> The rays were being reconstructed with the **unjittered** view-projection (`Scene::LastViewProj`, which is
> unjittered *on purpose* for TAA, §5.0c) while the depth buffer was rasterised **with** the jitter. For TAA that
> half-pixel disagreement is the accepted error. For a ray origin it is a half-pixel of *world space* —
> centimetres on a grazing road surface, the same size as the 5 cm bias meant to clear it — so self-intersection
> flipped in and out with the jitter phase and took a third of the screen with it. Same camera, different answer
> every frame.
>
> `Scene::LastViewProjJittered` now exists beside the unjittered one, and the rule is written on both: **TAA
> reprojects into a stable grid and must not see the jitter; anything reconstructing a real surface point from
> depth must.** After the fix: **13.6% shadowed, identical on every sample.** Views 11 and 12 (`V`) put the RT
> visibility and its disagreement with the shadow map on screen — red where the rays say shadow and the map
> says lit, blue the reverse, grey where they agree.

**Foliage is cut out by an any-hit test now, and it cost nothing measurable.** The BLAS holds two geometries —
opaque (`GEOMETRY_FLAG_OPAQUE`, the traversal commits on its own) and alpha-tested (no flag, every hit surfaces
as a candidate the shader accepts or refuses). `Scene::BuildRtInputs` splits the index buffer into the two
contiguous ranges a BLAS geometry needs, and hands over a per-triangle material index, a `{texture, cutoff}`
table and every scene texture; the shader reconstructs uv0 and vertex alpha from the barycentrics and runs *the
rasteriser's own test* — texture alpha × vertex alpha against the material's cutoff — so the shadow is cut out by
exactly the pixels the leaf was drawn with. Same 0.47 ms.

Measured, and worth keeping because it corrects an intuition: **13.6% of pixels shadowed with solid cards →
1.3% with the test, and the test passes 43% of the candidates it sees** (2,422 pass / 3,259 fail), so it is
discriminating rather than rejecting. On `track-108` 376k of 594k triangles are alpha-tested; at the start line
the old shadow was mostly *grass* blocking the ground between its own blades. Cut out properly, grass casts
almost nothing at a 49° sun — of 2,614 shadowed pixels ~2,420 are leaf hits and opaque geometry contributes
about 200. The pass/fail counter stays: it is the one number that distinguishes "cutting leaves out" from
"silently rejecting everything", and those look identical on screen from a distance.

Two approximations, both recorded: the alpha texture is sampled at a **fixed LOD 2** (a shadow ray has no
screen footprint to derive a mip from, and mip 0 on a 256² leaf card gives a shadow with more detail than the
leaf on screen has), and the **bias is along the ray**, not the normal, because there is no normal buffer yet.

## 2. Separating our world from the game's car and UI

### 2.1 Resource tagging (the core mechanism)

Do **not** classify draw calls heuristically by texture name, primitive count or render state. That approach is
fragile and is why most "replace the world" mods die.

> **Rev 4 correction — the earlier mechanism in this section was wrong.**
> Revisions 1–3 proposed bracketing RBRTM's `ParseLBS` detour as a "tagging window": tag every D3D resource
> created while the parse runs. That rests on the assumption that parsing uploads geometry to D3D. It doesn't.
> **Measured: every vertex buffer, index buffer and texture in RBR is created inside `HedgeHog3D.DLL`** — the
> Hedgehog engine RBR is built on — not in the exe. `ParseLBS` parses the file format; it creates no D3D
> resources, so the bracket would have tagged nothing. (Bracketing `ParseCOL` was doubly wrong: COL is
> collision/physics data and never reaches the renderer at all.)

**The mechanism that actually works: tag by creating call site.**

1. Every `CreateVertexBuffer` / `CreateIndexBuffer` / `CreateTexture` records `_ReturnAddress()` — the exact game
   function that asked for the resource — as a *call site*, identified by its **RVA within its own module**
   (stable across runs; raw addresses are not, since `HedgeHog3D.DLL` relocates every launch).
2. The site index is written into the resource's private data as its tag.
3. At draw time, the bound stream-0 VB's tag says which function created its geometry. Suppression is then
   "skip draws whose vertex buffer came from site X".

This is better than the window it replaces on every axis: it needs **no cooperation from the plugin**, no
modification to anti-cheat-adjacent hook installation, no assumption about load ordering, and it states what a
resource *is* rather than when it happened to be made. It also extends to the car for free — a different creating
function, a different tag.

> **Measured: creation call sites, one screen, one session.** 27 sites total. Per-frame draw attribution:
>
> | site | module | RVA | created | draws/frame |
> |---|---|---|---|---|
> | 19 | HedgeHog3D.DLL | `0x08881C` | 450 VBs, 14.4 MB | **242** |
> | 22 | HedgeHog3D.DLL | `0x0876D4` | 8 VBs, 1.25 MB | **152** |
> | 9 | HedgeHog3D.DLL | `0x0135D8` | 17 VBs, 295 KB | 20 |
> | 21 | HedgeHog3D.DLL | `0x0B9BC3` | 2 VBs, 47 KB | 7 |
> | 0 | RichardBurnsRally_SSE | `0x017DCA` | 44 VBs, 4.9 MB | 4 |
>
> Two sites account for ~95% of all draws. That is a very encouraging shape for suppression: if one of them turns
> out to be the track, the partition is a single config line. Identifying which requires a **stage load** —
> the table above is from a non-driving screen.

> **Measured in live stages: the engine partitions the world by content type, and each part is separately
> suppressible.** Determined by suppressing one creating call site at a time in-game and observing. The cycle order
> is sorted by (module, RVA) so it is reproducible across runs and track positions.
>
> | module | RVA | draws | notes |
> |---|---|---|---|
> | RichardBurnsRally_SSE | `0x017DCA` | **UI** | keep |
> | RichardBurnsRally_SSE | `0x20513D` | unidentified | 2 buffers, ~9 MB, ~3 draws/frame |
> | HedgeHog3D | `0x0135D8` | **car shadow** | external view only |
> | HedgeHog3D | `0x023511` | unidentified | external view only |
> | HedgeHog3D | `0x029780` | **car shadow**, and gates the **sun glare** | suppressing it makes the glare appear — almost certainly the occluder geometry behind an occlusion query |
> | HedgeHog3D | `0x0876D4` | **water, trees, grass, far scenery, animated objects** | |
> | HedgeHog3D | `0x08881C` | **car AND road/ground** | |
> | HedgeHog3D | `0x0B9BC3` | **skybox** | |
>
> Three consequences that change planning:
>
> 1. **The car shares a creating function with the ground** (`0x08881C`). Suppressing the world therefore takes the
>    car with it. "Don't render the car in early phases" (rev 4) turns out to be not just acceptable but *forced* —
>    and when the car does come back it cannot simply be left to the game while we replace the ground.
> 2. **`0x0876D4` is coarse** — water, trees, grass, scenery and animated objects all come from one function, so
>    that content cannot be phased in separately. Ground (`0x08881C`) and sky (`0x0B9BC3`) still can.
> 3. **Two sites appear only in external view**, so the site set is view-dependent. Any shipped site table must be
>    a set keyed by RVA, never an ordered list with indices.
>
> Method note, learned the hard way: an earlier version ordered the cycle by draws/frame. The two biggest sites
> differ by one draw (449 vs 450) and swapped places mid-session, making a whole round of observations
> unattributable. **Never order a diagnostic by a quantity that fluctuates.**
>
> **Caveat: RVAs are per-binary — RESOLVED by a content check.** Valid only for the exact `HedgeHog3D.DLL`
> measured. This looked like an open-ended fragility, but it is not: RBR has been unsupported since 2004 and
> every install ships identical files, so there is effectively **one** binary to recognise. That turns the
> problem from "the addresses might drift" into "is this the build we measured" — a yes/no answered before
> anything is suppressed.
>
> Implemented as a gate on size, PE timestamp **and a CRC32 of the file on disk**, checked once at first use:
>
> | binary | bytes | CRC32 |
> |---|---|---|
> | `HedgeHog3D.DLL` | 2,007,040 | `0xD4308BAB` |
> | `RichardBurnsRally_SSE.exe` | 4,378,624 | `0x31A75453` |
>
> The CRC is not redundant with size + timestamp: a patched binary can preserve both, and a wrong suppression is
> close to undebuggable from a screenshot — some arbitrary function's geometry silently disappears. On any
> unrecognised build the bridge logs which module failed and **refuses to suppress at all**, so the game renders
> normally rather than incorrectly. The F9 cycler is gated too, since it steers by the same site indices.
> `AllowUnknownBinaries=1` overrides, which is only useful for adding a new known-good build to the table.
>
> The CRC is taken over the **file**, not the mapped image: the mapping carries relocations and pages other
> plugins may have modified, so it would not be reproducible.

#### A second axis: vertex transform state — VERIFIED, and build-independent

Call site is not always fine enough. **Measured: RBR draws its 3D course markers from the same creating call
site as its UI** (exe `0x017DCA`), with the same texture atlas, so `SuppressSiteRva` cannot remove one without
the other. The frame dump for that site:

| draws | triangles | FVF | position type | what |
|---|---|---|---|---|
| 8 | 2 | `0x142` | **`XYZ`** — untransformed | 3D course markers |
| 1 | 214 | `0x144` | `XYZRHW` — screen space | UI |
| 1 | **2** | `0x144` | `XYZRHW` — screen space | UI |
| 1 | 274 | `0x144` | `XYZRHW` — screen space | UI |

Note the markers share a triangle count with a UI element, so "suppress 2-triangle draws from this site" would
take a piece of the UI with them. The reliable key is the **vertex transform state**:

- `XYZRHW` / `POSITIONT` — vertices are *already in screen space*. No view or projection applies, so by
  definition the draw is an overlay and cannot be part of our world.
- `XYZ` / `POSITION` — world space, i.e. content that belongs in our scene.

Implemented as `SuppressDraw=RVA:3D` (also `:2D`, and `:N` for a triangle count when neither fits). `SetFVF` and
`SetVertexDeclaration` are intercepted so the state is tracked as it is set, keeping the draw path free of COM
getters; the declaration path scans elements for `POSITIONT`, so it works whichever API the game uses.
**Verified in-game: markers gone, UI intact.**

> **One caution on how this was arrived at.** Tested *globally*, the transform test looks useless — only 3 of 152
> draws in a frame are transformed, because of course nearly everything is world space. It is only meaningful
> **within a call site**, as a tiebreaker once the site has already narrowed the candidates. Site first, transform
> state second.

This partly answers the RVA-fragility caveat above: world-space vs screen-space is a property of *what the
content is*, not of where the code happens to live, so it carries across builds in a way an RVA never can. It
does not replace the site table — it needs the site to narrow the field first — but it means the per-binary part
of the mechanism does less of the work.

Everything else passes through untouched, and the design degrades safely: anything tagging misses simply renders
the old way — visible and ugly, but not broken.

Per suppressed draw we record `(VB, IB, base vertex, start index, prim count, world matrix, textures, states)`.
That tuple is simultaneously the **identity key** into the converted asset and the source of animation transforms (§4).

> **Measured (P0a): RBR creates its device with `D3DCREATE_MULTITHREADED`** (behaviour flags `0x44` =
> `HARDWARE_VERTEXPROCESSING | MULTITHREADED`), so every structure here must be thread-safe. In practice all draw
> calls observed so far come from a **single** thread, so the fast path is uncontended — but correctness cannot
> rest on a game that merely happens not to use the freedom it asked for.
>
> **Tags live in the resource's own private data**, not a side table keyed by pointer. RBR creates and destroys
> geometry constantly; a pointer-keyed map lets a recycled address inherit a stale tag, which would misattribute
> draws in a way that is almost impossible to debug. Private data dies with the resource. Cost is one
> `GetPrivateData` per draw, absorbed by a one-entry cache since consecutive draws overwhelmingly share a buffer.

> **Measured (P0a): what a frame actually contains.** In-game, at 1024×728:
>
> | | |
> |---|---|
> | draw calls / frame | ~355 average, 400 in the captured frame |
> | triangles / frame | ~261k average, 305k captured |
> | unique vertex buffers | 129 |
> | unique textures | 55 |
> | draw threads | 1 |
>
> The shape matters more than the totals: one large draw (3968 tris) followed by hundreds of tiny ones — 14, 20,
> 30, 36, 44 triangles each — with the *same* vertex buffer rebound repeatedly and only the index range and
> texture changing. That is exactly the fragmentation §5.9 predicted, and it confirms re-batching in the converter
> is worth real effort. It is also encouraging for suppression: 129 buffers is a small enough set that tagging is
> cheap and auditable.

Extend the same mechanism to the car load path now, even though the car renderer comes later. Tagging is cheap;
retrofitting identity into an already-shipped asset format is not.

### 2.2 Camera — RESOLVED by measurement

Needed exactly, or the car swims relative to the world. The rev 3 text guessed this would be a hard RE task. It
isn't:

> **Measured (P0a):** RBR sets `D3DTS_VIEW` and `D3DTS_PROJECTION` through **`SetTransform`**, and the values are
> real. Sanity check that confirms it: the captured projection gives `proj[0][0]=1.5968`, `proj[1][1]=2.2460`, a
> ratio of **1.4066 — exactly the 1024/728 backbuffer aspect**. A wrong or stale matrix would not land on the
> aspect ratio by chance.
>
> RBR *also* uploads vertex-shader constants (`c0`, `c4`, `c8`, 4 vectors each) that look like a per-draw composed
> world-view-projection — the `w` component tracks per-object translation. So both paths exist. **Use
> `SetTransform` for the camera** and treat the VS constants as per-object transforms, which is exactly the
> harvesting source §4.1 needs.

Reading the game's camera structs directly (`StageObj` `0x007c36a8`, `CarObj` `0x007c28cc`) stays available as a
cross-check, but is no longer on the critical path.

> **Measured (P1, test scene): RBR's runtime world space is Y-up, and every stage has its own arbitrary origin.**
> A flat test grid on the Y=0 plane renders horizontal, not as a wall, so the runtime is Y-up — Wallaby's Y/Z swap
> is a *file*-space concern and does not apply to the runtime camera. Camera positions are per-stage arbitrary and
> **negative Y is normal** (stage start heights differ, some below zero).
>
> Two consequences for the converter: it must not assume anything about world origin or ground altitude, and
> `.ngg` geometry has to be emitted in the *same per-stage world space the game uses at runtime* — the camera we
> receive is in that space, and nothing rebases it.
>
> **Confirmed under motion.** A world-locked test grid rendered by the 64-bit host now sits correctly under the
> car on every stage tried, and tracks the world while driving — sliding underneath and rotating about the car
> exactly as ground should. The camera contract with RBR is settled: `SetTransform` matrices, used unchanged as
> row-major with row-vector multiplication, are sufficient to place geometry in the game's world.
>
> One bug worth remembering, because its symptom was misleading: recovering the eye position from the view matrix
> initially indexed columns instead of rows (`-t*R` rather than `-t*R^T`). The result was not obviously broken —
> the position had a plausible magnitude but pointed elsewhere, and because the error depends on camera heading it
> looked like a *per-stage* problem ("below us here, in front of us there"). Transposed-matrix bugs rarely look
> like bugs; the fix is guarded by a one-time dump of the view matrix rows and their lengths.

Multi-view from day one: rear-view mirror, replay cameras, and both VR eyes. Detect via `SetRenderTarget`/viewport
changes, run the environment pass per view. Retrofitting multi-view is painful; designing for it is nearly free.

Coordinate convention: Wallaby swaps Y/Z between file space and render space (`Tools.getSwapedVector`). Pick one
canonical convention in the converter, document it, and never let the swap leak into the runtime.

### 2.3 VR and the car

Stereo needs the car rendered twice per frame with different view matrices. Mechanism: the proxy already
intercepts every non-suppressed 3D draw, so **record them into a command list rather than executing immediately,
then replay the list once per eye** with our injected camera constants into per-eye shared RTs. UI renders once
and is composited onto a VR panel.

This is the single largest piece of VR-specific work and it should be prototyped early (P0c) — if RBR's car
rendering turns out to depend on per-frame state that can't be replayed, VR shifts from "hard" to "needs a
different approach", and that's much better to know in month one.

### 2.4 The mismatched-car problem, before we own the car

> **Rev 4: deferred entirely.** Early phases don't render the car at all, so there is no mismatch to solve. This
> section applies from the phase where the vanilla car is switched back on — if it ever is, since P5 may simply
> take the car over directly and skip the harmonization work below.

Until the car renderer exists, the game draws it with 2004 materials into our modern world. Mitigations:

1. **Stencil-tag car pixels** in the proxy, then apply our fog, exposure and tonemap to those pixels in the
   composite pass. Cheap; gets most of the way.
2. Override the ambient/sun constants the car shader reads, and replace the car's environment/cube map with one we
   render. Bigger payoff for reflections.
3. **Own the car** (§6) — the real fix, deferred.

---

## 3. Assets: format and conversion

### 3.0 The conversion spine: LBS → glTF → `.ngg` — DECIDED

**The shipping path for a converted track is `LBS → glTF → (edit) → .ngg`.** The direct `LBS → .ngg` converter
(`wallaby-cli convert`) still exists, but it is a development shortcut — the fastest way to re-check the pipeline
against the source of truth — and it is expected to be removed. Assume the glTF spine when reasoning about
conversion; §7.2's `authored glTF → .ngg` is then not a second pipeline but the same one entered further down.

```
                 wallaby-cli gltf                        wallaby-cli import
track.lbs ──────────────────────────► project ──────────────────────────► track.ngg
           de-atlas, shading           .glb                                (runtime only)
           overrides, weld,           .scene.json   ◄── Blender / Wallaby
           texture bundle             .textures.zip     (only what automation got wrong)
```

**Why the detour is the point.** The value of the overhaul is in what a human decides — remodelled vegetation,
material classes, normal maps, light placement — and none of that can be expressed against a binary the converter
regenerates from scratch on every run. glTF is where the human works, so it has to be *on* the path, not beside it.

**The ordering is load-bearing: automation first, human second.** The export runs the same de-atlas, shading-override
and weld passes as the `.ngg` path, so the author opens geometry that is already as good as the converter can make
it. Exporting raw and fixing by hand was tried and is backwards — an author would spend the session correcting
normals the converter corrects anyway, then fight it on the next reconversion.

**A project is three files, not one.** `.glb` + `.scene.json` + `.textures.zip`. glTF has nowhere to put an RBR
layer id, a material flag, or a two-stage texture pairing, and a bare `.glb` import silently gives you
scenery wearing foliage flags. The sidecar and the bundle are files only our own tooling writes, which is exactly
why they survive a round trip through a DCC tool that rewrites the `.glb` and drops extensions it does not know.
The player's RBR install is an import-time input only; after that the project stands alone.

**Equivalence is measured, not assumed.** On Joux Verte a project-only rebuild — archive never consulted — matches
a direct `convert` on layers, material flags, materials (119), two-stage bindings (87) and textures (74,
byte-identical, the 45 de-atlased cells included). The one remaining difference is 10 zero-area triangles that
SharpGLTF drops on export, which draw nothing.

**What this changes on the renderer side: nothing in `.ngg`, everything about where requests land.** `.ngg` stays
export-only — never edited, never round-tripped (§3.1). But a converter request from this side is now only real if
it survives the glTF hop, which means it must be either a pass that runs *before* export or a field in the sidecar.
Per-object culling bounds (§3.3e, §9b item 4) are the live example: they cannot live in the `.glb`, so they are
sidecar data or they are recomputed on import.

### 3.1 Principle: sidecar for converted tracks, native for new ones

For a *converted* track, `<track>_<tod>.ngg` sits **next to** the original and carries rendering data only. The
game keeps loading the original LBS/COL/MAT and keeps owning collision, drive line, triggers, scripts, physics
surfaces and timing. For a *native or generated* track the relationship inverts — see §7 — but the runtime treats
both identically, because in each case the game owns gameplay and `.ngg` owns pixels.

- No risk of changing gameplay or invalidating existing tournament CRCs.
- Fallback is trivial: no `.ngg`, or CRC mismatch against the source LBS → render vanilla.
- **The converter must never move a vertex.** Visual/collision divergence is the fastest way to lose a competitive
  community. Additional LOD levels beyond gameplay-relevant distance are the only exception.

### 3.2 Un-baking the lighting

Old vertices carry `D3DCOLOR diffuse` = baked sun + ambient + occlusion, plus for ground meshes a baked shadow
texture on a second UV set (`meshtype 11`: "pos + colour + tx0 + tx1 + shadow tx"). Light dynamically while keeping
those and everything double-darkens.

> **Rev 3 reprioritisation.** With ray-traced AO and GI in the baseline (§1.8.3), we no longer need to *recover*
> occlusion from the bake — we compute it. **Tier 0 becomes the default and ships in P1.** Tier 2 survives, but its
> value is now (a) estimating each variant's sun direction and colour, which we'd otherwise author by hand per
> track, and (b) seeding night light candidates from `_N` (§5.3) — not occlusion recovery. Tier 1 becomes a
> fallback for the VR quality tier, where full RT indirect isn't affordable. This meaningfully shrinks P2.

**Tier 0 — discard and re-derive.** Drop vertex colour. Recompute smooth normals from topology (angle threshold +
UV-seam splitting) and tangents. Correct on convex geometry, flat and wrong wherever the original relied on baked
occlusion (under bridges, inside forests, building interiors).

> **Measured, and it constrains this section: the baked colour is stored at HALF scale, 7-bit precision, and its
> alpha is not lighting at all.** Across 178k vertices of Joux Verte no RGB channel exceeds **128** — a hard cap,
> not a rolloff. Two consequences for the solve: every baked value must be doubled before it means anything, and
> the input is quantised to 0..128, i.e. ~7 bits per channel. That is the precision ceiling on any albedo or
> occlusion recovered from it, and worth knowing before fitting a model that assumes 8-bit inputs.
>
> Separately, the **alpha** channel takes all 256 values (68% at zero) and is the *terrain blend weight* between
> the two ground textures. So a D3DCOLOR here is two unrelated quantities — RGB is half-scale baked light, A is a
> blend factor — and a solver that treated the whole thing as lighting would be fitting noise.
>
> **CORRECTED AGAIN — the half-scale finding is a property of the EDITOR, not of the format or the track.**
> Measured across two tracks:
>
> | track | authored in | p50 | p99 | scale needed |
> |---|---|---|---|---|
> | Joux Verte (42) | Kangaroo, the original tool | 71 | **128** | **1.99** |
> | track-108 | Wallaby | 255 | **255** | **1.00** |
>
> Kangaroo bakes at half scale; Wallaby bakes at full scale and near-saturated. A constant tuned on one drives
> the other to solid white, which is exactly what happened when a Wallaby-authored community track was first
> rendered. Since community tracks are a mix of both, the scale **must come from the asset**: the converter now
> measures it and writes it into `SceneHeader::bakedScale` (format v2).
>
> Use a **percentile, not the maximum**. Joux Verte's max is 255 on the strength of a handful of stray vertices
> while its p99 is 128 — taking the max would have concluded that track needed no scaling at all. p99 recovers
> 1.99 for Kangaroo and 1.00 for Wallaby, independently reproducing the value that had been hand-tuned.
>
> A second observation with consequences for §3.2: **the Wallaby track's bake carries almost no shading
> information** (p50 = 255, i.e. half its vertices are pure white). There is nothing there to un-bake. Tracks
> authored this way gain the most from dynamic lighting and lose nothing to Tier 0 discarding the bake — but they
> are also useless as input to the Tier 2 photometric solve.

> **Correction from §3.2b: the factor of 2 is not a format convention, it is the sun colour.** The measurement
> above is right; the `MODULATE2X` explanation for it was wrong. RBR's own vertex shader multiplies the baked
> colour by a per-frame lighting constant and lets `ps_1_1` saturate the result. The doubling is what that
> constant happens to equal in clear daylight — it is *weather- and time-dependent*, which is exactly the knob
> §3.4 wants. Do not bake 2.0 into the format.

**Tier 1 — baked colour as occlusion.** `AO ≈ luminance / percentile95(luminance)` per mesh, stored per vertex,
applied to the ambient/sky term only, never to direct sun.

**Tier 2 — photometric solve across `_M`/`_N`/`_E`/`_O`.** The payoff, and only possible because RBR ships the same
geometry baked under up to 4 lighting conditions. Per vertex we have up to 4 observations under different
(sun direction, sun colour, ambient) conditions; solve a small least-squares system for
`(albedo_scale, skyOcclusion, sunVisibility_i)` — coarse photometric stereo with 4 samples. It yields:

- a real **sky-occlusion / AO** term,
- **directional occlusion** hints that validate our shadow maps,
- an estimate of each variant's **sun direction and colour**, which we would otherwise author by hand per track,
- and from the `_N` variant, a map of where the original artists *intended* light to be at night (§5.3).

Match vertices across variants by position hash, falling back to a spatial kd-tree with tolerance. Variants can
differ in geometry (night versions add/remove objects), so the solver must handle partial coverage and drop to
Tier 1 per vertex when fewer than 3 observations match.

> **Measured on Joux Verte (Map 42), `_M` vs `_O`** — the assumption this whole tier rests on, now checked:
>
> | | |
> |---|---|
> | `_M` vertices | 178,594 |
> | matched in `_O` **by exact position** | 178,594 (**100%**) |
> | of those, baked colour differs | 174,718 (97.8%) |
> | of those, baked colour identical | 3,876 (2.2%) |
>
> Identical geometry, different baked lighting, and the correspondence is *exact* — no tolerance or kd-tree
> needed for this pair, position equality is bit-for-bit. That is the photometric solve's premise confirmed on
> real data.
>
> **But the variants are not structurally identical.** `_M` has 398 sub-meshes and `_O` has 245, for the same
> total triangle count and bounding box: the same terrain is partitioned differently and stored with *different
> vertex formats* per region (overcast needs no sun-shadow mesh types). So there is no index correspondence
> between variants, and matching must be by position — as this section already required, now for a demonstrated
> reason rather than a cautious one. `_O` also has far fewer distinct positions (47,486) than `_M` has vertices,
> i.e. the variants duplicate shared vertices differently; the solver must deduplicate before fitting.

**Validation harness:** re-render the converted track under the solved `_M` lighting and diff against a vanilla
screenshot from the same camera. That diff is the regression test for the entire converter, and it lives in Wallaby.

### 3.2b The original lighting model, recovered exactly

RBR ships its shaders as loose `ps_1_1`/`vs_1_1` bytecode in `PixelShaders/` and `VertexShaders/`, **with debug
info and original source paths intact** (`l:\Rally_7\SourceMaterial\CodeBase\Game\Pc\HedgeHog3D\…`). They
disassemble cleanly with `fxc /dumpbin`. Full listing of all 61: [`rbr-shaders.txt`](rbr-shaders.txt).

This replaces inference with ground truth for the un-bake. The ground pipeline is:

```
vs:  oD0.rgb = vertexColour.rgb * r3      r3 = extinction(depth) * sunColour * exposure  (c55,c56,c65)
     oD0.a   = vertexColour.a             passed through untouched — the blend weight
     oD1     = r4                          in-scatter (Rayleigh/Mie), per-vertex
     oFog    = saturate((fogEnd - depth) * fogScale)

ps:  weight = tex1.a * oD0.a
     base   = lerp(tex0.rgb, tex1.rgb, weight) * oD0.rgb
     base  += 4 * texSpec.rgb * oD1.a                      [Specular variants]
     base  *= texShadow.rgb                                [Shadow variants]
     result = base + oD1.rgb                               in-scatter add
     result = lerp(result, fogColour, fogAmount)            fixed-function fog
```

Four things follow that we had wrong or missing:

1. **The blend weight is `tex1.a * vertexAlpha`,** not vertex alpha alone. Joux Verte's ground textures are
   opaque so the two agree there — but they will not agree on a track whose blend layer carries alpha.
2. **The `×2` is `r3`, the sun colour × exposure**, saturated by `ps_1_1`. Weather-dependent, not a constant (see
   the correction in §3.2).
3. **In-scatter is a separate per-vertex term that is ADDED**, not modulated. Any solve that models the bake as
   purely multiplicative will mis-attribute it — this is an additive haze the un-bake must subtract.
4. **Fog and in-scatter are the distance compressors.** RBR's far field is heavily fog-blended; we render none,
   which is why our distant ground shows contrast the original never does.

**Sub-mesh index *is* shader variant** — measured over Joux Verte's 558 drawn ground chunks, the mapping is
perfectly clean, with no mixing inside a sub-mesh:

| sub-mesh | vertex type | shadow map | specular map | pixel shader | chunks |
|---|---|---|---|---|---|
| 1 | 8  | — | — | `RLDoubleTexture` | 304 |
| 2 | 14 | — | ✓ | `…Specular` | 60 |
| 3 | 11 | ✓ | — | `…Shadow` | 117 |
| 4 | 17 | ✓ | ✓ | `…SpecularShadow` | 77 |

So **35% of ground chunks carry a baked shadow map**, and 25% a specular map. The converter discards
`shadowMapIndex` / `specularTxtrIndex` (`lbs2ngg.py:132`), and **that stays discarded — by decision, not
oversight** (see the scoping rule below).

#### Scoping rule: matching the old renderer is a debugging tool, not the goal

Reproducing RBR's output exactly is *not* a requirement. Baked shadows in vertices and textures are precisely
what this project exists to replace with dynamic lighting, so a faithful reproduction of them is work spent on
something we intend to delete. Concretely:

- **Baked shadow maps, baked specular: dropped.** Not converted, not rendered. Their replacement is RT shadows
  and real specular from §1.8.3.
- **Fog and in-scatter: not reproduced.** RBR's are a fixed per-track linear fog and a `vs_1_1`-era scattering
  approximation. We want real atmospherics driven by the weather preset (§3.4), so copying theirs is a detour.
- **Baked vertex colour: scaffolding with an expiry date.** It is currently the *only* lighting we have, so it
  stays until the dynamic path lands — then Tier 0 discards it (§3.2). It is not part of the destination.
- **What we DO owe the original:** geometry, UVs, texture assignment, and the terrain blend weight. Those are
  authored content, not lighting, and they must survive conversion exactly.

This also gives a clean stopping rule for the seam investigation: if a difference from vanilla traces to a
*missing bake* (shadow map, fog, in-scatter), it is out of scope and we stop. If it traces to *our own pipeline*
(depth precision, geometry, sampling, blend weight), it is a bug and we fix it. Reverse-Z (§1.4) is the current
test that discriminates between the two.

One nuance kept open: the baked shadow map is directional-occlusion data, so it is a *potential* input to the
Tier 2 photometric solve even though it will never be rendered. With RT shadows in the baseline that value is
marginal — recorded here so the option is not lost, not scheduled.

**Animation, unexpectedly cheap** (bears on §4): `…Anim` variants are a single instruction —
`mad oT0.xy, c85.x, c85.yzzz, v7.xyyy`, i.e. UV scroll by `time × direction`. And `…Sway` compiles to *no vertex
motion at all* — it declares two extra inputs and never reads them, emitting the same `oPos` as the static
variant. Whatever sway RBR has is not in its vertex shaders.

### 3.3 Materials

Inputs for automatic assignment: texture name and content; mesh type (ground 8/11 vs general 6/7/9/14/17 vs
movable) which already separates road from prop from foliage; the type-9 flag whose vertices carry **grass tilt
amplitude and time base** (`MeshTools.cs:234`), i.e. the format tells us "this is foliage"; and the **`.MAT`
per-texel surface classification** (gravel/tarmac/grass/snow), which maps directly onto roughness/specular presets
and wetness response.

Pipeline: heuristics produce a per-track `materials.json` listing every texture with its guessed class and
parameters → humans override entries in Wallaby → reconvert. Improvable without touching converter code.

Generated per texture: BC7 albedo (optional AI upscale as a separate offline step), BC5 normal (luminance
high-pass by default, hand-authored where it matters), packed roughness/AO, and an **emissive mask** — which
matters far more now that night lighting is in scope. Derived maps go in a content-addressed cache so reconversion
is fast.

### 3.3b RBR's ground textures are ATLASES — and that is the road-seam bug

**MEASURED** on `track-42.ngg` and then **confirmed by experiment in the viewer**, closing §9b item 8. The route
there is worth keeping, because the first attempt got the mechanism wrong while having the right suspect, and the
negative result was what corrected it.

"Plate" in RBR's texture names means **atlas page**:

| | |
|---|---|
| `TrackTexturePlateOpaque.dds` | 2048×2048, **4 mips**, **18 distinct cells** of 256×256 |
| road material | 406 objects, 72,168 tris, all UVs inside 0..1, each chunk in ONE cell |
| authored gutter | UVs inset ≈ 2.25 texels from the cell edge |

The gutter is sized for mip 0. It halves with every mip:

| mip | page | cell | gutter | |
|---|---|---|---|---|
| 0 | 2048 | 256 | 2.25 texels | fine |
| 1 | 1024 | 128 | 1.13 | marginal |
| 2 | 512 | 64 | **0.56** | bilinear tap crosses into the neighbouring cell |
| 3 | 256 | 32 | **0.28** | well across |

And the road's cell is bordered by **measurably darker content** — decoded from the DDS itself:

| region | luma |
|---|---|
| cell interior | **163** |
| just outside left | **100** |
| just outside bottom | **112** |
| just outside right / top | ~165 (similar content) |

So any filter tap that leaves the cell darkens the pixel.

#### The wrong mechanism, and the negative result that fixed it

The first write-up said the cause was mip-chain contamination, and predicted that clamping `MaxLOD` to 1 would
remove the seams. **It did not** — the clamp visibly changed other objects, so it was reaching the shader, and
the road was untouched. That looked like a falsification of the atlas theory entirely, and the constraint was
re-stated as "distance-dependent but NOT mip-dependent", which points away from sampling and towards geometry.

Debug view 8 (flat grey — no texture, no vertex colour, no blend) then came back **clean**, ruling out geometry,
depth and rasterisation, while view 3 (tex0 only) still showed the bands. The artefact really was in the texture
fetch. So what reaches outside a cell without depending on the mip level?

**Anisotropy.** Aniso spreads its taps along the major axis *independently of the mip level it selects*. At the
grazing angle a road is viewed from, that is a long line of taps running well past the cell edge into the
luma-100 region. Confirmed in the viewer: switching to plain linear filtering (key `M`, mode 1) makes the bands
**almost entirely disappear**.

Two mechanisms, then, both of them the atlas:

| | contribution |
|---|---|
| anisotropic tap spread crossing the cell edge | **dominant** — removing aniso removes most of it |
| bilinear tap (±0.5 texel) at high mips, where the 2.25-texel gutter is sub-texel | **residual** — the faint banding still visible with aniso off |

The original gutter arithmetic was therefore right but second-order, which is exactly why clamping mips alone
showed nothing. **A negative result that contradicts the stated prediction is worth more than the story it
kills** — this one was misread once (as "not the atlas") before being read correctly (as "not the mip *level*").

Sampler mode 1 is a diagnostic, not a fix: losing anisotropy costs sharpness on every surface in the game to work
around 18 texture cells.

**The fix is de-atlasing in the converter:** split each cell
into its own texture and rescale that cell's UVs to 0..1. Then mip chains are correct at every level, address
modes mean something again, and later BC7 recompression and streaming operate on real textures. Cheap here
because textures are already embedded per track (§3.5) and this plate holds only 18 cells.

Three things to get right when doing it:

- **16 of 406 road objects span >90% of the page**, so they cross cell boundaries and cannot simply be rescaled.
  Check whether individual *triangles* stay within one cell (they probably do, since chunks do) and split the
  object per cell; if any single triangle genuinely spans cells, that geometry needs its own handling.
- **Not every ground texture is atlased.** `MB_04_GROUND_C` is a standalone 256×256 that genuinely tiles
  (measured u 0..2.5), and rescaling its UVs would be exactly wrong. De-atlas by texture, not globally.
- **Address mode per extracted cell is a real decision, not a default.** Once a cell is its own texture, `WRAP`
  makes it tile with itself and `CLAMP` smears its edge row. Road chunks appear to tile along the road, so `WRAP`
  is likely right there — but a cell that is a one-off decal would want `CLAMP`, and picking wrong reintroduces
  an edge artefact that looks exactly like the bug being fixed. Decide it from the UV range per cell.

If de-atlasing turns out to be blocked by the page-spanning objects, the fallback is a per-material UV rectangle
in the format plus a shader clamp to `rect ± half a texel`. That is the standard fix when you cannot split an
atlas — strictly worse than de-atlasing (it does not fix mips, BC7 or streaming), so it is a fallback and not a
plan.

Found along the way, unrelated but real: **material 13 has a garbage UV** — `v` reaching 3.1e36 across 52
triangles. That is a converter defect, not a rendering one.

### 3.3c The blend weight is NOT dead — measured across two tracks

While isolating the road seams, debug view 5 showed the blend weight as solid black over the visible road on
Joux Verte, and that was nearly written up as "RBR's second texture stage is inert on road surfaces". It is not,
and the generalisation was wrong on both counts:

| | vertex alpha > 0 |
|---|---|
| track-42 (Kangaroo), road material | **24.9%** |
| track-42, other ground materials | 55% – 85% |
| **track-108 (Wallaby), every material** | **100%** |

So even on Joux Verte the road blends in a quarter of its area — the stretch on screen simply happened to be in
the unblended majority — and on a Wallaby-authored track the second stage is live everywhere. Any lighting work
must reproduce the two-stage blend properly; treating it as single-texture would look right on one track and
wrong on the next, which is the same trap `bakedScale` fell into (§3.2b).

### 3.3d RBR ships its own visibility data — MEASURED, and one block is misnamed

`wallaby-cli info track-42_M.lbs` lists 14 blocks. Two bear on culling:

**`(2,10)` "ground bounding boxes", 3796 bytes — usable as-is.** A `uint32` count of **158** (exactly Joux Verte's
ground region count) followed by 158 × 24-byte records. The records are **`(origin, size)`, not `(min, max)`**:
the second triple is positive in every record, ranging 0.3–77.9 m and averaging ~50 × 17 × 46, which is a
plausible ground-region size. Mistaking it for a min/max pair would put 152 of 158 boxes inside-out on one axis
alone.

**`(1,4)` "clipping planes", 2916 bytes — NOT planes.** Leading count is 12, and `2912 / 12` is not an integer,
so it is not an array of 16-byte planes. The payload reads as all-zero floats but as integers gives packed 16-bit
pairs (`0x00010000`, `0x00030002`, `0x00050004`) alongside larger index-like values — i.e. *geometry*, likely a
clipping volume or portal mesh. **The name is one of Wallaby's reverse-engineered guesses** (its own `CLAUDE.md`
warns those are informed guesses rather than documentation) and this one appears to be wrong. Whether RBR's
runtime still consults the block is a separate question that can only be answered by watching the game, not by
reading the file.

Neither is needed to start culling — see §3.3e.

### 3.3e Culling bounds belong to the CONVERTER, with a load-time fallback

RBR's `(2,10)` boxes are per **ground region** — 158 of them. Our draw unit is the **object** (one index range,
one material), of which Joux Verte has 1569+. So the game's own data culls at roughly 10× coarser granularity
than we can actually skip work at, which is reason enough not to build on it.

The converter should emit a per-object AABB, for three reasons and not just the obvious one:

1. **Free at runtime** — no load-time pass over 1.3M indices on every stage load.
2. **Exactly the right granularity** — the converter creates the draw unit, so it can bound it precisely.
3. **It can change the granularity, which is the real win.** Objects today come straight from RBR's
   `GMeshRenderChunk` partitioning, which was built for a 2004 renderer's batching rather than for culling.
   The converter can cluster spatially coherent objects that share a material, so each bound encloses something
   compact instead of something scattered across a hillside. That is §1.8's meshlet direction reached cheaply,
   and it is not something a renderer-side load-time pass can do — by then the partitioning is fixed.

**Keep a load-time fallback in the renderer** for files with no bounds chunk. `Scene::Load` already holds the
vertex and index arrays, so it is one O(indices) pass. Without the fallback, culling would not work until every
existing `.ngg` had been reconverted, and the renderer would gain a hard dependency on a converter version — the
kind of coupling that turns a cheap optimisation into a migration.

### 3.3f Single-stage materials — and the guard that never fired

`kMaterialSingleStage` (`ngg_format.h`) says: sample texture0 only, do not read uv1 or texture1. The scene
shader branches on it per draw — a uniform branch, so the second fetch genuinely does not happen — and
`weight` is forced to zero rather than merely small.

This is how an **object** mesh differs from a **ground** mesh, and RBR draws the same distinction: object meshes
run `RLSingleTexture.pso`, three instructions, `t0` only (§3.2b). Two consequences follow from stage 1 never
being read there, and both bite us:

- **uv1 on object meshes is unvalidated data.** It was never wrong for RBR because RBR never looked at it.
- **Vertex alpha means COVERAGE on objects and BLEND WEIGHT on ground** — the same channel, two meanings. Used
  as a weight on an object mesh it blends toward a texture the artist never intended, in proportion to a number
  that means something else.

> **The isfinite() guard had never once fired, and the reason is worth keeping.**
>
> The vertex shader has carried `o.uv1 = all(isfinite(i.uv1)) ? i.uv1 : i.uv0;` since the problem was first
> found, and this file described the bad data as "non-finite". **It is not non-finite — it is finite garbage.**
> The largest values measured are around `3.1e36`, comfortably inside float's `3.4e38` range, so `isfinite` is
> true for every one of them and the fallback branch is dead code. Measured directly on the shipped assets:
> `track-42` has **0 non-finite uv1** and **233,730 vertices with |uv1| > 1e6**.
>
> A guard that cannot fail is worse than no guard, because it gets read as protection and stops the question
> being asked again.

**What separates good uv1 from garbage is magnitude, and the threshold is not arbitrary.** Past about `1e6` the
UV derivative across a triangle is thousands of texels per pixel, so the sampler collapses to the top mip and
returns the texture's average colour whatever the coordinate was. At that point stage 1 carries no information
and can only dilute stage 0.

**It is worse than a wasted fetch on a de-atlased track.** Measured on `track-42`, every affected material has
flags `AlphaTest|TwoSided` — foliage and objects, exactly as predicted — but de-atlas moved *stage 0* to the
extracted cell and left *stage 1* pointing at the whole page:

| material | stage 0 | stage 1 | refs with unusable uv1 |
|---|---|---|---|
| 82 | `ObjectTexturePlateOpaque_c0r0.dds` | `ObjectTexturePlateOpaque.dds` | 182,097 of 182,097 |
| 84 | `ObjectTexturePlateOpaque.dds` | `ObjectTexturePlateOpaque.dds` | 25,179 of 25,179 |
| 81 | `ObjectTexturePlateTransparent.dds` | (same) | 161,292 of 685,644 |

So what was being mixed into every leaf is *the average colour of the entire atlas page*, at a weight given by
the leaf's coverage. That is a real contributor to foliage reading flat, not a tidiness matter.

**`Scene::MeasureSingleStage` is the load-time fallback**, in the same shape as `WeldNormals`: measure first,
act only where the data is unambiguous, log what it did, and stand down by itself once the converter sets the
flag. Two decisions in it:

- **Per object, not per material.** A material can be shared by geometry that disagrees — material 81 above is
  23% unusable and 77% fine — and flagging the whole material would break the 77%.
- **All of an object's vertices, not most.** A partial object has uv1 that is usable somewhere, and silently
  dropping a real blend layer would look exactly like a missing detail texture. Partials are counted and
  reported, not touched.

Measured through the renderer's own log: `track-42` → **323 objects flagged, 126 mixed and left alone**;
`track-108` → nothing at all, its uv1 is clean. Neither number comes from the converter yet.

### 3.3g Materials are metallic-roughness now — `.ngg` v3, and the 2004 model is gone

**DECIDED, by the user: stop carrying RBR's format design forward.** v2's material was RBR's model wearing our
names — two texture stages, a blend weight in vertex alpha, lighting baked into vertex colour. That was the
right shape while the job was reproducing 2004 output faithfully enough to A/B against it. It is the wrong shape
for the renderer that now exists: nowhere to put a normal map, nowhere to say how rough a surface is, no way to
make something emit light. That is most of what separates a 2004 image from a modern one.

`Material` is therefore **replaced, not extended** — 16 bytes to 64, format v3:

```
baseColour  detail  normal  orm  emissive  flags        (5 texture slots + flags)
baseColourFactor[4]
roughness  metallic  normalScale  occlusionStrength  emissiveStrength  alphaCutoff
```

**It is glTF metallic-roughness, near enough 1:1, and that is the whole argument.** §7.2 already decided glTF
is the interchange format and §3.0 made it the conversion spine; a runtime material model that maps onto it
means an authored material survives the trip without a translation layer inventing values along the way. `orm`
uses glTF's own channel packing (R occlusion, G roughness, B metallic) so an exported material needs no shuffle.

**Two things are kept from RBR, and both earned it by measurement rather than inheritance.** The `detail` layer
blended by vertex alpha — §3.3c measured it live on both test tracks and it is what hides terrain chunk seams;
detail texturing is a standard technique in its own right, so this is not a legacy stage. And `baseColour.a` as
coverage on alpha-tested geometry. **One thing is deliberately absent:** the baked vertex colour stays in the
vertex data as the A/B reference (§3.2), not as a material property — it is a record of one particular sun on
one particular day.

**`kMaterialSingleStage` disappeared, and its absence is the design.** §3.3f needed a flag only because the
format could not express "this material has no second layer". v3 can: `detail == kNoTexture`. The renderer
derives the draw flag at load, so the file states the fact once instead of twice in a way that can disagree
with itself. Same for the normal map.

**Factors multiply their map rather than replacing it**, which is glTF's rule and the reason an untextured
material is a *complete* material rather than a special case: an empty slot binds the 1×1 white fallback, so
`map * factor == factor`. That is what lets the converter ship plausible PBR for RBR's 6,515 textures before a
single map has been authored — and `DefaultMaterial()` gives an unconsidered material a rough dielectric rather
than a black or mirror-finish one.

#### The tangent frame comes from screen-space derivatives

Two reasons, and the second decided it: the `.ngg` vertex is 44 bytes with no tangent, so adding one is a format
break *and* a converter change — and **RBR's geometry carries no tangents either**, so they would have to be
derived somewhere regardless (§3.2b: the disassembly has zero `dcl_tangent`/`dcl_binormal` and zero `texbem`,
because RBR has no normal mapping anywhere). Four derivative instructions in the pixel shader works on converted
and authored content alike, with no format cost.

The normal map is gated on a **flag**, not on the sampled value: an empty slot binds the white fallback, which
decodes to (1,1,1) and would tilt every normal 54° off true. Neutral for a normal map is (0.5,0.5,1), and no
single fallback texture can be neutral for every slot at once.

#### sRGB is a per-SLOT decision, and only the material knows the slot

`baseColour`, `detail` and `emissive` are artwork and decode through sRGB. `normal` and `orm` are **numbers**,
and pushing them through the sRGB decode bends every value — a 0.5 roughness would arrive as 0.21. Nothing in a
DDS says which it is, so `Scene::Load` collects the indices bound to normal/ORM slots and hands them to
`UploadTextures`, which picks `BC*_UNORM` instead of `BC*_UNORM_SRGB`. This is the same class of bug as §5.0's
colour-management failure and it is being closed *before* any such map exists, rather than after four rounds of
tuning around it.

#### Specular, and the units note that has to be here

GGX: Trowbridge-Reitz, height-correlated Smith, Schlick Fresnel. Nothing bespoke — the value of a standard BRDF
is that an authored glTF material looks the same in Blender as it does here.

> **The diffuse term in this renderer is `albedo * E`, which is π times the physical `albedo/π * E`.** That
> convention is inherited from the first lighting pass and absorbed into `sunDiffuse`. Specular therefore carries
> the same factor explicitly (`kSpecScale = π`), or the two are out of balance by π and everything wears a chrome
> sheen. Both collapse to the physical form together when exposure becomes physical (§9b item 17). This project
> has lost two rounds to exactly this kind of missing π; it is written down rather than remembered.

**No specular on foliage, deliberately.** A leaf card's normal is a shading fiction already
(`SphericalFoliageNormals`), and a highlight computed from a fiction lands in the wrong place and flickers as
the camera moves. **No ambient specular yet** either: it needs a prefiltered environment and a split-sum BRDF,
which is exactly what §9b item 14's environment LUT is for. Faking it with a roughness-scaled `f0` would be a
fudge that then has to be un-tuned when the real thing lands.

#### Live editing: the API exists, the dialog belongs in Wallaby

`ngg_get_material` / `ngg_set_material` / `ngg_material_name` / `ngg_object_material` (design 8.2's C DLL), on
top of `Scene::GetMaterial` / `SetMaterial`. **A scalar edit costs a memcpy and shows on the next frame** — the
material reaches the shader as root constants read from the renderer's table every frame, so there is nothing
to upload and nothing to rebuild. That is the reason the material is root constants rather than a buffer, and
it is what lets a slider feel like a slider instead of like a reconversion.

Three decisions inside it:

- **The whole descriptor goes in and out**, not one field at a time. No partial-update state, and no list of
  per-field setters that grows every time the material does.
- **The derived flags are not exposed.** "Single layer" is `detail == kNoTexture`; the renderer recomputes it
  on every set, so a caller cannot put the flags and its own texture handles into disagreement.
- **The MEASURED single-stage decision survives a material edit.** §3.3f's fallback came from the geometry, not
  from the material, so it is stored per object and re-applied when flags are recomposed — otherwise an
  unrelated slider would silently restore the atlas-average blend.

Only a texture-slot change rewrites the SRV heap, and the renderer detects that by comparing slots rather than
trusting the caller. Persistence is Wallaby's: these values belong in the scene-description sidecar beside the
shading overrides, keyed on the **material** for the reason §3.0 gives — object indices move when the converter
regroups. `rbrtm.exe --view` drives the same calls from `1`/`2` and `3`/`4`, so the path is testable before any
UI exists.

#### v2 is still readable, and that is a transition with an expiry date

The converter is in another repository. A hard cut would mean both sides landing the same change in the same
hour or every asset stops loading, so `Scene::Load` reads a v2 material table and fills the PBR fields from
`DefaultMaterial()`, logging that it did. Delete the branch, `MaterialV2`, and the version test once Wallaby
writes v3. Measured: `track-42` (119 materials) and `track-108` (92) both load and render unchanged apart from
the new specular lobe and §3.3f's single-stage fix.

### 3.4a `TrackSettings.ini` already IS the weather preset system — 762 presets

**MEASURED.** `Maps\TrackSettings.ini` holds **762 sections** keyed `<map><variant>_<sky>_<cloud>`, e.g.
`[42M_crisp_clear]`, `[42O_M_heavyrain_heavycloud]`, `[108M_lightfog_clear]`. This is §3.4's design, shipped in
2004, and it means the weather presets do not have to be invented — they can be read, matched, and then
extended.

What each section carries is not a set of magic constants but a **physically parameterised atmosphere**:

| key | meaning |
|---|---|
| `SunDir` | the sun direction, per track and variant — this is what §3.2's photometric solve was going to recover |
| `Sun_Intensity`, `AmbientRGB`, `DiffuseRGB` | the lighting constants behind `bakedScale` (§3.2b) |
| `Turbidity`, `Rayleigh_Multiplier`, `Mie_Multiplier`, `Greenstein_Value` | Preetham/Mie scattering parameters — RBR was running an analytic sky model |
| `Extinction`, `Inscattering` | the `r3` and `oD1` terms in `RLDoubleTexture.vso` (§3.2b), now named |
| `FogStart/End/RGB`, `UseFog`, `SuperbowlFogStart/End` | fog, including a separate distant-terrain range |
| `SkyboxName`, `SkyboxSaturation`, `Skybox_Scale`, `CloudName`, `Cloud_Scale` | which sky and cloud layer |
| `MipMapBias = -3.0` | RBR biased mips *sharper*, worth remembering next to §3.3b |

Two consequences:

- **The sky work starts from RBR's own parameters rather than from taste.** A physical sky driven by `SunDir`,
  `Turbidity` and the Rayleigh/Mie multipliers matches vanilla by construction, and improving on it afterwards is
  then a deliberate choice rather than an accident.
- **It reduces §3.2's photometric solve from "recover the sun" to "verify the sun".** The solve is still worth
  doing for tracks whose settings are missing or wrong, but it stops being on the critical path.

Note the layering this implies for §3.4: our preset system should *read* these as the baseline, not replace them.

> **Superseded by 5.0q (measured):** the scattering numbers are art knobs, not physics (turbidity −2.8…9, g =
> 1.0). Only the category in the section name, the fog distances and `SunDir` translate. Our presets are our own
> physical format, stored in the `.ngg`, and this file is an import source.
And `SunDir` belongs in the `.ngg` weather preset, not re-parsed from the game directory at runtime — a native
or generated track has no `TrackSettings.ini`.

### 3.4 Weather and daylight presets

Weather becomes an authored, named, unbounded set — decoupled from RBR's four TOD variants, which are demoted to
*source data for the solve* rather than the runtime lighting model.

A preset (`weather/<name>.json`) declares:

```
name, description
sun:      elevation, azimuth, colour, intensity        (or datetime + latitude → computed)
sky:      model params (turbidity/cloud coverage/type), cloud speed
atmos:    fog curve, visibility distance, rain/snow rate, wind vector+gust
ground:   wetness 0..1, puddle density, snow cover
night:    streetlight/window emissive multiplier, headlight profile, moon
exposure: EV bias, tonemap curve
bindings: mat: "<file>.mat"        ← physical surface map the game must load
          gameWeather: <RBR enum>   ← what physics/sound/pacenotes see
          tod: <M|N|E|O>            ← which baked variant to source geometry lighting from
```

Two hard rules:

- **Render weather is a pure function of the game's weather state.** A wet-looking road with dry grip is worse than
  the old graphics. The preset picks the `.MAT` *and* declares the game weather enum; they must agree.
- The `.MAT` swap goes through the existing `LoadFileMem`/`CreateFileA` hooks — no new plumbing.

Preset id is part of the event definition and hashed into the fairness check (§3.6).

### 3.4b Layers and stage variants

A stage is not one fixed set of geometry. The base terrain and road are shared, while walls, spectators,
vegetation and props vary between a stage's variants — and RBR ships those variants as separate LBS files whose
object sets genuinely differ (night versions add and remove things). Converted assets have to express that, or
every variant becomes a full copy.

**Model:**

- **Object** — the unit of addressing. Stable id, owning layer, geometry range, transform, material, binding
  mode. Everything below is a selection over objects, so *nothing* works unless geometry is stored per-object
  rather than as one merged blob (which is exactly what format v0 does today, and why v0 cannot express any of
  this).
- **Layer** — a named group of objects: `terrain`, `road`, `walls`, `spectators`, `vegetation`, `props`.
  Layers are the granularity at which content is included, replaced or authored.
- **Variant** — a named selection: which layers are present, plus per-object add/remove/replace overrides, plus
  which lighting set applies. `42_M` and `42_O` become two variants over one shared geometry pool.

**Geometry is stored once.** This is measured, not assumed: `_M` and `_O` of Joux Verte match bit-exactly by
position (§3.2), so a variant that re-listed the same terrain would duplicate ~180k vertices for nothing, across
207 shipped track files.

**One container per stage, holding the shared pool and all its variants** — rather than a base file plus variant
sidecars. The deciding factor is §3.6: one file is one CRC in the fairness chain. Sidecars would mean validating
a set, and a set is something an attacker can make inconsistent.

**Community and generated content reuse the same mechanism**: an overlay adds a layer (§7.1), so "better trees",
"AI-scattered detail" and "this stage variant has different walls" are one feature, not three.

New chunks, all additive to the existing container:

```
NGGL  layers      [id, name, flags]
NGGO  objects     [id, layerId, vertex/index range, transform, materialId, binding mode, bbox]
NGGR  variants    [id, name, layer set, override list (add|remove|replace objectId), lightingSetId]
```

A reader that knows none of them still renders the shared pool, which keeps v0 files valid.

### 3.5 Format sketch

Chunked, GPU-ready, little-endian, memory-mappable, every chunk independently skippable so a version bump never
means "reconvert everything or nothing".

```
NGGH  header      magic, version, producer id+version, TOD id, flags,
                  origin: {converted | native | generated}
                    converted → source LBS CRC32+size          (drives the vanilla-fallback check)
                    generated → generator id, seed, input hash (drives reproducibility + fairness)
                  encryption envelope: {scheme=none, keyId=0, reserved}      ← reserved now, unused
NGGM  materials   name, class, texture ids, roughness/metal/normalScale, alpha mode,
                  emissive colour+intensity, foliage params, car-paint flags (clearcoat, flake)
NGGT  textures    ids → BC7/BC5/BC4 blobs, or external .nggtex package for streaming
NGGV  vertices    pos f32x3 | normal+tangent (10-10-10-2) | uv0 f16x2 | uv1 f16x2 |
                  AO+skyOcclusion u8x2 | foliage phase/amp u8x2
NGGI  indices     u16/u32
NGGC  clusters    64–128-tri meshlets: bbox, normal cone, material id, ranges, LOD level
NGGN  nodes       BVH over clusters for CPU/GPU culling
NGGO  objects     per object: id, cluster ranges, default transform, animated flag, and a
                  binding mode:
                    bound   → VB/IB identity key, rendered at the harvested transform of a
                              suppressed game draw          (converted tracks)
                    free    → rendered unconditionally from its own transform/animation
                              (generated tracks, and added content on converted ones)
NGGL  lighting    solved sun dir/colour per source TOD, sky params, SH2 irradiance probe grid
NGGP  lights      punctual lights: type (point/spot/area), position, direction, cone, colour,
                  intensity, radius, shadow flag, on/off schedule, provenance (auto|authored)
NGGS  skeletons   bones + bind poses for animated/skinned objects (car and props)
NGGX  provenance  per-object source block type/subtype, for debugging and the diff harness
```

`NGGP` and `NGGS` exist from v1 even though nothing writes much into them yet — reserving them costs nothing and
retrofitting a chunk that the runtime, converter and editor all need is expensive.

### 3.6 Fairness

This is an online racing platform, so rendering changes are potentially competitive changes: longer view distance
means seeing corners earlier; thinner fog, brighter night or thinner foliage means information the reference client
doesn't give. Night lighting makes this sharper, not softer — "how bright is night" is now a *tunable*.

So: a **sanctioned render profile** — view distance, fog/visibility, foliage density, night exposure and light
intensity, LOD fade distances — locked per event and hashed into the same check chain the track CRC uses. Free
settings offline and in practice. This constrains the config system, so it belongs in v1.

### 3.7 Encryption (deferred, not closed)

No crypto now. What v1 must do: keep the `NGGH` envelope field, route all converter output through a single sink
interface, and keep chunk framing such that payloads can be encrypted independently. When protected tracks matter,
the converter runs client-side at install from the already-licensed original, using the Blowfish infrastructure
already in the plugin. Until then, the converter should still *refuse* to write output for a keyed source, so we
don't accidentally ship a content-leak tool before the protection exists.

---

## 4. Animation

### 4.1 Harvest it, don't reimplement it

The game still runs animation logic for objects whose *draws* we suppress. So each suppressed draw hands us, free
and per frame, the world matrix the game computed — and for CPU-skinned meshes, the **already-skinned vertex data**
in the dynamic VB.

Phase plan: match the suppressed draw's identity key to an `NGGO` object and render our replacement at the
harvested transform. Zero animation code, exact fidelity, no RE required. This is a much better trade than
reimplementing `DriveEvents`/`AnimTriggers`/`AnimationSets`, especially since `AnimationSetItem` is an unparsed
stub and the curves live in SGS files Wallaby reads only partially.

The same mechanism is how the **car's runtime damage deformation** gets handled when we take the car over (§6) —
RBR deforms the car mesh at runtime, so reading the deformed result beats re-deriving the deformation model.

> **Confirmed in-game, and it makes harvesting mandatory rather than merely convenient.** Block 1/6
> (`MovableMeshes`) converts and renders correctly — fences, banners, rally signs, the start clock; 24 objects at
> 625 instance matrices on Joux Verte. But **they no longer move when hit.** The physics still runs and the
> game's own copy still moves; what does not move is *our* copy, because the converter **baked** each instance
> matrix into vertices.
>
> The block name is literal: these are driven by the collision/physics engine, so their placement is **runtime
> state, not asset data** — the file stores only the initial transform. Two consequences:
>
> 1. **Stop baking movable transforms.** They belong in the format as a per-object transform the runtime
>    overwrites, not folded into vertices. Baking is correct only for genuinely static content (terrain,
>    vegetation, scenery).
> 2. **This is the first real customer for §2.1's harvested world matrix.** That tuple was recorded speculatively
>    for "animation later"; block 1/6 is the concrete case, and it shows the moment a player clips a fence —
>    which is often. A knocked-over fence that springs back to attention is worse than one we never drew.
>
> This is *not* an argument for depth interop or for letting the game draw them (§1.3). The transform is cheap to
> read at the suppressed draw. It is an argument about where the transform lives in our data.

### 4.1a The .lbs image is transient — MEASURED, and it settles the mechanism

An attractive idea, and worth recording why it fails, because it would have been the cheapest possible solution.

**The proposal.** We already know where the loaded `.lbs` lives — RBRTM's `ParseLBS` detour recovers the buffer,
and RBR reads the whole file into *one contiguous allocation*, so a file offset is also a memory offset. The
converter therefore records the file offset of every movable-mesh instance matrix, and at runtime we read
`base + offset` once per object per frame. No draw harvesting, no identity mapping between game draws and our
objects, no gaps when the game culls something we still draw. The offset *is* the identity.

**The measurement.** Instrumented the bridge to check, every N frames, which 4 KB pages of the image are still
readable and which have changed (`WatchLbsPages`). Two tracks, two loads:

| track | image | outcome |
|---|---|---|
| Joux Verte | 30,586,235 bytes at `0x16ECC020` | **unmapped 0.8 s after load**, 0 bytes readable |
| Map 4 | 1,922,782 bytes at `0x15AE4020` | **unmapped 0.14 s after load**, 0 bytes readable |

`0 of N bytes readable` — the whole allocation is released, not trimmed. **RBR parses the file into its own
structures and frees the buffer immediately.** There is no persistent file image, so there is nothing to read at
an offset, and the scheme is dead.

> **Corollary, and it matters for §7.** The related idea of shipping a *minimal `.lbs`* for natively-authored
> tracks survives, but only in one direction. It is still a valid way to get objects **into** RBR's collision
> engine — which is required regardless of who renders them, since they must collide with the car. It cannot be
> used to read transforms back **out**. Input yes, output no.

So both movable meshes and fences resolve to the same mechanism: **harvest at the suppressed draw.** The two
mechanisms in §4.1b collapse to one, which is a simpler outcome than the taxonomy suggested — at the cost of the
identity-mapping problem, which now has to be solved for real.

> **Open, and worth deciding rather than defaulting into: how deeply to couple to RBR's collision engine.**
> Everything above treats the physics as a black box we observe through draw calls. There is a stronger option —
> engaging the collision/physics engine directly, the way `.col` and the movable/fence systems already do — which
> would give object state as *data* rather than as an inference from rendering, and would sidestep both the
> identity mapping and the culling gaps.
>
> It is a bigger commitment: more surface against a 2004 binary, and it makes the renderer depend on internals
> that the fairness profile (§3.6) then has to account for. But note it is not purely a rendering question — native
> tracks (§7) must feed RBR's collision anyway for the car to hit anything, so *some* coupling is required no
> matter what. The question is whether the same channel can serve both. **Deferred deliberately; the answer
> changes what the format needs to store, so it should be settled before format v2 (item 7) is cut.**

### 4.1b Runtime state is not one problem — a taxonomy

Movable meshes turned out to carry runtime state (§4.1). They are not the only ones, and the others do not all
want the same solution. Sorting the cases by *what* the game changes at runtime:

| content | source | what changes at runtime | mechanism |
|---|---|---|---|
| terrain, vegetation, scenery | `.lbs` 1/0, 1/1, 1/3 | nothing | **bake at conversion** |
| movable meshes | `.lbs` 1/6 | a rigid **transform** per instance | read the live matrix |
| **fences** | **`.fnc`** | **per-panel deformation** | harvest drawn vertices |
| pacenote marks / signs | `.trk` driveline | placement, from a spline | regenerate (§1.4) |
| car | physics | pose **and** mesh deformation | harvest (§6) |

**Fences are the awkward one, and they are not a variant of movable meshes.** Measured on Joux Verte: `.fnc` v2
holds 39 fence groups and 642 points, with a `FenceTileID` / `FenceStolpeID` per group. So a fence is *not stored
as geometry at all* — it is a point list, and the game generates panels and posts along it, the same shape of
problem as the pacenote marks. Three consequences:

1. **Different memory image.** `.fnc` is a separate file, so the "base + offset" scheme of §4.1 needs a *second*
   base. The identity stored in `.ngg` must therefore be **`(imageId, offset)`**, not a bare LBS offset — with
   `imageId` naming lbs / fnc / col. Cheap to generalise now; a painful retrofit later.
2. **Generated, not stored.** Converting them means replicating RBR's panel generation, with the same
   fidelity risk as the pacenote marks (§1.4) — a fence in the wrong place is a wall the driver did not expect.
3. **Deformation, not placement.** RBR fences *bend* when hit. A per-object transform cannot express that, so
   reading a matrix is not enough. This is precisely the case §4.1 already anticipated: harvest the
   **already-deformed vertex data** from the dynamic vertex buffer at the suppressed draw, exactly as planned for
   the car's damage mesh.

So the two mechanisms are complementary rather than competing: **offset-reads for rigid transforms, draw
harvesting for deformed geometry.** Movable meshes want the first, fences want the second, and the car wants the
second. Neither subsumes the other.

> **Also note: the `.fnc` fences are currently missing from our render.** What we draw are fence *pieces* from
> blocks 1/1 and 1/6 (textures `FENCE01`, `MB_FENCE002`, `FENCE_WOOD03`, `FENCE_PIG01`). The long roadside fences
> come from `.fnc`, which the converter does not read, and the game's own copy is suppressed. Worth knowing
> before judging completeness of the current picture.

### 4.2 Where harvesting is not enough

- **Objects beyond the game's cull distance.** Longer draw distance is the point; if the game culls an animated
  object we get no matrix. Answer: idle/static pose at distance (nobody sees a spectator's arms at 400 m), with a
  native animation player only if it visibly matters.
- **Full takeover**, if we ever stop the game rendering 3D entirely.

Scope a native DLS/SGS animation player as a separate investigation with its own RE budget, starting by completing
`AnimationSets` and SGS animation-track parsing in Wallaby — where there's an interactive viewer to validate against.

### 4.3 Vertex animation

Foliage sway ships in phase 1: type-9 meshes already carry per-vertex tilt amplitude and time base; carry them
through conversion into `NGGV` and reproduce the sway in our vertex shader, improved with a wind field, gusts and
distance-based amplitude damping.

---

## 5. Night and lighting

The original renderer has no dynamic lights at all, so this is genuinely new content, not a port.

### 5.0 Physical sky — DONE (first version), and it settles the skybox question

`ngg/core/sky.*` renders a single-scattering Rayleigh + Mie atmosphere per pixel as a fullscreen pass, replacing
the colour clear in both the launcher and the viewer.

**This closes §9b item 6 by making it moot.** RBR's skybox is a painted mesh; a painted sky cannot respond to a
sun that moves, and dynamic time of day is the point of the overhaul. So the skybox is not converted at all — the
sky is evaluated. That also gives the ambient term a real source, which matters because "directional light plus a
hardcoded ambient constant" is the kind of placeholder that never gets removed.

**What is and is not taken from the game.** `SunDir` is used directly from `TrackSettings.ini` — it is
unambiguous, per track *and* per variant, and it is exactly what §3.2's photometric solve existed to recover.
`Terrain_Reflectance` becomes the ground albedo (0.084 on Joux Verte). But `Turbidity`, `Rayleigh_Multiplier`,
`Mie_Multiplier` and `Greenstein_Value` are **deliberately not** used as physical quantities: Joux Verte ships
`Turbidity = 0.0` and `Greenstein_Value = 1.0`, both degenerate for any real scattering model, so they are
internal scales for RBR's ps_1_1 constants. Feeding them to a physical model would produce garbage that looked
like a bug in the model. Physical constants are used instead, with turbidity as our own haze control.

The RBR-specific half lives in `ngg/host/tracksettings.*` and `ngg-core` takes a plain `SkyParams`, keeping §8's
rule intact — the viewer drives the same sky with no game present.

#### Aerial perspective — DONE, and it shares ONE model

The world now gets the same scattering integral the sky does, over the camera-to-surface segment instead of the
whole ray. Distant geometry is **both dimmed by transmittance and tinted by in-scatter**; applying only the tint
is what makes cheap distance fog look like paint laid over the world rather than air in front of it.

The model lives once, in `ngg/core/atmosphere_hlsl.h`, and is prepended to both shaders at compile time. That is
deliberate and not tidiness: the sky and the aerial perspective are the same integral over different segments,
and the seam between them — the horizon — is exactly where two implementations would visibly disagree. This
project has already paid once for a duplicated model (Wallaby's fixed-function ground blend against RBR's real
shader, §8.1).

The scene pass marches 8×3 against the sky's 16×6, because it runs on every geometry pixel rather than only where
sky shows. The viewer's title bar carries a frame-time readout for exactly this reason — the dev machine has an
integrated GPU, and "is per-pixel marching affordable" is a question to answer with a number. If it is not, the
answer is a precomputed aerial-perspective LUT, not a cheaper hand-written fog.

#### Dynamic surface lighting — DONE (unshadowed)

Viewer key `G` cycles three modes, and mode 0 is kept deliberately:

| mode | |
|---|---|
| 0 **baked** | RBR's vertex colour × `bakedScale`. Kept as the **A/B reference**, not as a fallback |
| 1 **dynamic** | sun + sky from the same atmosphere that draws the sky; the bake is discarded entirely |
| 2 **dynamic + AO** | as 1, but the bake's *luminance* darkens the **ambient only**, never the sun |

Sun colour comes from `AtmSunTransmittance` — a single march, no inner loop, along the sun ray. Low-sun
reddening therefore falls out of the model rather than being an authored gradient, which is the whole reason for
a physical sky. It also returns black when the sun is below the horizon, so the ground does not stay lit through
the earth at night.

**Mode 2 exists for a specific reason and has an expiry date.** RBR's bake contains real occlusion — valleys,
undersides, the shaded side of a cutting — that we cannot yet reproduce because there are no dynamic shadows.
Using its luminance as an ambient-occlusion term keeps that information while still letting the sun move.
Applying it to the sun term as well would re-introduce the baked sun direction and defeat the point. **Once
shadows land, mode 1 becomes the honest one and mode 2 should go.** Left undocumented, a stopgap like this
quietly becomes the shipping default.

**Mode 2's first implementation broke exactly the rule stated above.** It multiplied ambient by the bake's *raw
luminance* — but RBR's vertex colour is not occlusion, it is **full baked lighting including the directional sun
term**. So a wall facing away from the *baked* sun had its ambient crushed by that baked sun, which is precisely
what "never the sun" existed to prevent. Shaded walls rendered below sRGB 0.1, and it was mistaken for missing
multiple scattering.

What caught it was evaluating the shader's own model offline and comparing numbers: the model puts that wall at
**sRGB ≈ 0.31/0.36/0.40, a ratio of 0.42 to the lit side** — if anything on the bright side of realistic. The
frame showed near-black. **When a render disagrees with the model that produced it, the bug is between them**,
and tuning either end will never find it.

Two guards now bound it, neither of which makes it correct:

- normalise by the scene's **mean baked luminance** (computed at load; 0.571 on Joux Verte) so the term is
  relative occlusion rather than absolute brightness — otherwise a track with a globally dark bake, which
  `bakedScale` shows is common, has its ambient crushed everywhere;
- **cap the darkening at 50%**, so a dark bake can never cancel the ambient outright.

Separating occlusion from direct light properly is §3.2's photometric solve. Until then **mode 1 is the honest
mode**; mode 2 trades correctness for the shape RBR's artists baked in.

#### RBR's normals were NEVER used for shading — and it shows

**MEASURED, and it explains the faceted look of the first dynamic build.** RBR's ground vertex shader declares
`dcl_position v0`, `dcl_color v5`, `dcl_texcoord v7`, `dcl_texcoord1 v8` — **there is no `dcl_normal`**
(`doc/rbr-shaders.txt:1825`). Lighting came entirely from the baked vertex colour, so the normals in the track
data have sat unexercised for twenty years. Our dynamic lighting is their first consumer.

Unsurprisingly, they are not shading normals. Sampled over `track-42.ngg`: all are unit length and none are
degenerate — but of co-located vertex groups, **2920 of 3355 carry DIFFERENT normals**. They are split, not
welded, so a smooth hillside shades as hard facets and foliage cards flip between fully lit and fully unlit.

The fix is to **weld by position and average, with an angle threshold** so genuine hard edges (walls, kerbs,
building corners) stay hard while terrain smooths. That belongs in the converter — it is asset data, done once,
rather than on every load.

**The converter now does this** (`wallaby-cli`, on by default), and the renderer fallback **defers to it**.

The first fallback would have made a converted asset *worse*, not merely wasted time, and it is worth recording
why because the failure was silent:

- it bucketed positions at 1 cm, which merges across layers the converter deliberately isolates — 1,572
  cross-layer groups against 914 at exact equality, i.e. welding a fence post into the terrain it stands on;
- it averaged each vertex with its own in-threshold neighbours, and **"within 60°" is not transitive**. Given
  A–B and B–C inside the threshold but A–C outside, that leaves all three disagreeing. The converter partitions
  by complete linkage instead. Re-running the naive version would have undone that.

So `Scene::WeldNormals` now **measures before acting**: it counts co-located pairs that are within threshold and
checks whether they already agree. On a converted `track-42` — **302,496 agree, 24,378 differ** — it logs that
the converter did it better and does nothing. It uses exact position keys now rather than buckets, matching the
converter's own measurement that RBR's duplicated vertices are bit-identical.

`SphericalFoliageNormals` gained the same guard, measuring the mean angle between the stored normal and the
outward direction: 89.1° means card normals (apply it), under 30° means the converter's `spherical` override is
already in place (skip, or the blend compounds and over-rounds the canopy). It currently reports 89.1°, so the
renderer pass is still doing real work — the converter has the *mechanism* but not yet the classification.

**The general rule for every renderer-side fallback: measure whether the converter already did it, and defer.**
A fallback that cannot tell is a fallback that corrupts good input.

One measurement error worth recording, since it caused the wrong conclusion first: the original pass reported
"268,706 of 749,342 vertices smoothed" on an already-welded asset. It was counting vertices that *had an
in-threshold neighbour*, not vertices whose normal changed — a number describing the input rather than the work,
and it made a no-op look like a substantial pass. It now counts actual movement.

This is worth generalising: **any RBR vertex attribute the game's shaders do not declare is unvalidated data.**
It was never wrong for RBR because RBR never read it. Before relying on one, check the `dcl_` list.

#### THE actual cause of "everything is too dark": no colour management

Found by the pixel probe, after four rounds of adjusting ambient terms that were never the problem. On a shaded
wall the probe reported a fully self-consistent chain — `albedo × ambient = 0.1937` matching `lit`, tonemapped to
`0.1437`, aerial correctly negligible at 47 m. **The model was right. The output was in the wrong colour space.**

Two errors, and they partly cancel, which is what hid them:

| | effect |
|---|---|
| textures decoded as `BC*_UNORM` when the artwork is **sRGB** | albedo too bright in linear space |
| output written to an 8-bit target with **no sRGB encode** | everything too dark on screen, shadows crushed hardest |

Linear 0.144 written raw displays as 0.144; its correct sRGB encoding is **0.42**. Highlights barely move,
shadows are crushed — precisely the reported symptom of "the lit side looks fine, the shadow is black". Every
ambient fix was fighting a transfer function.

Fixed as: textures use `BC*_UNORM_SRGB` so the hardware decodes to linear on sample, and `LinearToSrgb` is
applied at the end of both the scene and sky shaders. **The encode is in the shader rather than via an `_SRGB`
render-target format** because the game path renders into a texture the 32-bit bridge creates, whose format we do
not control from here — one encode, both paths.

The lesson is the expensive one: **before tuning any lighting value, verify the colour pipeline end to end.**
Linear-space lighting written to a display-space buffer looks exactly like "the ambient is too weak", and no
amount of adjusting the ambient will ever fix it.

#### The constant buffer was misaligned — and it invalidated several rounds of judgement

**HLSL packs constants into float4 registers and will not let a `float3` straddle a register boundary; it inserts
padding. A tightly packed C++ struct does not.** When `gGroundColour` (a float3) was added after two floats, HLSL
pushed it to the next register and **every field after it read the wrong two floats**:

| field | HLSL float index | C++ float index |
|---|---|---|
| `gSunDiffuse`, `gSkyAmbient` | 52, 53 | 52, 53 |
| `gGroundColour` | **56–58** | **54–56** |
| `gGroundBounce` | 59 | 57 |
| `gBakeMean` | 60 | 58 |
| `gProbeOn` | 63 | 61 |

Consequences: the pixel probe never fired (`gProbeOn` read padding, always 0, reporting all zeros), and the
ground bounce colour, bounce strength and bake mean read **each other's values** from the moment they were
introduced. Several rounds of "still too dark / now it's red" were therefore judged against a shader that was
not receiving the constants it was being tuned with.

The fix is structural, not a corrected offset: **every group from there on is exactly one float4**, so the two
layouts agree by construction rather than by careful counting, plus a `static_assert` that the struct is a whole
number of registers. Counting offsets by hand is what produced the bug; it cannot be the thing that prevents it.

Two lessons worth more than the bug:

- **A silent binary-layout mismatch presents as a plausible-looking wrong picture**, never as an error. Every
  hypothesis in the preceding rounds was about lighting maths, because the picture *looked* like a lighting
  problem. Only instrumentation that reported a value the shader could not plausibly hold — a probe returning
  exactly 0.0000 for eight unrelated terms — pointed at plumbing rather than physics.
- **Enabling the D3D12 debug layer while discarding its messages is worse than not enabling it**, because it
  looks like validation is running. `Device::DrainMessages` now pumps the info queue into the log. It did not
  catch this one (constant-buffer contents are not validated) but it caught a resource-state warning in the same
  session, and the cost is a few lines.

#### Navy-blue shadow — the ambient was sky-only

Buildings rendered with their sun-facing walls correct and their shaded walls **saturated navy blue**. The colour
is the diagnosis: ambient was a single sample of the *sky*, so a surface facing away from the sun received pure
Rayleigh scatter and nothing else. Real shadow is dominated by light bounced off the ground in front of the
surface, which is why it reads desaturated and slightly warm rather than blue.

Ambient is now hemispherical — sky above, ground bounce below, mixed by how much of the lower hemisphere the
normal sees (0 facing up, 0.5 for a vertical wall, 1 facing down). The bounce colour is **RBR's own
`Terrain_Reflectance_R/G/B`** (§3.4a), so it is measured rather than invented: Joux Verte gives 0.49/0.49/0.50.

The sky sample is reused as the light arriving at the ground rather than marching a second time toward the
zenith. That is a real approximation, but the alternative doubles the per-pixel cost of the pass for a term that
is then multiplied by a ~0.5 albedo.

**This took three attempts, and all three failures were the SAME units error.** `ambient` is added to
`sunColour * ndl` and the sum multiplied by albedo, so it must be an **irradiance** — the same units as
`sunColour`. `AtmScatter` returns a **radiance**. Over a hemisphere the two differ by a factor of π.

| attempt | error | symptom |
|---|---|---|
| sky-only ambient, radiance used directly | π too **dark**, and blue | shaded walls navy |
| ground bounce added without its 1/π | π too **bright**, and warm | shaded walls red |
| corrected bounce, sky still radiance | both too dark again | shaded walls near-black |

The two errors partly cancelled, which is why fixing either one alone appeared to break the other side. Written
out properly:

```
E_sky    = PI * skyRadiance
E_ground = groundAlbedo * (sunIrradiance * cos(sunZenith) + E_sky)
ambient  = lerp(E_sky, E_ground, howMuchGroundThisNormalSees)
```

The ground term needs no explicit 1/π: the one converting irradiance to bounced radiance and the π converting
that back to irradiance on our surface cancel.

**`skyAmbient` went from 1.6 back to 1.0** as part of this. The 1.6 was a fudge compensating for the missing π —
which is exactly how a wrong model gets locked in behind a constant that "looks about right", and why the next
two fixes then had to fight it.

The lesson worth keeping: **"shadows are the wrong colour" is almost never a hue problem.** It is an energy
balance problem, and a colour correction would have buried it three times over. The reliable tell is ambient
*overpowering the albedo* — a shaded red-brick wall should still read as red brick, only dimmer. When it reads
as the ambient's hue instead, the ambient term is wrong in magnitude, whatever colour it happens to be.

**This is also why "just flatten the lighting on general meshes" was the wrong fix.** The proposal was to stop
applying `N·L` to general meshes because their shaded sides looked wrong. But buildings have genuine, welded
normals — their `N·L` was correct, and the darkness was a *missing ambient term*. Flattening them would have
hidden the bug rather than fixed it, and left the same missing bounce quietly darkening terrain in every valley.
The distinction that matters is **whether a surface has a meaningful normal**, not which LBS block it came from:

| | |
|---|---|
| terrain, buildings, rocks | keep `N·L` — normals are real |
| foliage cards | normals are measured garbage (89.1° off outward); heavy wrap, near-flat |
| sunset warming/dimming | already normal-independent, via `AtmSunTransmittance` |
| real occlusion | shadow maps — not something a shading term can stand in for |

It belongs as a **per-material lighting mode**, not a hardcoded category rule, because it expires twice: once
when shadow maps land, and again when §7.2b replaces cards with geometry that has real normals.

#### Black foliage — TWO causes, both fixed

Once the exposure bug was gone, trees and grass facing away from the sun rendered as black silhouettes. That
turned out to be two independent problems that happened to look like one:

1. **Everything is rasterised `CullMode NONE`,** so the back of a surface is routinely visible — and a back
   face's interpolated normal points *away* from the viewer. `N·L ≤ 0` follows, and the surface goes black even
   when the sun is on the side being looked at. Fixed with `SV_IsFrontFace`: flip the normal on back faces. This
   applies to all geometry, not just foliage.
2. **A leaf with the sun behind it is backlit, not unlit.** Real foliage transmits light; a pure Lambert term
   drops it to ambient alone, which reads as a black cut-out against a bright sky. The alpha-tested PSO is
   exactly the foliage path, so it now uses **wrap diffuse** (0.4, softening the terminator) plus a
   **transmission** term (0.5 × `saturate(dot(-N, L))`). Both are cheap stand-ins for subsurface scattering.

Worth separating in the record because fixing only the first would have left backlit vegetation black and looked
like the fix had failed.

#### Foliage cards need SPHERICAL normals — a third, separate cause

After the facing flip and the transmission term, trees still showed a hard difference between faces meeting at
roughly 90°. Measured on `track-42`: **1167 alpha-tested objects, median 20 triangles carrying 15 distinct
normals.** RBR's trees are a handful of crossed cards, each with its own plane normal — so two cards at 90° have
normals 90° apart. `WeldNormals` deliberately refuses to merge those, and it is right to: geometrically they
*are* a hard edge. The consequence is one card catching the sun while the card behind it goes dark.

The fix is to stop pretending a billboard has a meaningful surface normal. `SphericalFoliageNormals` (viewer key
`B`, on by default) points the normal outward from the object's own centre, blended 75% with the original so
trunks keep some orientation. A canopy then shades as a rounded volume rather than as crossed cardboard.

**Three distinct causes, one symptom.** Back-face normals, missing transmission, and card normals all produced
"dark vegetation", and each needed its own fix — fixing any one alone would have looked like failure. Worth
remembering the next time a single visual complaint gets a single hypothesis.

**STILL NOT FULLY SOLVED, and the reason is structural.** Large single cards remain visibly darker than their
neighbours. The spherical trick derives curvature from the spread of vertex positions about the object centre —
but every vertex of one big planar quad sits on the same side of that centre, so it resolves to a single
direction for the whole card. There is no curvature to recover because the geometry has none.

Knobs that would reduce it, none of which fix it: raise the blend from 0.75 toward 1.0, raise `kFoliageWrap`
and `kFoliageTransmission`, or make foliage ambient-dominant so directional light barely varies across it. All
of those trade correctness elsewhere for hiding this.

**The honest framing: this is a 2004 asset limitation, not a shading bug.** RBR's vegetation is a handful of
crossed cards with per-plane normals, authored for a renderer that lit from baked vertex colour and never
sampled a normal at all. No shading model fully rescues that. It gets properly fixed by §7's content path —
replacing vegetation with geometry that has real volume — and the shading work here is what makes that
replacement worth doing rather than a substitute for it.

#### Tonemapping — a bug the first build shipped with

The first dynamic build blew every surface to white, and the exposure control visibly worked on the sky while
doing nothing to the ground. Both were one defect: the scene pass applied `gExposure` to the aerial in-scatter
term only and **never tonemapped `lit` at all**. Dynamic lighting is linear HDR radiance and the target is 8-bit
UNORM, so it hard-clipped at 1.0.

Now `lit = 1 - exp(-lit * gExposure)` in the dynamic modes, matching the sky pass. **Baked mode is deliberately
left untonemapped** — RBR's vertex colour is already display-referred, and pushing it through the curve would
darken the very reference the A/B exists to compare against.

Known gaps: single scattering only, so deep shadow reads too dark until a multiple-scattering term is added; the
sky ambient is one biased sample rather than a hemisphere integral, so a surface facing a cliff still sees full
sky; no clouds, though `CloudName` and `Cloud_Scale` sit in the preset; the tonemap is a placeholder
`1 - exp(-x)`; `sunDiffuse`/`skyAmbient` are hand-set scales that exist only because sky and surfaces are not yet
in the same units, and they disappear when exposure becomes physical.

**The next real step is shadows.** Until then, mode 1 will look flatter than the bake in places — that is the
expected consequence of removing baked occlusion, not a regression.

#### Cost, and the optimisation it implies

Sun transmittance (8 steps) and sky ambient (6×2) are evaluated **per pixel**, on top of aerial perspective
(8×3). All three depend only on the sun direction and the pixel's normal, so almost all of that work is
redundant. The proper fix is a small **environment LUT** rendered once per frame — sun colour plus a few sky
directions — sampled instead of marched. The viewer's frame-time readout is there to say whether that is needed
now or later; the dev machine is an integrated Radeon, which makes it the honest place to measure.

### 5.0b Cascaded shadow maps — DONE (hard shadows)

`ngg/core/shadow.*`. Four cascades at 2048², depth-only passes reusing the scene's own vertex shader — rendering
from the light is just a different view/proj.

Choices that are about **stability**, which matters more here than in most scenes because a rally camera moves
fast and rotates constantly, and an edge that crawls is far more objectionable than an edge that is soft:

| | |
|---|---|
| **bounding-sphere fitting** per cascade | a sphere's extent does not change as the camera rotates, so the projection stops breathing |
| **texel snapping** of the light-space origin | without it, sub-texel camera motion changes which texel a world point lands in — the classic shimmer, and it reads as a *filtering* bug, sending you to the wrong place |
| **normal-offset bias**, not depth bias | offsetting along the normal by the shadow texel's own world size is scale-correct by construction; depth bias trades acne for peter-panning and must be retuned per cascade because texel size changes |

Alpha-tested foliage casts through a dedicated shadow pixel shader — otherwise a tree casts the shadow of its
bounding cards. Those casters are drawn two-sided (a card culled by winding casts nothing); opaque casters use
front-face culling so residual acne lands where the camera cannot see it. A sun below the horizon skips the pass
entirely rather than fitting a degenerate projection.

**Not built: soft shadows.** Three different things get conflated — *occlusion* (built), *filtering* (3×3
hardware PCF, built) and *penumbra* (edge widening with distance from the caster, **not** built; needs PCSS or
ray-traced shadows). Edge softness currently varies with distance because each cascade has a different world
size per texel — roughly 4 cm at cascade 0 against ~2 m at cascade 3. That is inherent to cascades, not a defect.
Cross-fading between cascades is also not implemented, so a resolution step may be visible at the boundaries.

#### The bug that made everything shadowed, and why it took a probe to find

`ShadowMaps::CreateSrv` was written and **never called**. The heap slot was reserved, the heap sized for it, the
descriptor table bound to it — and the descriptor itself never written.

**An unwritten D3D12 descriptor reads as zero, and it is not an error.** The debug layer said nothing. Every
`SampleCmp(0.498 <= 0)` failed, so the entire world came back shadowed.

The probe distinguished it in one click, and the distinguishing value is worth remembering:

| STORED depth reads | meaning |
|---|---|
| **1.0** | shadow map is empty — cleared, nothing rendered into it |
| **0.0** | descriptor never written — *not* the same thing, and the difference is the whole diagnosis |
| ≈ receiver depth | the surface is its own occluder — bias too small |

"Reserve the slot, bind the table, forget to write the descriptor" produces a plausible picture, no validation
message, and a symptom ("everything is in shadow") that invites hours of matrix debugging.

### 5.0c HDR intermediate, TAA and a real tonemap — DONE

`ngg/core/post.h/.cpp`. Sky and scene now render into an **RGBA16F intermediate**; one pass at the end does
temporal anti-aliasing, then exposure, a tonemap curve and the sRGB encode, into whatever the client's real
target is (the swap chain, or the shared texture in-game). Both clients go through it — `viewer.cpp` and the
`Renderer` in `main.cpp` — because a post chain that only exists in the viewer is a post chain that is wrong
in the game.

**TAA was done first, and not because aliasing was the worst artefact.** Soft shadows (PCSS), SSAO, stochastic
alpha and every ray-traced effect are noisy techniques that are only affordable once a temporal filter exists to
accumulate them. Building any of them before TAA means building them twice.

**Reprojection needs no motion vectors, and that is a property of the content rather than a shortcut.** The
converter bakes every transform into world space, so the world is rigid and the only thing that moves is the
camera — camera reprojection from depth is then *exact*, not an approximation. That stops being true the day
per-object transforms land (§9b item 7), and a velocity buffer has to appear with them. The assumption is
written into `post.h` so it cannot be forgotten there.

Three decisions inside it are worth keeping:

- **Reprojection stays in homogeneous coordinates.** Reconstructing a world position first needs a divide by
  `w`, and for a sky pixel — reverse-Z depth 0, the far plane — that point is at infinity and the divide gives a
  NaN. Carried as `float4(ndc, depth, 1) * (invVP · prevVP)`, one matrix maps the finite geometry *and* the point
  at infinity correctly, and the sky reprojects as pure camera rotation with no special case at all.
- **PostFx is handed `Scene::LastViewProj`** rather than rebuilding the product from view and proj, because
  reverse-Z is applied inside `Scene::Record` and a reprojection that differs from the rasteriser by one term
  produces a wrong image that reads as a TAA tuning problem.

> **The jitter must NOT be in that matrix, and getting it wrong shakes the whole picture.** First version
> reprojected with jittered matrices on both sides, on the reasoning that the reprojection should match the
> rasteriser exactly. It matched — and the image visibly wobbled, reported straight away as "why is the picture
> shaking when TAA is enabled".
>
> The reasoning was wrong about *what* TAA aligns. It accumulates into a **fixed pixel grid**; the jitter varies
> only where inside a pixel the scene is *sampled*. Reproject through it and the history follows the jitter
> rather than staying put, so the resolved image oscillates across the whole Halton pattern — up to a full pixel
> of whole-image movement, every frame.
>
> `LastViewProj` is therefore reverse-Z **included**, jitter **excluded**, and the asymmetry is the point:
> reverse-Z is part of what the depth buffer means, the jitter is not. Reprojecting an unjittered ray against a
> depth value that *was* jittered is wrong by at most half a pixel of parallax — real, universally accepted, and
> orders of magnitude smaller than the shake it replaces.
- **The jitter goes to the sky too.** `Sky` subtracts it when building its ray. A sky that does not jitter with
  the geometry anti-aliases the world against a static background, and the horizon — the one place the two meet
  — crawls.

**Exposure and the encode moved out of the shaders that write colour.** This closes the bug in §5.0 properly
rather than by hand: the sky pass applied exposure and the scene pass did not, which is exactly why the exposure
control appeared to work on the sky and do nothing to surfaces. There is now one place that can get it wrong.

Five curves, and the last three are the only ones a user picks:

| curve | when | why it exists |
|---|---|---|
| raw | any debug view | debug views carry **false colour, not radiance**; a curve would silently change the numbers they exist to show, and TAA would blend them |
| sRGB only | baked lighting (mode 0) | RBR's vertex colour is display-referred already, so mode 0 stays byte-identical and the A/B against vanilla stays honest |
| `1-exp(-x)` | `T` | what dynamic lighting shipped with — kept so the change can be judged against it rather than remembered |
| **ACES** (Narkowicz) | `T`, **default** | punchy, cheap, the familiar game look |
| AgX | `T` | rotates towards the achromatic axis in log space, so a clipping channel loses saturation instead of hue — ACES turns a blown sun disc yellow before it turns white, and this does not |

Viewer keys: `T` cycles the curve, `Y` toggles TAA. History is dropped on reload and on resize, because in both
cases the previous image describes something else.

Still open here: no history rectification beyond a 3×3 variance clip (a Catmull-Rom history filter is the usual
next step), no sharpening to recover what the temporal filter softens, and the exposure is still a hand-set
constant rather than a measured or auto-adapted one.

### 5.0d Ambient specular — the prefiltered sky, and half of the environment LUT

`ngg/core/env.h/.cpp`. A 64×64 octahedral RGBA16F map with a 7-level mip chain, rebuilt **every frame** from the
same `SkyParams` the sky pass draws with. The scene samples it along the reflection vector, weighted by Karis'
analytic split-sum BRDF fit.

**What it fixes is a hole, not a subtlety.** Until now only the *sun* had a specular lobe. Everything else in
the frame reflected nothing at all, so a polished surface and a rough one differed solely in the size of one
highlight. That reads as flat however good the material data is — and it mattered right now rather than later,
because PBR maps are about to arrive (§3.3g) and would otherwise have been judged on half their signal, with
roughness looking like it barely did anything.

**One texture, two jobs, and the second is the point of §9b item 14.** Sky radiance depends only on direction
and sun position — both uniform for the frame. Marching the atmosphere per pixel to answer a question with
4,096 distinct answers was always the wrong shape. The map costs 4,096 marches for the whole frame against one
per pixel.

**Octahedral, not a cube map.** A 2D texture takes an ordinary mip chain and an ordinary bilinear sampler; a
cube map needs per-face filtering and a seam policy to get the same thing. The cost is a small filtering error
along the octahedron's own boundary, invisible for a sky — which is nearly a gradient — and the wrong trade for
a sharp reflection probe, so it should be revisited when one exists.

**The mip chain IS the roughness axis.** Mip 0 is the mirror, each level a 2×2 box downsample of the last, and
roughness picks a level. A GGX-weighted prefilter is more correct at the middle levels; successive downsampling
is what most engines actually ship, and calling it an approximation is more useful than pretending otherwise.

Four decisions worth keeping:

- **`AtmSkyRadiance` is now shared** — scatter plus the below-horizon ground blend, called by both the sky pass
  and the environment pass. Same argument as the top of `atmosphere_hlsl.h`: they meet at the horizon, which is
  exactly where two copies would disagree. The sky pass previously carried its own copy of the ground blend.
- **The sun disc is deliberately NOT in the shared function.** The environment feeds a specular term that
  already has its own direct sun lobe; a disc in both counts the sun twice, as a hard bright spot in every
  reflection.
- **Rebuilt every frame, with no caching policy.** The whole chain is under 40 KB, so following a moving sun
  costs less than any rule for deciding when it went stale — and a staleness rule is a thing that can be wrong.
- **The same `×π`** as the direct lobe, for the reason §3.3g spells out: the diffuse here is `albedo * E`
  rather than `albedo/π * E`, so every specular term has to carry the factor or the balance is off by π.

**The failure mode this was written against:** an unwritten D3D12 descriptor reads zero with no validation
error. That cost a day when `ShadowMaps::CreateSrv` was never called and every pixel came back in shadow
(§5.0b). Here it would be quieter still — nothing would reflect anything, which looks exactly like "the feature
is subtle". So the environment SRV is created on first use, logged with its heap slot, and the probe reports
the term by name (`ambient SPECULAR (rgb) / envOn`).

`J` toggles it in the viewer, because a term that cannot be A/B'd cannot be judged.

**The ambient DIFFUSE is a lookup too now**, which closes item 14. `E_sky` used to march `AtmScatter` for every
pixel in direction `normalize(N + up)`; it samples the map in that direction instead. The swap is faithful
rather than approximate: `N + up` is in the upper hemisphere for *every* possible normal, and that is exactly
where `AtmSkyRadiance` and `AtmScatter` agree term for term.

**Measured on track-108, same camera: 17.7 fps (56.65 ms) → 19.0 fps (52.54 ms).** A 4.1 ms saving, about 7%.
Smaller than the removal of a 6×2-step march per pixel suggests, and the reason is worth having on record:
**aerial perspective is now the dominant per-pixel atmosphere cost**, at 8×3 steps — twice what was removed —
and it cannot use this map because it integrates over the camera-to-surface segment, which varies per pixel by
distance. The standard answer is a froxel aerial-perspective LUT, and that is now the next thing worth doing to
this pass rather than a guess about what is slow.

One honest caveat: this is not bit-identical to what it replaced. The scene's own march used 6×2 steps and the
environment map is built at 16×4, so the ambient is now computed *more* accurately than before, not identically.
Any small brightness difference is the old approximation being replaced, not a new one being introduced.

`gEnvReady` gates this and is deliberately separate from `gEnvOn`: `J` toggles ambient specular only, so
turning it off to judge the specular term does not silently change the diffuse underneath it. A client that
never sets an environment still gets the march, because a renderer that quietly drops ambient when a caller
forgets a setter is worse than one that is merely slow.

### 5.0e Ray-traced sun shadows are the lighting's shadow term — and the frame got faster, not slower

The probe (§1.8.5) became the real thing. `R` in the viewer switches the sun-visibility source between the
cascaded shadow map and the ray tracer, and **RT is the default whenever an acceleration structure exists.**

**Measured on `track-108`, same camera, integrated Radeon:**

| | fps | frame |
|---|---|---|
| cascades, no prepass | 18.7–19.0 | 53 ms |
| **depth+normal prepass, RT shadows** | **28.9–29.8** | **34 ms** |

Nineteen milliseconds *back*, from adding ray tracing. Two things stacked, and both are structural rather than
tuning:

- **The cascades are not rendered at all in RT mode.** Four extra full-scene passes were the single most
  expensive thing in the frame (§5.0b: they took 40 fps to 12 before culling recovered it to 25). The ray
  tracer costs 0.5 ms for one ray per pixel and replaces all four.
- **The depth+normal prepass makes the main pass shade every pixel exactly once.** `Scene::RecordPrepass`
  draws the geometry with a trivial shader into depth and an `R8G8B8A8_SNORM` normal buffer; the main pass then
  tests `GREATER_EQUAL` against depth it already has. That is the classic Z-prepass win, it applies whichever
  shadow source is on, and it is the reason the prepass runs even in cascade mode once RT exists.

**Why a prepass and not "trace in the pixel shader".** Inline RayQuery inside the lighting shader would need no
G-buffer and no reordering — but it would put every ray-traced term into a pixel shader with overdraw, and
ambient occlusion (next) wants several rays plus a denoise, which is compute work. Depth+normal → compute RT
passes → forward lighting consumes textures is the shape that scales to AO, reflections and GI. The prepass
also removed the one-frame lag the probe had: depth and normal now exist before the trace, in the same frame.

**The normal-offset bias, finally.** The probe offset along the ray because it had no normal; the prepass
provides one. The origin steps 2 cm along the geometric normal — flipped towards the sun, so a two-sided leaf
card is pushed to whichever side the ray leaves from — growing 0.2%/m with distance because depth precision
does. Measured effect: shadowed fraction 1.3% → 5.1%, leaf-test passes 2.4k → 9.7k. More shadow, and it is the
*correct* extra: the old 5 cm step along the ray skipped every occluder within 5 cm of the origin, which is
exactly where grass shades the ground beneath it.

> **One bug on the way, and it took the device down.** The prepass parked its constant buffer in the same
> keep-alive slot the main pass uses, so the main pass released it before the GPU had read it. A freed upload
> buffer under a root CBV is a GPU page fault: `DXGI_ERROR_DEVICE_HUNG`, then a cascade of barrier-mismatch
> errors that were all fallout from that one release. The first validation message in the log named it —
> "resource deleted prior to closing the command list" — and it was the only one that mattered. When the debug
> layer prints five errors, the first is the cause and the rest are weather.

> **A second bug, reported by the user as "the screen blinks between two scenes every frame", and it was not
> TAA.** It vanished with TAA off, so it looked like a TAA bug; the depth+normal buffers dumped from two
> consecutive frames were identical, and the HDR target was not — so the *main pass* was drawing differently
> on alternate frames against a constant prepass depth. The cause was one line's position: `SetJitter` sat
> *after* `RecordPrepass`, so the prepass rasterised with **last frame's** sub-pixel offset and the main pass
> with this frame's. A different jitter is a different depth; `GREATER_EQUAL` failed wherever the main pass
> landed fractionally farther, the sky's below-horizon colour showed through, and the pattern flipped with the
> Halton sequence's alternating sign. TAA off means jitter zero means both passes agree — which is why the
> symptom pointed at the wrong suspect.
>
> The rule, now written where the jitter is set: **every pass that rasterises a frame must use the same
> offset, and it must be set before the first of them.** And the method that found it in four runs rather than
> an afternoon: `--dump N` writes the back buffer *and every intermediate* (HDR, TAA history, depth, normal) for
> N consecutive frames, and `--rt 0|1|2` plus `--keys` bisect the pipeline from the command line. "The picture
> is wrong" became "this buffer is wrong" became "these two passes disagree", and each step was a diff of two
> files.

**Soft shadows, and the denoiser they forced.** The user's remark that RT shadows were "too sharp" was right for
the wrong reason: they were correct *for a point sun*. The sun is a disc about 0.53° across, so each pixel's ray
now goes to a random point on that disc — uniform over the area (`sqrt` on the radius, or the centre is
oversampled and the penumbra hardens in its middle) — scaled by the same `sunDiscSize` the sky pass draws, so the
visible disc and the shadow's softness agree by construction. Cost 0.50 → 0.57 ms.

That immediately produced shadows that "blink and jitter in places", and it was the colour TAA doing exactly its
job: its variance clip exists to *reject* history that disagrees with the neighbourhood, which is what a
converging penumbra looks like from inside it, so patches never converged. **The visibility is now filtered
before the lighting sees it** (`kDenoiseSrc` in `rt.cpp`): a depth-aware 5×5 mean of this frame's coin tosses,
then a temporal blend at 0.85 with last frame's filtered result, reprojected through the *unjittered* matrix (the
same rule as PostFx) and accepted only where it lands on the same surface — linear depth within 3%. The lighting
reads the RG16F history (visibility, depth) rather than the raw R8. Measured: raw has 2 distinct values and a
frame-to-frame diff of 0.78; filtered has 256 and 0.34, and 0.7% of pixels sit in a genuine penumbra. The colour
TAA then has nothing to fight.

`--dump` writes both (`rtraw`, `rtvis`), which is how the above was checked rather than eyeballed.

> **And then the RT shadows vanished, silently.** The denoiser ping-pongs two history textures, so
> `Rt::Filtered()` returns a different pointer every frame; `Scene::SetRtVisibility` saw a new resource each
> frame and reset its "SRV written" flag each frame; and the camera constant buffer, filled *before* the block
> that rewrites the descriptor, read that flag as "not ready" on every main pass. `gRtReady` was 0, the lighting
> fell back to the shadow map, and in RT mode the shadow map is not rendered — so no shadows, and no error,
> because every individual step was valid. The flag now gates only the descriptor write; the constant buffer
> tests the resource. A "ready" flag that a *legitimate* change of resource resets is a flag that turns a
> feature off by accident, and it did.
>
> Also from the same round: `H` (shadows off) now applies to both sources. It switched off the cascades and
> left the ray-traced term running, which reads as "the other mode has no shadows" the moment `R` is pressed.

#### Ray-traced ambient occlusion — the term that was exactly zero

Same acceleration structure, same surface reconstruction, same any-hit test, same denoiser. `kAoMainSrc` in
`rt.cpp` fires cosine-weighted hemisphere rays from the prepass position and normal — cosine-weighted so the
plain average of hit/miss *is* the occluded fraction of the diffuse ambient integral with no extra weighting —
out to a 3 m radius, 2 rays per pixel per frame, into its own R8 output, through its own pair of RG16F histories
in the shared `Denoise`. The lighting multiplies it into `matAo`, which scales the **ambient terms only** — sky,
ground bounce, ambient specular — and never the sun, which has its own visibility. That is what occlusion means:
how much of the sky this point can actually see. Until now that was "all of it", everywhere, which is most of why
props read as pasted onto the terrain.

**The radius is where occlusion stops and global illumination starts.** An occluder ten metres away blocks sky,
but a wall that far away also reflects light this renderer does not simulate; beyond the radius the honest
answer is "unknown", and unknown means unoccluded here. 3 m is the scale of what should darken — the base of a
wall, the gap between tyre and tarmac, the inside of a canopy — not a hillside.

Measured: shadow + AO together **1.5 ms at 224k pixels** on the integrated Radeon (AO ≈ 1 ms of it), rising to
7.4 ms at 930k pixels in dense foliage with the camera moved — 2.8M rays. On the intended floor hardware that is
around a millisecond; here it is the place to spend the half-resolution or one-ray budget if it ever has to be
spent. Frame rate with everything on: 26.8 fps, from 29 with RT shadows alone.

`O` toggles it, view 13 shows it raw (`V`), `--dump` writes `rtao`. On the start-line field it darkens the tree
crowns and the ground under the hedgerow and little else, which is correct for open ground and a 3 m radius.

**What the cascades still do:** exist, one key away, as the reference. Their bias, PCF and per-cascade fitting
are not deleted yet, because the RT path has been seen on one GPU in one viewer. When it has been seen in-game
on discrete hardware, §9b gets an item to remove them.

**Known and recorded:** the normal buffer is the *geometric* normal, not the normal-mapped one — a ray-origin
offset wants the surface, not the paint. And `SunVis` still samples the shadow map in debug views 11+ so view 12
can compare the two; that lookup is a few taps and is not where the map's cost was.

### 5.0f Aerial perspective is a froxel table — the last per-pixel atmosphere march is gone

§9b item 21. After the environment map took the ambient march out of the pixel shader (5.0d), the one march
left was aerial perspective: `AtmScatter` over the camera-to-surface segment, 8 view × 3 light steps, on every
geometry pixel. The environment map could not answer it, because it tabulates direction only; aerial
perspective depends on **direction and distance**. So that is the table: `AerialLut` (`ngg/core/aerial.h`),
two RGBA16F volumes of **64 × 64 screen cells × 32 distance slices** holding in-scatter and transmittance. One
compute dispatch fills it per frame; the scene pass replaces the march with two trilinear fetches.

**Same integral, same quadrature.** Each froxel calls the shared `AtmScatter` with the same 8×3 steps the
per-pixel path used. 16×4 was tried first: it cost 0.60 ms of fill for no difference that survived the
tonemap. Matching the reference has a second payoff: the per-pixel march survives as an *exact* reference
(`L` cycles table → march → off), so any difference between the two is the table's own interpolation error
and nothing else.

Design points, each of which would be a bug if done the other way:
- **Radial distance, not view depth.** The integral is along the ray, and the march measured `length(wpos -
  camPos)`. Slices are quadratic (`w = sqrt(d / maxDist)`) so the near field, where contrast is judged against
  the road, gets most of the resolution.
- **Below the first slice centre** (a few metres), the lookup scales towards zero in-scatter and full
  transmittance by `d / d0`. Both are linear in distance that close, so this is exact to first order. Clamping
  to slice 0 would put that much air between the camera and its own bonnet.
- **Reach = 1.5 × the far plane**, read from the projection per frame: the far plane bounds view depth, and
  radial distance at the frame corners runs ~1.4× further. Unreadable projection → 12 km.
- **Unjittered matrix for the cell directions.** A cell spans ~30 px, so the TAA jitter is irrelevant to it,
  and following the jitter would only make the table shimmer. This is the jitter rule from 5.0c applied the
  other way round: nothing here reconstructs a surface point.
- **CLAMP sampler (s4).** Wrapping in `w` would blend the farthest slice into the nearest.
- **Owned by `Scene`, filled inside `Scene::Record`**, so the game host, the viewer and Wallaby's API all
  get it without being wired up, the way the environment map had to be. The prepass does not pay for it.
- **Camera-fixed, history-free.** The model puts the camera at a constant altitude on a planet-sized sphere,
  so the table depends only on view orientation and the sun. It is rebuilt from scratch each frame with
  nothing to reproject.

**Measured, on track-108 at the start camera, GPU timestamps on the main pass** (new: `Scene::LastPassMs` /
`LastAerialMs`, on the HUD and in the log every ~120 frames):

| aerial | main pass | aerial's share |
|---|---|---|
| off | 1.65 ms | — |
| per-pixel march (old) | 3.55 ms | **1.9 ms** |
| table, 16×4 fill | 2.42 ms | 0.77 ms (0.60 fill) |
| **table, 8×3 fill** | **2.14 ms** | **0.50 ms** (0.29 fill + ~0.2 lookup) |

Image A/B on the same frame: **table vs march, max 1/255 on the HDR buffer, 99.4 % of pixels bit-identical**;
table vs aerial off, up to 66/255 with 7 % of pixels moving by more than 4. So the term is real at this camera
and the table reproduces it.

**The saving grows with what is on screen.** The march costs ~10 ns per *geometry* pixel. At the start camera
only ~194k pixels are geometry; the rest is sky. The fill is a fixed 0.29 ms whatever the view. In the 930k-pixel
foliage view of 5.0e the march would extrapolate to ~9 ms and the table to under 1.5 ms. That is an
extrapolation from per-pixel costs, not a measurement at that camera.

**Whole-frame fps did not move** (24.6–26.1 across all three modes) and that is not a contradiction. The
viewer's frame is synchronous, so fps is CPU recording + GPU execution + readbacks in series, and at this camera
the frame is not bound by the main pass. That is why the pass timer exists: fps cannot attribute a change, and
a pass can lose 40 % of its cost without fps showing it. Anything perf-related from here on is judged by GPU
timestamps.

Knobs, if they are ever needed: 32×32 screen cells would cut the fill ~4× (UE4 ships 32×32×32). It was not
taken because 64×64 already costs less than the lookup it feeds, and the Mie lobe towards a low sun is the
one feature that would test a coarser grid. That case has not been looked at yet.

### 5.0g Auto exposure — metered from the image, adapted on the GPU

§9b item 17. Exposure was a hand-set constant (`SkyParams::exposure`, 0.8). It was right for the view it was
tuned on, and wrong for any view much darker or brighter: under a canopy, with the sky off, or at a low sun.
`PostFx` now meters the image it is about to tonemap and adapts. It is two compute passes in the same command
list, with no CPU in the loop:

1. **`CSHistogram`**: 128 bins of log2 luminance over [2^-12, 2^8], one sample per 2×2 block (a statistic over
   a million pixels does not need all of them). It meters the *resolved* image (the TAA output, or the raw HDR
   without TAA), because that is what the tonemap sees. **Centre-weighted 4:1**, aspect-corrected: the driver
   looks down the road, and a stage puts the most sky at the top edge and the most verge at the bottom corners.
2. **`CSAdapt`**: one group. It takes the mean log2 luminance between the **40th and 90th percentiles**,
   counting straddling bins fractionally so the result moves smoothly, and clears the histogram for the next
   frame. It then adapts **exponentially in log space**, so a step of N EV settles in the same time whatever
   N is. Adaptation is asymmetric: 3/s towards bright (closing down to glare), 1.2/s towards dark (opening up
   to shade). A forest exit into sun is the case that matters. The result is clamped to ±3 EV around the dial,
   so night stays night.

A **histogram, not a log-average**, because a plain mean is dominated by exactly the pixels metering should
ignore: the sun disc pulls it up and deep shadow pulls it down. Percentiles drop both tails.

**The dial keeps one meaning.** With auto on, `SkyParams::exposure` is the exposure at *reference brightness*:
`exposure = dial × 2^clamp(ref − adapted, ±3)`. The reference was **measured, not chosen**: the track-108 start
camera, the view the 0.8 dial was tuned on, meters at log2 **−2.025**. So switching auto on leaves that view as
it was: **0.798 auto against 0.800 manual**, measured. `-`/`=` still move the dial, and `X` turns auto off for
A/B. Darker views get more exposure, brighter ones less.

**Measured step response:** sky off (`K`) makes the frame 1.58 EV darker. Exposure goes 0.80 → 1.54 → 2.02 →
2.24 → 2.38 at ~30-frame log intervals and settles at exactly +1.58 EV with no overshoot. A static camera
meters identically frame to frame (±0.001 EV): the TAA jitter does not make the meter hunt.

Details that would be bugs the other way round:
- **Buffers decay to COMMON at the end of every `ExecuteCommandLists`.** The first version declared the
  exposure buffer `PIXEL_SHADER_RESOURCE` "between frames", which is false from frame 2 on. Both buffers now
  start each frame from COMMON, and the state buffer's state is tracked through the frame (TAA reads it, the
  meter writes it, a copy reads it, the tonemap reads it).
- **TAA's firefly weight uses exposed luminance**: `1/(1 + luma × exposure)`. The weight is meant to behave
  like a display-referred average, which only holds if 1.0 means "bright on screen". With exposure fixed near
  1 the difference was invisible; with exposure swinging several EV it is not. TAA runs before the meter, so
  it uses last frame's exposure, which is exact enough for a weight.
- **Only the exposing curves meter** (1-exp, ACES, AgX). The baked-reference and raw-debug curves never apply
  exposure, and the meter snaps instead of adapting when it resumes, because whatever it held is stale.
- **Snap on `ResetHistory`** (reload, teleport): adapting from the previous world is wrong.
- **dt is measured inside `PostFx`** (QPC, clamped to 0.1 s) so every client adapts without having to pass a
  frame time, and a breakpoint does not count as seconds of adaptation.
- **The editor must be able to lock it**: `ngg_set_auto_exposure(scene, 0)`. A meter partly undoes a
  brightness edit: brighten a material that fills the view and exposure comes down to meet it. Noted in
  Wallaby's CLAUDE.md for the material inspector.

**Not done, and not what it was sold as.** §9b item 17 said a physical exposure "finally removes `sunDiffuse` /
`skyAmbient` as separate fudges". It does not, and could not: exposure is one global multiplier, and those two
scale *surfaces relative to the sky*. Whether they can go is a units question about the lighting (item 24),
and auto exposure is neutral to it. Cost of the two passes: not timed separately. The read is quarter-resolution
and the second pass is one 128-thread group; the main-pass timer did not move.

### 5.0h TAA history filtering and sharpening

§9b item 16. TAA was soft for **two separate reasons**, and they need different fixes:

1. **History resampling, under motion only.** The history was fetched with one bilinear tap at its
   reprojected position. Any camera motion that is not a whole number of pixels (every frame of driving)
   makes that a small blur. At a 0.9 history weight the blurs **compound**, so the image converges to
   something softer than any single frame. At a static camera the tap lands on texel centres and costs
   nothing, which is why this was easy to miss in still-frame comparisons. **Fix: Catmull-Rom**, the standard
   5-tap form (the middle rows/columns of the 4x4 kernel merged into bilinear taps placed at the ratio of their
   weights, the four negligible corners dropped and the rest renormalised). It interpolates rather than
   smooths, so detail survives repeated resampling. Clamped at 0, because its negative lobes can undershoot
   next to a bright edge and a negative value must never reach the 1/(1+luma) weight.
2. **Accumulation, always.** Averaging jittered samples integrates over the pixel footprint, a 1-pixel box
   filter, so TAA is softer than one aliased sample even at rest. That is inherent. **Fix: a post-tonemap
   sharpen**: an unsharp mask on the 4-neighbour cross, **clamped to that cross's min/max**. The clamp is the
   whole safety argument: an edge can get steeper, but no value is created that was not already among the
   pixel and its neighbours, so there are no halos or ringing to tune away. It runs on display values
   (after the curve, before the encode) because in HDR a sharpen is dominated by the brightest edge in the
   frame (the sun against the sky) and does almost nothing to the road texture next to it. Applied only to
   a TAA-resolved image, since without TAA there is nothing to give back.

**Measuring motion needed a new instrument**: `--pan <deg/frame>` turns the camera by a fixed amount per
*frame*, not per second, so frame N has the same camera in every run, TAA on or off. Metric: mean |Laplacian|
of luma below the HUD (detail energy), and mean frame-to-frame change across two consecutive dumps
(temporal stability, meaningful at a static camera). track-108, start camera:

| | TAA off (aliased ref) | bilinear | Catmull-Rom | CR + sharpen 0.5 |
|---|---|---|---|---|
| panning 0.5°/frame (~10 px/frame) | 4.97 | 1.63 (33 %) | 2.12 (43 %) | **2.55 (51 %)** |
| static | 7.96 | = CR | 4.20 (53 %) | **5.10 (64 %)** |

The TAA-off column includes aliasing energy (stair edges, sparkle), so 100 % is not the target; it is the
ceiling. At a static camera sharpen 0.5 raises frame-to-frame change from 0.206 to 0.238 (+15 %, out of 255):
it re-amplifies a little of the jitter residual. That is the cost, and it is why the amount is a dial. Checked
by eye at 2x on the horizon band: bilinear is visibly smeared under the pan, Catmull-Rom clearly crisper,
sharpened closest to the no-TAA frame without its stair-stepping, no halos.

Defaults: Catmull-Rom on, sharpen 0.5. Viewer: `I` toggles the history filter (bilinear is the reference),
`P` cycles sharpen 0 / .25 / .5 / .75; both on the HUD.

**What is left, measured rather than guessed:** under the pan TAA still keeps only half the detail it keeps
at rest (51 % vs 64 %). That remainder is the variance clip and the fixed 0.9 blend, not the fetch. The
standard next step is a motion-adaptive blend (less history as reprojection velocity rises). It is not taken
here because 10 px/frame is a fast pan, and driving speeds should be judged in the game before tuning for them.

### 5.0i The units audit: what `sunDiffuse` and `skyAmbient` were compensating for

§9b item 24. The two dials scale surface lighting *relative to the sky*, so no exposure can remove them (5.0g).
The question was whether they hide a units error or a model deficit. They hide **two deficits**, and then
overshoot.

**Where the code stood, relative to physical Lambert + GGX** (the sky pass is physical radiance, 1×):

| term | code | physical | code ÷ physical |
|---|---|---|---|
| diffuse, sun | `albedo · 0.18 · E_sun` | `albedo/π · E_sun` | 0.57× |
| diffuse, ambient | `albedo · 1.0 · E_amb` | `albedo/π · E_amb` | 3.14× |
| specular, sun / ambient | `π ·` either | no factor | 0.57× / 3.14× |

Internally consistent (specular tracks diffuse), but ambient is **5.5× stronger relative to the sun** than the
physics. The ground bounce also mixed a sunDiffuse-scaled sun with an unscaled sky.

**What the model itself says.** I ported `AtmScatter`/`AtmSunTransmittance` line for line to numpy and
integrated the sky over the hemisphere (a cosine-weighted 24×48 quadrature):

| sun elevation | E_sun horizontal | E_sky true | shader's `π·L(N+up)` | model sun:sky | code sun:sky |
|---|---|---|---|---|---|
| 50° (track-108) | 14.79 | 0.973 | 0.635 (**0.65×**) | 15.2 : 1 | 4.2 : 1 |
| 30° (default) | 9.10 | 0.905 | 0.446 (0.49×) | 10.1 : 1 | 3.7 : 1 |
| 10° | 2.21 | 0.699 | 0.300 (0.43×) | 3.2 : 1 | 1.3 : 1 |

A real clear sky at 50° has a diffuse fraction of ~10–15 %, i.e. ~6–9 : 1. The two deficits:

1. **The sky-irradiance estimate was a different, smaller number.** `π · L(N + up)` is one sample, and for an
   upward surface it looks at the zenith, the darkest part of a clear sky. It was 0.43–0.65× the true
   integral, worst at low sun.
2. **The model is single-scattering only**, so its sky is about 2× too dim against real diffuse fractions.
   That is a known property of single scattering, worst at low sun and near the horizon.

**Built: the irradiance map** (`Environment`, 16×16 octahedral). For each normal it holds the cosine-weighted
integral of the sky over the upper hemisphere, by 16×32 quadrature with exact per-ring solid angle. It is
rebuilt every frame with the radiance map. **Verified against the numpy integral for the same direction:
GPU 0.6221 / 1.0234 / 1.6709, reference 0.6220 / 1.0247 / 1.6755, within 0.3 %.**

**Built: a physical balance**, `Z` in the viewer (`Scene::SetPhysicalLighting`). It uses diffuse `albedo/π`,
specular with no factor, the sun at its model irradiance, the sky from the irradiance map, and the ground as
a uniform Lambertian plane (`ρ_g · (E_sun·sinα + E_sky,up) · (1−N.y)/2`, where the π's cancel). There is no
lerp and no dials. **Default stays the established balance** until item 25 (below) lands.

**Measured, start camera, aerial off and manual exposure, ACES inverted to scene radiance, lit/shadow
classified by the RT visibility mask:** physical is **1.51× brighter in sun and 1.29× in shade**, so
lit:shadow contrast is only **×1.17** higher. The absolute 1.5× is what auto exposure absorbs. The contrast
barely moved, against the 3.6× the horizontal-surface table predicts, because of the **ground bounce**:
physically it is driven by the full sun, against 0.18× before, and at RBR's ground albedo (~0.35) it is the
dominant light on anything facing sideways or down. 2,752 of the 2,783 shadowed pixels here are foliage and
vertical faces. **Only 31 face upward**, so cast shadows on open ground, where physical *will* be darker
(the sky term is 0.49× of before), cannot be judged at this camera.

**Found on the way, and fixed:**
- **The environment map existed only in the viewer.** The game host and Wallaby's API never built one, so
  in-game there was **no ambient specular**, and the sky ambient fell back to the per-pixel march. `Scene`
  now owns the environment and rebuilds it at the start of each main pass (as it owns the aerial table), so
  every client gets the same lighting. *In-game ambient specular is new behaviour from this change.*
- **`ngg_set_sun` never reached the scene's lighting**: it updated the API's sky parameters, which reached
  `Scene` only in `ngg_set_sky`. Moving the sun in the editor moved the sky's sun and not the shading.
  `ngg_render` now syncs every frame, as the viewer and the game host already did.

**What is left, in order:** multiple scattering in the atmosphere (item 25) is the remaining deficit. When it
lands, physical becomes the default and `sunDiffuse`/`skyAmbient`/`kSpecScale` are deleted rather than
re-tuned.

### 5.0j Multiple scattering, and physical lighting becomes the default

§9b item 25, the deficit 5.0i left. `AtmosphereMs` (`ngg/core/atmosphere_ms.*`) is Hillaire 2020's section 5.5:
a 32×32 table Ψ_ms(altitude, μ_s). It computes second-order scattering (isotropic, from 64 directions × 20
steps, including the ground as a Lambertian reflector) plus the fraction f_ms of isotropic light the
surroundings scatter back, and sums all higher orders as `1/(1 − f_ms)`. `AtmScatter` adds
`σ_s · Ψ_ms · T_view` per view step, so the sky, the radiance and irradiance maps, the aerial table and the
fallback marches all change together, from the one implementation.

**The table does not depend on where the sun is.** μ_s is a lookup coordinate, so the table changes only with
the atmosphere's composition (turbidity, ground albedo). It is recomputed only when those change. That removes
any ordering problem: the sky pass runs before the Scene, so each keeps its own instance, and the same inputs
give the same table. It binds at `t0/s0 space1` in all four root signatures (sky, environment, aerial, scene)
through `MsSrvRange()` / `MsSampler()`. Off is an all-zero table, which is *exactly* the old
single-scattering model, so the A/B (`C`) costs no shader branch.

**Verified:** the GPU texel at (937.5 m, μ_s 0.781) is `0.02319 0.02943 0.04071` against a numpy port of the
same integral at `0.02319 0.02944 0.04073`, i.e. fp16 precision.

**Cost**, with the timer split into environment / aerial / draws (see below): environment + irradiance
**0.32 ms**, aerial fill **0.48 ms** (from 0.29: the per-step table lookup, paid whether the table is zero
or not), main pass 2.87 ms on track-108. Switching multiple scattering on costs nothing over off.

**Measured in the lab** (5.0k), with an 18 % grey upward ground in sun and in the wall's shadow, exact linear
values from the pixel probe:

| | sun : sky on flat ground | lit : shadow |
|---|---|---|
| established, single scattering (the old default) | 4.2 : 1 | 4.95 : 1 |
| established + multiple scattering | 3.2 : 1 | 3.94 : 1 |
| physical, single scattering | 14.0 : 1 | 15.5 : 1 |
| **physical + multiple scattering** | **10.6 : 1** | **11.8 : 1** |

The lab reproduces the numpy audit of 5.0i (14 : 1 and 4.2 : 1), which validates both. Multiple scattering adds
32 % sky light (E_sky 0.97 → 1.28). **10.6 : 1 is clean, clear air** at a 45° sun; real clear skies run about
5–10 : 1 by haze, and `turbidity` is the dial for a hazier day. The established balance behaves like an
overcast-to-hazy sky, with shadows about 3× too bright against the sun.

**So physical + multiple scattering is now the default** (`Scene::m_physical = true`). Auto exposure keeps the
overall brightness: it maps the metered average to the same key (dial × 2^ref ≈ 0.2, a middle grey), so the
track-108 start view meters at exposure 0.506 and looks like it did, with a cleaner blue sky and more depth in
the shaded tree lines. **The established balance stays behind `Z` for one round** of in-game comparison; then
`sunDiffuse`, `skyAmbient`, `kSpecScale` and the lerp ambient are deleted (item 26).

**Found on the way:** the main-pass timer billed the environment pass, now recorded inside `Scene`, to "aerial
fill". The HUD showed 1.35 ms for a 0.48 ms pass. The timer now has one timestamp per stage (`LastEnvMs`), and
the lesson is general: *a stage recorded before another stage's first timestamp silently bills its time to
that stage.*

### 5.0k The lab — a synthetic scene for measuring, not judging

Every measurement on a real track needed a paragraph of caveats: baked textures of unknown albedo, 31
upward-facing shadowed pixels at the start view, exposure dominated by an 85 % sky. `--lab` loads a scene built
in memory (`ngg/host/testscene.*`) through the ordinary `Scene::LoadFromMemory`, so the real path is what gets
measured. It contains:

- a 12 km **ground plane** at albedo 0.18, so the horizon and aerial perspective are real;
- an 8 m **wall** whose shadow falls on open ground in the middle of the frame, giving lit and shadowed upward
  ground side by side;
- **spheres** at albedo 0.04 / 0.18 / 0.50 / 0.90 plus a glossy one (every normal, and the albedo axis);
- **cubes** square and turned 45°;
- a **checkerboard** panel and 3 cm **poles** at 10/20/40/80 m for TAA;
- **distance markers** from 50 m to 5 km, scaled with range.

Everything is untextured with an exact linear albedo. The sun is fixed (45° up, from the right and behind the
wall), the camera is fixed, and the lab's ground-bounce colour and atmosphere ground albedo are its own 0.18.

**`--probe "x,y;x,y"`** fires the pixel probe at those pixels, one per frame from frame 100, and logs every
lighting term in linear HDR. With a known camera, a world point projects to a pixel by the viewer's own
`fovY = 1.204` (checked: a sphere centre landed within one pixel). Together with `--keys` and `--pan`, a lab
experiment is a fixed command line, with no mouse and no track caveats.

The track stays for what only real content shows: performance, texture behaviour, and how it looks.

### 5.0l Mip chains for textures that shipped without one — a renderer fallback

§9b item 20. track-108 ships **76 of its 88 textures with no mip chain**: 73 DXT5 and 3 DXT3, 17 of them
alpha-tested (trees, fences, houses, the backdrop scenery). The cure belongs in the converter. This is the
**renderer fallback** in the same pattern as single-stage detection and normal welding: it checks each texture
first, leaves any DDS that has mips exactly as it is, and so stands down by itself once the converter does it.

`GenerateDdsMips` (`ngg/core/mipgen.*`) is a pure function of the DDS bytes, run at load on up to 8 threads:

- **Mip 0's blocks are copied verbatim.** Full resolution is byte-identical to what shipped; only the missing
  levels are created.
- **Each level is built from the previous one in linear light.** Colour slots are decoded from sRGB, averaged
  and re-encoded; averaging encoded values darkens every minified edge.
- **Alpha-tested textures keep their coverage (Castaño 2010).** A box filter pulls alpha towards the middle,
  so at the cutoff a tree loses leaves at every level until it vanishes at distance, which is a worse artefact
  than the shimmer being fixed. Each level's alpha is rescaled (by bisection) so the fraction of texels
  passing the material's own `alphaCutoff` matches mip 0. Colour is alpha-weighted, so the colour of discarded
  texels (often black) does not bleed into the kept ones.
- **Levels 1+ are re-encoded in the source format** (DXT1/3/5, principal-axis endpoints, nearest-palette
  indices). The chain costs a third more memory rather than 4–8× as uncompressed.

**Measured on track-108:**
- **Load:** 76 textures in **203 ms** on 8 threads. Memory **21.2 → 28.3 MB** (+33 %, a mip chain's
  theoretical cost), against 113 MB for an uncompressed chain. Re-encode error **3.55/255** mean, normal for BC.
- **Start camera, horizon band** (rows 290–430):
  - Shimmer at rest with TAA on (mean frame-to-frame change): **0.79 → 0.47** (−41 %).
  - Aliasing energy with TAA off (mean |Laplacian|): **27.1 → 15.6** (−43 %).
- **Coverage, per alpha-tested texture at level 4** (mip 0 / box / as written):

| texture | mip 0 | box | written |
|---|---|---|---|
| FIELD01_FENCE | 36.5 % | 26.6 % | 37.5 % |
| divaci | 51.1 % | 44.6 % | 51.2 % |
| divaci1 | 56.0 % | 51.6 % | 56.0 % |
| ObjectTexturePlateOpaque3 | 93.9 % | 92.1 % | 93.9 % |
| FENCE_WOOD03 | 73.4 % | 81.2 % | 76.6 % |

The box filter can err either way, and preservation holds mip 0's coverage to within the resolution of the
alpha values. Whole-frame geometry coverage at the start view barely moved (151,234 / 151,108 / 151,167 px)
because the band is mostly opaque terrain. Distant tree lines are where this shows, and that is a driving
judgement.

`--mips 0|1|2` (off / plain box / coverage-preserving, default 2) selects the mode at load for A/B. The
load log lists the coverage table above for every alpha-tested texture.

### 5.0m What ray tracing actually costs — foliage, AO and the cascades, measured

Item 23 named three unmeasured risks for an RT-only shadow path. This is the first: **alpha-tested foliage**. The
lab gained a forest (`--lab`; `--lab-bare` without it): 120 RBR-style trees, each a trunk and three crossed,
two-sided, alpha-tested 7×7 m leaf cards on a generated DXT5 leaf texture (~50 % coverage, shipped without
mips, so the mip fallback builds them too). The cascade pass got its own GPU timer (`LastShadowMapMs`), and
`--cam x,y,z,yaw,pitch` fixes the view so every configuration sees the same frame. All times are GPU ms on the
integrated Radeon. The RT figure is the trace dispatches; the denoiser is separate.

| view | geometry px | RT shadow only | RT shadow + AO (2 rays) | cascades |
|---|---|---|---|---|
| lab, forest | 965 k | **1.89** | 7.90 | 0.85 |
| lab, bare | 789 k | **1.29** | 4.88 | 0.56 |
| track-108 start | 194 k | **0.86** | 2.25 | **2.93** |

Three findings, two of them against expectation:

1. **Foliage any-hit is not the problem.** It makes the shadow ray 46 % more expensive (1.29 → 1.89 ms), with
   227 k leaf tests per frame. That is manageable, and it is the risk item 23 worried about most.
2. **On real track geometry the cascades cost MORE than RT shadows** (2.93 vs 0.86 ms at the track-108 start
   view). The lab flatters cascades because its geometry is tiny; four full-scene passes over 600 k triangles
   are not.
3. **AO is the real cost**: 75–80 % of the ray tracing (3.6–6.0 ms at full coverage), from 2 incoherent
   hemisphere rays per pixel against 1 coherent sun ray. The ~7.4 ms seen driving through foliage (5.0e) was
   mostly AO.

**So AO now traces one ray per pixel by default** (`--ao-rays N`, design default 1), and the existing temporal
denoiser does the accumulating. Measured in the forest view: RT **7.87 → 4.28 ms (−46 %)**. Mean AO is
identical (231.6 / 231.6), so one ray is unbiased. The two converge to the same picture (mean difference
1.27/255). The cost is +17 % frame-to-frame noise in the denoised AO at rest (5.34 → 6.27/255), on a term that
only scales the ambient light.

**A waste fixed on the way:** `O` hid the AO term but still traced its rays, so switching AO off saved nothing.
The viewer now traces AO only when it is applied, and a frame without AO drops the AO history so re-enabling
does not reproject a stale one.

**And the finding that reorders the plan: the game host does not ray trace at all.** Its frame is sky →
cascaded shadows → scene → post. There is no depth prepass, no RT shadows, no AO, and Wallaby's API is the
same. Everything in 5.0e lives in `viewer.cpp`: the prepass, the trace, the denoiser, and the jitter-before-
prepass ordering whose absence made the screen blink. This is the class of bug the environment map was (5.0i):
a feature wired up by one client. Item 23's prerequisite, "RT shadows seen in-game", cannot happen until the
frame is orchestrated in `ngg-core` and every client records the same one (item 27).

### 5.0n One frame for every client — `ngg::core::Renderer`

§9b item 27. The frame now lives in `ngg-core` (`ngg/core/renderer.*`). The viewer, the game host and Wallaby's
API each call one `RecordFrame`. The split of responsibility:

- **The client owns** its window, its input, its overlays and its presentation.
- **The renderer owns** everything from "here is a camera" to "the tonemapped image is in your target":
  curve rules, the multiple-scattering switch mirrored to the sky, the jitter set *before* the prepass, the
  depth + normal prepass, ray-traced shadows and AO or the cascades, sky, scene, TAA, exposure and tonemap.
- **Settings are the client's** (shadow source preference, AO, sky, TAA, curve); **order and correctness are
  not**. A client can no longer reintroduce the blinking of 5.0e by recording the prepass before the jitter.

**What each client gained.** The **game host** goes from sky + cascades + scene + post to the full frame:
prepass, ray-traced shadows and AO, the environment and aerial tables. This is the first time any of 5.0e
onwards can run in-game. The **API** gets the same frame. The **viewer** is unchanged by construction. Its
ray-traced shadow mask is *bit-identical* before and after (max difference 0), and the final frame differs by
a mean of 0.07/255, which is the intended AO change from 2 rays to 1.

**The renderer also owns whether RT shadows are valid** for the scene as it stands. The BVH holds the file's
static geometry at its authored positions. `Scene` now keeps a **geometry revision** (bumped by load, add,
remove, hide, move, compact) and answers **`RtRepresentsScene()`**:

- **Hide or remove** an object: representable, so the BVH is rebuilt. Hidden objects are now left out of the
  BVH input too; they used to keep casting shadows.
- **Add a dynamic mesh or move an object** (the editor does both): not representable, so the frame uses the
  cascades and logs why once, rather than casting shadows from where things used to be. Item 7 (per-object
  instancing) is what lifts this.

**The rebuild needed its own command context.** `Rt::Build` is synchronous on the context it is given. Called
from inside `RecordFrame` on the client's list, which is open, it reset the allocator mid-recording: a D3D12
error, a failed build, and RT silently off. `Device::CreateSecondary` makes a second allocator/list/fence on the
same device and queue. The rebuild executes and completes on the queue before the client's list is even
submitted, so GPU ordering is free and clients need no "call this between frames" rule.

**Verified without Wallaby:** `ngg-api-smoke <file.ngg>` (new, `ngg/api/smoke.cpp`) drives the C API the way the
editor does. On track-108 all 8 checks pass:
- the BVH is built once;
- hiding object 0 rebuilds it once (176 alpha-tested triangles leave the BVH), RT stays active;
- showing it again rebuilds once more;
- moving it pauses RT, logged once;
- restoring the identity transform rebuilds and resumes RT;
- no D3D12 errors. The API creates its device *without* the debug layer, so this check is weak there; the
  viewer runs with it and stays at the baseline 5 warnings.

**Deployment had the same gap one level down.** RT compiles its shaders at runtime with `dxcompiler.dll` +
`dxil.dll`. Without them beside the binary, ray tracing declines quietly and the frame renders cascades, which
looks like nothing changed. `build-ngg.ps1` now copies them beside `ngg-api.dll`, and `-Deploy` copies them into
the game folder with `rbrtm.exe`. The `rbrtm.exe` in `C:\Brb` was from 2026-08-14, so none of 5.0f onwards has
been in the game yet.

**Not yet verified: the game host itself.** It compiles and records the same `RecordFrame`, but it runs only
inside RBR. That is the user's check: `build-ngg.ps1 -Deploy` with RBR closed, then drive.

### 5.0o Instancing in the BVH — moving geometry keeps its ray-traced shadow

The renderer half of §9b item 7. Item 27 made the editor fall back to cascades whenever an object was added or
moved, because the one BVH held the file's geometry at its authored positions. The same limitation would block
movable meshes in-game (item 5) and the car (P5). The BVH is now organised the way the draw list is: **meshes
and instances**.

- `Scene::DescribeRt` lists mesh keys and instances:
  - the **world**: every static object at its authored position, one BLAS. Its key hashes which static
    objects it *leaves out*;
  - each **moved** static object: its own BLAS at its authored position, placed by its instance transform;
  - each editor-added **dynamic** mesh: one BLAS, as many instances as use it, the material per instance
    (`FORCE_OPAQUE` on instances drawn with an opaque material).
- `Rt` caches one BLAS per key. `Rebuild` builds only the keys it lacks, drops dead ones, and rebuilds the
  shared any-hit buffers. `UpdateInstances` rebuilds the TLAS and the instance table and nothing else.
- The any-hit test now looks its triangle up through `gInst[CandidateInstanceID()]`: the alpha-triangle base,
  the vertex base, which vertex buffer (static or the dynamic pool), and the material when the mesh's triangles
  do not carry one.
- `Scene` keeps a CPU copy of the static indices (~9 MB on track-108). Rebuilding the world BLAS used to read
  them back from an upload heap, which is write-combined memory.

**The two speeds this buys.** A change in *which meshes exist* rebuilds BLASes: a load, an object's first
move, a hide or show (the world's key changes), an added mesh. A change in *where things are* rebuilds only
the TLAS. **Dragging an object is TLAS-only.**

**Verified:**
- `ngg-api-smoke` on track-108 (updated, all pass):
  - first frame: one rebuild, a 92 MB world BLAS in 65–94 ms;
  - hide or show: one rebuild each;
  - first move: one rebuild building the world and the object's own BLAS (141 ms), and **RT shadows stay
    active**;
  - **ten drag steps build no BLAS**;
  - restoring identity: one rebuild.
- **Lab, wall moved 8 m (`--move 1,8,0,0`):** at the probe that was in the wall's shadow, visibility went
  0 → 1; at the probe 8 m to the right, 1 → 0. **The shadow moved with the instance.**
- **Track-108:** shadow fraction and AO are as before (AO mean 253.5 vs 253.7).

Two side fixes:
- Scratch buffers are created in COMMON, which is what they are anyway. Asking for UAV earned a validation
  warning per rebuild; the viewer baseline is now 4 warnings.
- The viewer gained `--move index,dx,dy,dz` (the editor's operation, for lab experiments).

**Not measured here:** RT timing on the instanced path. These runs had a different window size (1600×900) and
erratic GPU clocks (aerial fill 0.35 → 0.09 ms, env 0.5–2.1 ms on the same adapter), probably the GPU being
shared. Times from them are not evidence either way.

**What item 7 still needs:** per-object transforms *in the format*, so the converter can keep movables as
instances, and draw harvesting to animate them in-game (items 4 and 5). The renderer side, instancing that
follows the scene, now exists.

### 5.0p Quality tiers — §1.8.4's interface, built

§1.8.4 requires tiers over one lighting path from P1, so VR at 90 Hz (P4) does not mean a second renderer. They
now exist in `ngg::core::Renderer` (`Quality::Low/Medium/High`, `Renderer::Tier`), set the same way by every
client:
- the viewer: `--quality low|medium|high`, `F3` to cycle, shown on the readout;
- the game host: `ngg.ini` beside `rbrtm.exe`, `[Renderer] Quality=…`, default medium;
- the API: `ngg_set_quality`.

**What a tier changes is only what has been measured to matter** (5.0m). The RT sun shadow is cheap, and on
real geometry cheaper than the cascades, so every tier keeps it (falling back to cascades only where RT is
unavailable). The tiers differ in AO, which is most of the ray-tracing cost:

| tier | contents | RT ms, lab forest (965 k px) | RT ms, track-108 start |
|---|---|---|---|
| **low** | RT sun shadows, no AO, sharpen 0.25 (the VR baseline) | **1.55** | **0.59** |
| **medium** (default) | + RT AO, 1 ray | 3.0 | 1.22 |
| **high** | + RT AO, 2 rays (less noise at rest) | 4.27 | 1.56 |

Stable within the run (a few percent). Absolute times are lower than 5.0m's in the same view, because the GPU's
clock state differs between sessions, so the *ratios* are the result: low is about a third of high's ray
tracing in the forest. Whole-frame fps cannot separate medium and high there: both sit at exactly 30.0, which
looks like vsync dividing a 60 Hz display.

New techniques join a tier by adding a field to `QualityTier`, not a mode: half-resolution AO, probe-based
indirect for VR, or a single cascade for low if item 23's in-game measurement says so. The individual setters
remain for A/B work; changing one takes the client off its tier.

### 5.0q Weather — our own presets, stored in the `.ngg`, rendered by the one atmosphere

**Why now:** of `TrackSettings.ini`'s 757 presets, about 70 % are not clear sky (overcast 207, rain 193, clouded
131, partly 46). NGG rendered every one of them as a clear blue sky with a hard sun, which made weather the
largest fidelity gap left against vanilla content.

**§3.4a was half wrong, measured.** It assumed RBR's scattering parameters were physical and could drive our
model "by construction". They are art knobs for a 2004 shader:
- `Turbidity` runs −2.8…9 with a median of 0;
- `Greenstein_Value` (the Mie phase g) is 1.0, a delta function, in most presets;
- track-108's clear sky has `Rayleigh_Multiplier = 0.025`.

What *does* translate is the weather **category** in every section name (`<map><variant>_<haze>_<cloud>`) and
the fog distances, which are visibilities. The data also has authoring bugs: 8 presets write `SunDir` with
**decimal commas**, and **31 daytime presets put the sun below the horizon** (mostly overcast ones, where RBR's
hand-set ambient made the sun irrelevant). A physical sky renders those as night.

**So the presets are ours, physical, and inside the `.ngg`:**
- **Format** (`ngg/core/weather.*`): versioned `ngg-weather` JSON with sun, turbidity, ground albedo, cloud
  cover, fog visibility (Koschmieder, metres) and fog height, rain, snow, and `source` provenance. Unknown keys
  are ignored, so it grows without breaking. JSON is the authoring and interchange form (Wallaby edits it).
- **Storage: the `NGGW` chunk.** The presets live *in* the file, covered by its CRC and (later) its
  encryption envelope. A preset is part of an event's definition (§3.6), and a loose file beside the game
  would let a competitor thin the fog. The runtime never reads loose JSON. A file with no `NGGW` falls back to
  translating its legacy section on the fly, logged as the transition path. *(The chunk first held the JSON
  itself; since 5.0t it holds binary records.)*
- **Import** (`host/tracksettings.*`, now an *import source* only):
  - `rbrtm --import-weather <track.ngg>` writes every legacy section for that map into the file's `NGGW`. It
    refuses non-plaintext files (§3.7), keeps every other chunk byte-identical, and keeps a `.bak`;
  - `rbrtm --export-weather <dir>` writes all 757 as JSON for authoring;
  - the translation: `crisp`/`hazy` set turbidity 1/3; `lightfog`/`heavyfog` set fog visibility from
    `FogEnd`; `partcloud`/`lightcloud`/`heavycloud` set cover 0.35/0.6/0.95; rain and snow set visibility
    and precipitation; decimal commas are parsed; a daytime sun below the horizon is mirrored to a low sun on
    the same bearing; night keeps its own. *(Corrected 2026-10-30, in WRally's port: `N` is NOON, not night -
    its suns stand ~50 degrees up, and RBR has no night stages. 17 overcast noon presets with the sun 2.8 degrees
    up were left as dusk; the port raises every preset's low sun, 48 in all.)*
- **Selection:** the viewer's `--weather <name>`; the game host uses the file's default until the bridge
  passes the event's chosen preset (item 28).

**Rendering, in the one shared `AtmScatter`**, so the sky, environment, irradiance and aerial table all follow:
- **Fog** is an exponential height layer on the camera's own ground, composited *analytically*. The view
  march's kilometre steps would step over a 150 m layer. Its optical depth along a ray has a closed form; it is
  lit by the sun reaching into it (Mie phase) and by a diffuse field (half sky, half ground bounce); and the
  direct sun at a surface inside it is attenuated through the whole layer. Visibility is met by construction:
  2.1 % contrast at `FogEnd`.
- **Clouds** dim the direct sun (`1 − 0.98·smoothstep(0.3, 1, cover)`), turn the sky toward a CIE overcast
  dome whose brightness is 30 % of the clear global irradiance, and light the air with the deck's diffuse
  light. Clear-sky diffuse irradiance uses a fit to this model's own integral, `0.064 · I · sin(elev)^¼`
  (within ~5 % from 10° to 50°, from 5.0j's numbers).
- A clear, fog-free preset returns from the weather path untouched, so the clear look is unchanged.

**Found and fixed on the way: the sky pass's constant buffer was mis-packed for as long as the sky has been
jittered.** HLSL starts a `float2` on a new register when it would straddle one. The C++ `SkyCB` packed
`jitterX` into the previous slot, so the sky read its TAA jitter one slot off (x from `jitterY`, y from
padding). The new weather fields after it read their neighbours: the fog *height* became fog *extinction*,
and the clear sky turned to fog. It now has a pad and `static_assert`s on the offset. It is the same class of
bug 5.0 recorded for the scene's buffer.

**Verified on track-108** (the injected copy, 11 presets): crisp_clear is unchanged; heavycloud is a grey
dome with flat diffuse light; lightfog (4.4 km) fades the distance; heavyfog (130 m) is a white-out at the
start view; heavyrain is grey and soft. Exposure adapts, and validation stays at 4.

**Not done:** cloud *shapes* (partly cloudy is a uniform blend for now; the choice between RBR's cloud textures
and volumetric clouds is open), rain and snow themselves (wet surfaces, particles), night, and fog/cloud in
the multiple-scattering table.

### 5.0r A GPU hang (TDR) from a reused pointer — and DRED, which named it

**Symptom.** The user's AMD driver reset several times in an evening (Windows LiveKernelEvent 0x141,
VIDEO_ENGINE_TIMEOUT_DETECTED, with "send report" dialogs). The resets clustered at 23:41 and 00:11–00:13,
and the second cluster matched a loop of `ngg-api-smoke` runs exactly. The smoke test had been "flaky"
(roughly one run in three failing) in a telling way: once a run failed, *every* later structural change in
it was silently skipped. That is what a removed device looks like when nothing checks for removal.

**Making it visible first.** Nothing in the renderer checked for device removal, so a GPU reset showed up only
as later calls failing quietly. `Device` now:
- enables **DRED** (Device Removed Extended Data) before creating the device: auto-breadcrumbs (the last
  command each command list completed) and page-fault data (the faulting address and recently freed
  allocations near it);
- names its command lists (`ngg frame`, `ngg secondary (BVH builds)`);
- calls `CheckRemoved` after every submit and on a failed `Begin`, which logs the removal reason, the
  breadcrumb window around the hang, and the page fault. Once per device.

**Caught on the 3rd of 10 runs:**
- `DEVICE_HUNG`, in `ngg frame` at command 2200 of 4370: a `DrawIndexedInstanced` straight after a `Dispatch`.
  That is the main pass, just after the ray tracing.
- A **page fault**, with the nearby allocations being **recently freed resources** (DRED type 34).
- On the frame of the object's first move, a rebuild, which recreates the ray tracer's output textures.

**The bug.** `Scene::SetRtVisibility`/`SetRtAo` rewrote their descriptor only when the texture **pointer**
changed: `if (tex != m_rtVis) …`. When the ray tracer recreates its targets, the new texture can be allocated
**at the address the old one had** (ABA). Same pointer, so no rewrite, and the main pass sampled the *freed*
texture's memory: a page fault, then a hang, then the driver reset. It is intermittent because address
reuse is luck. It predates instancing: any recreation could hit it, including a **viewer window resize**.
Instancing (5.0o) made recreation frequent enough to see. The 23:41 burst, during viewer runs, is plausibly
the same bug.

**The fix.** Those two descriptors are rewritten on every set. Two descriptor writes a frame cost nothing, and
a pointer is not an identity. Nothing else in the renderer keys a descriptor on a pointer (checked); the ray
tracer already rewrites its depth and normal views every trace.

**Verified:** 10 of 10 smoke runs pass, against about one failure in three before; Windows filed no GPU reports
after the fix; the viewer with the debug layer stays at its 4 baseline warnings, with moved-object shadows
correct.

**Lesson, general:** a cache of "what I last bound" keyed on a raw pointer is a use-after-free waiting for the
allocator to reuse an address. Key on a generation, or rewrite.

### 5.0s The game tells the renderer which weather (9b item 28)

Until now the game host rendered every stage under the `.ngg`'s DEFAULT preset, whatever the event said. The
weather is decided where RBRTM starts a stage: it writes the event's codes into the game (`SetWeather`: time of
day, the overcast variant's time of day, "special weather" and clouds), and RBR then lights the stage from one
`TrackSettings.ini` section, named after those codes. **That section name is already the preset's name**: the
importer (5.0q) names each preset after the section it came from. So the identity needed no new scheme; it
needed one string passed through.

**The path.**
- RBRTM, right after `SetWeather`, reads the four fields back from the game. It reads them back rather than
  taking the event's codes, because a code the event leaves unset (-1) leaves the game's own value in place,
  and the read-back names what RBR actually uses. It builds the section name with the scheme the plugin's
  `CheckTrackSettingsCRC` already uses (`GetWeatherSectionName`, RBRTMHelp.h), e.g. `108M_crisp_clear` or
  `108O_M_heavyfog_clear`. It then calls the bridge's new **`nggSetWeather`** export, resolved dynamically
  like `nggSetTrack`, so no bridge means no change.
- The bridge keeps the name across a link that is not up yet and publishes it in `FrameState.weatherName`
  (protocol **v4**, block 320 bytes). Weather is published before the track, so a stage and its weather that
  arrive in the same frame land in that order.
- The host re-selects whenever the name changes, and after every track load (a load resets to the default):
  `Scene::FindWeather(name)`. A name the file does not carry falls back to the default, **logged**: the wrong
  sky is better than none, but it must not be silent. A file without NGGW translates that legacy section.

**Found on the way: single-digit maps.** `TrackSettings.ini` zero-pads them (`04M_crisp_clear`), but the
importer searched for `4M_...`, so stages 1–9 imported no presets at all. Map 4 now imports 43. The
plugin's own `CheckTrackSettingsCRC` builds `"%2d"` (`" 4M_..."`), which matches no section for those maps
either. Left alone: it is the event-integrity check, outside the renderer's remit, and noted here.

**Verified:** all 11 presets in track-108's NGGW have names the plugin's formula can produce; proxycheck
resolves `nggSetWeather`; the bridge, host and plugin build (`build.ps1`), and `loadcheck` loads the new
RBRTM.dll. **Not verified here:** the in-game run. That run is the check: the host log should
say `weather: [<section>] from the .ngg` with the event's condition.

**Consequence for authoring:** a preset the game should ever select must carry the section name of the
condition it stands for. Free-text names are fine for experiments, but the game never selects them. Noted for
Wallaby's weather editor.

### 5.0t Weather in the `.ngg` is binary records; JSON is the project form

The first `NGGW` held length-prefixed JSON. The argument was that the schema could grow without a format bump,
and that the file is protected by its CRC and envelope, not by being binary. Both still hold, but they argued
for the wrong layer. The `.ngg` is the *runtime* format (§8.1: it must not become the editing format), and
every other chunk in it is a fixed binary layout. A text parser in the load path, for 17 numbers per preset,
is the odd one out. So the split is now explicit:

- **Project / editor form: the `ngg-weather` JSON** (`core/weather.h`). Wallaby's sidecar holds it (request in
  its CLAUDE.md), `--export-weather` writes it, and the flat API speaks it (`ngg_weather_to_json`/`_from_json`).
  It can carry what the runtime does not need, such as `source` (provenance).
- **Runtime form: `NGGW` = `WeatherChunkHeader` + `count` × `WeatherRecord`** (`format/ngg_format.h`):
  `version` 1, `recordSize`, then per record a 64-byte name and 15 floats (124 bytes). Converters pack the
  JSON into these records (`core::PackWeatherChunk`; Wallaby's `NggWriter` mirrors it).
- **Growth without a bump:** new fields are *appended* to `WeatherRecord`. A reader takes
  min(`recordSize`, its own size) and keeps defaults for what an older record lacks; a newer record's tail
  is skipped. `version` changes only if an existing field changes meaning. That is the property JSON was
  bought for, kept at a fraction of the cost.

**Old files:** a JSON-era chunk is *refused*, not misread. Its first word is a preset count, not 1, and its
sizes cannot add up. The load logs `NGGW not understood (version 11, ...) - re-run rbrtm --import-weather`,
and the track renders its built-in default. Only local test files ever had the JSON form.

**Verified:** the JSON-era scratch file is refused with that message. Re-injected, track-108's chunk is
16 + 11 × 124 = 1,380 bytes (JSON: 5,612), and `ngg-api-smoke` passes all 13 checks, including preset
selection, live edit and the JSON round trip. The game's `C:\Brb\ngg\track-108.ngg` is re-injected; the
pristine original is kept as `track-108.ngg.orig`.

### 5.0u Glass: blended materials (2026-10-30, in WRally)

`kMaterialBlend` existed in WRally's renderer interface and nothing drew it: the car's transparent material went in
as alpha-TESTED, so a window was either solid or a hole. The i20's glass texture (DXT3, 2048²) is dark tint, rgb
~34/255, at alpha ~85/255 over the windows (53 % of its texels). Below the 170/255 cutoff that made every window a
clean hole: the cabin behind, black, and **no reflection at all**.

**What a blended material is now:**
- **Format:** `kMaterialBlend` = bit 3. Bit 2 stays unused: it was v2's single-stage, and the v2 upgrade masks it,
  but a reused bit is a misread waiting to happen. The interface keeps its bit 2, and the module maps it. Blend wins
  over the alpha test.
- **Its own pass, after everything opaque:** sorted far to near by the box centre, depth tested (GREATER_EQUAL,
  reverse-Z), no depth write. It is drawn over the sky too, which comes first. It has no part in the prepass, so
  the depth, normals and motion that the ray tracer and TAA read are those of what lies behind the pane.
- **PREMULTIPLIED** (ONE, INV_SRC_ALPHA). The shader's `BLEND` variant scales the tint's light (diffuse, baked
  colour, aerial in-scatter) by coverage (base colour alpha × vertex alpha × factor). The reflection (sun GGX and
  the environment's split sum) stays whole. A clear pane reflects as much sky as a tinted one does; only the tint
  is partial. glTF's `BLEND` alpha mode scales everything, reflection included, which reads as a matt film.
- **Lit without the screen buffers:** the ray-traced sun visibility and AO are per *pixel*, and the pixel is the
  opaque surface's behind the glass. So glass takes AO 1, and under RT the sun unshadowed. Under the cascades it
  looks its world position up as anything else does.
- **No shadow, not in the BVH:** the cascade pass skips it, and so do `DescribeRt` and the world BLAS. The sun
  reaches the seats through the windows.
- **The car:** `assets::CarRender` sends the `.sgc`'s transparent materials as `kMaterialBlend | TwoSided` with
  roughness 0.05 (smooth glass; the body stays 0.45).

**Measured** (stage 41, imported, chase camera, tick 162, lockstep, the same frame before and after): every changed
pixel lies inside the car's windows (a 410×252 box, 37,792 pixels). The rear window's centre went from 0–5 (the
black cabin through a hole) to ~85: the sky's reflection, ~10 % of it at that angle, which is what Fresnel gives a
dielectric there. It is physically right, but it reads lighter than RBR's own dark windows, so it is the first
thing to judge when driving. `ngg-check`'s made-up scene carries a blended material (round trip and the renderer's
loader); all fast CTests pass.

**Not done:** within one object, faces are not sorted, so a closed glass shell draws its two layers in index
order. RBR's glass is single sheets, drawn two-sided, so a pixel is covered once. Coloured transmission (a red
taillight lens filtering what is behind it) needs dual-source blending; this tint is grey. Wallaby's `NggWriter`
mirrors the flags and should learn bit 3 when the editor gets glass.

### 5.0v Wet surfaces and puddles (2026-10-30, in WRally)

The presets carried `rain` from the start (5.0q), and the physics drove a wet road (`--road wet`), but nothing looked
wet. The wetness the picture shows is the **road's**, not the sky's: rain that has stopped still leaves the road
wet, and a light shower on a dry road is not yet a wet one. So it comes from the game, which knows the road: renderer
interface **v4** adds `FrameDesc::wetness` (0 dry .. 1 wet), and the game sends its road state (dry 0, damp 0.5, wet
1). It reaches the shader through the camera constants' former pad (`gWetness`), so no buffer layout changed.

**The shading** (opaque, non-foliage materials; after Lagarde's "Water drop 2b"):
- **Exposure to rain:** an upward face takes it all, a wall half, an overhang none. Sheltered surfaces (the
  ray-traced AO's dark: a canopy, the car over the road) take as little as 0.35.
- **Darker where porous:** water fills the pores and traps light. Porosity is taken from roughness (the rough are
  the porous: asphalt, gravel), and albedo scales to 0.45 there, 0.85 on smooth surfaces.
- **Glossier:** the water film puts a ceiling on roughness, 0.25 when fully wet. With the sky's reflection, which
  the environment's split sum already gives, that is most of the wet look, the sheen of sky down a wet road.
- **Puddles** on the ground layer, from damp on: a world-fixed two-octave value noise (cells of ~3 m and ~1 m) above
  a threshold that falls as it gets wetter (0.74 damp, 0.62 wet), on flat ground only (N.y > 0.93). A puddle is
  dark (albedo 0.35x), near-mirror (roughness 0.02) and LEVEL: its normal turns to straight up.
- **Not on grass:** green ground soaks the water up, so it neither pools nor turns to a mirror. RBR's materials
  carry no tag, but the albedo tells: green-dominant ground gets no puddles, and its wet roughness only falls to
  0.6. Without this the lawns went to blue-grey mirror sheets.

**Measured** on stage 41 at the start, same frame: Wallaby's `.ngg` dry, damp and wet under the sun ("after the
rain"), and the imported `.lbs` under `41O_M_lightrain_heavycloud`, dry vs wet. The wet road darkens, reflects the
overcast sky down its length and catches the sun where it is lit. The import's own wet textures (RBR's wet set)
and this shading stack sensibly. All fast CTests pass at interface 4.

**Not done:**
- The puddles reflect **only the sky**: the environment map is the sky. Under a canopy that is wrong: it should be
  the dark trees. Ray-traced reflections for the mirror-like pixels (puddles, glass) are the cure, and they are
  measurable against the RT budget (5.0m).
- Rain itself: drops, spray and splashes (particles, design.md 3.2), rain on the glass.
- Drying: wetness is a step today (dry, damp, wet), with no time in it.
- Snow cover, and the cloud shapes (item 29).

### 5.1 Renderer implications

A track at night wants hundreds of small emitters (street lamps, windows, marshal posts, spectator torches, car
headlights) plus a handful of shadow-casting ones. That means **clustered light culling** running per view (mirror
and both eyes included), and — on the DX12 Ultimate baseline — **ray-traced shadows for the important lights**
instead of a priority-allocated shadow atlas. Many-light sampling (ReSTIR-style reservoir resampling) is the
natural fit and is what makes "hundreds of emitters" affordable rather than aspirational.

The VR tier (§1.8.4) can't afford that, so it falls back to a small shadow atlas for the few nearest lights plus
unshadowed contribution beyond — same light data, same materials, different sampling budget.

This is the strongest technical argument for D3D12 over D3D9 anywhere in this document, and it's the reason not to
write a D3D9 backend at all.

### 5.2 Car lights

Headlights as shadow-casting spotlights with a real IES-ish profile, tail/brake lights as emissive + small point
lights, driven from `CarObj` state. Headlights lighting our world correctly is likely the single most striking
visual difference from vanilla, so it should land in the same phase as night, not later.

### 5.3 Where do the lights come from?

Tracks have no authored light sources, so we derive candidates and let a human confirm:

1. **Emissive texture detection** — textures whose name or content reads as lamp/window/sign become emissive
   materials automatically.
2. ~~**The `_N` baked variant as a light map.**~~ **WITHDRAWN 2026-10-30: there is no night variant.** RBR's `N`
   is NOON (its suns stand ~50 degrees up; RBR has no night stages at all), so `_N` holds no painted light pools.
   Stage lights come from (1) and (3) only; see 5.4.
3. **Manual placement in Wallaby** — a light-placement mode with live preview through `ngg-host`, producing
   `provenance=authored` entries that survive reconversion.

That ordering matters: (1) and (2) mean a track converts to *something plausible* at night with zero artist time,
and (3) upgrades the tracks people care about most. Across ~100 tracks, that difference is the difference between
shipping and not.

**Confirmed scope (rev 3): all three.** Auto-derivation runs on every track; the Wallaby editor exists for
hand-polishing the tracks people actually race. So P3 owes a real editing mode — light gizmos, live preview
through `ngg-host`, and a reconversion path that never clobbers `provenance=authored` entries. That last property
is a format and tooling requirement, not a UI nicety: if reconverting a track silently destroys a night an artist
spent a day on, nobody will use the editor twice.

### 5.4 Night in WRally - decided 2026-10-30

RBR has no night stages, so night is WRally's own, and with dynamic lighting **any stage can be driven at any hour**.
Four decisions, the user's:

1. **Scope: car lights, the night sky, and stage lights** in the first version, not car lights alone.
2. **Time of day = date + time + place.** A preset (or the menu, or an event) gives a date, a local time and the
   stage's latitude/longitude. The sun's and the moon's positions, the moon's phase and the stars follow
   astronomically, so dusk, night and dawn come out right on every stage. RBR's stages carry no location: a
   default latitude stands in until a stage's own metadata (stage.json, design.md 7) says where it is.
3. **Every light is ray traced:** shadows for all of them, made affordable by many-light sampling (ReSTIR-style
   reservoir resampling, 5.1), not a shadow map or a "few shadowed, the rest fill" split. The VR tier (P4) and
   min-spec are the risk, measured, not designed around.
4. **Car lights are the CAR's,** in this order:
   1. the car's own WRally light specification (`car.json`, design.md decision 9: JSON with a schema). Per
      lamp: role, position and aim on the car, beam shape (an IES-like profile), colour and intensity, and the mesh
      that glows when it is lit;
   2. else **derived from RBR's car `.ini`**, which already names the lamps: `Point_light_lf/rf` headlights,
      `_bon1..6` the bonnet pod, `_lb/rb` tail, `_brake_lb/rb` brake, `_reverse_lb/rb` reverse, `_lit_c` a centre
      lamp, each with a position and a direction. The `.sgc` has a lit and an unlit mesh per lamp
      (`e_light_lf` / `e_light_lf_lit`, switched by the `.ini`'s `SwitchBox = light`). 80 car `.ini`s in 44 car
      folders of the stock install have them. Per-role defaults fill what the `.ini` lacks: photometry, and the
      **indicators**, which no RBR car has, placed at the corners beside the head and tail lamps;
   3. else a generic default, placed from the model's size.

   The roles: low beam, high beam, pod, tail, **brake, reverse, left and right indicator, hazard**. The game
   switches them from the car's state: the brake pedal, reverse gear, the indicator and hazard keys, high beam,
   the pod. Damage (a broken lamp goes dark) comes with body damage (design.md phase 6).

   **Every car's lights are different, and every LAMP has a character** (the user, 2026-10-30). A rally car carries
   up to **8** auxiliary lamps (the bonnet pod plus roof or bumper lamps), and they are not copies of one another:
   pencil/spot beams for reach down the road, driving beams, wide floods, cornering ("ditch") lamps aimed off to
   the side, each with its own colour temperature (halogen warm, HID, LED cold) and intensity. So the spec is per
   LAMP, never per car: position, aim, a beam profile (an IES-like intensity table, or a named preset: `pencil`,
   `driving`, `flood`, `ditch`, `low`, `high`), colour temperature, intensity, which switch it is on (high beam,
   pod, separately switched lamps) and its glowing mesh. RBR's `.ini` gives the positions and aims and nothing of
   the character: the i20's six pod lamps (`_bon1..6`) aim 20 degrees out to each side in two groups of three (normal
   x = ±0.342), and the derived default must give them a sensible mix of profiles (the outer ones flood or ditch,
   the inner ones driving or pencil), not six identical beams. The look of real rally lamps at night is the
   reference: the user collects screenshots, and the profiles are tuned against them.

**Phases, each ending in something to look at:**
- **N1a - DONE 2026-10-30: the sun from the time and place.** `core/astronomy.*` (Meeus: the sun's low-accuracy
  method, the moon's main terms, its lit fraction, sidereal time; x east, y up, z north turned by `northDeg`),
  checked by CTest `astronomy-check` against Meeus's worked examples (the sun to 0.00001 degrees, the moon's
  longitude to 0.0007, its distance to 29 km, its lit fraction to 0.002) and the solstice's noon sun at 50 N
  (63.44 degrees, due south, 13:04 CEST). The weather record grows by appending (124 -> 160 bytes:
  `timeFlags`, date, local hour, UTC offset, latitude, longitude, north); readers take min(recordSize, sizeof)
  over the defaults, so a 124-byte record reads as no time set (checked on track-108's). The JSON gains
  `"time": {"date", "local", "utcOffset"}` and `"place": {"latitude", "longitude", "north"}`. Renderer interface
  **5**: `FrameDesc::skyTime` puts the sun there over any weather; the game's `--time HH:MM [--date]
  [--utc-offset] [--latitude] [--longitude] [--north]`. Stage 41 on 21 June at 50 N: noon, a low evening sun at
  20:30, civil dusk at 21:30 lit by the sky alone, and black at 22:30 - what the moon and the night exposure
  (N1b, N1c) are for.
- **N1b/N1c - DONE 2026-10-30: the moon, the stars, the night's exposure and colour.**
  - **The light passes to the moon** at the end of civil twilight (the sun 6 degrees down, its direct light long
    gone): the renderer's one directional light - the direct light, its ray-traced shadows (moon shadows), the
    aerial table - becomes the moon's, at the full moon's 2.3e-6 of the sun's illuminance times its phase's
    brightness (Krisciunas-Schaefer: 0.026 a + 4e-9 a^4 magnitudes over the phase angle a).
  - **The sky is both bodies':** `SkyParams::light2` adds the other one's sky in `AtmSkyRadiance` (sky pass and
    environment): the moon's through the dusk, the sun's twilight into the night, only while it shows (not the
    moon's by day, not the sun's 20 degrees down). The first version handed the sky over at -11 degrees and it
    jumped (a full moon's sky is brighter than the twilight there); either side of -6 degrees it is now the same
    picture.
  - **The moon's disc** (0.52 degrees), by day too: a sphere lit by the real sun, so the phase draws itself, its
    radiance albedo 0.12 x the sun's irradiance / pi, with earthshine and the atmosphere's extinction.
  - **The stars:** procedural on the celestial sphere (cube-face cells, ~9,000 to magnitude 6, brighter ones
    rarer as N(<m) ~ 10^(0.5 m)), turned by the sidereal time about the pole, each a pixel-wide gaussian holding
    its flux (as bright at any resolution), magnitude 0 = 2.3e-11 of the sun's irradiance. The moon outshines the
    faint ones by itself.
  - **The glow** (airglow + starlight, ~0.001 lux) in `AtmSkyRadiance`, brighter towards the horizon, dimmed by
    cloud: what lights a moonless night.
  - **Night exposure:** the meter reaches 2^-32 (the day's floor was 2^-12, where a night counted as black and the
    meter froze); beyond 3 EV under the day's grey only 0.75 of the rest is made up, so a full moon sits ~4 EV and
    a moonless night ~6 EV under the day: dark, readable.
  - **Night vision** in the tonemap: per pixel, on the scene's own luminance before exposure, full colour down to
    8 EV under the day's grey, the rods alone (desaturated, blue-shifted: the Purkinje shift) from 17 EV. A surface
    in a beam keeps its colour beside a moonlit verge that has none.
  - Checked on stage 41: dusk, a full moon (hard moon shadows, a grey-blue moonlit stage), a moonless night (the
    stars, the glow), the handover either side of -6 degrees (the same picture), and the day unchanged (mean
    difference 0.03/255 against the frame before N1). `PHYS_DRIVE_LOOK_UP=<deg>` tilts the view to the sky.
  - Not done: the moon's phase is too small to see at its true size (as to the eye); twinkling; the Milky Way; a
    stage's real latitude and north (stage metadata).
- **N2 - DONE 2026-10-30: the light system.**
  - **The interface (renderer 6):** `FrameDesc::lights` / `lightCount`, a list per frame of `iface::LightDesc`
    (128 bytes): point or spot, position and axis, a linear tint and an intensity in **candela**, a range (0: from
    the intensity, to where it gives 0.1 lux, at most 300 m), the emitter's radius (how soft its shadows and its
    highlight are), a spot's inner and outer half-angles, and an optional 16-entry beam profile over the cone
    (`kLightProfile`; what N3's lamp characters fill). `kLightNoShadows` for a lamp nothing should shadow.
    `RenderStats` gains `lightsMs` and `lightCount`. A candela at a metre is a lux, 2e-4 of the sun's irradiance
    unit (22 = a clear day's ~110,000 lux).
  - **Clusters** (`core/lights.*`): the view cut into 16 x 9 tiles x 24 depth slices (exponential, 0.25-1000 m),
    each with the list of lights whose range touches it, built on the CPU per frame (a conservative screen box
    and slice span per light), uploaded and copied to GPU-local buffers that the scene pass and the ray tracer
    both read - so both see exactly the same lights. Up to 512 lights and 65,536 list entries.
  - **Every light ray traced, at one ray per pixel however many there are** - the ratio estimator (Heitz, Hill
    and McGuire 2018). The scene pass sums every light's UNSHADOWED light exactly (Lambert, and GGX for the
    highlight); the ray tracer picks one light per pixel in proportion to what it gives there (a one-pass
    weighted reservoir) and traces one ray to a random point on its emitter's disc; that 0/1 is an unbiased
    estimate of the shadowed fraction of the sum - smooth, like the sun's visibility - so it goes through the
    same denoiser, and the light is the sum times it. Foliage cards take the wrapped cosine and no highlight, as
    for the sun; glass takes the fraction as 1.
  - **The highlight is the emitter's**, its lobe widened by the disc's angular size (Karis 2013's widened
    roughness, the lobe blurred and its energy kept): a point's highlight on a wet road (roughness at its 0.002
    floor) would peak near 1e5 x the irradiance.
  - Checked on stage 41 at 23:30 with `PHYS_DRIVE_TEST_LIGHTS` (two 40,000 cd headlight spots and a 2,000 cd lamp
    above and behind the car; `=lamp` the lamp alone, `=noshadow` none occluded): the beams on the road, and the
    lamp's light with the car's own shadow on the road ahead (a wedge the roof casts; without the rays the road
    there was evenly lit). Fast tests pass. Without lights a frame is bit-identical to the one before N2 (noon and
    23:30, lockstep, TAA off in both: a fresh build's settings had it on, which looked like a difference).
  - **What it costs** (1080p, the development GPU, a Radeon 780M; three lights covering the whole screen): the
    light rays 3.2 ms (0.4 with none occluded), their denoising +0.8 ms, and **the scene pass about +4 ms** (7.0-7.4
    -> 10.7-11.9). Measured apart (N2b, below), it is the lights' arithmetic per FRAGMENT.
  - **N2b - DONE 2026-10-30: the scene's shaders on DXC, and what the lights' cost is.**
    - **DXC (shader model 6.0)** compiles the scene's whole set (VS, the three PS variants, prepass, shadow - a
      pipeline cannot mix DXIL and DXBC), `-HV 2018` for FXC's language; FXC (5.1) is the fallback when
      dxcompiler.dll is missing or DXC refuses the source, which compiles under both (the wave path behind
      `__SHADER_TARGET_MAJOR`). The picture is the same: without lights night is identical, noon differs by 1/255
      on 0.1% of the pixels (rounding).
    - **A wave-uniform light loop:** the wave takes its clusters in turn (`WaveReadLaneFirst`, at most 4, the rest
      and helper lanes per pixel) and reads the cluster, the indices and the records with scalar loads (RGA: all
      scalar, 117 VGPRs from 122).
    - **Clusters cull a spot by its CONE** (each cluster's bounding sphere against the cone, a point light's sphere
      against the cluster's): half the list entries in the test view (8784 -> 4734), the picture the same (1/255 on
      0.1%, the ray tracer's light pick). The shader skips a light that gives nothing (outside its cone or range,
      facing away), and the divisions a light alone decides are made on the CPU (`invRange2`, `spotScale`,
      `invOuterRad`; one rsqrt for distance and direction): 140 -> 130 instructions, 10 -> 6 transcendental.
    - **What the cost IS** - PIX per-draw timings (`NGG_PIX=<frame>` captures that frame; `pixtool open-capture
      <file> --disable-gpu-plugins save-event-list <csv> --counters=EOP*to*EOP* --counters=PS*Invocations`): the
      lights' time is in the main pass's draws, in proportion to each draw's pixel shader invocations - foliage's
      overdraw included (without a prepass, 12.3 M invocations for 2 M pixels). Per light per fragment ~130 vector
      instructions in one dependent chain, at 4 waves per SIMD stalled more than computing (RGA: 52 `s_delay_alu`
      in the loop). Not memory: scalar loads changed nothing measurable, nor did the cone culling (in the test view
      the three lights do reach most of the screen) or the cheaper arithmetic: three lights still add ~3 ms to the
      main pass at 1080p on the 780M (7.3 -> 10.1-10.6 ms).
    - The remedy is fewer fragments per light, not cheaper loads: N2c.
    - PIX: per-draw timings work only with GPU plugins disabled - with AMD's counter plugin the replay hung the GPU
      (TDR, three times).
  - **N2c - DONE 2026-10-31: the deferred local lights.** The opaque world's lights are shaded after it, once per
    PIXEL, by one full-screen pass (`PSDeferredLights`) - in the main pass they were shaded per FRAGMENT, foliage's
    overdraw and every 2x2 quad's helper lanes included.
    - While there are local lights the opaque and alpha-tested draws use GBUF variants of their pipelines, which
      leave the surface in two 32-bit targets: R10G10B10A2 (the shading normal octahedral, roughness, the kind:
      opaque, or a foliage card - wrapped cosine, no highlight) and R8G8B8A8 sRGB (albedo, metallic). A first
      version with a third target (the aerial transmittance) and the normal in half floats cost the opaque world
      +3.8 ms in PIX's replay; packed, +0.3. The transmittance is computed again in the pass (`AerialAt`, the
      scene pass's own, now one function).
    - The pass runs under the scene's root signature (its light table grows by the two targets and the depth),
      reads the depth through a READ-ONLY depth view, rebuilds the position with the jittered view-projection's
      inverse and calls the same `LocalLights`; added (ONE, ONE) times the transmittance. Glass stays forward,
      drawn after the pass, over the lit world. Day (no local lights): the old pipelines, nothing written or run.
    - The picture: against the forward lights at most 4/255 (the 10-bit normal), mean 0.12/255; without lights
      night identical, noon 1/255 on 0.1% of the pixels (rounding). `NGG_FORWARD_LIGHTS` shades them in the main
      pass instead - the A/B.
    - **The cost, by the whole frame's GPU time** (`RenderStats::frameMs`, renderer interface 7, in the readout as
      "gpu frame"; 1080p, the 780M, the three test lights over the whole screen): no lights 21.8-22.7 ms, forward
      29.3-33.1, deferred 27.2-27.9 - the lights' shading now ~1.2 ms of the +5.3, the rest their rays (3.1) and
      denoising (0.8). PIX's replay: the opaque world 19.44 ms without lights, 25.88 forward, 19.71 deferred, the
      pass 0.89.
    - **Measuring, learned the hard way:** a pass's own timestamps do not isolate it on this GPU - "gpu main" kept
      reading +3.7 ms with the lights shaded forward or deferred alike, and a timestamp pair round the deferred
      draw read 0.00 ms. The frame's first-to-last time is the measure; PIX's per-draw replay (`NGG_PIX`) the
      breakdown. The vsync quantises fps (30 or 20), so fps says nothing either.
  - **N2d - DONE 2026-10-31: the depth test before the shader, and the draw order.** Every pass's cost before it.
    - **The scene shader was depth-tested AFTER it ran.** Its debug probe writes a UAV (`gProbe`), and a pixel
      shader with UAV writes is tested late unless it says otherwise - so every fragment the rasteriser made ran the
      whole shader and only then failed the test: 6.4 per pixel on stage 41 at 1080p, the prepass's final depth
      notwithstanding (PIX's PS invocations: 13.3 M in the main pass for 2.07 M pixels, with a prepass or without).
      The opaque, glass and deferred variants are `[earlydepthstencil]` now (exact: no clip, no SV_Depth); the
      alpha-tested one clips, so it leaves the probe out, and after a prepass draws over the prepass's depth without
      writing it (`m_psoAlphaKept`), which lets the driver test it before the shader too. Main pass: 13.3 M ->
      2.88 M invocations (1.39 per pixel), PIX's replay 16.2 -> 4.0 ms; real time 7.3 -> 2.2-2.4 ms.
    - **Front to back**: the opaque draws sorted, the solid before the alpha-tested, each by the distance to the
      nearest point of its box (they were in the stage file's order). The main pass does not care (the prepass's
      depth is final); the prepass: 8.1 -> 7.4 M invocations. Ray tracing "off" still runs the prepass (it is there
      whenever there is a BVH).
    - Culling: per object, its box against the frustum (behind the camera and beside it) in every pass - but the
      stage's objects are large, so on stage 41 only 299 of 2987 are culled. Finer objects, distance and occlusion
      culling are open.
    - The picture: identical (noon with and without ray tracing, the lit night; within the ray tracer's +-1).
    - The GPU frame at full clock (`PHYS_DRIVE_OFF=vsync`: capped by the display, the integrated GPU clocks down
      into the slack and every pass reads slower): 15.2-16.8 ms without local lights, 19.2-20.7 with the three test
      lights (their rays 3.0, denoising +0.7, shading ~0.5). With the overdraw gone the forward lights cost about
      what the deferred ones do (19.5-21.0); deferred stays - its cost does not grow with the overdraw that is left
      and it runs in a small shader, which counts once N3 brings a dozen lamps.
  - **Lessons.** A buffer the shaders read from the UPLOAD heap is CPU-visible memory the GPU barely caches -
    copy to a default-heap buffer (no gain measured here, but it is the rule). A struct copied from a buffer and
    indexed by a variable (the profile) is an indexable temporary, i.e. scratch memory (~1.5 ms per light): read
    fields from the buffer by index. Under PowerShell 7.6, `$env:X = ''` can leave X defined but empty, so the
    game reads an empty value as unset. Probing a shader's cost with a runtime switch measures what the
    COMPILER made of the switch: zeroing a result after computing it removes nothing, and a select by a constant
    may become a branch that skips the work - check the ISA (`NGG_DUMP_PS` writes what Radeon GPU Analyzer needs:
    the blobs, the root signature, the source).
- **N3 - car lights.** The spec and its fallbacks (above), lit meshes as emissive, the game's switching (brake,
  reverse, indicators, hazard, beams, pod). Analysed 2026-10-31 against the user's reference shots (onboard night
  stages: a 4-pod bonnet ramp; two LED bars):
  - **What the look is made of.** The BEAM SHAPE first: a pod throws an intense, fairly narrow hot spot down the
    road, an LED bar a wide, flat flood that lights both verges and the trees beside them - wide horizontally,
    narrow vertically, a sharpish upper edge. N2's round cone cannot make a flat flood. Then the REACH (halogen
    headlamps ~15-60 k cd; rally LED pods rated ~1 lux at several hundred metres, i.e. a few hundred k cd - to be
    calibrated on the references), the AIM (lamps 0.6-1 m up, ~0.5-1.5 degrees down: the hot spot lands 50-150 m
    ahead), the COLOUR (LED ~5700-6500 K, the references' blue-white; HID ~4300 K; halogen ~3200 K). Around the beam:
    the lamps' own glowing lenses and LED strips, the dark pod housings in the onboard view, retroreflectors (marker
    poles, signs, vests) flashing back because the lamp sits almost at the eye, haze in dust and rain, glare, and an
    exposure that keeps the blown-out near verges and the dark distance both readable.
  - **The lamp model:** a beam is a sum of a few LOBES, each with its own horizontal and vertical spread (a pod:
    pencil + driving; a bar: one very wide lobe), in the lamp's own frame (an axis AND an up vector); a colour
    temperature; candela. Real IES/LDT photometry can come later as one more kind of lobe. The 300 m range cap goes
    to ~1 km (a pencil beam's reach).
  - **What RBR gives** (`C:\Brb\Cars`, 41 car .inis with lamps): the headlamps' positions with usable forward
    normals (`Point_light_lf/rf`), the tail, brake and reverse positions. Bonnet lamps in 5 of them only
    (`Point_light_bon1-4`), their normals mostly sideways - positions, no aim; a ramp mesh (`e_ramp`,
    `e_ramp_light_lit`) in 4, switched off (`Predicate=NotUsed`): RBR modelled ramps and never used them. So the
    beams' character, the layouts and most ramps are WRally's (car.json presets), RBR's positions where they exist.
  - **Decided (the user, 2026-10-31):**
    1. **Layouts per car, chosen in the setup:** car.json lists the configurations a car can run (e.g. 2 headlamps +
       a 2-lamp ramp, a 4-pod ramp, 2 LED bars, no ramp); the driver picks one; a car without its own spec gets a
       sensible default ramp.
    2. **The aim is set in the setup**, per ramp, yaw and pitch a few degrees (as crews do at service); while
       driving only the switches: low/high beam, the ramp on/off; brake, reverse and the indicators by the car's
       state and keys.
    3. **Beams visible in dust, mist and rain: a later phase**, with the weather and dust work they depend on.
  - **Phases:** N3a the lamp model (lobes, orientation, colour temperature, reach; the renderer interface; the
    car.json lamp schema; presets: halogen low/high, LED pod pencil/driving, LED bar flood, ditch lamp). N3b the
    car's layouts and controls (derived from RBR where it has data, the setup's choices, the switches, the glowing
    lenses). N3c tuning on the references (intensities, spreads, aim, exposure). Later: haze, retroreflective
    materials, glare. Cost: shadows stay one ray per pixel however many lamps; shading ~0.15 ms per lamp over the
    whole screen on the 780M.
  - **N3a - DONE 2026-10-31: the lamp model.**
    - **Beams** (renderer interface 8): `kLightBeam`, a lamp's light as the sum of up to 4 LOBES (`iface::LightLobe`):
      each an elliptical peak in candela in the lamp's own frame (its axis and its UP), falling to half at the
      half-width across and the half-height up and down, centred a yaw and a pitch off the axis, with a sharpness
      (2 a gaussian, higher a flatter top and a harder edge - a dipped beam's cut-off). In the shader the angle is
      measured in the lamp's tangent plane, I = sum of candela x 2^-(q^(sharpness/2)). The radial profile of N2 is
      gone (nothing used it). A light reaches up to 1 km (a pencil beam's 0.1 lux; it was 300 m), and the clusters
      cull a beam by the cone its lobes reach (to 1% of their peaks).
    - **The game's lamps** (`game/src/assets/car_lights.*`): a lamp is a BEAM preset at a place on the car (its
      frame: x LEFT, -y forward, z up, from the centre of gravity) aimed yaw right and pitch up. The presets, round
      figures until N3c tunes them on the references: `halogen-low` (3200 K, 16 kcd hot spot 2 degrees under the
      axis, cut-off), `halogen-high` (3300 K, 50 kcd), `led-pencil` (5800 K, 350 kcd in +-3.5 x 2.5 degrees),
      `led-driving` (5800 K, 150 kcd, a 22-degree spread), `led-bar` (5700 K, a flat flood +-30 degrees, 60 kcd, and a
      +-60-degree spill), `ditch` (5800 K, 40 kcd, to be aimed off to the side). Colour temperature -> linear sRGB of
      luminance 1 (Planckian locus, Kim et al.), so candela stay candela whatever the colour. The axes' swap into the
      renderer mirrors the car's right, so a lobe's yaw takes its sign from where the car's right ends up.
    - `PHYS_DRIVE_TEST_LIGHTS=ramp|bars|low` (`assets::TestLayout`): two main beams and a 4-pod ramp (pencils
      inside, driving beams outside toed 2 degrees out, all 0.8 degrees down), two main beams and two LED bars, the
      dipped beams. Checked from the bonnet camera on stage 41 at night: the pods' long, intense beam down the road;
      the bars' wide flood over both verges and the spectators beside the road; the warm, short dipped beams.
    - Cost (1080p, the 780M, full clock): the ramp layout's 6 lamps add 5.5-8 ms to the GPU frame - their light
      rays 3.9 ms (the ray tracer weighs every lamp of a pixel's cluster to pick one: next to optimise), denoising
      +1.0, shading ~1.2. The day is unchanged.
    - **car.json's lamps (draft, N3b settles it):**
      ```json
      "lamps": {
        "beams":   { "<name>": { "kelvin": 5800, "radius": 0.08,
                                 "lobes": [ { "candela": 350000, "halfWidth": 3.5, "halfHeight": 2.5,
                                              "yaw": 0, "pitch": 0, "sharpness": 2 } ] } },
        "lamps":   [ { "name": "lf", "role": "main", "beam": "halogen-high", "at": [0.65, -2.0, 0.05],
                       "yaw": 0, "pitch": -0.5, "mesh": "e_light_lf_lit" } ],
        "layouts": { "4-pod ramp": { "mesh": "e_ramp", "lamps": [ { "beam": "led-pencil", "at": [0.12, -1.9, 0.3],
                                                                    "pitch": -0.8 } ] },
                     "no ramp":    { "lamps": [] } }
      }
      ```
      `beams` adds the car's own beams beside the presets; `lamps` the ones every layout has (headlamps, tail,
      brake, reverse, indicators: `role` says how the game switches them); `layouts` the choices for the setup (the
      ramp's lamps, the mesh that is shown with them). The setup's aim (yaw, pitch) is added to a layout's lamps.
  - **Tuning on a wall, and the lamp lab (2026-10-31).** `tools/lamp-wall/wall.py` draws the lamps on a wall 10 m
    ahead without the engine (the renderer's beam formula; a copy of the presets and the test layouts). The user's
    tuning there: each lamp its own spot - the main beams 10 degrees out (80 kcd), the outer pods 20 degrees out, the
    inner ones 5 out and 4 up; the pods' beams narrow with a harder edge, and a wide, dim spill around each (~7% of
    its peak, +-25-30 x 10-12 degrees). `WRally --lab` (`assets::BuildLampLab`): the test lamps on a car standing in
    a synthetic scene - a floor, a wall 10 m ahead of the ramp, thin white poles at 5 and 8 m, a person-sized block -
    with no stage or physics; `--camera`, `--time`, `--exit-at`, `PHYS_DRIVE_SHOTS` as on a stage.
  - **"The shadows look like one lamp's."** Checked against an exact reference (`NGG_LIGHTS_REF=1`: a ray to every
    lamp; `=2`: the one-pick estimate averaged over 16 picks): in the lab the game's shadows ARE the reference's.
    With the pods aimed apart and their beams narrow, every patch was lit almost by one lamp, so each thing cast that
    lamp's one dark shadow; the spill gives every patch several lamps and each thing a fan of lighter shadows, as in
    the reference shots. New debug view 15: the lamps' shadowed fraction. Open: on stage 41 the timing clock's legs
    still differ from the reference (one hard shadow), which the lab does not show - the clock is a physics object.
- **N4 - stage lights.** A light chunk in the `.ngg` (with `provenance=auto|authored`, 5.3), emissive texture
  detection for the automatic part, Wallaby's light-placement mode for the authored part (its own task).

---

## 6. The car (prepared now, built later)

Deferred, but these hooks go in from v1 so the door stays open:

- Tagging extends to the car load path, so car resources are identified the same way track resources are.
- `NGGM` carries car-paint material parameters (clearcoat, metallic flake) and `NGGS` carries skeletons.
- Wallaby's existing SGS parsing (`SGS/AnimMesh.cs`, `MaterialDefinition.cs`, `MaterialDeclaration.cs`) is the
  starting point for a car converter; completing it is a known, bounded RE task.
- Runtime damage deformation and dirt are harvested (§4.1) rather than re-derived.
- Until then, §2.4's stencil harmonization keeps the vanilla car from looking pasted on.

---

## 7. Native and AI-generated tracks

Requirement: be ready for tracks that were never RBR content in the first place. This is cheaper than it sounds,
because Wallaby is **already a track authoring tool** — `Filebuilder.cs` writes LBS/DLS/FNC/COL/MAT,
`MeshImporter.cs` imports external meshes, and `DriveLineCreator`, `colgen`, `FenceBuilder` and `CamCreator`
already generate the gameplay-side data. What's missing is a modern content path into it, not the fundamentals.

### 7.1 The key structural insight

A generated track still needs the game to be able to *drive* on it: collision meshes, drive line, checkpoints,
triggers, cameras, `.MAT` surfaces, pacenotes. But it does **not** need the game to be able to *draw* it, because
we suppress the game's world rendering anyway.

So for a generated track the LBS becomes a **gameplay stub**: real collision, real drive line, real triggers, real
surface maps — and visual geometry reduced to whatever minimum keeps the engine happy (the engine still culls,
streams and CRC-checks it). All actual visual richness lives in `.ngg`, authored directly, with no format
compromise inherited from 2004.

Consequences worth stating plainly:

- The un-baking machinery (§3.2) simply doesn't apply. Generated content is authored PBR from the start, so every
  Tier 0/1/2 field in the format must be **optional**, never required.
- `NGGO` objects are `free`-bound (§3.5): there is no suppressed draw to match them against, so they render from
  their own transform. This is why the binding mode is in the format from v1 rather than being assumed.
- The same mechanism lets **added content on converted tracks** work — new props, better trees, replaced buildings —
  which is the feature the community will ask for first, long before anyone generates a whole stage.
- Minimum-viable-LBS is its own RE question: how little visual geometry can the stub contain before the engine
  misbehaves? **Rev 3 promotes this to the P0 critical path**, because fully generated stages are now in scope and
  this experiment sets the ceiling on how free they can ever be. It needs nothing but the game and Wallaby, so
  there's no reason to defer it.

### 7.1b What full stage generation additionally requires

Generating whole stages (confirmed in scope) means Wallaby must emit **gameplay** data, not just visuals. The
foundations exist — `DriveLineCreator`, `colgen`, `FenceBuilder`, `CamCreator`, `PacenotesEditor` — but they were
built for a human working interactively, not for a generator. The gap list:

- **Drive line + checkpoints + start/finish** from a generated centreline, with the sanity properties RBR expects.
- **Collision meshes** generated from render geometry, with the §3.1 rule inverted: here collision is authored
  *first* and the visual mesh must conform to it, never the reverse.
- **Surface maps (`.MAT`)** painted from the generator's own material assignment — which we get for free, since the
  generator knows which polygons are tarmac and which are gravel.
- **Pacenotes**, derivable from the centreline curvature. The plugin already has pacenote infrastructure
  (`PacenotesMap.h`, Wallaby's `PacenotesEditor`), so this is assembly rather than invention.
- **Triggers, cameras, sound zones** — mostly templatable from the drive line.

None of this touches the renderer. It's a Wallaby project that happens to share a format, and it should be
planned and staffed as one.

### 7.2 The content path

AI/generative tooling emits standard interchange formats, not RBR ones. So the pipeline is:

```
generator (AI / procedural / DCC)
      │  glTF or USD  (geometry, PBR materials, lights, instancing — all native concepts there)
      ▼
Wallaby importer ──► scene graph ──┬──► .ngg        (visuals: our renderer)
                                    └──► LBS/COL/DLS/MAT stub  (gameplay: the game)
```

**This is no longer only the generated-content path.** Converted tracks now travel the same spine —
`LBS → glTF → edit → .ngg` (§3.0) — so what follows describes one pipeline entered at two different points, not
two pipelines. Everything below about what glTF does and does not carry applies equally to a converted track.

Adopting **glTF as the interchange format** is the single highest-leverage decision here: it maps almost 1:1 onto
`NGGV`/`NGGM`/`NGGP` (PBR metallic-roughness, punctual lights, instancing, tangents), every generative and DCC tool
speaks it, and it means we never have to define an authoring format of our own. Wallaby's existing `.x` importer
is the template; glTF import is a bounded addition.

> **DECIDED: glTF, not USD.** Both were on the table; the deciding factors were reach and weight, not features.
>
> - **Tool support.** glTF is the de-facto real-time interchange — native in Blender, 3ds Max, Cinema 4D,
>   Houdini, Substance, Unity, Unreal, Sketchfab and the web engines; Maya via maintained exporters. For a
>   community modding pipeline the realistic authoring tool is Blender, where glTF is excellent. USD's tooling is
>   strong in Houdini/Maya and thin elsewhere, and it brings a heavy C++ dependency.
> - **Where USD would have won:** its layer composition and variant sets map almost exactly onto §3.4b's layers
>   and stage variants. That is a real loss, and the scene-description layer below has to carry that instead.
>
> **Animation, checked rather than assumed.** glTF 2.0 covers node TRS keyframes, skinning, and morph targets —
> which is most of what animated scenery needs. It does **not** natively carry **UV scrolling**, and the shader
> disassembly (§3.2b) showed RBR's `…Anim` variants are exactly that: `oT0 = time * direction + uv`. Reproducing
> it through glTF needs `KHR_texture_transform` driven by `KHR_animation_pointer` — ratified, but thinly
> supported by exporters — so a plain round-trip will lose it.
>
> This matters less than it sounds: §4 harvests animation from the running game for *converted* tracks, so glTF's
> animation support only governs **native** content. And RBR's own needs turned out modest — `…Sway` compiles to
> no vertex motion at all.
>
> **glTF is a transport format, not a working format.** Its accessor/bufferView indirection sits deliberately
> close to GPU buffers and is awkward to edit against. Wallaby's in-memory geometry should be a plain mesh
> structure that glTF maps onto at import/export (§8.1) — transport and working representations differing is
> correct, not duplication.

A JSON **scene description** layer sits above it for things glTF doesn't carry: stage layout, drive line, weather
preset bindings, surface classes per material, light schedules. That layer is also the natural target for a
generative pipeline to emit directly — it's text, it's reviewable, and it's diffable.

### 7.2b Vegetation CAN be de-merged — MEASURED, and it is the tractable half

The obvious objection to replacing RBR's vegetation is that the LBS welds many plants into one mesh, so there is
no "tree" to extract. **That objection does not survive measurement.** Separate plants do not touch each other,
so connected components over quantised vertex positions separates them with no help from the file at all.

On `track-42`, over alpha-tested geometry:

| | |
|---|---|
| triangles | 337,944 |
| **connected components** | **38,033** (from 1167 objects — ~33 plants welded per object) |
| triangles per component | median **6**, p90 10 |
| footprint | median **1.3 m**, p90 5.1 m |
| taller than wide, footprint < 25 m | **14,579 (38%)** — tree-like |
| flat, height < 0.35 × footprint | 2,494 (7%) — grass/ground |
| sprawling > 40 m | **98 (0.3%)** — genuinely merged or not vegetation |

Median 6 triangles is 2–3 crossed cards: one bush. The de-merging works.

**Shape alone is NOT enough to classify, and this is where it would go wrong.** The largest components are not
vegetation at all:

- `MB_SUPERBOWL_4.tga`, 946 tris spanning **7326 × 665 × 5347 m** — the distant terrain backdrop, alpha-tested
- `MB_FENCE002.tga`, several components of 400–540 tris — **fences**, which are §9b item 4, generated from a
  point list and deformed at runtime. Replacing those as vegetation would be actively wrong.

So classification needs **texture name AND shape**, with fences and backdrop excluded explicitly rather than
filtered out by a size threshold that happens to work on one track.

**Effort, honestly split:**

| | |
|---|---|
| detection / de-merging | **easy** — the analysis above is essentially the implementation |
| classification (tree / bush / grass / fence / backdrop) | moderate; heuristics plus a review pass, and it wants validating per track |
| replacement assets | **the actual project** — a vegetation library or generator, this is where the time goes |
| rendering 38k instances | needs GPU instancing + LOD, which is also where the performance comes back |

That last row is worth noting as a *benefit*: 38,033 instances of a few source meshes is far cheaper to draw
than 338k unique triangles, so this pays for itself in frame time as well as looks.

**Fairness constraint (§3.6):** collision comes from `.col`, not from these meshes, so replacing the visual
geometry does not change what is drivable — provided the collision data is left untouched. That has to stay
true, or the replacement becomes a gameplay change rather than a visual one.

This is also the honest fix for the dark-card problem in §5.0: no shading model rescues crossed-card geometry,
and this is the path that removes the cards.

### 7.3 AI in the nearer term

Full stage generation is the far end. The same infrastructure serves things that pay off much sooner and should be
treated as the same feature, not a different one:

- **Texture upscaling and material synthesis** — already in §3.3 as an offline step; generated normal/roughness/
  emissive maps from a 256×256 2004 diffuse texture is a large visual win per unit of effort.
- **Light placement** — §5.3's auto-extraction from the `_N` baked variant is exactly the kind of task a model does
  well, and the format already marks `provenance=auto|authored` so machine output stays distinguishable from a
  human's and survives reconversion.
- **Detail injection** — scattering generated vegetation/props over converted tracks using the `.MAT` surface
  classification as the placement mask. Purely additive, `free`-bound, and it makes converted tracks feel new
  without touching a single original vertex.

### 7.4 Fairness, again

Generated and augmented content changes what players see, so it lands in the same check chain as everything else
(§3.6): the `.ngg` hash and the generator id/seed/input hash from `NGGH` are registered per event. "Reproducible
from a recorded seed" is the property that makes generated content admissible in competition at all — which is why
it's a header field rather than a comment.

---

### 1.9 The 64-bit launcher owns the session

Rev 4 spawned `ngg-host.exe` lazily from the bridge, on the game's first `Present`. That is backwards, and two
defects made it obvious: the host **orphaned** when the game crashed (two dead instances holding file locks
across sessions), and its startup raced the game's device creation.

**Invert it. The 64-bit process starts first and launches the game.** It is a graphical launcher that *contains*
the renderer:

```
ngg-launcher.exe (x64)          the user runs THIS
   ├── ngg-core                 the D3D12 renderer (was ngg-host.exe)
   ├── IPC server               already listening before the game exists
   ├── OpenXR session           VR belongs here - runtimes are 64-bit (§1.3)
   ├── UI                       weather/daylight presets (§3.4), track and .ngg management,
   │                            conversion, profile selection, fairness profile (§3.6)
   └── child: RichardBurnsRally_SSE.exe (x86)
           └── d3d9.dll  = ngg-bridge   → connects to the parent's IPC server
           └── RBRTM.dll              → detects the launcher
```

What this fixes, beyond tidiness:

- **Lifetime becomes owned, in both directions.** The launcher holds the child in a **job object** with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so the game cannot outlive it; and it waits on the child handle, so it
  knows immediately when the game exits or crashes. No orphans, no stale locks, and a crash becomes a reported
  event rather than a puzzle.
- **Startup ordering stops being a race.** The renderer is up and listening before the game's device exists, so
  the handshake is a connect rather than a spawn-and-hope.
- **VR lands in the right process.** §1.3 already wanted the renderer to own the OpenXR session; a 64-bit parent
  makes that natural instead of awkward.
- **The 64-bit side gets a home for tooling** — conversion, preset authoring, the standalone viewer — which today
  has nowhere to live because the host is a headless child.

**DECIDED — the plugin loads, but refuses to run outside the launcher, and says so.** `RBRTM.dll` initialises
normally under any launch method; if the launcher session is absent it replaces its menu with a single screen
explaining that the game must be started via the NGG launcher, which the installer places in the Start menu and
(optionally) on the desktop.

This is deliberately not the same as failing to load. A DLL that declines to load produces no message, no menu
entry and no diagnosis — the user sees a plugin that "disappeared". A DLL that loads and explains itself turns a
broken install into a one-line instruction, and leaves the rest of the game entirely untouched. It does still
require existing users to change how they start the game; that is the accepted cost, bought back by the message
being explicit about what to do.

It also keeps the integrity story simple: **a sanctioned session requires the launcher**, and there is exactly
one supported configuration to reason about (§3.6), rather than a matrix of "with launcher / without".

**Detection must be a deliberate handshake, not a process-name check.** A name check is trivially spoofed, which
would make the fairness profile theatre rather than a control. The launcher generates a random per-session token,
creates a named kernel object from it in the `Local\` namespace, and passes the name to the child through its
inherited environment. Only a process the launcher actually started can see it, and it costs nothing to verify.

## 8. Module layout

```
ngg-format/    (C++)  .ngg read/write, versioning, CRC/provenance, encryption envelope. No dependencies.
ngg-core/      (C++)  renderer: D3D12 only. Passes, shaders, clustered lighting, shadows, culling,
                      streaming, material system, sky/atmosphere, post (HDR + TAA + tonemap, 5.0c).
                      Knows nothing about RBR.
ngg-launcher/  (C++)  64-bit process the USER runs (§1.9). Owns ngg-core, the IPC server, OpenXR and
                      texture streaming; launches RichardBurnsRally_SSE.exe as a job-object child and
                      owns its lifetime. Also the standalone viewer (same binary, different entry)
                      and the front end for presets, conversion and profile selection.
ngg-bridge/    (C++)  in-game 32-bit DLL: d3d9 interception, tagging, suppression, harvesting,
                      shared-surface management, composite, present, legacy-plugin detection.
ngg-convert/   (C#)   inside Wallaby. Two halves of ONE spine (§3.0): LBS/COL/MAT/DDS/SGS → glTF project
                      (photometric solver, material heuristics, de-atlas, weld, texture bundle), then
                      glTF/USD import → .ngg + LBS/COL/DLS gameplay stub (§7). Direct LBS → .ngg is a
                      development shortcut, expected to go.
                      Plus the editor UI: material overrides, light placement, weather presets, A/B validation.
plugin glue    (C++)  RBRTM integration: lifecycle, config, fairness profile, CRC registration, fallback.
```

Two deliberate choices: the converter is **C#** reusing Wallaby, because the formats are already solved there and
offline tooling has no perf constraint — only the *reader* needs to be C++. And `ngg-core` never knows about RBR,
which is what keeps the viewer and the editor preview viable.

### 8.0 `ngg-core` exists — DONE

The renderer is now its own static library (`ngg/core/`), separate from the launcher that drives it:

| file | contents |
|---|---|
| `core/device.h/.cpp` | `Device` — D3D12 device, queue, one direct command list, fence. `Create(luid)` takes an optional LUID: **mandatory** when a resource will be shared cross-process, absent when the client is free to pick a GPU |
| `core/scene.h/.cpp` | `Scene` — `.ngg` load, pipeline, per-object draw. Moved from `host/`, unchanged in behaviour |
| `core/log.h/.cpp` | one sink for the whole library, replacing `SetSceneLogSink` |

`ngg/host/` keeps only what is genuinely about RBR: the session/job-object launcher, the IPC frame loop, the shared
texture, and the track-name → `.ngg` mapping. **Nothing in `ngg/core/` includes an RBR or IPC header**, and that is
the property worth protecting — the moment it does, the viewer and Wallaby's preview (§8.1) both stop being possible
and the two-renderer problem comes back.

**The camera contract is now stated in `core/scene.h` rather than assumed.** Row-major, row-vector (`v * M`), which
is D3D9's convention because RBR's matrices are used unchanged; and **reverse-Z is applied inside `Record`**, so a
client hands in a conventional near→0/far→1 projection and must not pre-flip. Both were previously true only by
accident of having exactly one caller.

#### The standalone viewer — DONE

Same binary, different entry, as the layout above specifies:

```
rbrtm.exe --view C:\Brb\Ngg\track-42.ngg
```

Free camera (WASD/QE, right-drag look, Shift ×8, Ctrl ×0.15), **F5 reloads the asset in place keeping the camera**,
D3D12 debug layer on — none of which is wanted in the game path.

The justification is iteration cost, and it is measured rather than assumed: without it every shader or material
change costs a full RBR launch and stage load, and **a deploy silently fails while the game is running while still
reporting success** — which has already cost this project a debugging session spent chasing a crash that was really
a stale binary. Dynamic lighting is going to be dozens of shader iterations; that loop has to be seconds.

Verified: Joux Verte loads and renders standalone (747,746 verts / 428,557 tris / 29 textures), with no game
process involved at all — which is also the proof that `ngg-core` carries no hidden dependency on RBR.

One thing deliberately *not* done: the initial camera speed was briefly scaled from the stage bounding box, which
gave ~400 m/s on an 8 km stage — excellent for crossing the map and useless for inspecting a road surface. It is a
fixed 25 m/s with Shift for traversal.

### 8.0b The viewer has an on-screen readout, and it reuses the editor's text

`ngg/host/hud.h/.cpp`, plus two flags on the existing label system. `F1` toggles it.

**Why it had to exist:** the roughness, tonemap and sun keys are all *sweeps* — change a value and watch — and
watching only works if the value is on the picture. Reading it out of a log after the fact is the thing this
project has been doing instead, and it does not work while a number is moving.

**Why the RENDERER draws it:** a native child window with a swap chain wins the airspace fight against anything
the host paints over it, so host-drawn text on a D3D12 viewport either flickers or never appears. That finding
already put labels in `ngg-core` for Wallaby (§8.2); the viewer has the same window and takes the same answer.

Two additions to the label system, both of which the editor gets for free:

- **`kOverlayScreenSpace`** — `Label::pos.xy` is pixels from the top-left instead of a world anchor, and the
  text is left-aligned on it rather than centred. A HUD has no world position to anchor to, and a caller laying
  out rows cannot predict a centred origin without measuring the text first. Implies no depth test: a frame-time
  readout a hillside can hide is not a readout.
- **`kOverlayTextShadow`** — the glyphs again, one pixel down-right and tinted black, underneath. White text
  over a sunlit road or a bright sky is invisible, which is exactly the case a readout has to survive. Two
  draws rather than a shader outline, because the atlas has no distance field to grow one from and 300 extra
  quads cost nothing. **Wallaby's world labels want this as much as the HUD does** — an object name over the
  sky has the same problem.

**The font is rasterised by the HOST with GDI**, as `SetFont` intends: nothing is embedded in the renderer, no
glyph blob in the binary, no frozen size or character set. 16×8 cells of 2:1 — the natural monospace aspect
*and* a square atlas, which matters because `SetLabels` derives the glyph aspect from `columns/rows` alone and
so assumes squareness.

> **The instrument was broken before the feature was.** First run reported **0 glyph pixels** and the obvious
> reading was that the text was not drawing. It was drawing the whole time: `ReadOverlayStats()` has to be
> called after the GPU wait — like `LogProbe` — and the viewer never called it, so the counter could only ever
> be zero.
>
> Two rounds went into the font atlas and the screen-space quad maths before that turned up, and the atlas was
> fine (measured: 13.4% ink, peak alpha 255). This is the third time on this project that the measuring device
> was the defect — after the probe reporting zeros twice — and the lesson generalises: **when an instrument
> reports "nothing happened", check that the instrument is connected before believing it about the feature.**
> The shadow pass was then confirmed the same way and in one line: 6,264 glyph pixels became 12,558.

### 8.1 Wallaby should render through `ngg-core`, not its own path

Proposed direction, not yet scheduled: rebuild Wallaby to drop its own SlimDX renderer and drive `ngg-core`
instead, keeping only the legacy formats still in use for gameplay (`.col`, `.trk`, `.fnc`, `.mat`).

**The case is stronger than "less code to maintain", because the current split has already cost this project.**
Wallaby's preview is a fixed-function approximation of RBR, not RBR. Implementing the ground blend from
`ModulateInvAlphaAddColor` — which Wallaby uses as an *editor fallback* and the game never executes, because RBR
renders ground with shaders — put the wrong textures on the road, and it took disassembling RBR's own
`RLDoubleTexture.pso` to find out (§3.2b). An editor whose preview is a second, differently-wrong implementation
of the renderer will keep generating that class of bug. One renderer means the editor cannot disagree with the
game, because there is nothing to disagree with.

Two further arguments:

- **The SlimDX dependency is already blocking.** Wallaby does not build here at all — SlimDX 4.0.13.43 is
  discontinued and unavailable — which is why `lbs2ngg` exists as standalone Python instead of living in Wallaby
  as this section originally specified. That is technical debt with an expiry date, not a preference.
- **§3.2's validation harness needs exactly this**: re-render a converted track under solved lighting and diff it
  against a vanilla screenshot. That is an editor feature, and it only means anything if the editor renders with
  the shipping renderer.

**Two design points to get right, though, or this becomes a different mess:**

1. **`.ngg` is a runtime format and must not become the editing format.** It is optimised for load-and-draw:
   textures embedded, objects flattened, instance transforms baked. An editor needs references, instancing, named
   objects and non-destructive history. Wallaby should author in an interchange format — §7 already nominates
   **glTF/USD** for the native path — and *export* `.ngg`. Editing the runtime format directly would either
   cripple the editor or bloat the format with authoring data the renderer must skip.
2. **Wallaby still has to emit legacy gameplay files, including geometry.** Dropping `.lbs` entirely is not
   possible: the car must collide with the world, and that collision is RBR's (§4.1a corollary). So a native
   track still ships `.col`/`.trk` and probably a minimal `.lbs` for the physics engine, even though nothing
   renders from it. "Wallaby only supports `.ngg`" is true for *rendering*, not for output.

**Integration shape: SUPERSEDED by §8.2 — do not build from this paragraph.** It proposed that Wallaby be a
second client of the launcher's IPC protocol (§1.9), "which avoids a C++/CLI or P/Invoke boundary between C# and
the renderer entirely". Two premises there are wrong: the launcher is out-of-process because *RBR* is 32-bit,
which is not a constraint Wallaby shares, and C++/CLI is not the same risk as P/Invoke — SlimDX is mixed-mode
C++/CLI, and that is exactly why it is unobtainable and why the editor is pinned to x86. §8.2 decides
**in-process, through a flat C ABI**, and records what that costs.

#### Measured: SlimDX is Wallaby's data model, not its renderer

"Drop SlimDX, render externally" sounds like one change. It is not — **106 of 136 source files reference it**, and
the heaviest uses are not rendering at all:

| type | uses | what it really is |
|---|---|---|
| `Mesh` | 408 | the geometry container every parser returns |
| `Vector3` | 361 | math |
| `VertexBuffer` | 158 | geometry storage |
| `Device` | 113 | required to *load* a mesh, not just draw it |
| `Matrix` | 93 | math |
| `IntersectInformation` | 32 | ray picking |

The signature that matters: `MeshTools.LoadMesh(BinaryReader, int type, Device d3dDevice)`. **Geometry cannot be
parsed without a GPU device.** That single coupling is why Wallaby cannot run headless, cannot be a CLI
converter, and why `lbs2ngg` had to be reimplemented in Python rather than reusing parsers that already worked —
the duplication this section originally warned against, forced by a dependency rather than chosen.

So sequence it, with value at each step rather than one big-bang rewrite of a working editor:

1. **Decouple the data model.** Plain managed mesh (positions/indices/attributes) plus `System.Numerics` for
   `Vector3`/`Matrix`; a ~30-line Möller–Trumbore replaces `Mesh.Intersect`. Rendering keeps using SlimDX for now,
   uploading from the plain mesh. **This step alone retires the Python converter duplication** and gives headless
   conversion — the largest single win, and it is not the rendering step.
2. **Replace rendering with `ngg-core`**, in-process through the flat C shim of §8.2 — not the IPC protocol,
   which is the game bridge's answer to RBR being 32-bit.
3. **Delete SlimDX, and flip to x64**, which is the same change: nothing else pins the process to x86.

Step 1 is the bulk of the work (408 `Mesh` uses) but it is mechanical, and it is the one that pays off
immediately whether or not steps 2 and 3 ever happen.

### 8.2 The editor integration: a flat C API, an instanced live scene — REVISES 8.1

§8.1 ends by proposing that Wallaby be a second client of the launcher's IPC protocol, "which avoids a C++/CLI
or P/Invoke boundary between C# and the renderer entirely". **That conclusion is revised here**, because two of
its premises do not survive contact with the editor's actual requirements.

- **The bitness argument belongs to RBR, not to Wallaby.** The launcher is out-of-process because the *game* is
  32-bit and D3D12 needs x64. Wallaby has no such constraint — .NET Framework 4.8 WinForms runs x64 happily —
  and going x64 is a stated goal, not an obstacle. What pins it to x86 today is only that a mixed-mode assembly
  is loaded in-process.
- **C++/CLI and P/Invoke are not the same risk, and conflating them costs the wrong decision.** SlimDX *is* a
  mixed-mode C++/CLI assembly, and that is precisely why it is discontinued, why one dev machine cannot build
  against it, and why the editor is stuck at x86. P/Invoke over a flat C ABI has none of those properties: the
  native side is an ordinary DLL with no CLR coupling, and the managed side stays pure IL.

**Decision: in-process, through a flat C ABI.** Not C++/CLI — repeating SlimDX's construction with our own code
would reproduce every failure mode we are currently paying to escape.

**The one real cost, and it is not bitness:** in-process, a device-removed or a driver hang takes the editor down
with it, losing unsaved work. That is the trade being accepted, and it is mitigable (handle device removal,
re-create, reload from the project). It should be the reason someone later argues for a separate process — not
the two premises above.

**This is also cheaper than it looks.** Measured across the editor's 106 SlimDX-touching files: **79 use it only
as math and geometry** (`Vector3`, `Matrix`, `Mesh`, `IntersectInformation`), which `Wallaby.Formats` already
replaces on `System.Numerics`; only **27 touch rendering** (`Device`, `VertexBuffer`, `Texture`, swap chains).
The 79 are a mechanical pass independent of the renderer. The 27 *are* this integration. x64 falls out of
finishing them, rather than being a prerequisite for starting.

**And the choice stays reversible if the API is designed first.** A flat C surface can be linked into the editor
or hosted in a separate process behind the same calls. Design the API, and in-process versus out-of-process
becomes a deployment decision made later with a working renderer, instead of an architecture decision made now.

#### The live scene is instanced, like glTF — not baked, like `.ngg`

`.ngg` objects are index ranges into one shared buffer (`firstIndex`, `indexCount`) with instance transforms
baked into vertices. That is right for a runtime format and wrong for an editor: "move this object" would
rewrite a vertex range, and "place fifty cones" would copy the geometry fifty times.

So the live scene carries **meshes plus instances**, which is the shape the glTF exporter already preserves (694
movable placements on Joux Verte, kept as nodes rather than baked). Baking stays where it belongs: in the `.ngg`
export, which `NggBuilder` already does.

```c
// Lifetime and device
ngg_scene*      ngg_create(HWND parent, int width, int height);
void            ngg_destroy(ngg_scene*);
void            ngg_resize(ngg_scene*, int width, int height);

// Bulk open: the serialised .ngg the editor already builds, from MEMORY, never a temp file
bool            ngg_load_memory(ngg_scene*, const void* ngg, size_t bytes);

// Content registry -- add/remove, refcounted, ids assigned by the renderer
ngg_mesh        ngg_add_mesh(ngg_scene*, const ngg_vertex*, uint32_t vcount,
                             const uint32_t* indices, uint32_t icount);
ngg_texture     ngg_add_texture(ngg_scene*, const void* dds, size_t bytes);
ngg_material    ngg_add_material(ngg_scene*, ngg_texture t0, ngg_texture t1, uint32_t flags);
ngg_instance    ngg_add_instance(ngg_scene*, ngg_mesh, ngg_material, uint32_t layer,
                                 const float xform[16]);

void            ngg_set_transform(ngg_scene*, ngg_instance, const float xform[16]);
void            ngg_set_visible(ngg_scene*, ngg_instance, bool);
void            ngg_remove_instance(ngg_scene*, ngg_instance);
void            ngg_remove_mesh(ngg_scene*, ngg_mesh);

// Renderer state -- these already exist on core::Scene; the shim only exposes them
void            ngg_set_camera(ngg_scene*, const float view[16], const float proj[16]);
void            ngg_set_sun(ngg_scene*, const float dir[3], float exposure);
void            ngg_set_sampler(ngg_scene*, int mode);       // aniso / linear / mip clamp
void            ngg_set_debug_view(ngg_scene*, int mode);
void            ngg_set_light_mode(ngg_scene*, int mode);    // baked A/B vs dynamic
void            ngg_set_shadows(ngg_scene*, bool on);
void            ngg_set_sky(ngg_scene*, bool on, bool aerial);

// Editor services the renderer is uniquely placed to answer
bool            ngg_pick(ngg_scene*, int x, int y, ngg_instance* out, float outWorld[3]);
void            ngg_set_selection(ngg_scene*, const ngg_instance* sel, uint32_t count);
void            ngg_render(ngg_scene*);
```

Everything under "renderer state" is wiring, not new rendering work: `core::Scene` already has `SetAtmosphere`,
`SetDebugMode`, `SetLightMode`, `SetShadowsOn`, `SetWeldNormals`, `SetFoliageNormals`, `SetCull`, `OnResize`,
`Bounds` and `FocusPoint`. The viewer's key handling is a thin layer over exactly these, so the first useful
milestone is small.

#### Addition and deletion

**Deletion is a draw-list edit — no GPU work.** Removing an instance stops it being drawn; its vertex bytes stay
resident until a compaction. That is not a compromise: it makes **undo instant too**, because the geometry never
left the buffer. Freeing storage on delete would make undo the expensive operation, which is backwards for an
editor. Compaction happens on save, on export, or when free space crosses a threshold — never on the interactive
path.

**Addition goes to a dynamic pool beside the static scene.** Two tiers, because their lifetimes differ:

| | contents | mutability |
|---|---|---|
| static set | the imported track, uploaded once | flags only: hidden, transform |
| dynamic pool | everything added this session | growable suballocator, grow-and-copy like a `vector` |

Adding one prop must not re-upload 35 MB, and this is also how editing actually proceeds: a large static import
plus a few dozen authored changes. Editing an imported object's *geometry* — rare — is "remove from static, add
to dynamic", so there is one mechanism rather than a special case.

Four details that are awkward to retrofit:

- **Duplicate placement must not duplicate geometry.** Fifty cones is one `add_mesh` and fifty `add_instance`,
  which only works if the scene is instance-based from the start.
- **Ids are assigned by the renderer** and mapped by Wallaby to its own object model, so storage can move under a
  stable handle.
- **Batch the uploads, not the calls.** P/Invoke is nanoseconds; separate GPU submits are not. The shim queues
  uploads and flushes once before each render — `core::Device` is synchronous (`Begin`/`EndAndWait`), so the
  frame boundary is the natural flush point.
- **Materials and textures need the same add/refcount treatment**, or "add a prop with a new texture" collapses
  back into a full reload.

#### Why the renderer must NOT load glTF

The obvious shortcut — teach `ngg-core` to open the editing format directly — reintroduces the exact bug class
this project has already paid for twice.

To show the truth, a glTF loader would have to reimplement the converter's passes: complete-linkage normal
welding, per-material shading overrides, de-atlasing, and the texture round-trip decision. Anything it skipped
would make the preview disagree with the shipped asset. **That is the two-renderers-disagree problem in a new
costume — two content pipelines instead of two renderers** — and it is what put the wrong textures on the road
and cost a `.pso` disassembly to find (§3.2b).

The property to protect: **the C# converter stays the only content pipeline, and the renderer consumes buffers it
is handed.** Then the preview is honest by construction rather than by discipline. A C++ glTF parser and PNG
decoder would also be a large dependency to carry for a preview.

#### "Immediately visible", by tier

Match the mechanism to what actually changed. Most interactions never touch geometry:

| what changed | cost | mechanism |
|---|---|---|
| camera, sun, exposure, sampler, debug view, shadows | free | setters that already exist |
| move/rotate an object, hide it, toggle a layer, select | ~free | per-instance transform and flags |
| weld angle, spherical, flip, mesh edit, repainted texture | scoped rebuild | re-run the C# pass for affected objects, push updated meshes |

Tier 1 is the one that matters most for Wallaby, which is a placement editor: moving and rotating objects is its
main verb.

`Scene::Load` already reads the whole file into a `std::vector<uint8_t>` and parses from `blob.data()`, so
`LoadFromMemory` is a five-line split — and it removes the temp file for opening a project. **Do not reach for it
on every edit**; that is the temp-file mistake moved from disk to PCIe. `load_memory` opens a project, deltas edit
it, and having both from the start is what keeps the API honest.

Tier 2 may start as "rebuild and re-push the whole scene" — 1.4 s for Joux Verte, textures and de-atlas included
— and get scoped down when that is the thing that hurts.

#### Overlays: split by what needs depth

Wallaby drawing overlays *over* a child window with a swapchain runs straight into the airspace problem: a native
child window paints over managed controls, so GDI overlays flicker or never appear. Three ways out:

1. renderer hands over a frame image, Wallaby composites — overlays trivial in C#, but a copy per frame and no
   depth buffer, so selection wireframes cannot occlude correctly;
2. renderer draws everything from selection data Wallaby sends — correct and free, but every overlay tweak is a
   C++ change;
3. **split by what needs depth** — the renderer draws in-world overlays (selection wireframe, grid, gizmos),
   Wallaby draws 2D chrome *outside* the viewport.

**Decision: 3.** Selection wireframes want the depth buffer, which lives in the renderer; a camera-position
readout does not need to be over the viewport at all. `ngg_set_selection` and `ngg_pick` above are the whole
interface that requires.

**Built, and two placement questions were decided by measuring rather than by taste.**

*Where in the frame.* The highlight draws **after the tonemap and after TAA**, straight onto the back buffer,
while still depth-testing against the scene's own buffer. Into the HDR target it would be tonemapped — so the
colour the editor asks for is not the colour on screen — and a one-pixel wire handed to a temporal filter ghosts
behind itself the moment the camera moves. The part that has to stay inside the renderer is the depth test, and
that is the part this keeps.

*Which matrix.* The overlay uses the **unjittered** view-projection, unlike `Record`. The image underneath is
temporally stable, so a wire drawn with the frame's jitter would crawl half a pixel against a still picture. The
cost is that wire and depth buffer disagree by up to half a pixel, absorbed by a depth bias — the right way
round, because a sub-pixel depth error is invisible and a crawling outline is the most obvious thing on screen.
The bias is **positive**, which is the opposite of the conventional-depth instinct: under reverse-Z nearer is
larger. That sign does not fail loudly — it pushes the wire into the surface it traces and the highlight simply
never appears.

Occluded wire is drawn too, dimmed, on the exact complement of the visible pass's depth function. A selection
that vanishes behind a hill is worse than no highlight.

**`[earlydepthstencil]` turned out to be load-bearing, and it is worth knowing why.** The overlay's pixel shader
counts the wire pixels it writes, so the editor can tell "nothing is highlighted" from "everything is highlighted
and rejected by the depth test" — from outside, both are an unchanged screen. But **a pixel shader that writes a
UAV has early depth-stencil disabled by default**: the depth test then runs *after* the shader, which still
suppresses the colour write, so the picture is correct — while the shader has already run and its atomic has
already landed. The counter was measuring rasterised pixels and calling them visible ones. Measured: both
counters reported exactly 25,932 on Joux Verte, every wire pixel counted once per pass with the depth test
contributing nothing. **Two identical numbers out of two complementary depth functions is the shape of this
bug.** With the attribute on, the same 25,932 splits 22,953 / 2,979.

Generalise it: **any instrument written from a pixel shader measures shader invocations, not surviving pixels,
unless early-Z is forced on.**

**And the probe now reports when it did not fire.** `ngg_probe_pixel` writes from the scene's pixel shader, so if
no geometry covers the pixel nothing is written and the readback still holds the *previous* armed frame — which
reads as "identical to the baseline" and passes. That false pass had already been paid for once in the transform
test; it was still there. The buffer is now zeroed before an armed frame and the shader sets a FIRED marker, so a
miss is reported as a miss. It immediately showed that **track-108's transform verification had never run at
all** — its camera aimed at the centre of the bounding box, which on a stage that bends is open air. The test now
searches for a pixel with geometry behind it, and "the geometry left the pixel" counts as the change it is
looking for.

#### Sequencing

1. ~~**Flat C shim over the state setters**~~ **DONE**, plus `ngg_load_memory` and a child HWND.
2. ~~**The mechanical math pass**~~ **DONE** — SlimDX's `Vector*`/`Matrix`/`Quaternion` are gone from the
   editor, which now speaks `System.Numerics` like everything else. (The "79 files" figure this step was sized
   with was wrong; see Wallaby's `CLAUDE.md`. The real count that could go fully clean was 5 — `Render(Device)`
   is in the map-object model itself, so 93 of 106 files name a D3D9 type.)
3. **The rendering files** onto the shim. SlimDX leaves here and x64 flips in the same change.

**Why 3 cannot be incremental, measured:** `SlimDX.dll` is x86 and `ngg-api.dll` is x64 (PE header, checked).
They can never share a process, so there is no "add the new viewport beside the old one" step — the flip is
atomic by construction, exactly as the editor's own notes assumed.

**And what the flip would cost if attempted today:** the editor's draw path is immediate-mode fixed-function —
35 distinct D3D9 device operations, led by `SetRenderState` (106 calls across 31 files), `SetTransform` (46) and
fixed-function `Material` (38). No retained-mode renderer can implement that interface, so it cannot be wrapped;
it has to be *replaced by a description*. More sharply: everything the editor draws that is not track geometry is
a gizmo — 7 `LineStrip` draws, 4 `TriangleStrip`, 3 `TriangleList` — and until ngg-core could draw those, a
flipped editor would render the track beautifully and none of the things a track editor exists to edit.

**So the overlay-line channel came first** (`ngg_set_overlay_lines`, below). The caller replaces a whole layer by
id and the renderer owns everything about how it is drawn: per-vertex colour, so there is no colour state to set;
segments rather than strips, so there is one topology and nothing to get wrong; depth-tested with the occluded
part dimmed, or `NGG_OVERLAY_NO_DEPTH` for manipulator handles, which must stay grabbable. *Described, not
commanded* — that is the difference that lets a D3D12 backend satisfy a request `SetRenderState` never could.

Verified on both tracks by the renderer's own pixel counters, with the same conservation argument the selection
pass uses: 12 verticals through the focus point (which is on the ground, so every line is half buried) come out
110 visible / 53 occluded on track-42; hiding every object empties the depth buffer and gives 163 / 0, the total
unchanged — emptying depth *moved* pixels rather than making them; and `NO_DEPTH` returns exactly 163 / 0 with
the scene restored. An odd vertex count is refused rather than silently dropping half a segment.

Open, and deliberately not decided yet: whether the live scene's per-instance transform should also become an
optional `.ngg` field (the format bakes today, and §9b item 4's culling bounds may want the same per-object
data); and whether picking should be GPU readback or a CPU ray cast against `Wallaby.Formats` meshes, which
already has Möller–Trumbore and would keep the renderer out of it.

---

## 9. Phasing

**P0 — Interception + interop spike. Retires the two biggest risks.** *(weeks, not months)*

- P0a: hook the device, dump a frame's draw calls, confirm the tagging window partitions track / car / UI
  correctly, extract view/proj, suppress all track draws. (No INTZ — see §1.4.)
  *Exit: game runs with an empty world, HUD and menus perfect, debug cube glued to world space.*
  - **Increment 1 — DONE.** ✅ `ngg/bridge/` ships a `d3d9.dll` proxy that routes `Direct3DCreate9` through
    `Direct3DCreate9On12`. Verified by `proxycheck` (7 checks) *and* in the real game: RBR creates its device
    successfully on D3D12 (1024×728, 3 backbuffers, `hr=0`), and `IDirect3DDevice9On12` + `ID3D12Device` are
    reachable from inside the game process. **A 2004 game running on D3D12 is no longer an assumption.**
    Findings: RBR uses `D3DCREATE_MULTITHREADED` (see §2.1) and requests SDK version 31, which 9on12 accepts.
  - **Increment 2 — DONE.** ✅ `IDirect3DDevice9` wrapper **generated from `d3d9.h`** by
    `tools/gen_d3d9_wrapper.py` (99 forwarded, 20 hand-implemented; the generator emits the intercepted
    *declarations* too, so a hand-written signature cannot drift). Resource tagging via private data, draw
    attribution, camera capture, per-frame stats, one-frame inventory dump (F11 or `DumpFrame=N`), and
    tag-based suppression behind `SuppressTagged`. Verified in-game — see the measured blocks in §2.1 and §2.2,
    which resolved the camera question outright.
  - **Increment 3 — DONE, and it invalidated the plan.** Call-site attribution replaced the `ParseLBS`
    bracket entirely (see the correction in §2.1). No plugin change, no anti-cheat-adjacent edit, and the
    partition is now a single config value.
  - **Increment 4 — DONE.** The track-geometry sites were identified in a live stage by cycling suppression
    in-game, and the result was better than expected: the world is partitioned by content type (§2.1), not
    lumped into one site. Suppression now takes a list of sites, and each module is fingerprinted so a site
    table can never be applied to the wrong binary.
  - **Increment 5 — DONE. P0a exit criterion met.** With ground+scenery+sky suppressed the world is gone and
    the UI still renders. The full site map is in §2.1, along with the two findings that change planning: the
    car shares a creating function with the ground, and `0x0876D4` bundles water/trees/grass/scenery together.
    Also measured: a 9on12 resource-loss defect (§1.2) that needs re-testing once suppression is permanent.
  - **P0a is closed.** Remaining P0 work is P0d (photometric spike, in Wallaby) and P0e (minimum-viable-LBS).

**P1 — in progress.**

- **`ngg-host` + IPC — DONE (v1).** The two-process architecture is real and verified end to end by
  `proxycheck`: the bridge creates a shared texture on the game's own 9on12 D3D12 device, launches the 64-bit
  `ngg-host.exe`, the host opens the texture by name and renders into it, and the bridge unwraps RBR's D3D9
  backbuffer in-process and copies the result in. A pixel written by the 64-bit process is then read back
  through D3D9 and checked. The IPC contract is `ngg/ipc/ngg_ipc.h`, with static_asserts that break the build
  if the 32- and 64-bit layouts ever diverge.

  Deliberately synchronous in v1: the game thread waits for the host's frame. Overlapping them would buy
  throughput at the cost of a frame of latency between the world and the car, which has to be measured before
  it is accepted, not assumed away. Every failure path (no host, wrong adapter, host hung or crashed) simply
  deactivates the link and falls back to vanilla rendering.

  Known gaps in v1, all recorded rather than hidden: no resize — a device reset deactivates the link instead
  of rebuilding the shared texture; no depth, so nothing can composite *against* the host's output yet; and the
  host paints a placeholder over the whole frame rather than rendering a world.

- **Test scene + camera contract — DONE.** `ngg/format/ngg_format.h` (chunked container, v0),
  `ngg/tools/make_test_ngg.py` (writes a world-locked test grid with axis markers), and `ngg/host/scene.*`
  (loader + D3D12 pipeline) put real geometry on screen through the game's own camera. Verified in-game across
  several stages, static and under motion. See the measured block in §2.2.

- **LBS -> .ngg converter (prototype) — DONE.** `ngg/tools/lbs2ngg.py` reads real RBR ground geometry and
  emits `.ngg`. Standalone by necessity: **Wallaby does not build here** — it needs SlimDX 4.0.13.43, which is
  discontinued and not installed — so its C# is used as the format *reference* rather than as the
  implementation. It is a CLI so Wallaby can shell out to it once it builds.

  The parser's self-check is the valuable part: a correct parse consumes each block's declared size **exactly**,
  and the tool fails loudly otherwise. That turns "the picture looks odd" into "the layout is wrong at this
  byte". **171 of 207 shipped track files convert with an exact match**; the remaining 31 desync near the end of
  the ground block, spread across all time-of-day variants (worst on `_O`, 16/83) — a format variation still to
  be identified, tracked as a known gap rather than papered over.

  Confirms two things: mesh vertices need **no Y/Z swap** (Wallaby's `getSwapedVector` applies to bounding boxes
  and object positions, not to vertex data), and the bounding boxes agree with a Y-up runtime — e.g. track-100
  spans 11 km in X and 3.8 km in Z but only 197..358 in Y.

- **PIPELINE PROVEN END TO END.** Joux Verte (Map 42) converted from `track-42_M.lbs` and rendered by the
  64-bit host **matches the original geometry exactly** in-game — road under the car, terrain where RBR thinks
  it is. That closes the architecture question the whole of P0/P1 existed to answer:
  `LBS -> .ngg -> 64-bit D3D12 -> shared texture -> D3D9 backbuffer`, with the game's own camera, is real.
  Everything after this is content and quality rather than "does the approach work".

- **Textures and the object model — DONE.** The converter now emits `NGGM` materials, `NGGO` objects and
  `NGGT` embedded DDS; the host decodes BC1/BC2, builds an SRV heap and draws per object. RBR's own
  `GMeshRenderChunk` maps *directly* onto an object: it already partitions each region into
  (sub-mesh, index range, texture), which is exactly an object with a material — so §3.4b's object model came
  out of the source data rather than being imposed on it.

  Joux Verte: 158 regions -> **746 objects, 3 materials, 3 embedded textures**. Only three because the ground
  genuinely uses three (`MB_04_GROUND_C`, `MB_03GROUND04`, `TrackTexturePlateOpaque`); the other 27 named
  textures belong to fences, banners and signs in the *general* meshes, still unconverted. Sub-mesh 0
  (vertex type 4, position+colour only) has zero render chunks in any region — it is never drawn.

  A texture that fails to decode keeps its descriptor slot and borrows a 1x1 white fallback, so a bad texture
  cannot leave a hole in the heap or crash the draw loop.

- **Complete the world — DONE for static content.** The converter covers general meshes (1/1), scenery (1/3) and
  movable meshes (1/6); RBR's world is suppressed wholesale and ours is drawn. Chosen over depth compositing
  because the latter builds a bridge the design intends to burn. What remains is not static geometry but
  *runtime-driven* geometry — fences and movable transforms (§9b items 4/5), both blocked on the harvesting-vs-
  physics-coupling decision.

- **`ngg-core` split + standalone viewer — DONE.** See §8.0. This is the prerequisite for the lighting work
  below: it makes a shader change cost seconds instead of a game launch.

- **Next: dynamic lighting.** The renderer still reproduces RBR's own ground shader exactly — two textures lerped
  by vertex alpha, modulated by the baked vertex colour, scaled by `bakedScale`. Faithful to 2004, and the thing
  the whole overhaul exists to replace. Order matters: **sky and atmosphere first**, because ambient has to come
  from somewhere and "directional light plus a hardcoded ambient constant" is a placeholder that never gets
  removed — and because an analytic sky *answers* §9b item 6 rather than deferring it again. Then sun and
  shadows, per-track direction from `TrackSettings.ini` until §3.2's photometric solve replaces it. `bakedScale`
  and the vertex-colour path then disappear by construction, exactly as `ngg_format.h` predicts.
- P0b: **interop — DONE.** ✅ `tools/interop-probe/` — 17 checks, passing. Established that the renderer creates the
  shared textures and the D3D9↔D3D12 hop stays in-process (§1.2). The next step is wiring the same calls into a
  real `d3d9.dll` proxy rather than a console harness.
- ~~P0c: VR per-eye draw replay~~ — **moved to P4.** It exists to render the *car* twice per frame, and there is no
  car in early phases. Nothing else needs it before the VR phase.
- P0d: **photometric spike** in Wallaby — match vertices across `_M`/`_E`/`_O`, run the solver, visualise recovered
  sun directions and the `_N` light hot-spots. Lower stakes than in rev 2 (RT covers occlusion), but it still
  decides how much per-track hand-authoring §5.3 and §3.4 need.
- P0e: **minimum-viable-LBS experiment.** How little visual geometry can a gameplay stub carry before the engine
  misbehaves? On the critical path now that fully generated stages are in scope (§7.1).

**P1 — Static world.** Converter v1 in Wallaby (Tier 0 lighting, heuristic materials, meshlet build) → `ngg-core`
D3D12: mesh-shader geometry, RT shadows, physical sky, fog matched to vanilla, foliage sway → flat-screen
composite. One track, fully ours. The quality-tier interface (§1.8.4) goes in here, even though only one tier exists.

**P2 — Quality and scale.** RT GI/AO/reflections, sampler-feedback texture streaming above 4 GB, material override
workflow, LOD chains, long view distance, wetness from `.MAT`, weather preset system, Tier 2 solve for sun
estimation, batch conversion of all tracks.

**P3 — Night and motion.** Clustered many-light path, emissive materials, auto light extraction **and the Wallaby
light editor**, headlights; transform harvesting for animated objects; car stencil harmonization and car env map.

**P4 — VR.** Stereo path, OpenXR submission, per-eye car replay, VR UI panel, and the RT-light quality tier —
which is where §1.8.4 gets paid for.

**P5 — The car.** SGS converter, car materials, damage/dirt harvesting, full takeover.

**P6 — Native content.** glTF import, scene description layer, gameplay-stub emission, additive content on
converted tracks, generative material/light/detail tooling (§7.3).

**P7 — Stage generation.** Wallaby-side gameplay generation: drive line, collision, surfaces, pacenotes, triggers
(§7.1b). Renderer-independent; can run in parallel with P4–P6 given people.

Every phase ships behind a config flag with vanilla fallback one setting away.

Three items are deliberately pulled *earlier* than their phase because they're cheap and they de-risk what
follows: the `free` binding mode ships in the P1 format, the quality-tier interface ships in P1 for a VR phase
three steps later, and the minimum-viable-LBS experiment runs in P0.

---

## 9b. Deferred work — the running TODO

Items parked deliberately, with the reason, so "later" does not become "lost".

| # | item | why deferred | blocked on |
|---|---|---|---|
| 1 | **Installer: Start-menu entry + optional desktop icon for `rbrtm.exe`** | packaging, not mechanism; the launcher already works when run directly, and the in-game warning screen already names it | nothing — do it when shipping |
| 2 | ~~Per-track `.ngg` selection + fallback~~ **DONE** | the bridge pushes the track name over IPC (`ngg_SetTrack` -> `host_link`), the host reloads on change, and **clearing `kHostFlagSceneReady` stops suppression** so an unconverted stage renders as plain RBR instead of an empty world | done |
| 2b | **Converter must merge the `_M/_N/_E/_O` variants into ONE `.ngg`** | assets are named per TRACK (`track-42.ngg`), because variants belong inside the file as layers and presets (§3.4b), not in the filename. But `lbs2ngg` still converts a single `.lbs`, so converting a second variant currently overwrites the first rather than adding to it | the photometric solve (§3.2) needs all variants anyway |
| 3 | **`.trk` reading for pacenote marks / signs** | needs to know which draws are pacenotes before regenerating them; guessing risks misplacing safety-critical content (§1.4) | identifying the draws |
| 4 | **`.fnc` fences** | generated from a point list and *deformed* at runtime, so they need draw harvesting, not conversion (§4.1b). The long roadside fences are currently missing from our render | harvesting |
| 5 | **Movable-mesh transforms** | correctness gap: they render at their authored position and no longer move when hit. Offset-reads are ruled out (§4.1a), so this needs draw harvesting + identity mapping | harvesting |
| 6 | ~~Skybox~~ **DECIDED — not converted** | an analytic sky replaced it (§5.0). A painted skybox cannot respond to a moving sun, which is the point of the overhaul | done |
| 7 | **Per-object transforms in `.ngg` (format v2)** - RENDERER SIDE DONE (5.0o): the BVH is per mesh with instance transforms, dragging is TLAS-only; the format field and the converter remain | movable meshes are baked, which is why they cannot move. Also wanted for GPU instancing of generated content (§7) | follows from 5 |
| 8 | ~~Road-chunk seams at distance~~ **EXPLAINED — §3.3b** | RBR's ground textures are 2048² atlases; the road's cell is bordered by luma-100 content and the gutter is ~2.25 texels. **Anisotropic tap spread** crossing the cell edge is the dominant term (confirmed: disabling aniso nearly removes it), sub-texel gutters at high mips the residual | closed — fix is item 9 |
| 9 | ~~De-atlas the ground textures~~ **DONE — in `wallaby-cli`** | shipped by the converter session; `track-42.ngg` went from 29 to 74 textures and **the road seams are gone**, confirmed in the viewer. Closes item 8 for real | done |
| 10 | ~~Object meshes carry a non-UV second UV set~~ **CURED renderer-side — `kMaterialSingleStage`, §3.3f** | the flag exists, the shader skips stage 1 entirely on it, and `MeasureSingleStage` sets it per object at load where the file does not. **And the isfinite() guard that was supposedly protecting this had never once fired** — see §3.3f | converter should set the flag; the fallback stands down when it does |
| 11 | **Carry the car's start position into `.ngg`** | the viewer has no idea where a stage actually starts, so it uses a geometry centroid as a stand-in. RBR keeps the real start in the track data | converter |
| — | ~~31 tracks fail to convert~~ **NOT A DEFECT** | measured over the corpus: **171 of 171 unprotected tracks convert; all 31 failures are protected (v88/89) tracks, a clean 1:1.** No layout bug ever existed. The converter now says so instead of failing deep in a mesh reader | closed |
| — | ~~Release-only pacenote crash~~ **FIXED** | detours replacing `__thiscall` game functions saved/restored `ECX` with inline asm around a compiler-managed prologue/epilogue; held in Debug, broke under `/O2`. Now `__declspec(naked)` free functions. Confirmed in-game | done |
| 12 | **9on12 loses trees after ~1 min** | pre-existing D3D9On12 defect, masked while the world is suppressed | re-test with suppression permanently on |
| 13 | ~~No culling at all~~ **DONE — frustum + per-cascade** | became urgent the moment shadows landed: four extra full-scene passes took track-108 from 40 fps to **12**. Per-object AABBs computed at load (the §3.3e renderer fallback), culled against the camera frustum and against **each cascade's own volume**. Measured: **763 drawn / 5068 culled**, and 12 fps → **25**. Converter-side bounds are still worth doing — only the converter can choose the grouping | done |
| 14 | ~~Environment LUT for the atmosphere~~ **DONE - 5.0d** | 64x64 octahedral, 7 roughness mips, rebuilt per frame. Answers BOTH the ambient specular and the ambient diffuse; measured 17.7 -> 19.0 fps on track-108 | done |
| 15 | **Cheaper distant cascades** | cascade 3 covers kilometres and changes slowly — updating it every second or third frame is invisible; 2048² for texels already 0.7 m across is wasted bandwidth | nothing |
| 16 | ~~TAA history filtering and sharpening~~ **DONE - 5.0h** | Catmull-Rom history (5 taps) + clamped post-tonemap sharpen 0.5. Detail under a 10 px/frame pan 33 % -> 51 % of the no-TAA frame, static 53 % -> 64 %; cost +15 % frame-to-frame change at rest. `--pan` added to measure motion. Remainder under motion is the clip/blend: motion-adaptive blend is the next step if driving shows it | done |
| 17 | ~~Exposure is still a hand-set constant~~ **DONE - 5.0g** | histogram metering (centre-weighted, 40-90th percentile), asymmetric log-space adaptation, ±3 EV around the dial; calibrated so the view the dial was tuned on is unchanged (0.798 vs 0.800). `X` A/Bs it; `ngg_set_auto_exposure` lets the editor lock it. The claim that it would remove `sunDiffuse`/`skyAmbient` was wrong - see item 24 | done |
| 18 | ~~No ambient specular~~ **DONE — 5.0d** | split-sum against the prefiltered sky, Karis' analytic BRDF fit, `J` to A/B it. What remains is that the environment is the SKY only - no local reflections, so a wall does not see the wall opposite. That is a reflection-probe or RT job, not this one | done |
| 19 | **PBR maps exist in the format and in no asset** | roughness, metallic, normal and emissive all read their factors and multiply by a white fallback, so every surface is currently a uniform rough dielectric. Generating maps from albedo is the converter half; the caveat that decides quality is that generated normals read painted-on shading as geometry, and RBR textures often have baked lighting | converter |
| 20 | **Mips are MISSING on community content** - RENDERER FALLBACK DONE (5.0l): coverage-preserving chains generated at load, mip 0 untouched, 203 ms / +7 MB on track-108, shimmer -41 %; the converter should still do it, and the fallback stands down when it does | measured through the loader: `track-42` (original RBR) 0 of 74 textures without a mip chain, `track-108` (community) **76 of 88**, including a 2048 atlas and both 3200x800 backdrops. A single-level texture cannot be minified, so it aliases at distance and anisotropy has nothing to filter - and TAA burns its budget on it. The renderer logs the count; the cure is decode/downsample/re-encode in the converter, which needs the BC encoder | converter, and it gates any upscaling |
| 22 | ~~RT probe~~ **RT SHADOWS ARE THE SHADOW TERM - 5.0e** | depth+normal prepass, normal-offset bias, any-hit foliage; 19 -> 29 fps because the cascades are no longer rendered and the main pass shades each pixel once | done |
| 23 | **Decide the cascaded shadow maps: delete, or keep ONE as the low tier** | RT is the shadow path and stays so: DX12 Ultimate is the floor, so every supported GPU has hardware RT, and it MEASURED faster (19 -> 29 fps, 5.0e) and better. That puts us with the RT-required titles (Metro EE, Indiana Jones, Doom TDA), not the console-bound hybrids that keep raster sun shadows. But three costs are unmeasured, and each is where RT shadows are known to hurt: (1) **alpha-tested foliage** - every leaf-card hit runs an any-hit test, RT's most expensive path, worst on AMD, and RBR stages are forests; (2) **moving geometry** - the car, movable meshes (item 5), foliage sway all need per-frame BVH updates that shadow maps get for free; (3) **weak-RT hardware and VR** (P4: two eyes at 90 Hz), where RT cost scales with pixels. Decide by measuring a foliage-heavy stage in-game on min-spec, with the car in the scene: within budget -> delete the cascades and their bias/PCF/fitting; over -> keep a single cheap cascade as the low quality tier (§1.8.4) | that measurement |
| 24 | ~~`sunDiffuse` / `skyAmbient` are relative fudges~~ **AUDITED - 5.0i** | they compensated two deficits: a one-sample sky irradiance at 0.43-0.65x the true integral (FIXED: irradiance map, verified within 0.3 %) and single scattering (item 25). Physical balance built behind `Z`; default unchanged until 25 | 25 |
| 25 | ~~Multiple scattering in the atmosphere~~ **DONE - 5.0j** | Hillaire 2020 Psi_ms table, verified to fp16 against numpy; sky light +32 %; physical + multiple scattering measured 10.6:1 sun:sky in the lab (clean clear air) and made the DEFAULT | done |
| 27 | ~~One frame, in `ngg-core`, for every client~~ **DONE - 5.0n** (game host verification is in-game) | the RT path (depth prepass, trace, denoise, AO, the jitter-before-prepass ordering) exists only in the viewer; the game host still renders cascades with no prepass, no AO, and Wallaby's API the same (5.0m). Move frame orchestration into ngg-core - one `RecordFrame` the viewer, the game host and the API all call - so "what the viewer shows" and "what the game shows" stop being two code paths. Prerequisite for item 23's in-game measurement | nothing |
| 28 | ~~The bridge passes the event's weather preset~~ **DONE - 5.0s** (in-game check pending) | RBRTM reads the weather it set back from the game, names it as RBR's TrackSettings section (`108M_crisp_clear`), and passes it through `nggSetWeather` -> `FrameState.weatherName` (IPC v4); the host selects that preset, logging any fallback to the default. Fixed on the way: single-digit maps (`04M_...`) imported nothing | in-game run |
| 29 | **Weather v2: cloud shapes, rain, snow, night** - WET SURFACES DONE (5.0v): the road's wetness darkens, glosses and puddles the ground | v1 (5.0q) does fog, haze and cloud COVER. Partly cloudy is a uniform blend; RBR's cloud textures vs volumetric clouds is undecided. Left of rain: drops, spray, rain on glass, reflections of more than the sky, drying. Snow is carried in the presets but not drawn. Night: RBR has none (its N is NOON, 2026-10-30) - night stages are WRally's own design question | user's choice on clouds; night design |
| 26 | **Delete the established lighting balance** | `sunDiffuse`, `skyAmbient`, `kSpecScale`, the albedo*E convention and the lerp ambient. Kept behind `Z` for one round so the physical default can be compared in-game | the user's in-game look at the physical default |
| 21 | ~~Aerial perspective is the last per-pixel atmosphere march~~ **DONE - 5.0f** | 64x64x32 froxel table of in-scatter + transmittance, filled per frame with the same `AtmScatter` and the same 8x3 quadrature; the march stays as the exact reference (`L`). Aerial's share of the main pass 1.9 -> 0.5 ms at the start camera, max 1/255 difference to the march | done |

## 10. Status of open questions

**Resolved.** Address space and process model; presentation model; graphics API; hooking method; GPU/feature
baseline; weather model; encryption timing; VR; legacy plugin policy; converter home; car preparation;
generated-content scope; night light authoring scope; **conversion spine — `LBS → glTF → .ngg` for converted
tracks too, with direct `LBS → .ngg` a shortcut expected to go (§3.0)**.

**Not blocking design, but needs an owner before the phase it belongs to.**

1. ~~Whether a D3D9On12-unwrapped resource is shareable~~ — **answered by experiment: it is not, and it doesn't
   matter.** See §1.2. Retired as a risk.
2. **RT-vs-VR budget (§1.8.4).** The architecture handles it via quality tiers, but *which* techniques land in the
   VR tier is a measurement question that can only be answered on real hardware in P4. Nothing to decide now;
   something to keep honest about, because "VR at 90 Hz with ray tracing" is the kind of promise that quietly
   becomes 45 Hz.
3. **Distribution.** Vanilla-only means our own launcher/distribution rather than riding an existing one. That's a
   product question, not a rendering one, but P1 shouldn't ship before someone owns it — a renderer nobody can
   install is not a renderer.
4. **Minimum-viable-LBS (P0e).** Not a question we can answer from the desk; the experiment answers it, and its
   result sets the ceiling on P7.
5. ~~How the tagging bracket gets installed~~ - **moot.** There is no bracket. Tagging is by creating call site
   (§2.1), so nothing in RBRTM needs to change and the tournament hook installation is untouched.

**Deliberately deferred.** Encryption (§3.7), native DLS/SGS animation player (§4.2), particle replacement.
