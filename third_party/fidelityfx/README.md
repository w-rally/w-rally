# AMD FidelityFX / FSR SDK - the ffx-api headers

From AMD's FSR SDK v2.4.0 ("Redstone"), <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>, tag `v2.4.0`,
unchanged, in the SDK's layout under `Kits/FidelityFX/`:

- `api/include/ffx_api.h`, `ffx_api_types.h`, `ffx_api_loader.h`, `dx12/ffx_api_dx12.h`
- `upscalers/include/ffx_upscale.h`

`LICENSE.md` is the SDK's `Kits/FidelityFX/docs/license.md`; it lists these headers and the signed DLLs under the MIT
licence. The DLLs (`signedbin/amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`, AMD-signed) are
fetched by `third_party/CMakeLists.txt` into the build tree and copied beside `WRenderer.dll`; they are not in git.
