// ---------------------------------------------------------------------------
// Physical sky - single-scattering Rayleigh + Mie, evaluated per pixel.
//
// This replaces RBR's skybox TEXTURE rather than converting it (design 9b item
// 6, now decided): a painted skybox cannot respond to a sun that moves, and
// dynamic time of day is the whole point of the overhaul. It also gives the
// ambient term somewhere real to come from, which matters because "directional
// light plus a hardcoded ambient constant" is a placeholder that never gets
// removed.
//
// WHAT IS AND IS NOT TAKEN FROM THE GAME (design 3.4a):
// RBR's TrackSettings.ini carries `SunDir` per track and variant, and that is
// used directly - it is unambiguous and it is exactly what a photometric solve
// would have had to recover. Its `Turbidity`, `Rayleigh_Multiplier`,
// `Mie_Multiplier` and `Greenstein_Value` are NOT used as physical quantities:
// Joux Verte ships Turbidity 0.0 and Greenstein 1.0, which are degenerate for
// any real scattering model, so they are internal scales for RBR's own ps_1_1
// constants and feeding them to this model would produce garbage. Physical
// constants are used instead, with turbidity as our own haze control.
//
// Like everything in ngg-core, this knows nothing about RBR - it takes numbers.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "atmosphere_ms.h"

namespace ngg {
namespace core {

struct SkyParams
{
    // Direction the light TRAVELS, i.e. from the sun towards the ground. This
    // is RBR's own convention for SunDir, kept so the value can be copied from
    // TrackSettings.ini without a sign flip going missing somewhere.
    float sunDir[3]     = { -0.843359f, -0.503774f, 0.186968f };   // Joux Verte morning
    float sunIntensity  = 22.0f;

    float turbidity     = 1.0f;    // haze: scales Mie. OUR control, not RBR's.
    float groundAlbedo  = 0.1f;    // TrackSettings Terrain_Reflectance is ~0.084

    // Colour of light bounced off the ground, from TrackSettings
    // Terrain_Reflectance_R/G/B. This is what stops a shaded wall being navy
    // blue: with sky ambient alone, a surface facing away from the sun receives
    // pure Rayleigh scatter and nothing else. Real shadow is dominated by
    // ground bounce, which is why it reads desaturated rather than blue.
    float groundColour[3] = { 0.49f, 0.49f, 0.50f };   // Joux Verte's values
    float groundBounce    = 1.0f;   // multiplier on the physical 1/PI bounce
    float exposure      = 0.8f;   // consumed by PostFx, not by the sky pass
    float sunDiscSize   = 1.0f;    // multiples of the real angular diameter

    // Surface lighting scales. Separate from sunIntensity because the sky and
    // the surfaces are not in the same units yet - there is no HDR pipeline and
    // no measured albedo, so these are the dial that puts them in the same
    // range. They disappear when exposure becomes physical.
    float sunDiffuse    = 0.18f;
    // 1.0 now that the sky/ground split is energy-consistent. It was 1.6 while
    // the sky term was a factor of PI too small - a fudge compensating for a
    // units error, which is exactly how a wrong model gets locked in.
    float skyAmbient    = 1.0f;

    // WEATHER (weather.h, design 5.0q), in physical units. Defaults are a
    // clear, fog-free day - exactly the atmosphere before these existed.
    float cloudCover    = 0.0f;    // 0 clear .. 1 overcast
    float fogVisibility = 0.0f;    // metres (2 % contrast); 0 = no fog layer
    float fogHeight     = 150.0f;  // fog layer scale height, metres

    // NIGHT (design 5.4, N1b), set by ApplySkyTime from a date, a time and a place; neutral otherwise - no moon is
    // drawn, no stars, no glow, and sunDir/sunIntensity are the sun's.
    float lightIsMoon   = 0.0f;    // 1: sunDir/sunIntensity are the MOON's (the night's light): no sun disc drawn
    float trueSunTowards[3] = { 0.0f, 1.0f, 0.0f };   // the real sun, towards it: it lights the moon's disc
    float moonTowards[3]    = { 0.0f, -1.0f, 0.0f };  // towards the moon
    float moonRadiance  = 0.0f;    // its disc's radiance where the sun lights it, model units; 0: no moon drawn
    float starIrradiance = 0.0f;   // a magnitude-0 star's irradiance, model units; 0: no stars
    float worldToEquatorial[9] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };   // the stars' frame
    float nightGlow     = 0.0f;    // airglow + starlight: the moonless sky's own radiance, model units
    // The OTHER body's sky: the moon's while the sun is the light, the sun's twilight while the moon is. The sky
    // and the environment add it to the light's own; the direct light stays the one body's. 0: none.
    float light2Towards[3] = { 0.0f, -1.0f, 0.0f };
    float light2Intensity  = 0.0f;
};

// Ground-level fog extinction from visibility (Koschmieder: 3.912 / V), per metre.
inline float FogBeta(const SkyParams& p)
{
    return p.fogVisibility > 1.0f ? 3.912f / p.fogVisibility : 0.0f;
}

class Sky
{
public:
    bool Create(ID3D12Device* device, DXGI_FORMAT rtvFormat);

    // Draws the sky over the whole target with NO depth test or write, so it is
    // recorded BEFORE opaque geometry and simply replaces the colour clear.
    // Cheap enough at this resolution; a depth-tested version that only shades
    // untouched pixels is an optimisation for later, not a correctness matter.
    //
    // view/proj follow ngg-core's camera contract (see scene.h). Pass the
    // ORIGINAL projection - only its two tangent terms are read, and those are
    // untouched by the reverse-Z flip Scene applies.
    void Record(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                const float* view, const float* proj, UINT width, UINT height,
                const SkyParams& params);

    bool Ready() const { return m_pso != nullptr; }

    // TAA jitter in NDC, from PostFx::Jitter. Must match what Scene was given.
    void SetJitter(float ndcX, float ndcY) { m_jitterX = ndcX; m_jitterY = ndcY; }

    // Multiple scattering (atmosphere_ms.h). The sky keeps its OWN table rather
    // than borrowing the Scene's: it is drawn first, and the table depends only
    // on the atmosphere's composition, so two owners with the same SkyParams
    // hold the same numbers with no ordering between them. Must match
    // Scene::SetMultipleScattering, or the sky and the lighting disagree.
    void SetMultipleScattering(bool on) { m_msOn = on; }

private:
    float m_jitterX = 0.0f, m_jitterY = 0.0f;

    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pso;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_frameCB;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;   // the MS table
    AtmosphereMs m_ms;
    bool         m_msOn = true;
};

} // namespace core
} // namespace ngg
