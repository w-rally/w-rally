// ---------------------------------------------------------------------------
// AerialLut - aerial perspective, integrated once per FROXEL instead of once
// per pixel (design 9b item 21, 5.0f).
//
// The scene pass used to march the atmosphere for every geometry pixel: 8 view
// steps x 3 light steps, on a million-plus pixels, to answer a question that
// varies smoothly in exactly two ways - the direction of the pixel and how far
// away its surface is. That is a 3D function, so it is tabulated as one: screen
// x, screen y, and distance, filled by one compute dispatch per frame and read
// with a single trilinear fetch per pixel.
//
// The environment map (env.h) could NOT answer this. It tabulates radiance by
// direction alone, i.e. the whole ray to the top of the atmosphere; aerial
// perspective is the camera-to-SURFACE segment, which depends on distance too.
// Hence the third axis.
//
// SAME INTEGRAL, not an approximation of it: each froxel calls AtmScatter, the
// one implementation the sky and the environment map use (atmosphere_hlsl.h).
// What changes is only where it is evaluated, not how: the same 8x3
// quadrature the per-pixel march used, so the march survives as an exact
// reference and any difference between the two is the table's own error.
//
// Distance is RADIAL (camera to surface), not view depth, because that is what
// the integral is over and what the per-pixel version measured; slices are
// spaced quadratically (w = sqrt(d / maxDist)) so near distances - where the
// eye judges contrast against the road - get most of the resolution.
//
// Camera-fixed in the same sense the per-pixel version was: the model places
// the camera at a constant altitude on a planet-sized sphere (kCamHeight), so
// the table depends on the camera's ORIENTATION and the sun, never on where on
// the stage the car is. That is why it can be rebuilt from scratch every frame
// with no history and no reprojection.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "atmosphere_ms.h"

namespace ngg {
namespace core {

class AerialLut
{
public:
    // 64x64 screen cells: a cell spans ~30 px at 1080p and ~1 degree of view,
    // against an in-scatter that varies over tens of degrees (the Mie lobe is
    // the sharpest feature, and trilinear filtering spans it). 32 slices on a
    // quadratic distribution. 2 x 1 MB.
    enum { kWidth = 64, kHeight = 64, kSlices = 32 };

    bool Create(ID3D12Device* device);
    bool Ready() const { return m_pso != nullptr && m_inscatter != nullptr; }

    // Fills both volumes. `viewProj` is the UNJITTERED, reverse-Z
    // view-projection (Scene::LastViewProj): a froxel spans tens of pixels, so
    // the sub-pixel jitter is irrelevant to it, and following the jitter would
    // only make the table shimmer for nothing. `sunTowards` points AT the sun.
    // `maxDist` is the radial distance the last slice reaches; anything further
    // reads the last slice.
    //
    // Sets its own compute root signature and descriptor heap; the caller must
    // re-bind its heap afterwards (Scene does, it always sets its own).
    // Both textures are left in PIXEL_SHADER_RESOURCE.
    void Record(ID3D12GraphicsCommandList* list, const float* viewProj,
                const float* sunTowards, float sunIntensity, float turbidity,
                float maxDist, const AtmosphereMs* ms,
                const float weather[4] = nullptr);   // cloud cover, fog beta, fog height, ground albedo

    // SRVs for in-scatter then transmittance, at `dest` and `dest + stride`.
    // The consumer owns the heap it binds - only one CBV/SRV/UAV heap can be
    // bound at a time - so it asks for descriptors rather than us reaching in.
    void CreateSrvs(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE dest,
                    UINT stride) const;

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pso;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_inscatter;      // rgb, linear HDR
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_transmittance;  // rgb, 0..1
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_uavHeap;        // u0, u1, then the MS table SRV
    UINT                                         m_stride = 0;
    // Must outlive GPU execution; the frame is synchronous (see the note at the
    // end of Scene::Record), so holding it until the next Record is enough.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_keepAlive;
};

} // namespace core
} // namespace ngg
