---
name: rbr-reverse-engineering
description: Load when you need to find out what RBR itself does - decompiling or disassembling RichardBurnsRally_SSE.exe or HedgeHog3D.dll (Ghidra headless, dumpbin), looking up a known exe address, structure or table (car frame, CarObj, the surface table), reading plugin-rbr's RE notes/scripts, or dumping an .lbs/.trk block layout with wallaby-cli info. Gives the tools, the recipes, the verified anchors and the method (prove it with data from several stages and RBR's own picture).
---
# Reverse-engineering RBR

What RBR does is the spec for W-Rally's physics (bit-exact) and for much of its renderer and importer. Start from
what is already known. Decompile only what is not.

## 1. Look it up first (in this order)

1. **Memory and docs.** W-Rally memory notes (`earlier-project-knowledge.md` lists where older decisions are).
   plugin-rbr's memory folder. `<plugin-rbr>/doc/physics-reimplementation-design.md` §1.2 (the engine map),
   `physics-standalone-plan.md` (S1 world inputs, M4), `rpl-format.md`, `rendering-overhaul-design.md`.
   W-Rally `docs/ngg-design.md` (renderer findings, e.g. §2.8 clip walls) and `docs/design.md`.
2. **plugin-rbr's RE tree** `phys/re/`:
   - `ported.txt` (status + name per function address)
   - `exe-strings.txt` (strings each function and its callees push)
   - `port-classes.txt`, `damage-map.txt`, `lsp-fields-m_lancer.txt`
   - `original/`: the ONLY real RBR sources that exist, the collision format and its loader. Do not ask for more.
3. **plugin-rbr's headers**: `RBRTMHelp.h`, `physic.h`, `IRBRGame.h`, `repair.h`, `records.h`. The plugin's game
   hooks and structures live there.
4. **Wallaby** (`<wallaby>`, branch `ng`, C#) already parses `.lbs .col .trk .mat .dls .fnc`: `CLAUDE.md`,
   `Wallaby.Formats`, `MapObjects/`. Its parsers are informed guesses, so verify them against the game.
5. **Grep the address** across all of them (`grep -rniE "0x0*5273a0"`). Skip `build*` and `.claude/worktrees`
   copies.

## 2. Tools

| tool | use |
|---|---|
| Ghidra 12.1 headless (`<ghidra>/support/analyzeHeadless.bat`) | decompile, xrefs, strings. Project `<ghidra-projects>/RBR` holds **only the exe** (fully analysed). |
| plugin-rbr `phys/re/ghidra/*.java` | `Decompile.java <outDir> <addr>...` (one .c per function + callers/callees), `XrefStrings.java <out.txt> <needle>...` (`~` = substring), `FunctionList.java` |
| VS `dumpbin` (`<msvc>/bin/Hostx64/x86/dumpbin.exe`) | quick disassembly: `dumpbin /nologo /disasm /range:0x1000c7a0,0x1000c800 <RBR>/HedgeHog3D.dll` |
| plugin-rbr `phys/re/py/` | `rbrdis.py <addr> <len> [noret]`, `strref.py <text>` (who pushes a string), `xrefs.py`, `rtti.py` (vtables from RTTI; Ghidra's RTTI pass gave none usable), `whatis.py <snap> <vtables.json> <addr>` (object behind an address), `exestrings.py`, `vtmap.py`, `callgraph.py`. They need `pefile` + `capstone`, which the current Python 3.13 lacks. |
| plugin-rbr probe + oracle | in-game snapshots (`phys/probe`, PhysProbe.dll) and offline replay of the original's `StepForward(1)` (`phys/build32/Release/phys-oracle.exe`, `PHYS_WATCH=<addr>` hardware watchpoints) |
| `wallaby-cli info <file.lbs/.trk> [offset ...]` | the block table. Offsets resolve to blocks and the nearest movable instance matrix. |

Headless recipes (from Bash; run from `<ghidra>/support`, log to the scratchpad, read the output file):
```bash
# the exe, read-only, no re-analysis (fast)
./analyzeHeadless.bat "<ghidra-projects>" RBR -process RichardBurnsRally_SSE.exe -noanalysis -readOnly \
  -scriptPath "<plugin-rbr>/phys/re/ghidra" -postScript Decompile.java "$S/dec" 0x5273a0 0x47f420 > "$S/gh.log" 2>&1
# HedgeHog3D.dll (or LocoMotion.dll): NOT in the RBR project - import once into a scratch project
./analyzeHeadless.bat "$S/gh/proj" HH -import "<RBR>/HedgeHog3D.dll" -scriptPath "<plugin-rbr>/phys/re/ghidra" \
  -postScript Decompile.java "$S/hh" 0x100590b0 > "$S/gh/hh_import.log" 2>&1     # minutes: full analysis
./analyzeHeadless.bat "$S/gh/proj" HH -process HedgeHog3D.dll -noanalysis -postScript ...   # later calls
```
- Keep `-readOnly` on the shared RBR project. Do not import other binaries into it without asking.
- The first import analyses for minutes. Run it in the background and wait for `REPORT: Post-analysis` or an
  `Exception` in the log, or for the output file.
- In `cmd //c "..."` form, use backslash paths. The Bash form above works with forward slashes.
- Write small custom GhidraScripts (Java) in the scratchpad. When editing one with a script, beware that `\n` in
  Java strings turns into real newlines.

## 3. Anchors (verified in sources; re-grep before you rely on one)

| what | value | source |
|---|---|---|
| exe image base / HedgeHog3D base | 0x400000 / 0x10000000 | PE headers |
| vanilla exe | `RichardBurnsRally_SSE.exe`, md5 `36c0406a89f2c6d5dcd3d5e3129e606b`, MSVC 7.1 `/arch:SSE`, has relocations | design §1 |
| physics step (live) | `CApplication::TicPhysicsRecording` 0x47f420 -> `StepForward(1)` 0x5273a0. `Advance` 0x523f80 is NOT called while driving. | design §1.2 |
| per-frame pass (output only) | CPhysics vslot 3, 0x526a10 | standalone plan |
| world inputs | every entity from a `CSigStuff` record to CPhysics 0x5267d0. `.trk` loader 0x46e300. | standalone plan, `data/trk.*` |
| FPU state in physics | x87 CW 0x007F (PC24), MXCSR 0x1FBB, dt 0.007 | design §1 |
| approximations | rsqrtss in `Normalize3Approx` 0x4fd0f0. fsin/fcos only in Sin 0x53f3b0 / Cos 0x53f3a0. 191 vendor-specific sites in total. | design §1.1 |
| stage surface | `GetSurfaceType` 0x4a1330: maps 10..18 snow, 41..49/73/85/86/94 tarmac, else gravel | `RBRTMHelp.h`, W-Rally `game/src/rules/stage.h` |
| car setup / options | `[0x7ecd00]` CCompleteCarSetup, `[0x7ecd04]` options; tyre type option 0x566250 | `phys/core/defs.cpp`, `rec.h` |
| damage store | `[[0x165fa90]+0xc98]`, 0x3eb0 per car. Save = 0x567930. | design, `rpl-format.md` |
| car frame | pose +0x140 = centre of gravity, quat +0x100; CWheel at car +0x470 + 4k (lf rf lb rb); +x = car's left, -y forward | W-Rally `sim/sim.h`, memory `car-render-frame.md` |
| `.lsp` defs | Tweak objects at def + 4*offset, value +0xc. 55 dicts at 0x723200..0x724900. `(float)atof` proven. | standalone plan, memory `m6-naming-sources.md` |
| HedgeHog3D D3DX | SSE2 dispatch table 0x100b1984 (mode 2). `MatrixRotationQuaternion` stays generic x87 at 0x100a65f5. Track loader block switch 0x10052220, clip walls (1,4) read at 0x100590b0. | W-Rally `data/sgc.cpp`, `docs/ngg-design.md` |
| `.lbs` blocks | (1,1) general meshes (NOT only plants), (1,4) clip walls, (2,11) car start, (2,13) drive line, (2,14)/(2,15) script objects + random groups | `data/lbs.h`, ngg-design |
| `.trk` blocks | (3,22) collision objects, (3,20) drive line | `data/trk.h` |

HedgeHog3D.dll is the renderer, but it also builds physics inputs: the car's collision meshes from the `.sgc`
(through D3DX) and the stage's draw/cull data. LocoMotion.dll is part of the physics too. `HedgeHog3D_NoSSE.dll` is
the non-SSE variant. Check which one RBR loads before trusting a code path.

## 4. Method

1. **Static guess -> prove it in the game.** Static analysis once picked `Advance`, and the probe disproved it.
   Use probe triggers (int3 on paths never reached), `PHYS_WATCH` watchpoints, or a 1-tick oracle pre->post compare.
2. **Branches and compares from the disassembly**, not the decompiler (NaN behaviour, jump kind). Constants from
   their bits (`push 0x438f8000`), `.rdata` floats read from the file.
3. **Name from the exe's own strings** before guessing. Check the function AND its callees for pushed `.rdata`
   strings (`__FUNCSIG__`-like `CSignalManager::...Emit` texts). This turned a guessed "GearGrind" into GearEngage.
4. **Verify on several stages and cars, not one.** Use tarmac, gravel and snow sessions, a 2WD car (MG ZR), and an
   impact run. Survey all of `<RBR>/Maps` with a script (~207 `.lbs`; some block versions fail to parse, and
   protected stages are encrypted). A rule that holds on stage 41 alone is a hypothesis.
5. **Compare with RBR's own picture.** For render or cull behaviour, put a W-Rally shot next to RBR's from the same
   spot and ask the user to confirm (clip walls: the forest hanging in mid-air on Joux Verte).
6. **Prove a check can fail.** Plant a mutation (one constant) and expect a difference. A loose "benign" rule once
   hid a real bug.
7. Known benign differences (don't chase): vec4 W lanes (stack garbage), stack addresses in scratch,
   profiler/rdtsc timing, `CTireSignalStatsUI`, IAT rewrites, the per-frame pass's fields (car +0x660/+0x814,
   wheel +0xee4/+0x1324).
8. **No plant/kind detection from RBR data.** Block (1,1) holds people, cars, houses and fences too. Vertex type,
   sway value and texture names do not separate them. Kinds come from Wallaby's tags.
9. Record the finding where the next session finds it: the design doc section, `ported.txt`, and a comment citing
   the address. Add a memory note only for decisions and gotchas.

## Pitfalls

| symptom | cause | fix |
|---|---|---|
| `ModuleNotFoundError: capstone` / `pefile` | Python 3.13 has none of them now | `dumpbin /disasm /range:` or Ghidra headless. Ask the user before you `pip install`. |
| `python3`: "Python was not found" | Windows Store alias | `python -I` |
| Ghidra finds no function at a HedgeHog3D address | the RBR project holds only the exe | import the DLL into a scratch project (above) |
| Ghidra output empty, script "failed" | Java compile error in a custom script (often after a scripted edit) | `grep -i "error\|exception" -A5` the headless log |
| an address from memory is off | addresses were recalled, not copied | copy from a source, then grep-check it; keep an address only if a source has it |
| a stage/format rule breaks on other maps | checked on stage 41 only | survey all maps; mod stages put `Surface=` in `Tracks.ini` |
| oracle run with a load log crashes | the log and the snapshot come from different game sessions (heap pointers) | use the log and snapshot of the same session |
| replay playback "doesn't run physics" | RBR's .rpl playback animates the stored states | record new vanilla runs; don't probe a playback |
| results differ on another PC | rsqrtss/rcpss/fsin are vendor-specific in the original too | compare on the same CPU, or through the fixed tables |
| `Remove-Item` on a path is blocked | the sandbox protects `<RBR>` and system paths | never delete in `<RBR>`; ask the user |
