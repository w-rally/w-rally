# W-Rally

**A rally simulator created by the community.**

A modular rally game for Windows, in C++20. The game executable is joined by swappable DLL modules: the renderer
(WRenderer, Direct3D 12), the physics (WPhysicsRBR, and others side by side later) and the audio (WAudio). Stages
use the `.ngg` format.

W-Rally runs on its own content, and when it is placed in (or pointed at) a Richard Burns Rally folder it also reads
RBR's stages, cars, sounds and replays in place. RBR's files are never part of this project and never shipped.

Status: early development. The design is in [docs/design.md](docs/design.md), the renderer and its `.ngg` scene format in
[docs/ngg-design.md](docs/ngg-design.md).

## Repositories

- **w-rally** (this one): the code - the game, the modules, the tools and the tests.
- **[content](https://github.com/w-rally/content)**: W-Rally's own content (stages, cars, audio, video), checked out
  here as the `content/` submodule.

```bash
git clone --recurse-submodules https://github.com/w-rally/w-rally.git
```

In a clone made without it: `git submodule update --init`.

## Building

Visual Studio 2022 (or newer) with the C++ workload, and CMake 3.20 or newer. x64 only.

```bash
cmake -G "Visual Studio 17 2022" -A x64 -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

Everything runs from `build/bin`. Some tests need an RBR folder (`WRALLY_TEST_RBR_DIR`, by default `<RBR>`) and
are left out without one.

## Licence

The code is under the [PolyForm Noncommercial License 1.0.0](LICENSE.md): use, change and share it freely for any
noncommercial purpose; commercial use is not allowed. The content repository is under CC BY-NC 4.0. Third-party
code in `third_party/` keeps its own licence.
