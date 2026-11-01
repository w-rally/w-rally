#include "renderer.h"
#include "log.h"

#include <math.h>

namespace ngg {
namespace core {

const QualityTier& Renderer::Tier(Quality q)
{
    // Costs on the development machine's integrated Radeon, lab forest view
    // (965k geometry pixels) - see design 5.0p for the measurement.
    static const QualityTier kTiers[3] = {
        { "low",    false, 0, 0.25f },   // RT sun shadows only - the VR baseline
        { "medium", true,  1, 0.50f },   // + RT AO, one ray (the default)
        { "high",   true,  2, 0.50f },   // + RT AO, two rays: less noise at rest
    };
    const int i = (int)q;
    return kTiers[(i < 0 || i > 2) ? 1 : i];
}

void Renderer::SetQuality(Quality q)
{
    const QualityTier& t = Tier(q);
    m_quality = q;
    SetAmbientOcclusion(t.ao);
    if (t.aoRays) SetAoRays(t.aoRays);
    m_post.SetSharpen(t.sharpen);
    Log("renderer: quality %s (AO %s, %u ray(s), sharpen %.2f)",
        t.name, t.ao ? "on" : "off", t.ao ? t.aoRays : 0, t.sharpen);
}

bool Renderer::Create(Device& dev, DXGI_FORMAT outputFormat, UINT width, UINT height,
                      bool rayTracing)
{
    m_outputFormat = outputFormat;
    m_width = width;
    m_height = height;
    m_renderWidth = width;
    m_renderHeight = height;

    ID3D12Device* d = dev.device.Get();
    if (!m_sky.Create(d, PostFx::kHdrFormat)) Log("renderer: sky unavailable - frames clear instead");
    if (!m_shadows.Create(d, 2048)) Log("renderer: cascaded shadow maps unavailable");
    m_scene.SetShadowMaps(&m_shadows);

    // Built here rather than at load: overlays depend on the OUTPUT format,
    // not on the HDR intermediate, and must survive a track change.
    m_scene.CreateSelectionPipeline(d, outputFormat);

    if (!m_post.Create(d, outputFormat)) { Log("renderer: post-processing failed"); return false; }
    m_post.OnResize(d, width, height);

    if (rayTracing && m_rt.Create(dev))  // logs and declines on an adapter without DXR 1.1
    {
        if (!m_buildDev.CreateSecondary(dev))
            Log("renderer: no secondary command context - the BVH cannot be rebuilt mid-frame");
    }
    return true;
}

void Renderer::OnResize(Device& dev, UINT width, UINT height)
{
    m_width = width;
    m_height = height;
    m_renderWidth = width;
    m_renderHeight = height;
    if (m_upKind != UpscalerKind::None)
        UpscaleRenderSize(width, height, m_renderScale, &m_renderWidth, &m_renderHeight);
    m_scene.OnResize(dev.device.Get(), m_renderWidth, m_renderHeight);
    m_scene.SetMipBias(m_renderWidth < width ? log2f((float)m_renderWidth / (float)width) : 0.0f);
    m_post.OnResize(dev.device.Get(), m_renderWidth, m_renderHeight, width, height);
    // (The ray tracer's targets follow the size it is traced at: Rt::Trace.)
}

void Renderer::SetUpscaling(Device& dev, UpscalerKind kind, float scale)
{
    if (!(scale > 0.0f) || scale > 1.0f) scale = 1.0f;
    if (kind == m_upKind && (kind == UpscalerKind::None || scale == m_renderScale)) return;
    if (kind != UpscalerKind::None && (kind != m_upKind || !m_upscaler))
    {
        // Another backend: the old one goes first (PostFx lets go of it), then the new one is made.
        m_post.SetUpscaler(nullptr);
        m_upscaler.reset(kind == UpscalerKind::Dlss ? CreateDlssUpscaler(dev.device.Get())
                                                    : CreateFsrUpscaler(dev.device.Get()));
        if (!m_upscaler) kind = UpscalerKind::None;
    }
    if (kind == UpscalerKind::None) scale = 1.0f;
    if (kind == m_upKind && scale == m_renderScale) return;
    m_upKind = kind;
    m_renderScale = scale;
    m_post.SetUpscaler(kind == UpscalerKind::None ? nullptr : m_upscaler.get());
    if (kind == UpscalerKind::None) m_upscaler.reset();
    Log("renderer: upscaling %s, render scale %.2f",
        kind == UpscalerKind::None ? "off (TAA)" : kind == UpscalerKind::Dlss ? "DLSS" : "FSR", scale);
    if (m_width && m_height) OnResize(dev, m_width, m_height);
}

bool Renderer::Load(Device& dev, const wchar_t* path)
{
    const bool ok = m_scene.Load(dev.device.Get(), dev.queue.Get(), path,
                                 PostFx::kHdrFormat, m_renderWidth, m_renderHeight);
    m_rtRevision = 0xFFFFFFFFu;          // rebuilt on the next frame
    m_post.ResetHistory();
    return ok;
}

bool Renderer::LoadFromMemory(Device& dev, const void* data, size_t bytes)
{
    const bool ok = m_scene.LoadFromMemory(dev.device.Get(), dev.queue.Get(), data, bytes,
                                           PostFx::kHdrFormat, m_renderWidth, m_renderHeight);
    m_rtRevision = 0xFFFFFFFFu;
    m_post.ResetHistory();
    return ok;
}

// The BVH follows the scene, per MESH (design 5.0o). When only instances
// changed - a drag, an instance of a mesh it already has - that is the TLAS
// alone; when the mesh set changed (a load, a first move, a hide, an added
// mesh) the missing BLASes are built and the stale ones dropped. Both on the
// SECONDARY context: this runs inside RecordFrame, with the client's list open.
void Renderer::KeepRtCurrent(Device& dev)
{
    (void)dev;
    if (!m_rt.Ready() || !m_scene.Ready() || !m_buildDev.list)
    {
        m_rtValid = false;
        m_rtReason = "no ray tracing on this adapter";
        return;
    }
    const uint32_t rev = m_scene.GeometryRevision();
    if (rev == m_rtRevision) return;
    m_rtRevision = rev;

    m_scene.FlushDynamic(m_buildDev.device.Get());     // a new mesh's vertices, before its BLAS
    std::vector<uint64_t> keys;
    std::vector<Scene::RtInstanceDesc> instances;
    m_scene.DescribeRt(keys, instances);

    bool sameSet = m_rt.HasScene() && keys.size() == m_rt.MeshCount();
    size_t missing = 0, moved = 0;
    for (uint64_t k : keys) if (!m_rt.HasMesh(k)) ++missing;
    for (const Scene::RtInstanceDesc& d : instances) moved += d.moved ? 1 : 0;
    sameSet = sameSet && missing == 0;
    Log("renderer: scene revision %u - %zu mesh key(s) (%zu not built, world %016llx), %zu instance(s), %zu moved -> %s",
        rev, keys.size(), missing, keys.empty() ? 0ull : (unsigned long long)keys[0], instances.size(), moved,
        sameSet ? "instances only" : "rebuild");

    if (sameSet)
        m_rtValid = m_rt.UpdateInstances(m_buildDev, instances);
    else
    {
        std::vector<Scene::RtMeshData> fresh;
        for (uint64_t k : keys)
        {
            if (m_rt.HasMesh(k)) continue;
            Scene::RtMeshData m;
            if (m_scene.BuildRtMesh(k, m)) fresh.push_back(std::move(m));
        }
        Scene::RtShared shared;
        m_rtValid = m_scene.BuildRtShared(m_buildDev.device.Get(), shared) &&
                    m_rt.Rebuild(m_buildDev, shared, fresh, keys, instances);
    }
    m_rtReason = m_rtValid ? nullptr : "the acceleration structure failed to build";
}

const char* Renderer::RtFallbackReason() const
{
    if (m_rtActive)              return nullptr;
    if (!m_preferRt)             return "cascades selected";
    if (!m_rt.Ready())           return "no ray tracing on this adapter";
    if (!m_scene.ShadowsOn())    return "shadows off";
    return m_rtReason ? m_rtReason : "not built yet";
}

void Renderer::RecordFrame(Device& dev, D3D12_CPU_DESCRIPTOR_HANDLE outRtv,
                           const float* view, const float* proj, UINT width, UINT height,
                           const SkyParams& sky)
{
    PixFrame();
    ID3D12GraphicsCommandList* list = dev.list.Get();
    if (!list) return;

    // `width` x `height` is the client's target, the OUTPUT; everything up to the upscaler (or the TAA) runs at the
    // RENDER size, which is the same without an upscaler.
    if (width != m_width || height != m_height) OnResize(dev, width, height);
    width = m_renderWidth;
    height = m_renderHeight;
    // The camera's planes and field of view, for the upscaler's depth (a conventional D3D projection: q = f/(f-n) at
    // [10], -q*n at [14], 1/tan(fov/2) at [5]).
    if (proj && proj[10] != 0.0f && proj[10] != 1.0f && proj[5] != 0.0f)
    {
        const float zNear = -proj[14] / proj[10];
        m_post.SetCamera(zNear, proj[10] * zNear / (proj[10] - 1.0f), 2.0f * atanf(1.0f / proj[5]));
    }

    // The whole frame's GPU time (LastFrameMs): last frame's, read now - the frame is synchronous, so it resolved.
    if (!m_frameTimer && dev.device && dev.queue && SUCCEEDED(dev.queue->GetTimestampFrequency(&m_frameHz)))
    {
        D3D12_QUERY_HEAP_DESC qh = {};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 2;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 2 * sizeof(UINT64);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_frameTimer))) ||
            FAILED(dev.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr, IID_PPV_ARGS(&m_frameReadback))))
            m_frameTimer.Reset();
    }
    if (m_frameTimer && m_framePending)
    {
        UINT64* t = nullptr;
        D3D12_RANGE r = { 0, 2 * sizeof(UINT64) };
        if (SUCCEEDED(m_frameReadback->Map(0, &r, reinterpret_cast<void**>(&t))))
        {
            if (t[1] >= t[0]) m_frameMs = double(t[1] - t[0]) * 1000.0 / double(m_frameHz);
            D3D12_RANGE none = { 0, 0 };
            m_frameReadback->Unmap(0, &none);
        }
        m_framePending = false;
    }
    if (m_frameTimer) list->EndQuery(m_frameTimer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

    // Which tonemap this frame gets is a rule, not a free choice: the debug
    // views carry FALSE COLOUR, not radiance, and a curve would change the very
    // numbers they exist to show; baked lighting is display-referred already.
    if (m_scene.DebugMode() != 0)      m_post.SetCurve(PostFx::kCurveRaw);
    else if (m_scene.LightMode() == 0) m_post.SetCurve(PostFx::kCurveSrgb);
    else                               m_post.SetCurve(m_curve);
    m_post.SetExposure(sky.exposure);
    m_post.SetTaa(m_taa && m_scene.DebugMode() == 0);

    // One atmosphere for every consumer, every frame: the scene's lighting,
    // its environment and aerial tables, and the sky pass. The multiple-
    // scattering switch is the Scene's, mirrored to the sky so the horizon -
    // where they meet - cannot disagree.
    m_scene.SetAtmosphere(sky, m_scene.Aerial());
    m_sky.SetMultipleScattering(m_scene.MultipleScattering());

    // THE JITTER IS SET BEFORE THE PREPASS, and this line's position is
    // load-bearing: every pass that rasterises this frame must use the same
    // sub-pixel offset, because the main pass tests GREATER_EQUAL against the
    // prepass's depth. When it sat after the prepass, half the terrain failed
    // the test and the sky showed through, flickering with the Halton sequence
    // (5.0e). Here it cannot be reordered by a client.
    float jx = 0.0f, jy = 0.0f;
    m_post.Jitter(width, height, &jx, &jy);
    m_scene.SetJitter(jx, jy);
    m_sky.SetJitter(jx, jy);

    m_rtActive = false;
    // The motion of moved objects comes from the prepass; without one, TAA
    // reprojects every pixel by the camera, as it always did.
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* rtShadow = nullptr;     // for TAA: where a moved object's shadow fell
    ID3D12Resource* rtAo = nullptr;         // and its occlusion, this frame and last
    ID3D12Resource* rtAoPrev = nullptr;
    if (m_scene.Ready())
    {
        KeepRtCurrent(dev);
        // The local lights' clusters for this view (lights.h), before anything reads them: the ray tracer below, the
        // scene pass after it.
        m_scene.PrepareLights(list, view, proj, width, height);

        // DEPTH + NORMAL PREPASS whenever there is a BVH: the main pass then
        // shades each pixel once, which pays for itself whatever the shadow
        // source (5.0e).
        const bool prepass = m_rt.HasScene();
        if (prepass)
        {
            m_scene.RecordPrepass(list, view, proj, width, height);
            motion = m_scene.MotionBuffer();
        }

        m_rtActive = m_preferRt && prepass && m_rtValid && m_scene.ShadowsOn();

        if (m_rtActive)
        {
            float sunTowards[3];
            {
                float len = sqrtf(sky.sunDir[0]*sky.sunDir[0] + sky.sunDir[1]*sky.sunDir[1] +
                                  sky.sunDir[2]*sky.sunDir[2]);
                if (len < 1e-6f) len = 1.0f;
                for (int k = 0; k < 3; ++k) sunTowards[k] = -sky.sunDir[k] / len;
            }
            // JITTERED matrix: this reconstructs the surface the prepass wrote.
            // The sun disc is the same one the sky draws, so the shadow's
            // softness and the visible sun agree by construction. AO only when
            // it is applied (5.0m).
            m_rt.Trace(dev, list, m_scene.Depth(), m_scene.NormalBuffer(), motion, m_scene.PlantBuffer(),
                       m_scene.LastViewProjJittered(), m_scene.LastViewProj(),
                       m_scene.LastCameraPos(), sunTowards, 4000.0f,
                       0.00465f * sky.sunDiscSize,
                       m_rt.AoAvailable() && m_ao, 3.0f, AoRaysTraced(), width, height, &m_scene.GetLights());
            m_rt.PrepareForRead(list);
            m_scene.SetRtVisibility(m_rt.Filtered());
            rtShadow = m_rt.Filtered();
            if (m_ao && m_rt.FilteredAo()) m_scene.SetRtAo(m_rt.FilteredAo()), rtAo = m_rt.AoDenoised(), rtAoPrev = m_rt.AoPrevDenoised();
            else                           m_scene.ClearRtAo();
            if (m_rt.FilteredLights()) m_scene.SetRtLights(m_rt.FilteredLights());
            else                       m_scene.ClearRtLights();
            m_scene.SetShadowSource(1);
        }
        else
        {
            m_scene.RenderShadows(list, view, proj, sky.sunDir);
            m_scene.ClearRtVisibility();
            m_scene.ClearRtAo();
            m_scene.ClearRtLights();   // (local lights unshadowed without ray tracing)
            m_scene.SetShadowSource(0);
        }
    }

    // Sky and scene draw into the HDR intermediate; PostFx puts the result in
    // the client's target at the end.
    const D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv = m_post.Begin(list);
    if (m_skyOn && m_sky.Ready())
        m_sky.Record(list, hdrRtv, view, proj, width, height, sky);
    else
        list->ClearRenderTargetView(hdrRtv, PostFx::kHdrClear, 0, nullptr);

    if (m_scene.Ready())
        m_scene.Record(list, hdrRtv, view, proj, width, height);

    m_post.Resolve(list, outRtv, m_scene.Depth(), motion, rtShadow, rtAo, rtAoPrev, m_scene.LastViewProj(), width, height);

    if (m_frameTimer)
    {
        list->EndQuery(m_frameTimer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(m_frameTimer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, m_frameReadback.Get(), 0);
        m_framePending = true;
    }
}

} // namespace core
} // namespace ngg
