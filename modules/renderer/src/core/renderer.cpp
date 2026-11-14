#include "renderer.h"
#include "log.h"
#include "pipeline_cache.h"
#include "shaders.h"

#include <algorithm>
#include <math.h>

namespace ngg {
namespace core {

const QualityTier& Renderer::Tier(Quality q)
{
    // Costs on the development machine's integrated Radeon, lab forest view
    // (965k geometry pixels) - see design §1.5 for the measurement.
    static const QualityTier kTiers[3] = {
        { "low",    false, 0, 0, 0.25f },   // RT sun shadows only - the VR baseline
        { "medium", true,  1, 1, 0.50f },   // + RT AO, one ray (the default); XeGTAO medium (2 slices x 2 steps)
        { "high",   true,  2, 2, 0.50f },   // + RT AO, two rays: less noise at rest; XeGTAO high (3 x 3)
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
    m_gtao.SetQuality(t.gtaoQuality);
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
    if (!m_gtao.Create(d, dev.queue.Get())) Log("renderer: no screen-space AO - the cascades go without AO");
    m_scene.SetShadowMaps(&m_shadows);
    if (!m_localShadows.Create(d, 1024)) Log("renderer: no local-light shadow maps - local lights unshadowed without RT");
    m_scene.SetLocalShadowMaps(&m_localShadows);

    // Built here rather than at load: overlays depend on the OUTPUT format,
    // not on the HDR intermediate, and must survive a track change.
    m_scene.CreateSelectionPipeline(d, outputFormat);

    if (!m_post.Create(d, outputFormat)) { Log("renderer: post-processing failed"); return false; }
    m_post.OnResize(d, width, height);

    if (rayTracing && m_rt.Create(dev))  // logs and declines on an adapter without DXR 1.1
    {
        if (!m_buildDev.CreateSecondary(dev))
            Log("renderer: no secondary command context - the BVH cannot be rebuilt mid-frame");
        if (m_preferRt) m_rt.PrepareTargets(d, m_renderWidth, m_renderHeight);
    }

    // Which upscalers can run here, for the settings menu: each backend made and let go once (it logs why not).
    // DLSS asks the GPU's vendor first, so a non-NVIDIA adapter never loads NGX.
    if (Upscaler* u = CreateFsrUpscaler(d)) m_fsrAvailable = true, delete u;
    if (Upscaler* u = CreateDlssUpscaler(d)) m_dlssAvailable = true, delete u;
    Log("renderer: upscalers available - FSR %s, DLSS %s", m_fsrAvailable ? "yes" : "no", m_dlssAvailable ? "yes" : "no");
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
    // The ray tracer's targets follow the size it is traced at (Rt::Trace) - made here already, so the first frame that
    // traces does not make them.
    if (m_rt.Ready() && m_preferRt) m_rt.PrepareTargets(dev.device.Get(), m_renderWidth, m_renderHeight);
}

void Renderer::SetDenoiser(Device& dev, Denoiser d)
{
    if (d == m_denoiser) return;
    // NRD is made the first time it is chosen (its pipelines, the ray tracer's variants): a game that never chooses it
    // pays nothing for it.
    if (d == Denoiser::Nrd && !m_nrdVis.Ready() && !m_nrdVis.Create(dev))
    {
        Log("renderer: NRD unavailable - the renderer's own denoisers");
        d = Denoiser::Ours;
    }
    m_denoiser = d;
    Log("renderer: denoiser %s", d == Denoiser::Nrd ? "NRD" : "ours");
}

const char* Renderer::DenoiserName() const
{
    if (!m_rtActive) return "";
    return m_lastSource == &m_nrdVis ? "NRD" : "ours";
}

double Renderer::DenoiseMs() const
{
    return m_lastSource == &m_nrdVis ? m_nrdVis.DenoiseMs() : m_rt.DenoiseMs();
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

void Renderer::ResetHistory()
{
    m_post.ResetHistory();
    m_rtVis.ResetHistory();
    m_cascadeVis.ResetHistory();
    m_nrdVis.ResetHistory();
}

bool Renderer::Load(Device& dev, const wchar_t* path)
{
    const bool ok = m_scene.Load(dev.device.Get(), dev.queue.Get(), path,
                                 PostFx::kHdrFormat, m_renderWidth, m_renderHeight);
    LogShaderStats("the scene's");
    LogPipelineStats("the scene's");
    SavePipelineCache();
    m_rtRevision = 0xFFFFFFFFu;          // rebuilt on the next frame
    m_rt.ForgetScene();                  // the old scene's BLASes: never part of this one's TLAS
    m_post.ResetHistory();
    return ok;
}

bool Renderer::LoadFromMemory(Device& dev, const void* data, size_t bytes)
{
    const bool ok = m_scene.LoadFromMemory(dev.device.Get(), dev.queue.Get(), data, bytes,
                                           PostFx::kHdrFormat, m_renderWidth, m_renderHeight);
    LogShaderStats("the scene's");
    LogPipelineStats("the scene's");
    SavePipelineCache();
    m_rtRevision = 0xFFFFFFFFu;
    m_rt.ForgetScene();
    m_post.ResetHistory();
    return ok;
}

// The BVH follows the scene, per MESH (design §4.2). When only instances
// changed - a drag, an instance of a mesh it already has - that is the TLAS
// alone. When meshes appear (a load, a car, a model, a first move, a hide) their
// BLASes are SUBMITTED on the ray tracer's own queue and the frame goes on
// without them: they join the TLAS in the first frame after the GPU has built
// them (Rt::Land). Dropped meshes leave at once. The TLAS on the SECONDARY
// context: this runs inside RecordFrame, with the client's list open.
void Renderer::KeepRtCurrent(Device& dev)
{
    (void)dev;
    if (!m_rt.Ready() || !m_scene.Ready() || !m_buildDev.list)
    {
        m_rtValid = false;
        m_rtReason = "no ray tracing on this adapter";
        return;
    }
    const bool landed = m_rt.Land();
    const uint32_t rev = m_scene.GeometryRevision();
    if (rev == m_rtRevision && !landed && !m_rt.InstancesStale()) return;
    m_rtRevision = rev;

    // A new mesh's vertices, before its BLAS. A new upload is a new buffer, which the any-hit test's view must follow.
    const bool poolMoved = m_scene.FlushDynamic(m_buildDev.device.Get());
    std::vector<uint64_t> keys;
    std::vector<Scene::RtInstanceDesc> instances;
    m_scene.DescribeRt(keys, instances);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    std::vector<uint64_t> built;
    size_t missing = 0, building = 0, moved = 0;
    for (uint64_t k : keys)
    {
        if (m_rt.HasMesh(k)) built.push_back(k);
        else if (m_rt.IsBuilding(k)) ++building;
        else ++missing;
    }
    for (const Scene::RtInstanceDesc& d : instances) moved += d.moved ? 1 : 0;
    // The same set: nothing landed or dropped (what is missing is submitted below and changes no TLAS yet) - the TLAS
    // alone, and the pool's view when it was uploaded again for a mesh now building. So the frame a mesh appears in
    // allocates no more here than a frame the car moves in.
    const bool sameSet = m_rt.HasScene() && !landed && built == m_rt.PublishedKeys();

    if (missing)
    {
        std::vector<Scene::RtMeshData> fresh;
        for (uint64_t k : keys)
        {
            if (m_rt.HasMesh(k) || m_rt.IsBuilding(k)) continue;
            Scene::RtMeshData m;
            if (m_scene.BuildRtMesh(k, m)) fresh.push_back(std::move(m));
        }
        if (!fresh.empty() && m_rt.SubmitBlases(m_buildDev, m_scene.RtVertexStride(), fresh)) building += fresh.size();
    }
    Log("renderer: scene revision %u - %zu mesh key(s) (%zu built, %zu building%s), %zu instance(s), %zu moved -> %s",
        rev, keys.size(), built.size(), building, landed ? ", some just landed" : "", instances.size(), moved,
        sameSet ? "instances only" : "publish");

    if (sameSet)
    {
        if (poolMoved) m_rt.FollowDynamicPool(m_buildDev.device.Get(), m_scene.DynamicVertexBuffer(),
                                              m_scene.DynamicVertexCount());
        m_rtValid = m_rt.UpdateInstances(m_buildDev, instances);
    }
    else
    {
        Scene::RtShared shared;
        m_rtValid = m_scene.BuildRtShared(m_buildDev.device.Get(), shared) &&
                    m_rt.Rebuild(m_buildDev, shared, keys, instances);
    }
    // An empty TLAS while the first BLASes build (a load) is not a failure: the cascades meanwhile.
    m_rtReason = m_rtValid ? nullptr
               : m_rt.Building() ? "the acceleration structure is being built"
                                 : "the acceleration structure failed to build";
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
    m_vis = Visibility();
    m_scene.ResetLocalShadows();   // (drawn by the cascades' provider only)
    m_scene.BeginFrame(dev.device.Get());   // the models' instance stream starts over
    m_scene.SetReflectionTlas(0);
    ID3D12Resource* motion = nullptr;
    if (m_scene.Ready())
    {
        KeepRtCurrent(dev);
        // The local lights' clusters for this view (lights.h), before anything reads them: the ray tracer below, the
        // scene pass after it.
        m_scene.PrepareLights(list, view, proj, width, height);

        // DEPTH + NORMAL + MOTION PREPASS, whatever the shadow source: the main pass then shades each pixel once
        // (5.0e), the AO - traced or screen-space - reads its depth and normals, and TAA and the upscalers its
        // motion. It is drawn with the jitter set above, so the main pass's GREATER_EQUAL finds its depth exactly.
        const bool prepass = m_scene.RecordPrepass(list, view, proj, width, height);
        if (prepass) motion = m_scene.MotionBuffer();

        m_rtActive = m_preferRt && prepass && m_rt.HasScene() && m_rtValid && m_scene.ShadowsOn();
        // Back on the RT path (the first BVH of a load built, the switch, shadows on again): its histories are from
        // whenever it last ran.
        if (m_rtActive != m_rtTraced)
        {
            if (m_rtActive) m_rt.ResetHistory();
            Log("renderer: RT visibility %s", m_rtActive ? "on" : RtFallbackReason());
            m_rtTraced = m_rtActive;
        }
        // The car's own reflection rays (Scene::SetReflectionTlas) trace this TLAS - kept current wherever the adapter
        // ray traces (KeepRtCurrent), so with the cascades' shadows too.
        m_scene.SetReflectionTlas(prepass && m_rt.HasScene() && m_rtValid ? m_rt.TlasVa() : 0);

        // The sun's visibility and the AO (visibility.h): the ray tracer's, or the cascades' and XeGTAO's.
        VisibilityFrame f;
        f.depth = m_scene.Depth();
        f.normal = m_scene.NormalBuffer();
        f.motion = motion;
        f.plant = m_scene.PlantBuffer();
        f.view = view;
        f.proj = proj;
        f.viewProjJittered = m_scene.LastViewProjJittered();
        f.viewProj = m_scene.LastViewProj();
        f.camPos = m_scene.LastCameraPos();
        f.jitter[0] = jx;
        f.jitter[1] = jy;
        for (int k = 0; k < 3; ++k) f.sunTravel[k] = sky.sunDir[k];
        f.sunAngle = 0.00465f * sky.sunDiscSize;
        f.ao = m_ao && prepass;
        f.aoRays = AoRaysTraced();
        f.temporal = m_taa && m_scene.DebugMode() == 0;
        f.width = width;
        f.height = height;
        f.lights = &m_scene.GetLights();
        VisibilityProvider& source = !m_rtActive ? static_cast<VisibilityProvider&>(m_cascadeVis)
                                   : m_denoiser == Denoiser::Nrd && m_nrdVis.Ready() ? static_cast<VisibilityProvider&>(m_nrdVis)
                                   : m_rtVis;
        // Another source than last frame's (the denoiser switched): it starts from no history of its own.
        if (&source != m_lastSource && m_lastSource) source.ResetHistory();
        m_lastSource = &source;
        source.Record(dev, list, f, m_vis);

        m_scene.SetShadowSource(m_vis.shadowSource);
        if (m_vis.sun)    m_scene.SetRtVisibility(m_vis.sun);
        else              m_scene.ClearRtVisibility();
        if (m_vis.ao)     m_scene.SetRtAo(m_vis.ao, m_vis.aoView);
        else              m_scene.ClearRtAo();
        if (m_vis.lights) m_scene.SetRtLights(m_vis.lights);
        else              m_scene.ClearRtLights();   // (local lights unshadowed without ray tracing)
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

    // TAA: where a moved object's shadow fell and its occlusion, this frame and last (the RT denoisers' only), and
    // the moving glass's own motion.
    m_post.Resolve(list, outRtv, m_scene.Depth(), motion, motion ? m_scene.GlassMotionBuffer() : nullptr, m_vis.sun,
                   m_vis.aoTaa, m_vis.aoTaaPrev,
                   // without the ray tracer, the local lights' moved-shadow mask in its place (Scene::LocalShadowMask)
                   m_vis.lightsTaa ? m_vis.lightsTaa : m_scene.LocalShadowMask(), m_scene.LastViewProj(), width, height);

    // The BLAS batches this frame queued (KeepRtCurrent): their threads allocate while the frame waits for the GPU.
    m_rt.StartBatches();

    if (m_frameTimer)
    {
        list->EndQuery(m_frameTimer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(m_frameTimer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, m_frameReadback.Get(), 0);
        m_framePending = true;
    }
}

} // namespace core
} // namespace ngg
