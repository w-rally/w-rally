// ---------------------------------------------------------------------------
// Upscaler - temporal upscaling behind one interface: the scene, the prepass,
// the ray tracing and its denoiser run at a RENDER size below the output, and
// the upscaler resolves the jittered frames into the output size, in place of
// PostFx's own TAA (PSTaa). The tonemap, the HUD and the UI stay at the output.
//
// Two backends: FSR 3.1 (fsr.cpp: AMD's signed ffx-api DLLs, any DX12 GPU)
// and DLSS (dlss.cpp: NVIDIA's NGX, RTX GPUs only) - Super Resolution, or Ray
// Reconstruction, which denoises the lit colour too and so takes guides
// (UpscaleGuides, rr_visibility.h). What a
// backend needs is what PostFx already has, converted once to the common
// convention below; each backend writes down its own conversions.
//
// THE CONVENTION:
//   colour    linear HDR, before exposure and tonemap (PostFx's RGBA16F
//             target); the exposure goes with it, so the upscaler's own
//             tonemapped heuristics see what the screen will
//   depth     the scene's reverse-Z depth (R32, 1 near .. 0 far), infinite far
//   motion    RG16F, render size, (previous uv - current uv) of each pixel's
//             centre, unjittered: a camera reprojection for still pixels and
//             the prepass's motion for moved ones (PostFx's PSUpscaleIn)
//   reactive  R8, render size, how little of the history a pixel may keep
//             (moving glass: what is behind it moves on its own)
//   jitter    in render pixels, +x right, +y down - the offset the projection
//             was given (PostFx::Jitter turns it into NDC)
// Inputs are in PIXEL|NON_PIXEL_SHADER_RESOURCE, the output in
// UNORDERED_ACCESS, on entry and on return.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <stdint.h>

namespace ngg {
namespace core {

// DLSS Ray Reconstruction's guides (rr_guides.hlsl), render size, PIXEL|NON_PIXEL_SHADER_RESOURCE: linear diffuse
// and specular albedo (RGBA), the world normal with linear roughness in w, the bias-current-colour mask (R8: our
// moved-object mask and moving glass), and the camera - the client's view and CONVENTIONAL projection, row-vector
// row-major as ours are.
struct UpscaleGuides
{
    ID3D12Resource* diffuseAlbedo  = nullptr;
    ID3D12Resource* specularAlbedo = nullptr;
    ID3D12Resource* normalRoughness = nullptr;
    ID3D12Resource* bias           = nullptr;
    float view[16] = {};
    float proj[16] = {};
};

struct UpscaleDispatch
{
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* colour   = nullptr;
    ID3D12Resource* depth    = nullptr;
    ID3D12Resource* motion   = nullptr;
    ID3D12Resource* reactive = nullptr;   // may be null
    ID3D12Resource* exposure = nullptr;   // 1x1 R32F, may be null (exposure 1)
    ID3D12Resource* output   = nullptr;
    UINT  renderWidth = 0, renderHeight = 0;
    UINT  outputWidth = 0, outputHeight = 0;
    float jitterX = 0.0f, jitterY = 0.0f;
    float frameMs = 16.7f;
    float cameraNear = 0.1f, cameraFar = 10000.0f;
    float fovY = 1.0f;                    // radians
    bool  reset = false;                  // a cut: the history describes another image
    const UpscaleGuides* guides = nullptr;   // Ray Reconstruction's; null for the others
};

class Upscaler
{
public:
    virtual ~Upscaler() {}

    virtual const char* Name() const = 0;        // "FSR 3.1.x", for the readout
    // The output size and the largest render size it will be given; again on
    // every resize (the history goes with it).
    virtual bool Resize(ID3D12Device* device, UINT renderWidth, UINT renderHeight,
                        UINT outputWidth, UINT outputHeight) = 0;
    // The jitter sequence for this ratio: its length, and its offset at an
    // index (render pixels, see above).
    virtual int  JitterPhases(UINT renderWidth, UINT outputWidth) = 0;
    virtual void JitterOffset(int index, int phases, float* outX, float* outY) = 0;
    virtual bool Dispatch(const UpscaleDispatch& d) = 0;
};

// The backends. Null when this one cannot run here (its DLL missing, an
// unsupported adapter); logs why.
Upscaler* CreateFsrUpscaler(ID3D12Device* device);
// `rayReconstruction`: DLSS Ray Reconstruction instead of Super Resolution (its own NGX feature, nvngx_dlssd.dll).
Upscaler* CreateDlssUpscaler(ID3D12Device* device, bool rayReconstruction = false);

// The render size for an output size at a per-axis scale (1 native), even and
// at least 64 px.
void UpscaleRenderSize(UINT outputWidth, UINT outputHeight, float scale, UINT* outWidth, UINT* outHeight);

} // namespace core
} // namespace ngg
