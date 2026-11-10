// ---------------------------------------------------------------------------
// AtmosphereMs - multiple scattering for the atmosphere, as a 32x32 LUT
// (design 9b item 25, 5.0j).
//
// The model was single-scattering only: light that scatters once into the eye.
// Real sky light scatters many times, and leaving that out makes the sky about
// 2x too dim against measured clear-sky diffuse fractions - which is the deficit
// the lighting dials were compensating for (5.0i). Measured on our own model:
// 15:1 sun:sky on a horizontal surface at a 50 degree sun, against ~6-9:1 real.
//
// Hillaire 2020 ("A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique"), section 5.5: assume the light arriving at a point after the
// second bounce is ISOTROPIC, compute second-order scattering plus the fraction
// f_ms of isotropic light the surroundings scatter back, and sum the geometric
// series of all higher orders as 1/(1 - f_ms). The result Psi_ms depends only
// on where the point is in the atmosphere (altitude) and where the sun is
// relative to its local up (mu_s) - a 2D table.
//
// IT DOES NOT DEPEND ON THE CURRENT SUN. mu_s is a lookup coordinate, not an
// input, so the table changes only when the atmosphere's COMPOSITION does
// (turbidity, ground albedo) - i.e. almost never. That is what removes any
// frame-ordering problem between the sky pass, which runs first, and the
// Scene's environment and aerial passes: whoever owns an instance updates it
// when dirty, and two owners with the same inputs hold the same table.
//
// Consumed inside the ONE AtmScatter (shaders/atmosphere.hlsli), as gAtmMs at
// register t0 space1, so the sky, the environment map, the irradiance map and
// aerial perspective all gain it together. Every root signature that compiles
// AtmScatter binds it with MsSrvRange() + MsSampler().
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

namespace ngg {
namespace core {

class AtmosphereMs
{
public:
    enum { kSize = 32 };     // mu_s across, altitude down - Hillaire's own size

    bool Create(ID3D12Device* device);
    bool Ready() const { return m_pso != nullptr && m_lut != nullptr; }

    // Recomputes the table if the composition or the switch changed since the
    // last call; otherwise records nothing. Sets its own compute root signature
    // and descriptor heap - the caller re-binds its own afterwards. The table
    // is left in PIXEL | NON_PIXEL shader resource (pixel shaders and the
    // aerial compute pass both read it).
    //
    // enabled = false fills the table with zeros, which turns AtmScatter back
    // into the single-scattering model exactly: the A/B costs no shader branch.
    void Update(ID3D12GraphicsCommandList* list, float turbidity, float groundAlbedo,
                bool enabled);

    ID3D12Resource* Texture() const { return m_lut.Get(); }
    void CreateSrv(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE dest) const;

    // Root-signature pieces for t0 space1 / s0 space1. Identical in every
    // consumer, so written once.
    static D3D12_DESCRIPTOR_RANGE    MsSrvRange();
    static D3D12_STATIC_SAMPLER_DESC MsSampler(D3D12_SHADER_VISIBILITY visibility);

    // Diagnostics: one texel of the table, read back one frame after it was
    // computed. Used to check the GPU table against a numerical reference.
    bool ProbeTexel(float out[3]) const
    {
        if (!m_probeValid) return false;
        out[0] = m_probe[0]; out[1] = m_probe[1]; out[2] = m_probe[2];
        return true;
    }
    enum { kProbeX = 28, kProbeY = 0 };   // mu_s 0.781 (a ~51 degree sun), 937.5 m

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pso;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_lut;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_uavHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_readback;

    bool  m_valid = false;           // table computed at least once
    float m_turbidity = -1.0f;
    float m_groundAlbedo = -1.0f;
    bool  m_enabled = false;
    bool  m_probePending = false;
    bool  m_probeValid = false;
    float m_probe[3] = {};
};

} // namespace core
} // namespace ngg
