# NVIDIA NRD - fetched and built at configure, not in git

NVIDIA's Real-time Denoisers v4.18.0, <https://github.com/NVIDIA-RTX/NRD>, tag `v4.18.0`. Nothing of it is in this
repository: NRD is under the NVIDIA RTX SDKs licence (its `LICENSE.txt`), which lets its software ship only as part of
an application's object code, and NRD has no prebuilt release. So `third_party/CMakeLists.txt` downloads these
archives once, pinned by SHA-256, into `build/third_party/nrd/`, and builds NRD from them with FetchContent:

| archive | what | licence |
|---|---|---|
| `NVIDIA-RTX/NRD` tag `v4.18.0` | the denoisers: C++ library and HLSL compute shaders | NVIDIA RTX SDKs |
| `NVIDIA-RTX/ShaderMake` commit `3f01623` | NRD's build-time shader compiler driver | MIT |
| `NVIDIA-RTX/MathLib` tag `v11` | NRD's maths headers | MIT |
| `microsoft/DirectXShaderCompiler` `v1.9.2609` (`dxc_2026_09_29.zip`) | the `dxc.exe` ShaderMake runs (ours, `third_party/dxc`, has only the DLLs) | University of Illinois / MIT |

How it is built:

- a **static library** (`NRD_STATIC_LIBRARY`), linked into `WRenderer.dll` with our C runtime (/MT), every
  configuration; its shaders are compiled at build time and **embedded** in it, **DXIL only** (no SPIR-V, no DXBC);
- **no NRI** (`NRD_NRI` off): the renderer drives NRD's raw API (`nrd::GetComputeDispatches`) with its own device,
  root signatures, pipelines, descriptor heap and barriers (`modules/renderer/src/core/nrd_visibility.cpp`);
- normals encoded `R10G10B10A2` (NRD's default, `NRD_NORMAL_ENCODING` 2; the RGBA8 encodings do not compile in
  v4.18.0), roughness linear - the renderer's own pass writes them (`modules/renderer/shaders/nrd_pack.hlsl`, which
  spells out NRD's encodings itself rather than including NRD's HLSL).

Our build-time shader compile (`ngg-shaderc`) is unchanged: NRD's shaders are NRD's, compiled by its own ShaderMake.

`-DWRALLY_NRD=OFF`, or an archive that cannot be fetched (a warning, not a failed configure): no NRD in the build. The
game then runs with the renderer's own denoisers, and the Settings menu dims NVIDIA NRD. NRD is plain compute and runs
on any DX12 GPU that ray traces, not only NVIDIA's.

The licence's terms that bear on W-Rally: NRD's object code may ship inside the game (not on its own); its source and
headers may not be redistributed (hence nothing here); the game may not claim NVIDIA's sponsorship or endorsement. The
licence text is installed beside `WRenderer.dll` as `NVIDIA-NRD-LICENSE.txt`.
