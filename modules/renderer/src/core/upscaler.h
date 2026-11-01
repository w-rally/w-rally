// ---------------------------------------------------------------------------
// Upscaler - temporal upscaling behind one interface: the scene, the prepass,
// the ray tracing and its denoiser run at a RENDER size below the output, and
// the upscaler resolves the jittered frames into the output size, in place of
// PostFx's own TAA (PSTaa). The tonemap, the HUD and the UI stay at the output.
//
// FSR 3.1 is the first backend (fsr.cpp: AMD's signed ffx-api DLLs, any DX12
// GPU); DLSS Super Resolution (Streamline, RTX only) is meant to be the second,
// which is why this is an interface and not FSR's API. What a backend needs
// is what PostFx already has, converted once to the common convention below.
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

// The render size for an output size at a per-axis scale (1 native), even and
// at least 64 px.
void UpscaleRenderSize(UINT outputWidth, UINT outputHeight, float scale, UINT* outWidth, UINT* outHeight);

} // namespace core
} // namespace ngg
