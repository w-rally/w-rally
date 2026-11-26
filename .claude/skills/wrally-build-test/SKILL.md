---
name: wrally-build-test
description: Load before you configure, build, test or package W-Rally (cmake, ctest, golden replay, shaders, worktree build, MSI, CI, version bump). It gives the exact configure/build/ctest lines, the test list and its data variables, and the traps that break builds here.
---
# W-Rally: build, shaders, ctest, installer

W-Rally is one CMake project, x64 only, MSVC, C++20 (some targets C++17). Everything runs from `build/bin`.
For depth: `README.md` "Building", `CMakeLists.txt`, `tests/CMakeLists.txt` (each test has a comment saying what it
checks), `docs/ngg-design.md` §1.6 (shaders and pipelines), `docs/design.md` §4.2 (golden check), §4.3a (layers).

## Toolchain
- The generator is **`Visual Studio 18 2026`**, toolset v145 (MSVC 19.51). The comments in `README.md` and
  `CMakeLists.txt` still say "Visual Studio 17 2022". That is out of date: do not configure a tree with VS 17.
- **`cmake` and `ctest` are not on PATH** (in bash you get "command not found"; in PowerShell "not recognized").
  Use VS's own: `<cmake>` = `<VS>/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`, with
  `ctest.exe` beside it. `build/CMakeCache.txt` names them (`CMAKE_COMMAND`, `CMAKE_CTEST_COMMAND`).
- `vswhere.exe` is not on PATH either: `${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe`.
- For a bare `cl` (a probe .cpp in your scratchpad) run it inside `cmd /c "<VS>\VC\Auxiliary\Build\vcvars64.bat >nul && cl ..."`.

## Configure and build
```bash
CM="<cmake>"                       # quote it: the path has spaces
"$CM" -S <repo> -B <repo>/build -G "Visual Studio 18 2026" -A x64 \
      -DWRALLY_TEST_RBR_DIR=<RBR> \
      -DWRALLY_TEST_NGG=<phys-data>/m5/track-41.ngg \
      -DWRALLY_TEST_STAGE_NGG=<RBR>/Ngg/fr_cote_darbroz.ngg \
      -DWRALLY_TEST_GOLDEN=<phys-data>/m2/run-41-20261011-150633.phr
"$CM" --build <repo>/build --config Release -- /m          # or --parallel
"$CM" --build <repo>/build --config Release --target W-Rally WRenderer -- /m   # just the game and renderer
```
- Always pass **absolute** `-S/-B/--build` paths. The shell keeps its cwd, and `--build build` from a subfolder fails.
- Re-configure (`"$CM" <repo>/build`) after you add a source file or a test. Globs use `CONFIGURE_DEPENDS`, and
  build hashes are computed at configure time (`cmake/BuildHash.cmake`).
- Main targets: `W-Rally` (exe), `WRenderer` (bin/modules/renderer), `WPhysicsRBR` (bin/modules/physics),
  `WAudio` (bin/modules/audio), `ngg-shaderc`, `ngg-check`, `editor-api-check`, `module-info`, `approx-table`.
- Use Release. Debug builds work, but DLSS is left out of them: the NGX lib is /MT release
  (`modules/renderer/CMakeLists.txt`). Frames from different compilers differ at noise level, so compare renders
  only between builds from the same compiler.
- `PHYS_RSQRT` (`AMD` default | `INTEL` | `NATIVE`, `modules/physics-rbr/CMakeLists.txt`): keep `AMD`. The golden
  replay is bit-exact only with the fixed tables.

### Third-party downloads at configure (`third_party/CMakeLists.txt`)
| What | Where it goes | If the download fails |
|---|---|---|
| FidelityFX FSR loader and upscaler DLLs (SHA-256 pinned) | `build/third_party/fidelityfx` | warning, no FSR (TAA only) |
| DLSS SDK headers, lib and runtimes | `build/third_party/dlss` | warning, no DLSS |
| NRD + ShaderMake + MathLib + DXC zips (`WRALLY_NRD=ON`) | `build/third_party/nrd`, `_deps` | warning, no NRD |
| WNet.dll (private release, `cmake/WNet.cmake`) | `build/wnet` | warning, game offline; a failed **verify** stops configure |
A failed fetch never fails the configure. Read the configure log for `not fetched` before you blame the code.
The vendored DXC (`third_party/dxc/dxcompiler.dll`, `dxil.dll`) is in git.
NRD builds with warnings as errors. `/wd4834` is set for it (MSVC 19.52 `frexp` [[nodiscard]]).

## Worktrees
- Sessions work in `<repo>/.claude/worktrees/<name>` on `claude/<name>` branches.
- A new worktree has an **empty `content/`**. Run `git submodule update --init` first. Without it the game finds no
  content, and the installer and catalog tests fail.
- Each worktree needs its own `build/`: configure it with the same `-D` test paths (copy them from main's
  `build/CMakeCache.txt`: `grep -E "WRALLY_TEST_" build/CMakeCache.txt`). A full first build takes a few minutes.
- To try a worktree's shaders with main's exe, without rebuilding: `NGG_SHADER_DIR=<worktree>/modules/renderer/shaders`.
  That compiles them at run time and turns the pipeline cache off.

## Shaders (build time)
- `modules/renderer/shaders/*.hlsl|hlsli` are compiled by `ngg-shaderc` into
  `build/modules/renderer/generated/ngg_shaders.cpp`, which is linked into WRenderer.dll.
- Every shader permutation is a line of **`shaders/shaders.txt`**: `<file> <entry> <profile> [args]`. `*_5_x`
  profiles use FXC, `*_6_x` use DXC. **A new entry point or `-D` combination needs a new line.** Without it, the
  shader fails to load in a normal build and works only under NGG_SHADER_DIR (the log says "not listed").
- A change to any shader, to the list or to the tool recompiles all of them (a few seconds).
- To run it by hand, pass absolute paths: `build/bin/ngg-shaderc.exe --manifest <abs>/shaders.txt --dir <abs>/shaders --dxc <repo>/third_party/dxc --out <file>`.

## ctest
```bash
cd <repo>/build && "<ctest>" -C Release --output-on-failure            # everything (~10 min)
"<ctest>" -C Release -E "golden|net-check" --output-on-failure          # quick: no 3-5 min replay
"<ctest>" -C Release -R "wind-reference" -V                             # one test, its printed lines
```
The test list is in `ctest -N`, about 37 tests. A test whose data is missing is **left out at configure** (the
configure log says which variable to set). It is not reported as a failure.
| Needs | Tests |
|---|---|
| nothing | module-*, physics-rbr/audio/renderer-handshake, rcp-table, audio-offline-synthetic(-mix), car-parts, astronomy-check, wind-reference (GPU), ngg-roundtrip, physics-layer-*, mix-edit, yaml-subset, vfs-made-up |
| `WRALLY_TEST_RBR_DIR` | catalog-every-stage, legacy-import-41 / -clip-42 / -weather-4 / -protected, codriver-41, vfs-rbr (needs `physics.rbz`) |
| `WRALLY_TEST_NGG` (the **bare** stage 41, no model trees or grass) | instancing-41, grass-41, editor-api-41 (fixture of instancing-41), device-lost-41, ngg-producer-wallaby |
| `WRALLY_TEST_STAGE_NGG` (stage 41 as shipped, with trees and grass) | stage-41, device-lost-stage-41; golden draws it when set |
| RBR + NGG + `WRALLY_TEST_GOLDEN` | golden-replay-41, audio-offline-replay-41 |
| net module + `WRALLY_TEST_NET_SERVER` | net-handshake, net-check (usually left out) |

- **golden-replay-41** runs RBR's stage-41 recording through the game and checks that every pose is bit-exact
  (`replay check: N poses, all bit-exact`). It opens the game window and runs for minutes (timeout 600 s). It is
  the gate for any physics, compiler or float change, and it must stay bit-exact on every machine.
- The GPU tests (instancing, grass, editor-api, device-lost, stage-41, wind-reference) open hidden windows.
  **Do not run ctest at the same time as a game run or a measurement.** They fight for the GPU, time out, and
  spoil each other's numbers.
- CI (`.github/workflows/installer.yml`) runs ctest with no RBR folder, so only the "nothing" row runs there.

## Version, build info, installer
- **One version**: `project(wrally VERSION x.y.z)` in `CMakeLists.txt`. `cmake/BuildInfo.cmake` writes
  `build/generated/build_info.h` (`WRALLY_VERSION`, `WRALLY_COMMIT`, with `+` when the tree is dirty) on every build.
  The game, the MSI name and title, and the CI release all read it. A stale `WRALLY_VERSION` entry in the cache is unused.
- MSI: `installer/build-msi.ps1 -Build build -Config Release` produces `build/W-Rally-<version>-<commit>.msi`. It
  needs WiX 5 and its UI extension (`dotnet tool install --global wix --version 5.0.2`;
  `wix extension add -g WixToolset.UI.wixext/5.0.2`), a built `wrally-installer-ca` target and an initialised `content/`.
- CI builds on every push. Its artifact is the .msi itself. A tag `v<version>` creates a GitHub release, and the tag
  must equal the CMakeLists version.

## Pitfalls
| Symptom | Cause | Fix |
|---|---|---|
| `cmake: command not found` / `Výraz cmake nebyl rozpoznán` | not on PATH | full `<cmake>` path, or prepend its folder to PATH in the same command |
| `LNK1104: cannot open ...\bin\W-Rally.exe` (or a module DLL) | the game (or a test) is still running | `tasklist \| grep -i W-Rally`. Ask the user before you close their game. Then rebuild |
| `Error: <cwd>/build is not a directory` | relative `--build build` from a subfolder | absolute `--build <repo>/build` |
| `error C2001: newline in string literal` right after a scripted edit | Python or a heredoc turned `"\n"` in the replacement text into a real newline | use the Edit tool, or raw strings `r"..."` in Python |
| whole-file diffs, `^M` | Python `open(p,"w")` on Windows wrote CRLF | `open(p, "w", newline="")`; repair with `sed -i 's/\r$//' <file>` |
| `ngg-shaderc: error: no dxcompiler.dll in third_party/dxc` | relative `--dxc` from the wrong cwd | absolute path to `<repo>/third_party/dxc` |
| shader works with NGG_SHADER_DIR, fails in a normal build | permutation missing from `shaders.txt` | add the line exactly as the C++ passes the args |
| game starts with no stages or cars from a worktree | `content/` submodule not initialised | `git submodule update --init` |
| golden-replay-41 "left out" | a test variable is empty or the file is missing | set `-DWRALLY_TEST_*` and re-configure |
| golden-replay-41 not bit-exact, on one machine only | CPU-dependent float (rcpss/rsqrtss), FMA, compiler change | `PHYS_RSQRT=AMD`, check that `rcp-table` passes, never use hardware approx ops in physics; compare against a v145 build |
| instancing-41 "the file already has a model library", grass-41 "the stage already has NGGF" | `WRALLY_TEST_NGG` points at the shipped stage | point it at the bare `track-41.ngg`; the shipped one goes in `WRALLY_TEST_STAGE_NGG` |
| configure error "EXISTS AND EXISTS" | unquoted empty path in `if(EXISTS ...)` | quote the path: `if(EXISTS "${VAR}")` |
| FSR, DLSS or NRD missing at run time | its download failed at configure (it only warns) | re-run configure with network access; check `build/third_party/*` |
| `vfs-rbr` fails on another machine | that RBR folder has other cars with physics | known and machine-specific; compare against that machine's known set |
| `Blocked: sleep N` | the harness refuses long sleeps | `run_in_background`, or wait with an until-loop on a file |

## Rules
- Never commit local paths (RBR folder, data folders, user dirs, machine names). The repos are public.
  `git diff --cached | grep -inE 'C:\\|C:/|D:\\|Users'` before each commit.
- Do not run the game or ctest while the user is playing. Keep measuring runs short.
- Heavy builds, full ctest and GPU numbers can run on another machine; use a local hand-off skill if one is installed.
