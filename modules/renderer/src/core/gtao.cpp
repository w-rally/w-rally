#include "gtao.h"
#include "log.h"

#include <cassert>
#include <string.h>
#include <vector>

#include "XeGTAO.h"         // third_party/xegtao: GTAOConstants, GTAOSettings, GTAOUpdateConstants
#include "xegtao_src.h"     // the same file and XeGTAO.hlsli as bytes (modules/renderer/CMakeLists.txt)

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

// The passes' constants: XeGTAO's own, then ours - the view's rotation, for the prepass's world-space normals.
struct GtaoCB
{
    XeGTAO::GTAOConstants gtao;
    float viewRow[3][4];
};
static_assert(sizeof(XeGTAO::GTAOConstants) % 16 == 0, "GTAOConstants must be whole float4s");
static_assert(sizeof(GtaoCB) % 16 == 0, "GtaoCB must be whole float4s");

// Our entry points, after XeGTAO.h and before XeGTAO.hlsli (Record builds the whole source). They are Intel's sample
// ones (third_party/xegtao/vaGTAO.hlsl) with this renderer's inputs: the prepass's normal and the frame's noise.
const char* const kGtaoRegsSrc = R"HLSL(
cbuffer GtaoCB : register(b0)
{
    GTAOConstants g_GTAOConsts;
    float4        gViewRow[3];     // the view's rotation, row-vector convention: world -> view = mul(n, (float3x3)V)
};
SamplerState g_samplerPointClamp : register(s0);
)HLSL";

const char* const kGtaoMainSrc = R"HLSL(
// pass 1 (XeGTAO_PrefilterDepths16x16): the reverse-Z depth -> view-space depth, 5 MIPs
Texture2D<float>        g_srcRawDepth         : register(t0);
RWTexture2D<lpfloat>    g_outWorkingDepthMIP0 : register(u0);
RWTexture2D<lpfloat>    g_outWorkingDepthMIP1 : register(u1);
RWTexture2D<lpfloat>    g_outWorkingDepthMIP2 : register(u2);
RWTexture2D<lpfloat>    g_outWorkingDepthMIP3 : register(u3);
RWTexture2D<lpfloat>    g_outWorkingDepthMIP4 : register(u4);
// pass 2 (XeGTAO_MainPass)
Texture2D<lpfloat>      g_srcWorkingDepth     : register(t0);
Texture2D<float4>       g_srcNormal           : register(t1);   // the prepass's: world space, R8G8B8A8_SNORM
RWTexture2D<uint>       g_outWorkingAOTerm    : register(u0);
RWTexture2D<unorm float> g_outWorkingEdges    : register(u1);
// pass 3 (XeGTAO_Denoise, one pass: the last)
Texture2D<uint>         g_srcWorkingAOTerm    : register(t0);
Texture2D<lpfloat>      g_srcWorkingEdges     : register(t1);
RWTexture2D<uint>       g_outFinalAOTerm      : register(u0);

// The prepass's world normal in XeGTAO's view space. The sky (cleared to 0) and anything degenerate face the camera.
lpfloat3 LoadNormal(int2 pos)
{
    const float3 w = g_srcNormal.Load(int3(pos, 0)).xyz;
    if (dot(w, w) < 0.25) return lpfloat3(0, 0, -1);
    const float3 v = float3(dot(w, float3(gViewRow[0].x, gViewRow[1].x, gViewRow[2].x)),
                            dot(w, float3(gViewRow[0].y, gViewRow[1].y, gViewRow[2].y)),
                            dot(w, float3(gViewRow[0].z, gViewRow[1].z, gViewRow[2].z)));
    return (lpfloat3)normalize(v);
}

// Hilbert curve driving R2 (vaGTAO.hlsl's), the index walking with the frame: 0 when the noise stands still.
lpfloat2 SpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)
{
    uint index = HilbertIndex(pixCoord.x % 64, pixCoord.y % 64);
    index += 288 * (temporalIndex % 64);
    return lpfloat2(frac(0.5 + index * float2(0.75487766624669276005, 0.5698402909980532659114)));
}

[numthreads(8, 8, 1)]
void CSPrefilterDepths16x16(uint2 dispatchThreadID : SV_DispatchThreadID, uint2 groupThreadID : SV_GroupThreadID)
{
    XeGTAO_PrefilterDepths16x16(dispatchThreadID, groupThreadID, g_GTAOConsts, g_srcRawDepth, g_samplerPointClamp,
                                g_outWorkingDepthMIP0, g_outWorkingDepthMIP1, g_outWorkingDepthMIP2,
                                g_outWorkingDepthMIP3, g_outWorkingDepthMIP4);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAO(const uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, GTAO_SLICES, GTAO_STEPS, SpatioTemporalNoise(pixCoord, g_GTAOConsts.NoiseIndex),
                    LoadNormal(pixCoord), g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp,
                    g_outWorkingAOTerm, g_outWorkingEdges);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSDenoisePass(const uint2 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);
    XeGTAO_Denoise(pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges, g_samplerPointClamp,
                   g_outFinalAOTerm, false);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSDenoiseLastPass(const uint2 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);   // two horizontal pixels a thread
    XeGTAO_Denoise(pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges, g_samplerPointClamp,
                   g_outFinalAOTerm, true);
}
)HLSL";

// XeGTAO's quality levels (vaGTAO.hlsl CSGTAOLow..Ultra): slices, steps per slice.
const int kSlices[4] = { 1, 2, 3, 9 };
const int kSteps[4]  = { 2, 2, 3, 3 };

// Per pass, a table of 7: t0 t1 u0 u1 u2 u3 u4. The denoise ping-pongs between the two terms (A the main pass's, B)
// and its last pass writes the output, from whichever holds the term then.
enum : UINT { kTable = 7, kPassPrefilter = 0, kPassMain = 1, kPassLastFromA = 2, kPassLastFromB = 3, kPassAtoB = 4,
              kPassBtoA = 5, kTables = 6 };

D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER t = {};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = r;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    t.Transition.StateBefore = a;
    t.Transition.StateAfter = b;
    return t;
}

ComPtr<ID3D12Resource> MakeTarget(ID3D12Device* dev, DXGI_FORMAT format, UINT width, UINT height, UINT16 mips,
                                  D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = mips;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

} // namespace

bool Gtao::Create(ID3D12Device* dev, ID3D12CommandQueue* queue)
{
    if (!dev) return false;
    if (!m_dxc.Load())
    {
        Log("gtao: no shader compiler - no screen-space AO");
        return false;
    }

    // One table, two ranges: t0-t1, u0-u4. Constants as root constants (b0); the point/clamp sampler XeGTAO asks for.
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 5;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(GtaoCB) / 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_rootSig))))
    {
        Log("gtao: root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    // The source: our defines, XeGTAO.h, our registers, XeGTAO.hlsli without its include of XeGTAO.h (it is above),
    // our entry points. FP32 depths (see gtao.h), which XeGTAO supports only at full precision.
    std::string hlsli(reinterpret_cast<const char*>(kXeGtaoHlsli));
    const std::string inc = "#include \"XeGTAO.h\"";
    const size_t at = hlsli.find(inc);
    if (at != std::string::npos) hlsli.replace(at, inc.size(), "");
    // VA_SATURATE is Intel's framework's (vaShaderCore.h), which XeGTAO.hlsli uses unasked.
    // XE_GTAO_USE_DEFAULT_CONSTANTS 0: the thin-occluder compensation is ours to set (SetThinOccluders).
    m_source = std::string("#define XE_GTAO_FP32_DEPTHS 1\n#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0\n"
                           "#define XE_GTAO_USE_DEFAULT_CONSTANTS 0\n#define VA_SATURATE saturate\n") +
               reinterpret_cast<const char*>(kXeGtaoH) + kGtaoRegsSrc + hlsli + kGtaoMainSrc;

    auto pso = [&](const wchar_t* entry, const char* name, ComPtr<ID3D12PipelineState>& out) {
        ComPtr<ID3DBlob> cs = m_dxc.Compile(m_source, entry, L"cs_6_0", name, { L"-DGTAO_SLICES=1", L"-DGTAO_STEPS=2" });
        if (!cs) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_rootSig.Get();
        pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        return SUCCEEDED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&out)));
    };
    if (!pso(L"CSPrefilterDepths16x16", "ngg_gtao_prefilter", m_psoPrefilter) ||
        !pso(L"CSDenoisePass", "ngg_gtao_denoise", m_psoDenoiseMid) ||
        !pso(L"CSDenoiseLastPass", "ngg_gtao_denoise_last", m_psoDenoise))
    {
        Log("gtao: the passes did not compile - no screen-space AO");
        m_psoPrefilter.Reset();
        m_psoDenoise.Reset();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kTables * kTable;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap)))) return false;
    m_stride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 2;
    D3D12_HEAP_PROPERTIES rp = {};
    rp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 2 * sizeof(UINT64);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!queue || FAILED(queue->GetTimestampFrequency(&m_timerHz)) ||
        FAILED(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_timer))) ||
        FAILED(dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&m_timerReadback))))
        m_timer.Reset();
    Log("gtao: XeGTAO ready (FP32 depths)");
    return true;
}

bool Gtao::CreateTargets(ID3D12Device* dev, UINT width, UINT height)
{
    m_width = m_height = 0;
    m_depthMips = MakeTarget(dev, DXGI_FORMAT_R32_FLOAT, width, height, XE_GTAO_DEPTH_MIP_LEVELS,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_aoTerm = MakeTarget(dev, DXGI_FORMAT_R8_UINT, width, height, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_aoTermB = MakeTarget(dev, DXGI_FORMAT_R8_UINT, width, height, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_edges = MakeTarget(dev, DXGI_FORMAT_R8_UNORM, width, height, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_out = MakeTarget(dev, DXGI_FORMAT_R8_TYPELESS, width, height, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!m_depthMips || !m_aoTerm || !m_aoTermB || !m_edges || !m_out)
    {
        Log("gtao: %ux%u targets failed", width, height);
        return false;
    }
    m_width = width;
    m_height = height;

    // The tables. Every slot written; the unused ones with null views of their declared types.
    D3D12_CPU_DESCRIPTOR_HANDLE base = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto slot = [&](UINT pass, UINT k) { D3D12_CPU_DESCRIPTOR_HANDLE h = base; h.ptr += (SIZE_T)(pass * kTable + k) * m_stride; return h; };
    auto srv = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT mips, D3D12_CPU_DESCRIPTOR_HANDLE h) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = f;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = mips;
        dev->CreateShaderResourceView(r, &d, h);
    };
    auto uav = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT mip, D3D12_CPU_DESCRIPTOR_HANDLE h) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
        d.Format = f;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = mip;
        dev->CreateUnorderedAccessView(r, nullptr, &d, h);
    };
    // Prefilter: t0 the depth (Record, every frame), t1 null; u0..u4 the MIPs.
    srv(nullptr, DXGI_FORMAT_R32_FLOAT, 1, slot(kPassPrefilter, 1));
    for (UINT m = 0; m < 5; ++m) uav(m_depthMips.Get(), DXGI_FORMAT_R32_FLOAT, m, slot(kPassPrefilter, 2 + m));
    // Main: t0 the MIP chain, t1 the normal (Record); u0 the term, u1 the edges, u2..u4 null.
    srv(m_depthMips.Get(), DXGI_FORMAT_R32_FLOAT, XE_GTAO_DEPTH_MIP_LEVELS, slot(kPassMain, 0));
    uav(m_aoTerm.Get(), DXGI_FORMAT_R8_UINT, 0, slot(kPassMain, 2));
    uav(m_edges.Get(), DXGI_FORMAT_R8_UNORM, 0, slot(kPassMain, 3));
    for (UINT k = 4; k < 7; ++k) uav(nullptr, DXGI_FORMAT_R32_FLOAT, 0, slot(kPassMain, k));
    // Denoise: t0 a term, t1 the edges; u0 the other term or the output, u1..u4 null.
    auto denoise = [&](UINT pass, ID3D12Resource* from, ID3D12Resource* to) {
        srv(from, DXGI_FORMAT_R8_UINT, 1, slot(pass, 0));
        srv(m_edges.Get(), DXGI_FORMAT_R8_UNORM, 1, slot(pass, 1));
        uav(to, DXGI_FORMAT_R8_UINT, 0, slot(pass, 2));
        for (UINT k = 3; k < 7; ++k) uav(nullptr, DXGI_FORMAT_R32_FLOAT, 0, slot(pass, k));
    };
    denoise(kPassLastFromA, m_aoTerm.Get(), m_out.Get());
    denoise(kPassLastFromB, m_aoTermB.Get(), m_out.Get());
    denoise(kPassAtoB, m_aoTerm.Get(), m_aoTermB.Get());
    denoise(kPassBtoA, m_aoTermB.Get(), m_aoTerm.Get());
    return true;
}

void Gtao::Record(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* normal,
                  const float* view, const float* proj, const float jitter[2], bool moving, UINT width, UINT height)
{
    if (!Ready() || !list || !depth || !normal || !view || !proj || !width || !height) return;
    ComPtr<ID3D12Device> dev;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev)))) return;

    // This quality's horizon search, compiled the first time it is asked for (a few hundred ms, once).
    const int q = Quality(), dn = m_depthNormals ? 1 : 0;
    ComPtr<ID3D12PipelineState>& main = m_psoMain[dn][q];
    if (!main && !m_mainFailed[dn][q])
    {
        wchar_t slices[32], steps[32];
        swprintf_s(slices, L"-DGTAO_SLICES=%d", kSlices[q]);
        swprintf_s(steps, L"-DGTAO_STEPS=%d", kSteps[q]);
        std::vector<const wchar_t*> defs = { slices, steps };
        if (dn) defs.push_back(L"-DXE_GTAO_GENERATE_NORMALS_INPLACE=1");
        ComPtr<ID3DBlob> cs = m_dxc.Compile(m_source, L"CSGTAO", L"cs_6_0", "ngg_gtao_main", defs);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_rootSig.Get();
        if (cs) pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        if (!cs || FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&main))))
        {
            Log("gtao: the quality %d pass did not compile", q);
            m_mainFailed[dn][q] = true;
        }
    }
    if (!main) return;
    if ((width != m_width || height != m_height) && !CreateTargets(dev.Get(), width, height)) return;

    if (m_timer && m_timerPending)
    {
        UINT64* t = nullptr;
        D3D12_RANGE r = { 0, 2 * sizeof(UINT64) };
        if (SUCCEEDED(m_timerReadback->Map(0, &r, reinterpret_cast<void**>(&t))))
        {
            if (t[1] >= t[0] && m_timerHz) m_ms = double(t[1] - t[0]) * 1000.0 / double(m_timerHz);
            D3D12_RANGE none = { 0, 0 };
            m_timerReadback->Unmap(0, &none);
        }
        m_timerPending = false;
    }
    if (m_timer) list->EndQuery(m_timer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

    // The constants. XeGTAO's from the REVERSE-Z projection the depth was written with (Scene: col2 = col3 - col2),
    // then its view-space reconstruction shifted by the jitter: a surface at view x/z = t lands at NDC t/tanX + jx,
    // so x/z = (2u - 1 - jx) tanX, and likewise y with the screen's v pointing down.
    float rz[16];
    memcpy(rz, proj, sizeof rz);
    for (int r = 0; r < 4; ++r) rz[r * 4 + 2] = proj[r * 4 + 3] - proj[r * 4 + 2];
    XeGTAO::GTAOSettings settings;
    settings.QualityLevel = q;
    settings.DenoisePasses = m_denoisePasses;
    settings.Radius = m_radius;
    settings.ThinOccluderCompensation = m_thin;
    GtaoCB cb = {};
    XeGTAO::GTAOUpdateConstants(cb.gtao, (int)width, (int)height, settings, rz, true, moving ? m_frame : 0u);
    cb.gtao.NDCToViewAdd.x = -cb.gtao.CameraTanHalfFOV.x * (1.0f + (jitter ? jitter[0] : 0.0f));
    cb.gtao.NDCToViewAdd.y =  cb.gtao.CameraTanHalfFOV.y * (1.0f - (jitter ? jitter[1] : 0.0f));
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) cb.viewRow[r][c] = view[r * 4 + c];
    ++m_frame;

    // This frame's depth and normal into the tables.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += (SIZE_T)(kPassPrefilter * kTable + 0) * m_stride;
        dev->CreateShaderResourceView(depth, &d, h);
        d.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
        h = cpu;
        h.ptr += (SIZE_T)(kPassMain * kTable + 1) * m_stride;
        dev->CreateShaderResourceView(normal, &d, h);
    }
    auto table = [&](UINT pass) {
        D3D12_GPU_DESCRIPTOR_HANDLE g = m_heap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += (UINT64)(pass * kTable) * m_stride;
        return g;
    };

    D3D12_RESOURCE_BARRIER pre[3] = {
        Barrier(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(normal, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(m_out.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(3, pre);

    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_rootSig.Get());
    list->SetComputeRoot32BitConstants(0, sizeof(GtaoCB) / 4, &cb, 0);

    list->SetPipelineState(m_psoPrefilter.Get());
    list->SetComputeRootDescriptorTable(1, table(kPassPrefilter));
    list->Dispatch((width + 15) / 16, (height + 15) / 16, 1);

    D3D12_RESOURCE_BARRIER mid[1] = {
        Barrier(m_depthMips.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(1, mid);
    list->SetPipelineState(main.Get());
    list->SetComputeRootDescriptorTable(1, table(kPassMain));
    list->Dispatch((width + XE_GTAO_NUMTHREADS_X - 1) / XE_GTAO_NUMTHREADS_X,
                   (height + XE_GTAO_NUMTHREADS_Y - 1) / XE_GTAO_NUMTHREADS_Y, 1);

    D3D12_RESOURCE_BARRIER mid2[2] = {
        Barrier(m_aoTerm.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(m_edges.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(2, mid2);
    // The denoise: all but the last pass from one term into the other, the last into the output.
    const UINT dx = (width + XE_GTAO_NUMTHREADS_X * 2 - 1) / (XE_GTAO_NUMTHREADS_X * 2);
    const UINT dy = (height + XE_GTAO_NUMTHREADS_Y - 1) / XE_GTAO_NUMTHREADS_Y;
    bool inA = true;   // which term holds it now (in NON_PIXEL_SHADER_RESOURCE; the other in UNORDERED_ACCESS)
    for (int p = 1; p < m_denoisePasses; ++p)
    {
        list->SetPipelineState(m_psoDenoiseMid.Get());
        list->SetComputeRootDescriptorTable(1, table(inA ? kPassAtoB : kPassBtoA));
        list->Dispatch(dx, dy, 1);
        ID3D12Resource* from = inA ? m_aoTerm.Get() : m_aoTermB.Get();
        ID3D12Resource* to = inA ? m_aoTermB.Get() : m_aoTerm.Get();
        D3D12_RESOURCE_BARRIER swap[2] = {
            Barrier(to, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(from, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        list->ResourceBarrier(2, swap);
        inA = !inA;
    }
    list->SetPipelineState(m_psoDenoise.Get());
    list->SetComputeRootDescriptorTable(1, table(inA ? kPassLastFromA : kPassLastFromB));
    list->Dispatch(dx, dy, 1);

    D3D12_RESOURCE_BARRIER post[6] = {
        Barrier(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE),
        Barrier(normal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Barrier(m_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Barrier(m_depthMips.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(inA ? m_aoTerm.Get() : m_aoTermB.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(m_edges.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(6, post);

    if (m_timer)
    {
        list->EndQuery(m_timer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(m_timer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, m_timerReadback.Get(), 0);
        m_timerPending = true;
    }
}

} // namespace core
} // namespace ngg
