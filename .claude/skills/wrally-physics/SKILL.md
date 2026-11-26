---
name: wrally-physics
description: Load for any change to or question about W-Rally's RBR physics (modules/physics-rbr: num/ data/ native/ sim/, WPhysicsRBR.dll, game/src/physics), golden-replay-41 failing, bit-exactness, .phr/.rpl recordings, PHYS_RSQRT, rsqrt/rcp tables, x87/F32 emulation, or plugin-rbr's oracle and native checks. Gives the code map, the compile rules that keep it bit-exact, how to verify a change, and the traps.
---
# W-Rally physics (the native RBR physics)

The physics is RBR's vanilla physics (`RichardBurnsRally_SSE.exe`, not NGP), rewritten as C++ classes and
**bit-exact** with the game. "Done" means bit for bit, not "within tolerance". The only allowed difference is NaN
payloads (decision D5). Decisions and history: `docs/design.md` §4.2 and §4.3a, `docs/imported.md`. The
reverse-engineering record is in plugin-rbr: `<plugin-rbr>/doc/physics-reimplementation-design.md` (§1.1 holds the
bit-exact rules), `physics-standalone-plan.md` and `rpl-format.md`.

## Map

| path (W-Rally repo) | what |
|---|---|
| `modules/physics-rbr/src/num/` | `phys-num`: `f32.h` (F32: one scalar SSE op per operation), `x87.h` (X87: the game's x87 emulated, PC24), `approx.*` (rsqrtss/rcpss from fixed tables, `tables/*.inc`), `trig.*` (deterministic fsin/fcos), math, collide primitives |
| `src/data/` | `phys-data`: readers for `.trk .col .lsp .rbz(inflate) .rpl .phr(rec.h) .lbs .sgc .dls`, `files.h` (`ReadWholeFile`, all reads go through it) |
| `src/native/` | the physics as classes, split into layered libs: `rbr-base` < `rbr-collision` < `rbr-tyre` / `rbr-world` < `rbr-car` < `rbr-load` (see `CMakeLists.txt` comment) |
| `src/native/def_keys.inc`, `damage_tables.inc` | GENERATED in plugin-rbr (`phys/re/py/native_defs.py`, `native_damage.py`). Do not hand-edit. |
| `src/sim/` | `sim.h` `Physics` + `NativeSim` (`phys-sim`). There is no classic `Sim` in W-Rally. |
| `src/module/module.cpp` | `WPhysicsRBR.dll` (id `rbr`), output `bin/modules/physics/` |
| `include/wrally/iface/physics.h`, `records.h` | the module ABI. Any ABI change bumps `kPhysicsInterfaceVer`. Caps `kPhysicsCap*`. |
| `game/src/physics/physics.{h,cpp}` | the game's wrapper over the DLL |
| `game/src/session/session.cpp` | records `.phr` (`S`/`C`/`P`), replays one and checks poses (`CheckPose`) |
| `game/src/rules/stage.*`, `app/options.cpp` | stage surface (exe table 0x4a1330 / `Tracks.ini Surface=`), tyre type, CLI |
| `tools/approx-table/` | `--self-test`, `--verify`, `--capture` for the rcp table |
| `tests/physics-layers/` | link tests: each lib linked WHOLE with only the libs below it |

plugin-rbr (separate repo, not built by W-Rally) keeps the classic physics (`phys/core`, `phys/model`), the
32-bit oracle, the in-game probe, `phys/re/py` and the check scripts. The two copies of `native/` evolve separately.
A fix that matters to both is ported by hand (`docs/imported.md`). plugin-rbr's `ng` branch is no longer developed.

## Bit-exact rules (do not break)

- The physics libs compile with **C++17 and `/fp:precise`** (the rest of W-Rally is C++20). Never `/fp:fast`, never
  FMA or `/arch:AVX2` contraction. The flags sit in `modules/physics-rbr/CMakeLists.txt`. Do not "modernise" them.
- **All arithmetic goes through `F32` / `X87`**, in the original's operation order. Plain `float` math may be
  reordered, contracted or moved to x87. Packed `_mm_*_ps` add/mul gets reordered by MSVC, so use `F32x4` (four
  scalar ops). Packed min/max are safe.
- Branches on compares use the `jmp::A/AE/B/BE/E/NE/P` helpers named after the original's jump. The jump decides
  what a NaN does. Translate compares from the **disassembly**, never from the decompiler.
- rsqrtss and rcpss come from fixed tables (`num/approx.h`). They are vendor-specific in hardware.
  `PHYS_RSQRT` = `AMD` (default, all tests) | `INTEL` | `NATIVE` (not deterministic). It feeds the module's build
  hash. The car's collision meshes (`data/sgc.cpp`) use the AMD **rcp** table (fixed 2026-11, commit 7ef6cbc).
  Never call the `_mm_rcp*` / `_mm_rsqrt*` intrinsics directly.
- fsin/fcos come from `num/trig.h`. Wheel angles grow without bound, so large arguments are real.
- Float constants are taken from their bits (`FromBits(0x438f8000)`), not hand-converted.
- Keep the original's quirks and bugs. Enhancements belong in a different module (WPhysics, design §4.3), never in
  `rbr`.
- Do not throw C++ exceptions for control flow in physics code. `Unreached` (rbr-base) marks a path no classic run
  takes.
- A comment that cites an exe address must copy it from a source that has it (plugin-rbr's classic header or the
  design doc), then grep-check it. Six were once wrong from memory.

## Verify a change (W-Rally)

The gate is **`golden-replay-41`**: RBR's own recording of stage 41 (49,913 poses, impacts, torn-off parts) replayed
through `W-Rally.exe` and the DLL. Every pose must match word for word.

```bash
CM="<cmake>"                                    # VS 18's cmake.exe; not on Bash's PATH
"$CM" --build build --config Release -- /m 2>&1 | grep -iE " error |warning C|LNK"
ls -la --time-style=full-iso build/bin/W-Rally.exe build/bin/modules/physics/WPhysicsRBR.dll   # fresh?
ctest --test-dir build -C Release -R "golden-replay-41|rcp-table|physics" --output-on-failure
```
- `-R physics` also runs `physics-rbr-handshake` and the four `physics-layer-*` tests. `-E golden` skips the slow one.
- The golden takes about 2.5 to 4.5 minutes and **opens the game window**. It needs a desktop session.
- It exists only if configure found `WRALLY_TEST_RBR_DIR` (needs `Maps/track-41_M.trk`), `WRALLY_TEST_NGG` and
  `WRALLY_TEST_GOLDEN`. Otherwise configure prints "golden-replay-41 left out". Check `build/CMakeCache.txt`.
- The same run by hand, for its log line:
  `build/bin/W-Rally.exe <RBR> <stage-41 .ngg> --stage track-41_M --step 0.1 --no-sound --overrides none --replay <golden.phr> --out <scratch>/g.phr`
  It prints `replay check: N poses, all bit-exact with the recording` or `... K of N poses differ ..., the first at tick T`.
- The golden covers ONE stage (tarmac), one car (i20 R5 model on s_i2003 physics), damage off. For a change in
  gravel/snow, tyre or other cars' paths, also do an A/B on your own recordings: record with the old build
  (`W-Rally.exe <RBR> <ngg> --stage <s> --sgc <car.sgc> --car <physics folder> --exit-at <ticks> --out a.phr`). Then
  replay `a.phr` with the new build (`--replay a.phr`) and read the `replay check` line. A recording by W-Rally holds
  its own `P` poses.
- Plant a deliberate change (one constant) once, to prove that the check reaches your code path.
- Run on an Intel CPU too when tables or CPU-dependent code change: rcpss/rsqrtss differ between vendors, which is how
  the rcp bug was found. Report the "replay check" line and the first differing tick.
- Comparing renders: only within one compiler. Physics stayed bit-exact from v143 to v145, but frames did not.

## plugin-rbr checks (the classic as oracle)

Only for work that compares against RBR's own code. Build dirs `phys/build32` (oracle, probe) and `phys/build64`,
both configured for VS 18. Build with `<cmake>`, never VS 2022's (it fails quietly). Scripts are in
`phys/tools/check/` and run from that folder in Bash (README there):
`verify.sh "<Class>"` (fuzz + swap checks, needs numpy/capstone/pefile), `native_check_all.sh [jobs] [trials]`
(default 10 jobs, the user's choice), `sim_native_all.sh` (`PHYS_SIM_RESET=3000` too), `golden.sh check`.
31 stages have non-R7CO `.col` files that both physics refuse. Count them as skipped.

## Data and formats

- `.phr` (`data/rec.h`): `PHR1` header. Records `S` (StepForward), `C` (controls), `P` (13 pose words),
  `G`/`U`/`A`/`K`. One tick = dt 0.007 (142.857 Hz). The bit-exact contract is one tick.
- `.rpl` (RBR replay, `data/rpl.*`): controls plus pose samples every 5 tics. Sample k = the state after recorded
  tic 5k+4, recording starting at world tick 160. RBR's playback does not run physics, so test resimulation against
  the samples.
- Car physics: `--car` defaults to `Cars/<folder>/physics/` when it has `common.lsp`, else a `physics.rbz` folder
  (`s_i2003`). The tyres still come from `physics.rbz`.
- Surface vs tyres: the surface comes from the map number (0x4a1330: 10..18 snow, 41..49/73/85/86/94 tarmac, the
  rest gravel) or `Tracks.ini [MapNN] Surface=`. The tyre type (`--tyre-type 0..6`) is a separate choice. Gravel = 3.
- Car frame: the pose origin is the centre of gravity, +x = car's LEFT, -y forward, z up. Wheels are CWheel at car
  +0x470 + 4k (`sim/sim.h`).
- The reference `<RBR>/physics.rbz` is vanilla 2004 (md5 `afad3e64d7456e473c3c7fbc447c9a52`). NGP's copy is kept
  as `physics.rbz.ngp`, and `PhysicsNG.dll` is renamed away. Never test against NGP data.

## Pitfalls

| symptom | cause | fix |
|---|---|---|
| golden: "48646 of 49913 poses differ, the first at tick 1424" on Intel only | hardware rcpss/rcpps in the car's collision meshes (HedgeHog3D's D3DX SSE2 path) | fixed AMD table (`RcpApprox`); never hardware approximations |
| "bit-exact" after an edit, but the code did not change | stale exe: the build failed quietly (wrong cmake, error grep case-sensitive) | `grep -i`, check the DLL/exe timestamp before trusting a run |
| `cmake: command not found` in Bash | VS's cmake is not on PATH | full `<cmake>` path, or `vswhere -latest` + `Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe` |
| `error C2001: newline in string literal` after a scripted edit | a Python heredoc / `python -c` turned `\n` in a C++ string into a real newline | edit C/C++ string literals with the Edit tool |
| `LNK1104: cannot open ...exe` | the exe (W-Rally, a phys tool) is still running, often a background run | `tasklist`, stop it, rebuild |
| `LNK2038 RuntimeLibrary mismatch` linking a scratch tool against phys libs | W-Rally builds `/MT` (`CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded`) | build the scratch tool `/MT` too, Release |
| strange syntax errors around `near`/`far` | Windows macros | never name variables `near` or `far` |
| a planted-change run tests the mutant after the restore | the restored file keeps the copy's old mtime, MSBuild keeps the .obj | `touch`/`os.utime` the file after restoring |
| fuzz exact but in-place swap diverges (plugin-rbr) | the exe's LTCG callers keep xmm/GPRs live across calls | swap thunks save xmm0-7, ecx, edx |
| `$env:X = $null` blocked in PowerShell | sandbox | run env-var experiments from Bash |
| `python3` prints "Python was not found" | Windows Store alias | use `python` (3.13); add `-I` |
| `W-Rally.exe` exits 3 at load | the physics refused the car or stage (see the log line before) | read the `physics:` line; `.sgc` collision-node rotations are now supported (a12c9c5) |
| gravel run drives like tarmac | tyre index left at 0 (tarmac dry) | set the tyre type from the surface (`SurfaceTyreType`) |

## Memory notes with more depth
`physics-reimplementation.md`, `physics-no-exe.md`, `m6-step2-native.md`, `m6-cleanup-staged.md`,
`native-load-defs.md`, `rpl-replay-format.md`, `stage-surface-tyres.md`, `car-render-frame.md`, `wrally-build.md`,
and plugin-rbr's `physics-m2/m3/m4*.md`, `phys-build-cmake.md`.
