// ---------------------------------------------------------------------------
// NrdVisibility - the third visibility source (visibility.h): the ray
// tracer's own traces, denoised by NVIDIA's NRD instead of ours.
//
//   sun           Rt's shadow ray -> SIGMA_SHADOW (the penumbra from the hit
//                 distance and the sun's angular radius)
//   local lights  Rt's one ray to one lamp -> a second SIGMA_SHADOW (the
//                 penumbra of the lamp each pixel picked)
//   AO            our denoiser, as RtVisibility's. With NGG_NRD_AO=reblur
//                 Rt's AO rays -> REBLUR_DIFFUSE_OCCLUSION instead - which
//                 kept the car's occlusion behind a driving car (the ghost
//                 tools/car-ghost measures: +20 grey where the limit is 1),
//                 its own history rules being NRD's, not ours
//
// It fills the same Visibility as RtVisibility - the same formats and
// layouts, the moved-object distrust TAA reads included - so the main pass,
// TAA and the upscalers do not know which one ran. NRD is plain compute, any
// DX12 GPU; it is driven through its raw API with our device, our heaps and
// our barriers (no NRI). Built only when the SDK was fetched
// (third_party/nrd/README.md); without it Create says so and the renderer
// keeps its own denoisers.
// ---------------------------------------------------------------------------
#pragma once

#include <memory>

#include "visibility.h"

namespace ngg {
namespace core {

class NrdVisibility : public VisibilityProvider
{
public:
    NrdVisibility(Rt& rt, Gtao& gtao);
    ~NrdVisibility() override;
    const char* Name() const override { return "NRD"; }
    // NRD's instance, its pipelines, ours (nrd_pack.hlsl) and the ray tracer's NRD variants. False (logged) in a
    // build without NRD, or where any of it cannot be made. Call once, before the first Record.
    bool Create(Device& dev);
    bool Ready() const;
    // Whether this build has NRD at all (its SDK fetched: WRALLY_NRD).
    static bool InBuild();
    void Record(Device& dev, ID3D12GraphicsCommandList* list, const VisibilityFrame& f, Visibility& out) override;
    void ResetHistory() override;
    void SetAoSource(AoSource s) { m_aoSource = s; }
    // The GPU time of NRD's dispatches and our two passes around them, last frame's: ReadStats after the GPU wait.
    void ReadStats();
    double DenoiseMs() const { return m_denoiseMs; }

private:
    struct Impl;
    std::unique_ptr<Impl> m;
    Rt&      m_rt;
    Gtao&    m_gtao;
    AoSource m_aoSource = AoSource::Traced;
    char     m_aoName[48] = {};
    double   m_denoiseMs = 0.0;
};

} // namespace core
} // namespace ngg
