#include "visibility.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

namespace ngg {
namespace core {

namespace {

const char* GtaoName(int quality)
{
    static const char* const kNames[4] = { "XeGTAO low", "XeGTAO medium", "XeGTAO high", "XeGTAO ultra" };
    return kNames[quality < 0 ? 0 : quality > 3 ? 3 : quality];
}

// Gtao's AO into `out`, when it is wanted and can run.
void ScreenSpaceAo(Gtao& gtao, ID3D12GraphicsCommandList* list, const VisibilityFrame& f, Visibility& out)
{
    if (!f.ao || !gtao.Ready()) return;
    gtao.Record(list, f.depth, f.normal, f.view, f.proj, f.jitter, f.temporal, f.width, f.height);
    if (!gtao.Output()) return;
    out.ao = gtao.Output();
    out.aoView = Gtao::OutputSrvFormat();
    out.aoSource = GtaoName(gtao.Quality());
    out.aoMs = gtao.LastMs();
}

} // namespace

void RtVisibility::Record(Device& dev, ID3D12GraphicsCommandList* list, const VisibilityFrame& f, Visibility& out)
{
    float sunTowards[3];
    float len = sqrtf(f.sunTravel[0] * f.sunTravel[0] + f.sunTravel[1] * f.sunTravel[1] + f.sunTravel[2] * f.sunTravel[2]);
    if (len < 1e-6f) len = 1.0f;
    for (int k = 0; k < 3; ++k) sunTowards[k] = -f.sunTravel[k] / len;

    // JITTERED matrix: this reconstructs the surface the prepass wrote. The sun disc is the same one the sky draws, so
    // the shadow's softness and the visible sun agree by construction. AO only when it is applied (5.0m), and traced
    // only when it is the traced one.
    const bool traced = f.ao && m_aoSource == AoSource::Traced && m_rt.AoAvailable();
    m_rt.Trace(dev, list, f.depth, f.normal, f.motion, f.plant, f.viewProjJittered, f.viewProj, f.camPos, sunTowards,
               4000.0f, f.sunAngle, traced, 3.0f, f.aoRays, f.width, f.height, f.lights);
    m_rt.PrepareForRead(list);
    out.shadowSource = 1;
    out.sun = m_rt.Filtered();
    out.lights = m_rt.FilteredLights();
    out.lightsTaa = m_rt.LightsDenoised();
    if (traced && m_rt.FilteredAo())
    {
        out.ao = m_rt.FilteredAo();
        out.aoTaa = m_rt.AoDenoised();
        out.aoTaaPrev = m_rt.AoPrevDenoised();
        snprintf(m_aoName, sizeof m_aoName, "ray traced, %u ray%s", f.aoRays, f.aoRays == 1 ? "" : "s");
        out.aoSource = m_aoName;
        out.aoMs = m_rt.AoMs();
    }
    else if (m_aoSource == AoSource::ScreenSpace)
        ScreenSpaceAo(m_gtao, list, f, out);
}

void CascadeVisibility::Record(Device& dev, ID3D12GraphicsCommandList* list, const VisibilityFrame& f, Visibility& out)
{
    (void)dev;
    ScreenSpaceAo(m_gtao, list, f, out);
    m_scene.RenderShadows(list, f.view, f.proj, f.sunTravel);
    // The local lights' own shadow maps (Scene::RenderLocalShadows) - the ray tracer is not here to shadow them - and
    // the screen-space AO's share of their diffuse: at night the ambient XeGTAO darkens is next to nothing, so without
    // a share the lamps' light keeps no contact darkening at all (under the car on the Garage's turntable). A modest
    // share (kNightAoShare), faded in as the sun goes down - none while it is up, so the day is as it was.
    // NGG_LOCAL_SHADOWS=0: no maps (A/B); NGG_LOCAL_AO=<share>: another share at night.
    static const bool maps = [] { const char* e = getenv("NGG_LOCAL_SHADOWS"); return !e || e[0] != '0'; }();
    static const float kNightAoShare = [] { const char* e = getenv("NGG_LOCAL_AO"); return e ? (float)atof(e) : 0.5f; }();
    float len = sqrtf(f.sunTravel[0] * f.sunTravel[0] + f.sunTravel[1] * f.sunTravel[1] + f.sunTravel[2] * f.sunTravel[2]);
    if (len < 1e-6f) len = 1.0f;
    const float sunUp = -f.sunTravel[1] / len;   // sin of the sun's elevation
    const float night = sunUp >= 0.0f ? 0.0f : sunUp <= -0.1f ? 1.0f : -sunUp / 0.1f;
    const float share = f.ao ? kNightAoShare * night : 0.0f;
    if (maps) m_scene.RenderLocalShadows(list, share);
    else      m_scene.RenderLocalShadows(nullptr, share);
    out.shadowSource = 0;
}

} // namespace core
} // namespace ngg
