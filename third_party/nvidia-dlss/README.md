# NVIDIA DLSS SDK - fetched at configure, not in git

From NVIDIA's DLSS SDK v310.9.16, <https://github.com/NVIDIA/DLSS>, tag `v310.9.16`. Nothing of it is in this
repository: the SDK is under the NVIDIA RTX SDKs licence (its `LICENSE.txt`), whose headers are NVIDIA-proprietary and
may be distributed only as part of an application's object code, so `third_party/CMakeLists.txt` fetches each file,
pinned by SHA-256, into `build/third_party/dlss/` in the SDK's own layout:

- `include/nvsdk_ngx.h`, `nvsdk_ngx_defs.h`, `nvsdk_ngx_params.h`, `nvsdk_ngx_helpers.h`, `nvsdk_ngx_helpers_d3d.h`,
  `nvsdk_ngx_helpers_cuda.h` - the NGX API the renderer's DLSS upscaler (`modules/renderer/src/core/dlss.cpp`) is
  written against
- `lib/Windows_x86_64/x64/nvsdk_ngx_s.lib` - NGX's loader, linked into the renderer (built for the static release C
  runtime, /MT, so Release builds only); it finds the driver's NGX at run time
- `lib/Windows_x86_64/rel/nvngx_dlss.dll` - the DLSS Super Resolution runtime, NVIDIA-signed, copied beside
  `WRenderer.dll` and installed there; NGX loads it at run time
- `LICENSE.txt` - installed beside it as `NVIDIA-DLSS-LICENSE.txt`

The licence's terms that bear on W-Rally: the DLSS runtime may ship inside the game (not on its own); DLSS is for
NVIDIA GPUs only (the renderer offers it only where NGX says it is available); the use of DLSS is attributed with
NVIDIA's marks in the game's credits (the main menu's credits line); NVIDIA asks to be told before a commercial
release.
