// ---------------------------------------------------------------------------
// RrVisibility - the frame's visibility when DLSS Ray Reconstruction resolves
// it (UpscalerKind::DlssRr). RR denoises the LIT colour together with the
// upscale, so the main pass must light with the ray tracer's RAW visibility:
// the traces run without any denoiser (Rt::SetRawOnly) and CSDecode
// (rr_guides.hlsl) hands the main pass their plain 0/1 - the same three slots,
// the same shader. After the main pass RecordGuides turns its surface targets
// (Scene::SetGbufferWanted) into RR's guides (UpscaleGuides).
//
// Not a denoiser of its own and not a provider the settings pick: the renderer
// uses it on the RT path whenever the frame is resolved by RR, in place of
// RtVisibility / NrdVisibility. NGG design 4.11, rr-design.md.
// ---------------------------------------------------------------------------
#pragma once

#include "upscaler.h"
#include "visibility.h"

#include <wrl/client.h>

namespace ngg {
namespace core {

class RrVisibility : public VisibilityProvider
{
public:
    RrVisibility(Rt& rt, Gtao& gtao) : m_rt(rt), m_gtao(gtao) {}
    const char* Name() const override { return "ray traced, raw for RR"; }
    // Its pipelines (rr_guides.hlsl) and heap. False (logged) if they cannot be made.
    bool Create(Device& dev);
    bool Ready() const { return m_guides != nullptr; }
    void Record(Device& dev, ID3D12GraphicsCommandList* list, const VisibilityFrame& f, Visibility& out) override;
    void ResetHistory() override { m_gtao.ResetHistory(); }
    void SetAoSource(AoSource s) { m_aoSource = s; }
    // After the main pass, before the resolve: RR's guides from the scene's surface targets (in RENDER_TARGET, left
    // so), the depth (DEPTH_WRITE, left so) and the moving glass's motion (RENDER_TARGET, may be null); with this
    // frame's moved mask when Record ran (`traced`), else none. The guides end in PIXEL|NON_PIXEL_SHADER_RESOURCE.
    bool RecordGuides(Device& dev, ID3D12GraphicsCommandList* list, ID3D12Resource* gbufNormal,
                      ID3D12Resource* gbufAlbedo, ID3D12Resource* depth, ID3D12Resource* glassMotion,
                      const float* viewProjJittered, const float* camPos, const float* view, const float* proj,
                      UINT width, UINT height, bool traced, UpscaleGuides& out);

private:
    struct Tex
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> r;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    };
    bool Resize(ID3D12Device* device, UINT width, UINT height);

    Rt&      m_rt;
    Gtao&    m_gtao;
    AoSource m_aoSource = AoSource::Traced;
    char     m_aoName[48] = {};
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_decode, m_guides;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT m_stride = 0;
    UINT m_width = 0, m_height = 0;
    // Decode's: the three visibilities and the moved mask; the guides.
    Tex m_sun, m_ao, m_lights, m_moved;
    Tex m_diffuse, m_specular, m_normal, m_bias;
};

} // namespace core
} // namespace ngg
