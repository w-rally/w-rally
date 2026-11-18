// ---------------------------------------------------------------------------
// VisibilityProvider - where the frame's sun visibility and ambient occlusion
// come from (one renderer, a pluggable source - not a second renderer).
//
// Both run between the prepass and the main pass, on the prepass's depth,
// normals and motion, and hand the main pass what it binds: the sun's
// visibility (or "sample the cascades yourself") and an AO term for the AO
// slot. What the main pass, TAA and the upscalers do with them does not depend
// on which provider made them.
//
//   RtVisibility       the ray tracer (rt.h): RT sun shadows, RT AO and the
//                      local lights' shadows, through its own denoiser - or,
//                      as the cheap mode, its shadows with Gtao's AO.
//   CascadeVisibility  the cascaded shadow maps (Scene::RenderShadows), which
//                      the main pass samples, and Gtao's AO.
//
//   NrdVisibility      (nrd_visibility.h) the ray tracer's traces, undenoised, through
//                      NVIDIA NRD's SIGMA and REBLUR instead of our denoiser.
// DLSS Ray Reconstruction would be a fourth: it denoises lit colour, not
// visibility, so it is not a drop-in provider.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <stdint.h>

#include "device.h"
#include "gtao.h"
#include "lights.h"
#include "rt.h"
#include "scene.h"

namespace ngg {
namespace core {

// What a provider sees of the frame. The prepass's targets are in their usual states (depth DEPTH_WRITE, the rest
// RENDER_TARGET) and must be left so.
struct VisibilityFrame
{
    ID3D12Resource* depth = nullptr;        // reverse-Z, jittered
    ID3D12Resource* normal = nullptr;       // world space, R8G8B8A8_SNORM
    ID3D12Resource* motion = nullptr;       // Scene::MotionBuffer
    ID3D12Resource* plant = nullptr;        // Scene::PlantBuffer
    const float* view = nullptr;            // the client's view and CONVENTIONAL projection
    const float* proj = nullptr;
    const float* viewProjJittered = nullptr;   // Scene::LastViewProjJittered / LastViewProj (reverse-Z)
    const float* viewProj = nullptr;
    const float* camPos = nullptr;
    float jitter[2] = {};                   // NDC, as the prepass drew
    float sunTravel[3] = {};                // SkyParams::sunDir: the direction the light travels
    float sunAngle = 0.0f;                  // the sun's angular radius, radians
    bool  ao = false;                       // ambient occlusion wanted
    uint32_t aoRays = 1;                    // the RT AO's rays per pixel
    bool  temporal = true;                  // a temporal filter (TAA, FSR, DLSS) follows: noise may walk
    UINT  width = 0, height = 0;            // the render size
    const Lights* lights = nullptr;
};

// What the main pass (and TAA) get.
struct Visibility
{
    int shadowSource = 0;                   // Scene::SetShadowSource: 0 the cascades, 1 `sun`
    ID3D12Resource* sun = nullptr;          // the sun's visibility (RGBA16F, R); null with the cascades
    ID3D12Resource* ao = nullptr;           // the ambient's visibility (R), PIXEL_SHADER_RESOURCE; null: none
    DXGI_FORMAT     aoView = DXGI_FORMAT_UNKNOWN;   // the view `ao` is read through (UNKNOWN: its own format)
    ID3D12Resource* aoTaa = nullptr;        // the RT AO's denoise, this frame and last, for TAA's moved-object test
    ID3D12Resource* aoTaaPrev = nullptr;
    ID3D12Resource* lightsTaa = nullptr;    // and the local lights' (Rt::LightsDenoised), for their shadows
    ID3D12Resource* lights = nullptr;       // the local lights' shadowed fraction; null: unshadowed
    const char*     aoSource = "off";       // for the readout
    double          aoMs = 0.0;             // its GPU time, last frame's
};

class VisibilityProvider
{
public:
    virtual ~VisibilityProvider() = default;
    virtual const char* Name() const = 0;
    virtual void Record(Device& dev, ID3D12GraphicsCommandList* list, const VisibilityFrame& f, Visibility& out) = 0;
    // A camera's cut: whatever history the source keeps.
    virtual void ResetHistory() = 0;
};

// Gtao's AO into `out`, when it is wanted and can run: the cascades' AO, and a ray-traced frame's cheap mode.
void ScreenSpaceAo(Gtao& gtao, ID3D12GraphicsCommandList* list, const VisibilityFrame& f, Visibility& out);

// Which AO a ray-traced frame takes.
enum class AoSource { Traced = 0, ScreenSpace = 1 };

class RtVisibility : public VisibilityProvider
{
public:
    RtVisibility(Rt& rt, Gtao& gtao) : m_rt(rt), m_gtao(gtao) {}
    const char* Name() const override { return "ray traced"; }
    void Record(Device& dev, ID3D12GraphicsCommandList* list, const VisibilityFrame& f, Visibility& out) override;
    void ResetHistory() override { m_rt.ResetHistory(); m_gtao.ResetHistory(); }
    void SetAoSource(AoSource s) { m_aoSource = s; }
    AoSource GetAoSource() const { return m_aoSource; }

private:
    Rt&      m_rt;
    Gtao&    m_gtao;
    AoSource m_aoSource = AoSource::Traced;
    char     m_aoName[32] = {};
};

class CascadeVisibility : public VisibilityProvider
{
public:
    CascadeVisibility(Scene& scene, Gtao& gtao) : m_scene(scene), m_gtao(gtao) {}
    const char* Name() const override { return "cascades"; }
    void Record(Device& dev, ID3D12GraphicsCommandList* list, const VisibilityFrame& f, Visibility& out) override;
    void ResetHistory() override { m_gtao.ResetHistory(); }

private:
    Scene& m_scene;
    Gtao&  m_gtao;
};

} // namespace core
} // namespace ngg
