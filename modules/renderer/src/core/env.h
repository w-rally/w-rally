// ---------------------------------------------------------------------------
// Environment - the sky, prefiltered by roughness, as one small texture.
//
// It exists to answer a question the renderer could not previously answer at
// all: what does this surface SEE? Until now only the sun had a specular lobe,
// so a polished surface and a rough one differed solely in the size of one
// highlight, and everything else in the frame reflected nothing. That reads as
// flat no matter how good the material data is - which matters right now,
// because PBR maps are about to arrive and would otherwise be judged on half
// their signal (design 9b items 14 and 18).
//
// TWO JOBS, ONE TEXTURE, and that is the point rather than a saving:
//
//   - it is the prefiltered environment the split-sum specular needs, and
//   - it is design 9b item 14's LUT, because sky radiance depends only on
//     direction and sun position, both uniform for the frame. Marching the
//     atmosphere per pixel to answer a question with 4,096 distinct answers was
//     always the wrong shape.
//
// OCTAHEDRAL, not a cube map. A 2D texture takes an ordinary mip chain and an
// ordinary bilinear sampler; a cube map needs per-face filtering and a seam
// policy to get the same thing. The cost is a small filtering error along the
// octahedron's own boundary, which for a sky - nearly a gradient - is not
// visible. That trade would be wrong for a sharp reflection probe and should be
// revisited when one exists.
//
// The mip chain IS the roughness axis: mip 0 is the mirror, each level a box
// downsample of the last, and roughness picks a level. A real GGX-weighted
// prefilter is more correct at the middle levels; successive downsampling is
// what most engines actually ship and is honest about being an approximation.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "sky.h"     // SkyParams - the SAME parameters the sky pass draws with
#include "atmosphere_ms.h"

namespace ngg {
namespace core {

class Environment
{
public:
    // 64x64 is deliberately small. The sky has no detail to lose at this
    // resolution, and the whole chain is under 40 KB - so it can be rebuilt
    // from scratch every frame and follow a moving sun with no staleness
    // policy, no invalidation, and nothing to get wrong.
    enum { kSize = 64, kMips = 7 };     // 64,32,16,8,4,2,1

    // The IRRADIANCE map (design 5.0i): for each normal, the cosine-weighted
    // integral of the sky over the upper hemisphere - what a Lambertian surface
    // with that normal receives from the sky. Octahedral like the radiance map.
    //
    // It replaces pi * L(N + up), a single sample that looks at the zenith for
    // an upward surface - and the zenith is the DARKEST part of a clear sky.
    // Measured against the true integral of the same model: 0.65x at a 50
    // degree sun, 0.43x at 10 degrees. A one-sample irradiance estimate is not
    // an approximation of the right answer; it is a different, smaller number.
    //
    // Sky only, upper hemisphere only: light from below is the ground, which the
    // scene shader adds analytically from the sun and this map's own zenith
    // value - a mirrored sky (what the radiance map shows below the horizon) is
    // somewhere for reflections to land, not a light source.
    //
    // 16x16 because irradiance is a very low-frequency function of the normal:
    // the cosine lobe is 180 degrees wide.
    enum { kIrrSize = 16 };

    bool Create(ID3D12Device* device);
    bool Ready() const { return m_psoSky != nullptr && m_tex != nullptr; }

    // Rebuilds the whole chain. Call once per frame BEFORE the scene pass, with
    // the same parameters the sky is drawn with - two sets would drift, which
    // is the failure design 5.0 already documents.
    //
    // `ms` is the multiple-scattering table AtmScatter reads (the Scene's).
    void Record(ID3D12GraphicsCommandList* list, const SkyParams& params,
                const AtmosphereMs* ms);

    // The chain, for the scene pass to sample. Left in PIXEL_SHADER_RESOURCE
    // when Record returns.
    ID3D12Resource* Texture() const { return m_tex.Get(); }

    // Writes the SRV for the whole chain into `dest`. The Scene owns the heap
    // the scene pass binds, so it asks for the descriptor rather than us
    // reaching into it - the same shape as ShadowMaps::CreateSrv, and for the
    // same reason: only one CBV/SRV/UAV heap can be bound at a time.
    void CreateSrv(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE dest) const;
    void CreateIrradianceSrv(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE dest) const;
    ID3D12Resource* Irradiance() const { return m_irr.Get(); }

    // Irradiance on an upward-facing surface, read back from the map one frame
    // late. Diagnostics only - it is what the units audit compares against the
    // numerical integral of the model (5.0i).
    bool IrradianceUp(float out[3]) const
    {
        if (!m_irrUpValid) return false;
        out[0] = m_irrUp[0]; out[1] = m_irrUp[1]; out[2] = m_irrUp[2];
        return true;
    }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoSky;    // mip 0, from the atmosphere
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoDown;   // mip n from mip n-1
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoIrr;    // irradiance from mip 0
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_irr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_irrRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_irrReadback;   // one texel, for IrradianceUp
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_irrKeepAlive;
    bool  m_irrPending = false;
    bool  m_irrUpValid = false;
    float m_irrUp[3] = {};
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_tex;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;   // one per mip
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;   // one per SOURCE mip
    // The per-mip constant buffers must outlive GPU execution. The frame is
    // synchronous (see the note at the end of Scene::Record), so holding them
    // until the next Record is enough - and this is exactly the thing that
    // breaks the moment frames overlap, so it is called out rather than hidden.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_keepAlive[kMips];
    UINT m_rtvStride = 0;
    UINT m_srvStride = 0;
};

} // namespace core
} // namespace ngg
