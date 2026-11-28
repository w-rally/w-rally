#include "rt.h"
#include "frame_upload.h"
#include "log.h"
#include "pipeline_cache.h"
#include "shaders.h"

#include <string>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <math.h>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {













struct DenoiseCB
{
    float    reproj[16];
    uint32_t size[2];
    float    blend;
    float    valid;
    float    motionOn;
    float    blendMoved;
    float    period4;
    float    blendStill;
};
static_assert(sizeof(DenoiseCB) % 16 == 0, "DenoiseCB must be whole float4s");

struct TraceCB
{
    float    invViewProj[16];
    float    sunTowards[3];
    float    tMax;
    uint32_t size[2];
    float    bias;
    uint32_t stride;
    float    camPos[3];
    float    sunAngle;
    uint32_t frame;
    float    aoRadius;
    uint32_t aoRays;
    uint32_t pad2;
    float    camFwd[3];
    float    lightsOn;
    float    clusterParams[4];
};
static_assert(sizeof(TraceCB) % 16 == 0, "TraceCB must be whole float4s - see scene.cpp");

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                                  D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER t = {};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = r;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    t.Transition.StateBefore = a;
    t.Transition.StateAfter = b;
    return t;
}

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* dev, UINT64 bytes,
                                  D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;

    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes ? bytes : 1;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;

    ComPtr<ID3D12Resource> res;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                 IID_PPV_ARGS(&res));
    return res;
}

void MatMul(const float* a, const float* b, float* out)   // row-vector: v*a*b
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float acc = 0.0f;
            for (int k = 0; k < 4; ++k) acc += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = acc;
        }
}

// General 4x4 inverse; the view-projection is not affine, so the cheap
// rigid-transform shortcut does not apply. Same routine as post.cpp - kept
// local rather than shared because two copies of twelve lines is cheaper than
// a header that exists only to hold them.
bool Invert(const float* m, float* out)
{
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-20f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
    return true;
}

// One heap, one descriptor table. Fixed slots first, then every scene texture
// for the any-hit test - so the heap is sized per scene and (re)created in
// Build rather than in Create.
enum { kSlotTlas = 0, kSlotDepth = 1, kSlotOut = 2, kSlotCounters = 3,
       kSlotVerts = 4, kSlotAlphaIdx = 5, kSlotTriMat = 6, kSlotMaterials = 7,
       kSlotNormal = 8,
       // Denoise: two contiguous [raw, hist] SRV pairs so one table base
       // selects which history is read, plus a UAV per history for writing.
       kSlotPair0 = 9, kSlotPair1 = 11, kSlotHistUav0 = 13, kSlotHistUav1 = 14,
       // Ambient occlusion: its own raw output (u0) followed by counters (u1)
       // so the UAV table can point at either feature's pair, then its own
       // denoise pairs and history UAVs.
       kSlotAoOut = 15, kSlotAoCounters = 16,
       kSlotAoPair0 = 17, kSlotAoPair1 = 19, kSlotAoHistUav0 = 21, kSlotAoHistUav1 = 22,
       // Instancing (5.0o): the dynamic pool's vertices and the per-instance table.
       kSlotDynVerts = 23, kSlotInst = 24,
       // The scene's motion target, for the denoiser (a null view when none).
       kSlotMotion = 25,
       // The AO's wide pass output (rt_ao_wide.hlsl).
       kSlotAoWideUav = 26,
       // Foliage (scene.cpp FindPlants): the plant of each alpha triangle and
       // the prepass's plant per pixel, for the rays that must not hit their
       // own plant.
       kSlotTriPlant = 27, kSlotPlantId = 28,
       // Local lights (lights.h): their raw output (u0) and counters (u1), denoise pairs and history UAVs - the AO's
       // shape.
       kSlotLightOut = 29, kSlotLightCounters = 30,
       kSlotLightPair0 = 31, kSlotLightPair1 = 33, kSlotLightHistUav0 = 35, kSlotLightHistUav1 = 36,
       // Their wide pass output (rt_ao_wide.hlsl, as the AO's).
       kSlotLightWideUav = 37,
       // The NRD path's inputs (NRD_INPUTS, u2): the sun's and the lamps' penumbrae, the AO's hit distance.
       kSlotSunNrdUav = 38, kSlotAoNrdUav = 39, kSlotLightNrdUav = 40,
       kSlotTextures = 41 };

} // namespace

bool Rt::Create(Device& dev)
{
    if (!dev.device) return false;

    if (!dev.SupportsInlineRaytracing())
    {
        Log("rt: adapter reports tier %d.%d / SM %d.%d - inline ray tracing needs "
            "1.1 and 6.5, so the RT path is unavailable here",
            dev.rtTier / 10, dev.rtTier % 10, dev.shaderModel >> 4, dev.shaderModel & 0xF);
        return false;
    }
    if (!ShaderModel6Available()) return false;

    ComPtr<ID3D12Device5> dev5;
    if (FAILED(dev.device.As(&dev5)))
    {
        Log("rt: ID3D12Device5 unavailable despite a tier 1.1 report");
        return false;
    }

    // One table over the whole heap, in kSlot* order. The texture range is
    // UNBOUNDED: the shader indexes it by material at the hit point, and the
    // count is only known once a scene is loaded.
    D3D12_DESCRIPTOR_RANGE ranges[6] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;                    // t0 TLAS, t1 depth
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = kSlotTlas;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;                    // u0 output, u1 counters
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = kSlotOut;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 5;                    // t2..t6 verts, alpha idx, tri mat, materials, normal
    ranges[2].BaseShaderRegister = 2;
    ranges[2].OffsetInDescriptorsFromTableStart = kSlotVerts;
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[3].NumDescriptors = UINT_MAX;             // t0.., space1: the textures
    ranges[3].BaseShaderRegister = 0;
    ranges[3].RegisterSpace = 1;
    ranges[3].OffsetInDescriptorsFromTableStart = kSlotTextures;
    ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[4].NumDescriptors = 2;                    // t7 dynamic vertices, t8 instance table
    ranges[4].BaseShaderRegister = 7;
    ranges[4].OffsetInDescriptorsFromTableStart = kSlotDynVerts;
    ranges[5].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[5].NumDescriptors = 2;                    // t9 plant per alpha triangle, t10 plant per pixel
    ranges[5].BaseShaderRegister = 9;
    ranges[5].OffsetInDescriptorsFromTableStart = kSlotTriPlant;

    // The UAV pair is its OWN table so a dispatch can point it at either the
    // shadow output or the AO output; the SRVs are one table over the heap.
    D3D12_DESCRIPTOR_RANGE srvRanges[5] = { ranges[0], ranges[2], ranges[4], ranges[5], ranges[3] };
    D3D12_DESCRIPTOR_RANGE uavRange = ranges[1];
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    // u2, the NRD variants' second output (rt_common.hlsli NRD_INPUTS): a table of its own, unused by the others.
    D3D12_DESCRIPTOR_RANGE nrdRange = {};
    nrdRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    nrdRange.NumDescriptors = 1;
    nrdRange.BaseShaderRegister = 2;

    D3D12_ROOT_PARAMETER params[7] = {};
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[6].DescriptorTable.NumDescriptorRanges = 1;
    params[6].DescriptorTable.pDescriptorRanges = &nrdRange;
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 5;
    params[0].DescriptorTable.pDescriptorRanges = srvRanges;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &uavRange;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.Num32BitValues = sizeof(TraceCB) / 4;
    // The local lights (lights.h): root SRVs over the Lights buffers - t11 lights, t12 clusters, t13 indices.
    for (int k = 0; k < 3; ++k)
    {
        params[3 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[3 + k].Descriptor.ShaderRegister = 11 + k;
        params[3 + k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    // Linear, WRAP: the same addressing the rasteriser uses for these textures
    // (scene.cpp's static samplers), so the cut-out matches the drawn leaf.
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 7;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    {
        Log("rt: root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(CreateRootSignature(dev.device, 0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                               IID_PPV_ARGS(&m_rootSig))))
        return false;

    ComPtr<ID3DBlob> cs = LoadShader("rt_trace.hlsl", "CSMain", "cs_6_5");
    if (!cs) return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_rootSig.Get();
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    if (FAILED(CreatePipeline(dev.device, &pd, IID_PPV_ARGS(&m_pso))))
    {
        Log("rt: compute pipeline creation failed - the DXIL was probably rejected");
        return false;
    }

    ComPtr<ID3DBlob> aocs = LoadShader("rt_ao.hlsl", "CSMain", "cs_6_5");
    if (aocs)
    {
        pd.CS = { aocs->GetBufferPointer(), aocs->GetBufferSize() };
        if (FAILED(CreatePipeline(dev.device, &pd, IID_PPV_ARGS(&m_psoAo))))
            Log("rt: AO pipeline creation failed - shadows still work, AO does not");
    }

    // NGG_LIGHTS_REF=1|2: the local lights' shadows by a reference (rt_lights.hlsl) - for checking, never for play.
    const char* refEnv = std::getenv("NGG_LIGHTS_REF");
    ComPtr<ID3DBlob> lcs = !refEnv ? LoadShader("rt_lights.hlsl", "CSMain", "cs_6_5")
                         : LoadShader("rt_lights.hlsl", "CSMain", "cs_6_5",
                                      { refEnv[0] == '2' ? "-DLIGHTS_REF=2" : "-DLIGHTS_REF=1" });
    if (lcs)
    {
        pd.CS = { lcs->GetBufferPointer(), lcs->GetBufferSize() };
        if (FAILED(CreatePipeline(dev.device, &pd, IID_PPV_ARGS(&m_psoLights))))
            m_psoLights.Reset();
    }
    if (!m_psoLights) Log("rt: local-light pipeline creation failed - local lights go unshadowed");

    // --- the denoiser: its own root signature, same heap --------------------
    {
        D3D12_DESCRIPTOR_RANGE dr[4] = {};
        dr[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;   // t0 raw, t1 hist (a pair)
        dr[0].NumDescriptors = 2;
        dr[0].BaseShaderRegister = 0;
        dr[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;   // t2 depth
        dr[1].NumDescriptors = 1;
        dr[1].BaseShaderRegister = 2;
        dr[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;   // u0 history out
        dr[2].NumDescriptors = 1;
        dr[2].BaseShaderRegister = 0;
        dr[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;   // t3 motion
        dr[3].NumDescriptors = 1;
        dr[3].BaseShaderRegister = 3;

        D3D12_ROOT_PARAMETER dp[5] = {};
        for (int i = 0; i < 3; ++i)
        {
            dp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            dp[i].DescriptorTable.NumDescriptorRanges = 1;
            dp[i].DescriptorTable.pDescriptorRanges = &dr[i];
        }
        dp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        dp[3].Constants.ShaderRegister = 0;
        dp[3].Constants.Num32BitValues = sizeof(DenoiseCB) / 4;
        dp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        dp[4].DescriptorTable.NumDescriptorRanges = 1;
        dp[4].DescriptorTable.pDescriptorRanges = &dr[3];

        D3D12_ROOT_SIGNATURE_DESC drs = {};
        drs.NumParameters = 5;
        drs.pParameters = dp;
        ComPtr<ID3DBlob> dsig, derr;
        if (FAILED(D3D12SerializeRootSignature(&drs, D3D_ROOT_SIGNATURE_VERSION_1, &dsig, &derr)) ||
            FAILED(CreateRootSignature(dev.device, 0, dsig->GetBufferPointer(), dsig->GetBufferSize(),
                                                   IID_PPV_ARGS(&m_denoiseRootSig))))
        {
            Log("rt: denoise root signature failed");
            return false;
        }
        ComPtr<ID3DBlob> dcs = LoadShader("rt_denoise.hlsl", "CSMain", "cs_6_5");
        if (!dcs) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC dpd = {};
        dpd.pRootSignature = m_denoiseRootSig.Get();
        dpd.CS = { dcs->GetBufferPointer(), dcs->GetBufferSize() };
        if (FAILED(CreatePipeline(dev.device, &dpd, IID_PPV_ARGS(&m_psoDenoise))))
        {
            Log("rt: denoise pipeline creation failed");
            return false;
        }
        // The AO's wide pass, on the same root signature (rt_ao_wide.hlsl). Without
        // it the AO is the denoise's own output, as before.
        ComPtr<ID3DBlob> wcs = LoadShader("rt_ao_wide.hlsl", "CSMain", "cs_6_5");
        if (wcs)
        {
            dpd.CS = { wcs->GetBufferPointer(), wcs->GetBufferSize() };
            if (FAILED(CreatePipeline(dev.device, &dpd, IID_PPV_ARGS(&m_psoAoWide))))
                m_psoAoWide.Reset();
        }
        if (!m_psoAoWide) Log("rt: AO wide pass pipeline creation failed - the AO goes unwidened");
    }

    // Start, after the shadow, after the AO, after the local lights, after the denoise: a set per frame slot.
    m_timestamps.Create(dev.device.Get(), 5);
    dev.queue->GetTimestampFrequency(&m_timestampHz);

    m_stride = dev.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // The descriptor heap is created in Build: its size depends on how many
    // textures the scene has, and there is no scene yet.

    m_counters = MakeBuffer(dev.device.Get(), 4 * sizeof(uint32_t), D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_aoCounters = MakeBuffer(dev.device.Get(), 4 * sizeof(uint32_t), D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_lightCounters = MakeBuffer(dev.device.Get(), 4 * sizeof(uint32_t), D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_countersReadback = MakeBuffer(dev.device.Get(), 2 * 4 * sizeof(uint32_t),   // a range per frame slot
                                    D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    m_countersZero = MakeBuffer(dev.device.Get(), 4 * sizeof(uint32_t), D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
    if (m_countersZero)
    {
        void* p = nullptr;
        D3D12_RANGE none = { 0, 0 };
        if (SUCCEEDED(m_countersZero->Map(0, &none, &p)))
        {
            memset(p, 0, 4 * sizeof(uint32_t));
            m_countersZero->Unmap(0, nullptr);
        }
    }

    // The BLAS builds' own queue (SubmitBlases): COMPUTE, so they overlap the frames instead of going ahead of them on
    // the frame's queue. Without it a new mesh is never built: no ray tracing.
    {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (FAILED(dev.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_buildQueue))) ||
            FAILED(dev.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_buildFence))))
        {
            Log("rt: no compute queue for the BLAS builds - the RT path is unavailable");
            m_pso.Reset();
            return false;
        }
        m_buildQueue->SetName(L"ngg BLAS builds");
    }

    Log("rt: ready - inline RayQuery, tier %d.%d, SM %d.%d",
        dev.rtTier / 10, dev.rtTier % 10, dev.shaderModel >> 4, dev.shaderModel & 0xF);
    return true;
}

bool Rt::EnableNrdInputs(Device& dev)
{
    if (m_psoNrd) return true;
    if (!Ready()) return false;
    // The three traces again, writing NRD's inputs too (NRD_INPUTS). Made the first time NRD is chosen, so a game that
    // never chooses it never compiles or creates them.
    ComPtr<ID3D12PipelineState> pso[3];
    const char* files[3] = { "rt_trace.hlsl", "rt_ao.hlsl", "rt_lights.hlsl" };
    for (int i = 0; i < 3; ++i)
    {
        ComPtr<ID3DBlob> cs = LoadShader(files[i], "CSMain", "cs_6_5", { "-DNRD_INPUTS=1" });
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_rootSig.Get();
        if (cs) pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        if (!cs || FAILED(CreatePipeline(dev.device, &pd, IID_PPV_ARGS(&pso[i]))))
        {
            Log("rt: the NRD inputs' %s pipeline failed - no NRD", files[i]);
            return false;
        }
    }
    m_psoNrd = pso[0];
    m_psoAoNrd = pso[1];
    m_psoLightsNrd = pso[2];
    if (m_width && m_height && !CreateNrdTargets(dev.device.Get()))
    {
        m_psoNrd.Reset();
        return false;
    }
    return true;
}

bool Rt::CreateNrdTargets(ID3D12Device* dev)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = m_width;
    d.Height = m_height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (auto& r : m_nrdIn)
    {
        r.Reset();
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                nullptr, IID_PPV_ARGS(&r))))
            return false;
    }
    return !m_heap || WriteTargetViews(dev);
}

// Instance transform: our matrices are row-vector (p' = p * M, translation in
// the last row), D3D12's is a 3x4 for column vectors - the transpose of the
// upper three columns.
void ToInstanceTransform(const float* m, float t[3][4])
{
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c) t[r][c] = m[c * 4 + r];
        t[r][3] = m[12 + r];
    }
}

namespace {
double MsSince(const LARGE_INTEGER& t0)
{
    LARGE_INTEGER t1, hz;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&hz);
    return 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(hz.QuadPart);
}
UINT64 AlignAs(UINT64 v) { return (v + D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT - 1) &
                                  ~UINT64(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT - 1); }
// A BLAS at least this big gets a buffer of its own; smaller ones share arenas of up to kArenaBytes. A share is freed
// only with its whole arena, so a big BLAS that is replaced (the world, after a hide) must not pin a car's parts.
// SMALL ALLOCATIONS ON PURPOSE: the driver creates a buffer under a lock the frame's own small allocations (the TLAS's
// instance upload, every frame the car moves) then wait on - with 32 MB arenas the frame after a new car waited
// 143 ms for the batch's thread. Each 4 MB creation holds it for about a millisecond.
constexpr UINT64 kOwnBufferBytes = 4ull << 20;
constexpr UINT64 kArenaBytes     = 4ull << 20;
// The batch's scratch is reused from the start, past a UAV barrier, once this much is in use (a single bigger build
// gets what it needs). The builds then run in groups, which costs the build queue a little and the frames nothing.
constexpr UINT64 kScratchBytes   = 8ull << 20;
} // namespace

Rt::~Rt()
{
    // The batches' threads, then the build queue: it may still read and write memory the batches hold.
    for (auto& b : m_batches)
        if (b->work.valid()) b->work.wait();
    if (m_buildFence && m_buildFence->GetCompletedValue() < m_buildFenceValue)
    {
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (e && SUCCEEDED(m_buildFence->SetEventOnCompletion(m_buildFenceValue, e))) WaitForSingleObject(e, 5000);
        if (e) CloseHandle(e);
    }
}

bool Rt::IsBuilding(uint64_t key) const
{
    for (const auto& b : m_batches)
        if (std::find(b->keys.begin(), b->keys.end(), key) != b->keys.end()) return true;
    return std::find(m_failedKeys.begin(), m_failedKeys.end(), key) != m_failedKeys.end();
}

void Rt::ForgetScene()
{
    // (A frame in flight may still trace them - device.h: released once it has run.)
    for (auto& e : m_blas) DeferRelease(e.second.as.Get());
    DeferRelease(m_tlas.Get());
    m_blas.clear();
    ++m_blasRev;
    m_tlas.Reset();
    m_instanceCount = 0;
    m_appeared.clear();
    m_appearedFlagged = false;
    m_publishedKeys.clear();
    m_failedKeys.clear();
}

bool Rt::SubmitBlases(Device& dev, uint32_t stride, std::vector<Scene::RtMeshData>& newMeshes)
{
    if (!Ready() || !m_buildQueue || newMeshes.empty()) return false;
    auto b = std::make_unique<BlasBatch>();
    QueryPerformanceCounter(&b->submitted);
    for (const Scene::RtMeshData& m : newMeshes)
    {
        b->keys.push_back(m.key);
        // The source vertices, held from now: the build reads them, and the next upload of the dynamic pool replaces
        // (frees) its buffer - possibly before the thread has started.
        if (m.vb) b->keep.push_back(ComPtr<ID3D12Resource>(m.vb));
    }
    b->meshes.swap(newMeshes);
    b->device = dev.device.Get();
    b->stride = stride;
    // The allocations and the recording on a thread of their own: a car's 71 BLASes took 15-140 ms of CPU, most of it
    // creating buffers. The thread starts with the NEXT frame (Land): this one is the frame a mesh appears in, which
    // allocates plenty of its own (the car's materials, the scene's heap), and the driver makes the two wait on each
    // other - the Fabia's first frame +35 ms when started here.
    m_batches.push_back(std::move(b));
    return true;
}

// On the batch's thread: everything here is the batch's own, or free-threaded (the device's creation calls, the
// queue's submit under m_submitLock). No Log: the sink is the main thread's - Land reports.
bool Rt::RecordBatch(BlasBatch& b, ID3D12Device* device, uint32_t stride)
{
    std::vector<Scene::RtMeshData>& newMeshes = b.meshes;
    ComPtr<ID3D12Device5> dev5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dev5)))) { b.error = "no ID3D12Device5"; return false; }
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);

    // ---- what each needs: its geometries (opaque commits on its own; alpha-tested surfaces as a candidate for the
    // any-hit test - PrimitiveIndex counts per geometry, which is what lets the alpha one index from 0), and the
    // driver's sizes. The prebuild ask does not read the addresses, so the index buffer's are filled in later.
    struct Plan { D3D12_RAYTRACING_GEOMETRY_DESC geos[2]; const std::vector<uint32_t>* src[2]; UINT n;
                  UINT64 ib[2]; UINT64 result, scratch; };
    std::vector<Plan> plans(newMeshes.size());
    UINT64 indexBytes = 0, scratchMax = 0, scratchTotal = 0;
    for (size_t i = 0; i < newMeshes.size(); ++i)
    {
        Scene::RtMeshData& m = newMeshes[i];
        Plan& p = plans[i];
        p = Plan{};
        if (!m.vb) continue;
        auto describe = [&](const std::vector<uint32_t>& idx, bool opaque) {
            if (idx.empty()) return;
            D3D12_RAYTRACING_GEOMETRY_DESC& g = p.geos[p.n];
            g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            g.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            g.Triangles.VertexBuffer.StartAddress = m.vb->GetGPUVirtualAddress() + m.vbOffset;
            g.Triangles.VertexBuffer.StrideInBytes = stride;
            g.Triangles.VertexCount = m.vertexCount;
            g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            g.Triangles.IndexCount = (UINT)idx.size();
            g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            p.src[p.n] = &idx;
            p.ib[p.n++] = indexBytes;
            indexBytes += idx.size() * 4;
        };
        describe(m.opaque, true);
        describe(m.alpha, false);
        if (!p.n) continue;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.NumDescs = p.n;
        in.pGeometryDescs = p.geos;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
        dev5->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
        p.result = AlignAs(info.ResultDataMaxSizeInBytes);
        p.scratch = AlignAs(info.ScratchDataSizeInBytes);
        scratchMax = (std::max)(scratchMax, p.scratch);
        scratchTotal += p.scratch;
    }
    if (!indexBytes) { b.error = "no triangles"; return false; }

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&b.alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, b.alloc.Get(), nullptr,
                                             IID_PPV_ARGS(&b.list))))
    {
        b.error = "no command list";
        return false;
    }
    b.list->SetName(L"ngg BLAS builds");

    // ---- one upload for every index of the batch
    ComPtr<ID3D12Resource> indices = MakeBuffer(device, indexBytes, D3D12_HEAP_TYPE_UPLOAD,
                                                D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* mapped = nullptr;
    D3D12_RANGE none = { 0, 0 };
    if (!indices || FAILED(indices->Map(0, &none, reinterpret_cast<void**>(&mapped))))
    {
        b.error = "the index upload failed";
        return false;
    }
    b.keep.push_back(indices);

    // ---- one scratch, reused past a barrier when a batch outgrows kScratchBytes. COMMON: buffers are created in it
    // whatever is asked, and promote to UNORDERED_ACCESS on first use - asking for UAV only earned a validation
    // warning per rebuild.
    const UINT64 scratchBytes = (std::min)(scratchTotal, (std::max)(kScratchBytes, scratchMax));
    ComPtr<ID3D12Resource> scratch = MakeBuffer(device, scratchBytes, D3D12_HEAP_TYPE_DEFAULT,
                                                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!scratch) { indices->Unmap(0, nullptr); b.error = "the scratch allocation failed"; return false; }
    b.keep.push_back(scratch);

    // ---- results: the small ones packed into arenas, each big one in a buffer of its own
    ComPtr<ID3D12Resource> arena;
    UINT64 arenaUsed = 0, arenaSize = 0, scratchUsed = 0;
    auto openArena = [&](size_t from) {
        arenaSize = 0;
        for (size_t j = from; j < plans.size(); ++j)
        {
            if (!plans[j].n || plans[j].result >= kOwnBufferBytes) continue;
            if (arenaSize + plans[j].result > kArenaBytes) break;
            arenaSize += plans[j].result;
        }
        arena = MakeBuffer(device, (std::max)(arenaSize, UINT64(256)), D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        arenaUsed = 0;
    };

    for (size_t i = 0; i < newMeshes.size(); ++i)
    {
        Scene::RtMeshData& m = newMeshes[i];
        Plan& p = plans[i];
        if (!p.n) continue;
        for (UINT g = 0; g < p.n; ++g)
        {
            memcpy(mapped + p.ib[g], p.src[g]->data(), p.src[g]->size() * 4);
            p.geos[g].Triangles.IndexBuffer = indices->GetGPUVirtualAddress() + p.ib[g];
        }

        BlasEntry e;
        if (p.result >= kOwnBufferBytes)
            e.as = MakeBuffer(device, p.result, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        else
        {
            if (!arena || arenaUsed + p.result > arenaSize) openArena(i);
            e.as = arena;
            e.offset = arenaUsed;
            arenaUsed += p.result;
        }
        if (!e.as) { b.error = "a BLAS allocation failed"; continue; }

        if (scratchUsed + p.scratch > scratchBytes)
        {
            D3D12_RESOURCE_BARRIER ub = {};
            ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            ub.UAV.pResource = scratch.Get();
            b.list->ResourceBarrier(1, &ub);
            scratchUsed = 0;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
        bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        // PREFER_FAST_TRACE: a BLAS is built when its mesh appears and then traced for as long as it exists - moving
        // it is the TLAS's job.
        bd.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        bd.Inputs.NumDescs = p.n;
        bd.Inputs.pGeometryDescs = p.geos;
        bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress() + scratchUsed;
        bd.DestAccelerationStructureData = e.Va();
        b.list->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        scratchUsed += p.scratch;

        e.alpha.swap(m.alpha);
        e.alphaMat.swap(m.alphaMat);
        e.alphaPlant.swap(m.alphaPlant);
        if (e.alphaPlant.size() != e.alphaMat.size()) e.alphaPlant.assign(e.alphaMat.size(), 0xFFFFFFFFu);
        e.vbSelect = m.vbSelect;
        e.vertexBase = m.vertexBase;
        b.bytes += p.result;
        b.triangles += (uint32_t)((m.opaque.size() + e.alpha.size()) / 3);
        b.entries.emplace_back(m.key, std::move(e));
    }
    indices->Unmap(0, nullptr);
    if (FAILED(b.list->Close()) || b.entries.empty())
    {
        b.error = "the builds could not be recorded";
        return false;
    }
    b.scratchBytes = scratchBytes;
    ID3D12CommandList* lists[] = { b.list.Get() };
    {
        std::lock_guard<std::mutex> lock(m_submitLock);   // fence values in the order the queue sees them
        m_buildQueue->ExecuteCommandLists(1, lists);
        b.fence = ++m_buildFenceValue;
        m_buildQueue->Signal(m_buildFence.Get(), b.fence);
    }
    b.recordMs = MsSince(t0);
    return true;
}

void Rt::StartBatches()
{
    for (auto& p : m_batches)
    {
        BlasBatch& b = *p;
        if (b.started) continue;
        BlasBatch* raw = &b;
        b.work = std::async(std::launch::async, [this, raw] { return RecordBatch(*raw, raw->device, raw->stride); });
        b.started = true;
    }
}

bool Rt::PrepareTargets(ID3D12Device* device, UINT width, UINT height)
{
    if (!Ready() || !width || !height) return false;
    if (width == m_width && height == m_height && m_result) return true;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    // Without a heap yet the views wait for the first Rebuild, which writes them into the heap it makes.
    if (!CreateTargets(device, width, height)) { m_width = m_height = 0; return false; }
    Log("rt: targets %ux%u made ahead of the first ray (%.0f ms)", width, height, MsSince(t0));
    return true;
}

bool Rt::Land()
{
    if (m_batches.empty()) return false;
    bool landed = false;
    for (auto it = m_batches.begin(); it != m_batches.end(); )
    {
        BlasBatch& b = **it;
        if (!b.started) { StartBatches(); ++b.frames; ++it; continue; }   // (the renderer starts them: StartBatches)
        if (b.work.valid())
        {
            if (b.work.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++b.frames; ++it; continue; }
            b.ok = b.work.get();
            Log("rt: %zu BLAS(es) %s (%u triangles, %.1f MB, scratch %.1f MB): recorded in %.1f ms on their thread%s%s",
                b.entries.size(), b.ok ? "submitted" : "NOT submitted", b.triangles, b.bytes / 1048576.0,
                b.scratchBytes / 1048576.0, b.recordMs, b.error ? " - " : "", b.error ? b.error : "");
        }
        if (b.ok && m_buildFence->GetCompletedValue() < b.fence) { ++b.frames; ++it; continue; }
        if (b.ok)
        {
            for (auto& e : b.entries)
            {
                m_appeared.push_back(e.first);
                m_blas[e.first] = std::move(e.second);
            }
            ++m_blasRev;
            Log("rt: %zu BLAS(es) built, in the TLAS %.0f ms / %u frame(s) after the submit", b.entries.size(),
                MsSince(b.submitted), b.frames + 1);
        }
        else    // not tried again until the next scene: a failure every frame would be a submit every frame
            m_failedKeys.insert(m_failedKeys.end(), b.keys.begin(), b.keys.end());
        it = m_batches.erase(it);
        landed = true;
    }
    return landed;
}

bool Rt::Rebuild(Device& dev, ID3D12GraphicsCommandList* list, const Scene::RtShared& shared,
                 const std::vector<uint64_t>& liveKeys, const std::vector<Scene::RtInstanceDesc>& fixed,
                 uint32_t fixedRevision, const std::vector<std::pair<uint32_t, uint32_t>>& fixedMasks,
                 const std::vector<Scene::RtInstanceDesc>& instances)
{
    if (!Ready() || !shared.staticVb) return false;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);

    // ---- drop the BLASes nothing uses any more - except a superseded WORLD while its successor builds: hiding or
    // first moving a static object rebuilds the world's BLAS, and dropping the old one at once would take the whole
    // stage out of the rays for the frames the new one takes. It keeps its identity instance until then. (A new
    // scene is not this case: ForgetScene has emptied the cache.)
    bool worldBuilding = false;
    for (uint64_t k : liveKeys)
        if (Scene::IsRtWorldKey(k) && !m_blas.count(k) && IsBuilding(k)) worldBuilding = true;
    std::vector<uint64_t> held;
    for (auto it = m_blas.begin(); it != m_blas.end(); )
    {
        if (std::find(liveKeys.begin(), liveKeys.end(), it->first) != liveKeys.end()) { ++it; continue; }
        if (worldBuilding && Scene::IsRtWorldKey(it->first))
        {
            held.push_back(it->first);
            ++it;
            continue;
        }
        DeferRelease(it->second.as.Get());   // the frame in flight's TLAS may still name it (device.h)
        it = m_blas.erase(it);
        ++m_blasRev;
    }
    std::vector<uint64_t> published;              // what the TLAS can hold now: built and live (and a held world)
    for (uint64_t k : liveKeys)
        if (m_blas.count(k)) published.push_back(k);
    std::sort(published.begin(), published.end());
    m_publishedKeys = published;
    published.insert(published.end(), held.begin(), held.end());

    // ---- the shared alpha index buffer and per-triangle materials: every
    // live mesh's alpha triangles end to end, each BLAS knowing where its own
    // start. Rebuilt whenever the mesh set changes, which is not per drag.
    std::vector<uint32_t> alpha, alphaMat, alphaPlant;
    for (uint64_t k : published)
    {
        auto it = m_blas.find(k);
        if (it == m_blas.end()) continue;
        it->second.triBase = (uint32_t)(alphaMat.size());
        alpha.insert(alpha.end(), it->second.alpha.begin(), it->second.alpha.end());
        alphaMat.insert(alphaMat.end(), it->second.alphaMat.begin(), it->second.alphaMat.end());
        alphaPlant.insert(alphaPlant.end(), it->second.alphaPlant.begin(), it->second.alphaPlant.end());
    }
    if (alpha.empty()) { alpha.assign(3, 0); alphaMat.assign(1, 0); alphaPlant.assign(1, 0xFFFFFFFFu); }   // never an empty view
    // Written into the buffers the last Rebuild made when they are big enough (a third to spare when made): a car's
    // BLASes landing then allocates nothing here. Safe to overwrite - the frames are synchronous, and this runs before
    // the frame's first ray.
    auto write = [&](ComPtr<ID3D12Resource>& buf, const std::vector<uint32_t>& v) {
        const size_t bytes = v.size() * 4;
        void* p = nullptr;
        D3D12_RANGE none = { 0, 0 };
        // With frames in flight (device.h) the last frame's any-hit tests may still read it: a new one, the old one
        // released once that frame has run.
        if (FrameSlots() == 1 && buf && buf->GetDesc().Width >= bytes && SUCCEEDED(buf->Map(0, &none, &p)))
        {
            memcpy(p, v.data(), bytes);
            buf->Unmap(0, nullptr);
            return;
        }
        DeferRelease(buf.Get());
        buf = MakeBuffer(dev.device.Get(), bytes + bytes / 3, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (buf && SUCCEEDED(buf->Map(0, &none, &p)))
        {
            memcpy(p, v.data(), bytes);
            buf->Unmap(0, nullptr);
        }
    };
    write(m_triPlant, alphaPlant);
    write(m_ibAlpha, alpha);
    write(m_triMaterial, alphaMat);
    if (!m_triPlant || !m_ibAlpha || !m_triMaterial) return false;
    DeferRelease(m_materials.Get());   // (a frame in flight may still read the old table)
    m_materials = shared.materials;
    m_vertexStride = shared.stride;
    m_staticVb = shared.staticVb;
    m_dynVb = shared.dynVb;

    // ---- the descriptor heap, sized for THIS scene's textures
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSlotTextures + (UINT)(shared.textures.empty() ? 1 : shared.textures.size());
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;   // the CPU-only mirror: written here and wherever a view changes
    m_heap.Reset();
    if (FAILED(dev.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap)))) return false;
    m_heapCount = hd.NumDescriptors;
    // What the shaders read: a half per frame slot, filled from the mirror by Publish. The old one may still be read
    // by a frame in flight (device.h).
    DeferRelease(m_gpuHeap.Get());
    m_gpuHeap.Reset();
    hd.NumDescriptors = 2 * m_heapCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_gpuHeap)))) { m_heap.Reset(); return false; }
    // The outputs' views live in the heap: into the new one, with the targets and their histories kept (a new mesh
    // used to drop every history on the screen). Without targets yet, the first Trace makes them.
    if (!m_result || !WriteTargetViews(dev.device.Get())) m_width = m_height = 0;

    auto slot = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * m_stride;
        return h;
    };
    auto raw = [&](ID3D12Resource* r, UINT64 bytes, UINT at) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = (UINT)(bytes / 4);
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        dev.device->CreateShaderResourceView(r, &sd, slot(at));
    };
    auto structured = [&](ID3D12Resource* r, UINT count, UINT strideBytes, UINT at) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = count ? count : 1;
        sd.Buffer.StructureByteStride = strideBytes;
        dev.device->CreateShaderResourceView(r, &sd, slot(at));
    };
    raw(shared.staticVb, (UINT64)shared.staticVertexCount * shared.stride, kSlotVerts);
    // No dynamic pool yet: point the view at the static buffer, so the table
    // is never holed. No instance selects it until there is one.
    if (shared.dynVb && shared.dynVertexCount)
        raw(shared.dynVb, (UINT64)shared.dynVertexCount * shared.stride, kSlotDynVerts);
    else
        raw(shared.staticVb, (UINT64)shared.staticVertexCount * shared.stride, kSlotDynVerts);
    raw(m_ibAlpha.Get(), (UINT64)alpha.size() * 4, kSlotAlphaIdx);
    structured(m_triMaterial.Get(), (UINT)alphaMat.size(), 4, kSlotTriMat);
    structured(m_triPlant.Get(), (UINT)alphaPlant.size(), 4, kSlotTriPlant);
    structured(shared.materials ? shared.materials.Get() : m_triMaterial.Get(),
               shared.materials ? shared.materialCount : 1, 8, kSlotMaterials);
    for (size_t i = 0; i < shared.textures.size(); ++i)
    {
        ID3D12Resource* r = shared.textures[i];
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = r->GetDesc().Format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = r->GetDesc().MipLevels;
        dev.device->CreateShaderResourceView(r, &sd, slot(kSlotTextures + (UINT)i));
    }

    std::vector<Scene::RtInstanceDesc> all = instances;
    for (uint64_t k : held) all.push_back(Scene::RtInstanceDesc{ k });
    const bool ok = UpdateInstances(dev, list, fixed, fixedRevision, fixedMasks, all);

    Log("rt: published %zu BLAS(es)%s: %u instance(s) in the TLAS, %zu alpha-tested triangles for the any-hit test, "
        "%zu textures - %.1f ms", published.size(), held.empty() ? "" : " (the old world kept while the new builds)",
        m_instanceCount, alphaMat.size(), shared.textures.size(), MsSince(t0));
    return ok;
}

void Rt::FollowDynamicPool(ID3D12Device* dev, ID3D12Resource* dynVb, uint32_t dynVertexCount)
{
    if (!m_heap || !m_staticVb) return;
    ID3D12Resource* r = dynVb && dynVertexCount ? dynVb : m_staticVb;   // never a holed table (Rebuild)
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.NumElements = (UINT)(r->GetDesc().Width / 4);
    sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)kSlotDynVerts * m_stride;
    dev->CreateShaderResourceView(r, &sd, h);
    m_dynVb = dynVb;
}

// How far away a newly built mesh "was" last frame (UpdateInstances): far beyond any penumbra, so no history.
constexpr float kRtAppearedFrom = 1.0e4f;

// The TLAS and the instance table only. What a drag or a driving car costs: the
// TLAS rebuilt whole - FAST_BUILD, because it is rebuilt far more often than
// any single version of it is traced. Not refitted (ALLOW_UPDATE +
// PERFORM_UPDATE): measured (design 4.2), a refit saved 0.25 ms of a 0.4 ms
// build and the stretched boxes around a fast car cost 1-2 ms of tracing.
bool Rt::UpdateInstances(Device& dev, ID3D12GraphicsCommandList* list, const std::vector<Scene::RtInstanceDesc>& fixed,
                         uint32_t fixedRevision, const std::vector<std::pair<uint32_t, uint32_t>>& fixedMasks,
                         const std::vector<Scene::RtInstanceDesc>& instances)
{
    if (!Ready() || !m_heap) return false;
    ComPtr<ID3D12Device5> dev5;
    if (FAILED(dev.device.As(&dev5))) return false;

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs;
    std::vector<InstInfo> info;
    // The per-frame part's BLAS lookups, by position: its order holds from frame to frame, so a row whose key is
    // last frame's (and the BLAS set unchanged) takes last frame's entry - a thousand map lookups were 0.3 ms.
    // (And its GPU address, a call into the runtime per row.)
    if (m_restBlasRev != m_blasRev)
    {
        m_restKeys.clear();
        m_restEntries.clear();
        m_restVa.clear();
        m_restBlasRev = m_blasRev;
    }
    m_restKeys.resize(instances.size(), ~0ull);
    m_restEntries.resize(instances.size(), nullptr);
    m_restVa.resize(instances.size(), 0);
    auto lookup = [&](uint64_t key, size_t row, D3D12_GPU_VIRTUAL_ADDRESS& va) -> const BlasEntry* {
        if (row != ~size_t(0) && m_restKeys[row] == key) { va = m_restVa[row]; return m_restEntries[row]; }
        auto it = m_blas.find(key);
        const BlasEntry* e = it == m_blas.end() || !it->second.as ? nullptr : &it->second;
        va = e ? e->Va() : 0;
        if (row != ~size_t(0)) m_restKeys[row] = key, m_restEntries[row] = e, m_restVa[row] = va;
        return e;
    };
    descs.reserve(fixed.size() + instances.size());
    info.reserve(fixed.size() + instances.size());
    auto convert = [&](const Scene::RtInstanceDesc& d, uint32_t idBase, bool withAppeared, size_t row = ~size_t(0)) {
        D3D12_GPU_VIRTUAL_ADDRESS va = 0;
        const BlasEntry* e = lookup(d.mesh, row, va);
        if (!e) return;
        D3D12_RAYTRACING_INSTANCE_DESC id = {};
        ToInstanceTransform(d.xform, id.Transform);
        id.InstanceID = idBase + (UINT)descs.size();  // indexes the table below
        id.InstanceMask = d.mask ? d.mask : Scene::kRtMaskAll | (d.focus ? Scene::kRtMaskFocus : 0u);
        id.Flags = d.forceOpaque ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE
                                 : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        id.AccelerationStructure = va;
        descs.push_back(id);
        // A mesh whose BLAS landed since the last table (Land) is NEW to the rays: what they hit there was nothing
        // last frame. It goes in as an occluder that was far away (kRtAppearedFrom), so Allow gives the pixels whose
        // rays hit it no history - its shadow and AO come in at once instead of fading in over the old "unoccluded"
        // (design 4.5). One frame: the next table (InstancesStale) is the ordinary one.
        const bool appeared = withAppeared && std::find(m_appeared.begin(), m_appeared.end(), d.mesh) != m_appeared.end();
        InstInfo in = { e->triBase, e->vertexBase, e->vbSelect, d.material,
                        d.moved || appeared ? 1u : 0u };
        for (int r = 0; r < 4; ++r)                  // the rows' xyz (row vectors)
            for (int c = 0; c < 3; ++c) in.was[r * 3 + c] = d.prevXform[r * 4 + c];
        if (appeared)
            for (int c = 0; c < 3; ++c) in.was[9 + c] += kRtAppearedFrom;
        in.owner = d.owner;
        info.push_back(in);
    };
    // The fixed part (the placements): converted again only when they or the BLAS set changed. A frame with meshes
    // just landed flags them (appeared) in a converted copy, written to this slot alone.
    if (fixedRevision != m_fixedRevision || m_blasRev != m_fixedBlasRev)
    {
        descs.reserve(fixed.size());
        info.reserve(fixed.size());
        m_fixedOut.assign(fixed.size(), ~0u);
        for (size_t i = 0; i < fixed.size(); ++i)
        {
            const size_t before = descs.size();
            convert(fixed[i], 0, false);
            if (descs.size() != before) m_fixedOut[i] = (uint32_t)before;
        }
        m_fixedDescs.swap(descs);
        m_fixedInfo.swap(info);
        descs.clear();
        info.clear();
        m_fixedRevision = fixedRevision;
        m_fixedBlasRev = m_blasRev;
        ++m_fixedStamp;
    }
    const uint32_t nFixed = (uint32_t)m_fixedDescs.size();
    std::vector<InstInfo> fixedFlagged;   // the fixed part's info with this frame's appeared meshes flagged
    if (!m_appeared.empty())
    {
        bool any = false;
        for (size_t i = 0; i < fixed.size() && !any; ++i)
            any = std::find(m_appeared.begin(), m_appeared.end(), fixed[i].mesh) != m_appeared.end();
        if (any)
        {
            for (const Scene::RtInstanceDesc& d : fixed) convert(d, 0, true);
            fixedFlagged.swap(info);
            descs.clear();
            info.clear();
        }
    }
    for (size_t i = 0; i < instances.size(); ++i) convert(instances[i], nFixed, true, i);
    m_appearedFlagged = !m_appeared.empty();
    m_appeared.clear();
    const size_t total = nFixed + descs.size();
    if (total == 0) { DeferRelease(m_tlas.Get()); m_tlas.Reset(); m_instanceCount = 0; return false; }

    // The two tables go into upload buffers kept from frame to frame (mapped once, a quarter to spare): creating them
    // every frame was 2 ms of the 3.5 a moving car's TLAS cost. One per frame slot: overwriting is safe, as the frame
    // that last used this slot's has run (Device::BeginFrame). A grown one replaces it - deferred, not freed at once.
    const uint32_t fs = FrameSlot();
    // The fixed part is written only when this slot's copy is not of this stamp (or carries appeared flags): a new
    // or grown buffer, a changed fixed part, or a frame with landed meshes among the placements.
    const bool flagged = !fixedFlagged.empty();
    const bool grow = !m_instancesSlot[fs] ||
                      m_instancesSlot[fs]->GetDesc().Width < total * sizeof(D3D12_RAYTRACING_INSTANCE_DESC) ||
                      !m_instInfoSlot[fs] || m_instInfoSlot[fs]->GetDesc().Width < total * sizeof(InstInfo);
    const bool writeFixed = grow || flagged || m_slotFixedStamp[fs] != m_fixedStamp;
    auto fill = [&](ComPtr<ID3D12Resource>& buf, void*& mapped, const void* head, size_t headBytes, const void* tail,
                    size_t tailBytes) {
        const size_t bytes = headBytes + tailBytes;
        if (!buf || buf->GetDesc().Width < bytes)
        {
            mapped = nullptr;
            buf = MakeBuffer(dev.device.Get(), bytes + bytes / 4, D3D12_HEAP_TYPE_UPLOAD,
                             D3D12_RESOURCE_STATE_GENERIC_READ);
            D3D12_RANGE none = { 0, 0 };
            if (!buf || FAILED(buf->Map(0, &none, &mapped))) { buf.Reset(); mapped = nullptr; return false; }
        }
        if (writeFixed && headBytes) memcpy(mapped, head, headBytes);
        if (tailBytes) memcpy(static_cast<uint8_t*>(mapped) + headBytes, tail, tailBytes);
        return true;
    };
    if (!fill(m_instancesSlot[fs], m_instancesSlotMapped[fs], m_fixedDescs.data(),
              nFixed * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), descs.data(), descs.size() * sizeof(descs[0])) ||
        !fill(m_instInfoSlot[fs], m_instInfoSlotMapped[fs], flagged ? fixedFlagged.data() : m_fixedInfo.data(),
              nFixed * sizeof(InstInfo), info.data(), info.size() * sizeof(info[0])))
    {
        m_slotFixedStamp[fs] = 0;
        return false;
    }
    m_slotFixedStamp[fs] = flagged ? 0 : m_fixedStamp;   // (a flagged copy is rewritten the next time round)
    // This frame's masks over the fixed part, in the slot's own copy: last time's patches undone (unless the part was
    // just written whole), this frame's applied. A few dozen rows, where rewriting the part was 11k.
    {
        auto* rows = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(m_instancesSlotMapped[fs]);
        std::vector<uint32_t>& patched = m_slotPatched[fs];
        if (!writeFixed)
            for (uint32_t r : patched)
                if (r < nFixed) rows[r].InstanceMask = m_fixedDescs[r].InstanceMask;
        patched.clear();
        for (const auto& m : fixedMasks)
        {
            if (m.first >= m_fixedOut.size() || m_fixedOut[m.first] == ~0u) continue;
            const uint32_t r = m_fixedOut[m.first];
            rows[r].InstanceMask = m.second;
            patched.push_back(r);
        }
    }
    m_instances = m_instancesSlot[fs], m_instancesMapped = m_instancesSlotMapped[fs];
    m_instInfo = m_instInfoSlot[fs], m_instInfoMapped = m_instInfoSlotMapped[fs];

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top = {};
    top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    top.NumDescs = (UINT)total;
    top.InstanceDescs = m_instances->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo = {};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&top, &topInfo);
    const UINT64 scratchBytes = topInfo.ScratchDataSizeInBytes;

    // A quarter to spare: the count goes up and down by a few as car parts land and leave.
    if (!m_tlas || m_tlas->GetDesc().Width < topInfo.ResultDataMaxSizeInBytes)
    {
        DeferRelease(m_tlas.Get());   // a frame in flight may still trace the old one (device.h)
        m_tlas = MakeBuffer(dev.device.Get(), topInfo.ResultDataMaxSizeInBytes + topInfo.ResultDataMaxSizeInBytes / 4,
                            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    if (!m_scratch || m_scratch->GetDesc().Width < scratchBytes)
    {
        DeferRelease(m_scratch.Get());
        m_scratch = MakeBuffer(dev.device.Get(), scratchBytes + scratchBytes / 4, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_COMMON,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    if (!m_tlas || !m_scratch) return false;

    // Recorded in the FRAME's list, before its first ray (the prepass is next; the traces come after it): no
    // secondary context, no wait - the CPU goes on recording while the GPU builds. The UAV barrier orders the build
    // before every read of the TLAS in this list (the sun, AO, lamps, NRD's and DLSS RR's traces, the reflection rays).
    ComPtr<ID3D12GraphicsCommandList4> list4;
    if (!list || FAILED(list->QueryInterface(IID_PPV_ARGS(&list4)))) return false;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td = {};
    td.Inputs = top;
    td.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
    td.DestAccelerationStructureData = m_tlas->GetGPUVirtualAddress();
    list4->BuildRaytracingAccelerationStructure(&td, 0, nullptr);
    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = m_tlas.Get();
    list4->ResourceBarrier(1, &uav);

    auto slot = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * m_stride;
        return h;
    };
    // The TLAS descriptor has no resource: a raytracing SRV names a GPU address.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.RaytracingAccelerationStructure.Location = m_tlas->GetGPUVirtualAddress();
        dev.device->CreateShaderResourceView(nullptr, &sd, slot(kSlotTlas));
    }
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = (UINT)total;
        sd.Buffer.StructureByteStride = sizeof(InstInfo);
        dev.device->CreateShaderResourceView(m_instInfo.Get(), &sd, slot(kSlotInst));
    }
    m_instanceCount = (uint32_t)total;
    return true;
}

bool Rt::CreateTargets(ID3D12Device* dev, UINT width, UINT height)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // The filtered histories, RGBA16F: filtered visibility, the NDC depth it was computed at - so next frame can tell
    // whether a reprojected sample landed on the same surface - and two flags (a ray hit a moved object; the pixel is
    // a moved object's), see rt_denoise.hlsl.
    D3D12_RESOURCE_DESC hd2 = d;
    hd2.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    auto make = [&](const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out) {
        out.Reset();
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                      IID_PPV_ARGS(&out)));
    };
    const D3D12_RESOURCE_STATES uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (!make(d, uav, m_result) || !make(d, uav, m_aoResult) || !make(d, uav, m_lightResult) ||
        !make(d, uav, m_aoWide) || !make(d, uav, m_lightWide))
        return false;
    for (int i = 0; i < 2; ++i)
    {
        if (!make(hd2, srv, m_hist[i]) || !make(hd2, srv, m_aoHist[i]) || !make(hd2, srv, m_lightHist[i]))
            return false;
        m_histState[i] = m_aoHistState[i] = m_lightHistState[i] = srv;
    }
    m_aoWideState = m_lightWideState = uav;
    m_histValid = m_aoHistValid = m_lightHistValid = false;
    m_width = width;
    m_height = height;
    // NRD's inputs, once NRD has been chosen (EnableNrdInputs).
    if (m_psoNrd && !CreateNrdTargets(dev)) return false;
    // Without a heap yet (PrepareTargets before the first Rebuild), Rebuild writes the views.
    if (m_heap && !WriteTargetViews(dev)) return false;

    m_resultReadable = false;
    return true;
}

bool Rt::WriteTargetViews(ID3D12Device* dev)
{
    if (!m_heap || !m_result) return false;
    auto at = [&](UINT slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)slot * m_stride;
        return h;
    };
    auto uav2d = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u = {};
        u.Format = f;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(r, nullptr, &u, at(slot));
    };
    auto srv2d = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = f;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(r, &v, at(slot));
    };
    auto counters = [&](ID3D12Resource* r, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC c = {};
        c.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        c.Buffer.NumElements = 4;
        c.Buffer.StructureByteStride = sizeof(uint32_t);
        dev->CreateUnorderedAccessView(r, nullptr, &c, at(slot));
    };
    const DXGI_FORMAT r16 = DXGI_FORMAT_R16_FLOAT, rgba16 = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // The sun: the raw output, and the raw visibility as an SRV for the denoiser - twice, at the head of each
    // [raw, hist] pair - then the histories' SRVs (second in each pair) and UAVs.
    uav2d(m_result.Get(), r16, kSlotOut);
    srv2d(m_result.Get(), r16, kSlotPair0);
    srv2d(m_result.Get(), r16, kSlotPair1);
    counters(m_counters.Get(), kSlotCounters);
    // The same again for ambient occlusion: its raw output (u0) followed by counters (u1).
    uav2d(m_aoResult.Get(), r16, kSlotAoOut);
    counters(m_aoCounters.Get(), kSlotAoCounters);
    srv2d(m_aoResult.Get(), r16, kSlotAoPair0);
    srv2d(m_aoResult.Get(), r16, kSlotAoPair1);
    // And for the local lights.
    uav2d(m_lightResult.Get(), r16, kSlotLightOut);
    counters(m_lightCounters.Get(), kSlotLightCounters);
    srv2d(m_lightResult.Get(), r16, kSlotLightPair0);
    srv2d(m_lightResult.Get(), r16, kSlotLightPair1);
    for (int i = 0; i < 2; ++i)
    {
        srv2d(m_hist[i].Get(), rgba16, (i == 0 ? kSlotPair0 : kSlotPair1) + 1);
        uav2d(m_hist[i].Get(), rgba16, i == 0 ? kSlotHistUav0 : kSlotHistUav1);
        srv2d(m_aoHist[i].Get(), rgba16, (i == 0 ? kSlotAoPair0 : kSlotAoPair1) + 1);
        uav2d(m_aoHist[i].Get(), rgba16, i == 0 ? kSlotAoHistUav0 : kSlotAoHistUav1);
        srv2d(m_lightHist[i].Get(), rgba16, (i == 0 ? kSlotLightPair0 : kSlotLightPair1) + 1);
        uav2d(m_lightHist[i].Get(), rgba16, i == 0 ? kSlotLightHistUav0 : kSlotLightHistUav1);
    }
    // The wide passes' outputs (rt_ao_wide.hlsl): visibility only, read by the scene like the histories.
    uav2d(m_aoWide.Get(), r16, kSlotAoWideUav);
    uav2d(m_lightWide.Get(), r16, kSlotLightWideUav);
    // NRD's inputs (u2 of the NRD variants), once made.
    if (m_nrdIn[0])
    {
        uav2d(m_nrdIn[0].Get(), r16, kSlotSunNrdUav);
        uav2d(m_nrdIn[1].Get(), r16, kSlotAoNrdUav);
        uav2d(m_nrdIn[2].Get(), r16, kSlotLightNrdUav);
    }
    return true;
}

void Rt::PrepareForRead(ID3D12GraphicsCommandList* list)
{
    // The filtered history is already left in PIXEL_SHADER_RESOURCE by the
    // denoiser; nothing to do. Kept so callers do not have to know that.
    (void)list;
    m_resultReadable = m_filtered != nullptr;
}

void Rt::Trace(Device& dev, ID3D12GraphicsCommandList* list, ID3D12Resource* depth,
               ID3D12Resource* normal, ID3D12Resource* motion, ID3D12Resource* plant,
               const float* viewProj, const float* unjitteredViewProj,
               const float* camPos, const float* sunTowards, float tMax, float sunAngle,
               bool ao, float aoRadius, uint32_t aoRays, UINT width, UINT height, const Lights* lights)
{
    if (!Ready() || !HasScene() || !m_heap || !depth || !normal || !viewProj) return;
    ReadStats();   // this frame slot's last trace (it has run: Device::BeginFrame)
    if (width != m_width || height != m_height)
    {
        LARGE_INTEGER tt;
        QueryPerformanceCounter(&tt);
        const bool made = CreateTargets(dev.device.Get(), width, height);
        Log("rt: targets %ux%u made in the frame (%.0f ms)", width, height, MsSince(tt));
        if (!made) return;
    }

    TraceCB cb = {};
    if (!Invert(viewProj, cb.invViewProj)) return;
    for (int k = 0; k < 3; ++k) cb.sunTowards[k] = sunTowards[k];
    cb.tMax = tMax;
    cb.size[0] = width;
    cb.size[1] = height;
    // In METRES along the ray, not in shadow-map texels: there is no texel here,
    // and the reconstruction error this has to clear is a depth-buffer
    // quantisation, not a projection footprint.
    // In METRES along the normal. Small, because the normal offset clears the
    // depth quantisation far more efficiently than a step along the ray did;
    // the distance term in the shader grows it for the far field.
    cb.bias = 0.02f;
    cb.stride = m_vertexStride;
    for (int k = 0; k < 3; ++k) cb.camPos[k] = camPos ? camPos[k] : 0.0f;
    cb.sunAngle = sunAngle;
    cb.frame = m_frame++;
    cb.aoRadius = aoRadius;
    cb.aoRays = aoRays ? aoRays : 1;
    const bool doLights = lights && lights->Active() && m_psoLights && m_lightResult;
    if (lights)
    {
        for (int k = 0; k < 3; ++k) cb.camFwd[k] = lights->CamFwd()[k];
        cb.clusterParams[0] = lights->TileW();
        cb.clusterParams[1] = lights->TileH();
        cb.clusterParams[2] = Lights::kNear;
        cb.clusterParams[3] = lights->SliceScale();
    }
    cb.lightsOn = doLights ? 1.0f : 0.0f;

    // Depth becomes a NON-pixel shader resource: this is a compute dispatch, and
    // naming the wrong stage is a validation error rather than a wrong picture.
    D3D12_RESOURCE_BARRIER pre[4] = {
        Transition(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(normal, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    UINT nPre = 2;
    if (motion) pre[nPre++] = Transition(motion, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (plant)  pre[nPre++] = Transition(plant, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(nPre, pre);

    // The motion view, or a null one (reads zero: nothing moved), so the
    // denoiser's table is never an unwritten descriptor.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC md = {};
        md.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        md.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        md.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        md.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE mh = m_heap->GetCPUDescriptorHandleForHeapStart();
        mh.ptr += (SIZE_T)kSlotMotion * m_stride;
        dev.device->CreateShaderResourceView(motion, &md, mh);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
        nd.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE nh = m_heap->GetCPUDescriptorHandleForHeapStart();
        nh.ptr += (SIZE_T)kSlotNormal * m_stride;
        dev.device->CreateShaderResourceView(normal, &nd, nh);
    }
    // The plant per pixel, or a null view (reads 0: no plants).
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC pd = {};
        pd.Format = DXGI_FORMAT_R32_UINT;
        pd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        pd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        pd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE ph = m_heap->GetCPUDescriptorHandleForHeapStart();
        ph.ptr += (SIZE_T)kSlotPlantId * m_stride;
        dev.device->CreateShaderResourceView(plant, &pd, ph);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC dd = {};
    dd.Format = DXGI_FORMAT_R32_FLOAT;
    dd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    dd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE dh = m_heap->GetCPUDescriptorHandleForHeapStart();
    dh.ptr += (SIZE_T)kSlotDepth * m_stride;
    dev.device->CreateShaderResourceView(depth, &dd, dh);

    list->CopyBufferRegion(m_counters.Get(), 0, m_countersZero.Get(), 0, 4 * sizeof(uint32_t));
    D3D12_RESOURCE_BARRIER toUav = Transition(m_counters.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &toUav);


    // Every view of this frame written (above, and earlier in the frame: the TLAS, the instances, the pool): into
    // this frame slot's half, which is what the frame binds.
    Publish(dev.device.Get());
    ID3D12DescriptorHeap* heaps[] = { m_gpuHeap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_rootSig.Get());
    list->SetPipelineState(m_pso.Get());
    auto gpuSlot = [&](UINT slot) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = GpuBase();
        h.ptr += (UINT64)slot * m_stride;
        return h;
    };
    list->SetComputeRootDescriptorTable(0, GpuBase());
    list->SetComputeRootDescriptorTable(1, gpuSlot(kSlotOut));
    list->SetComputeRoot32BitConstants(2, sizeof(TraceCB) / 4, &cb, 0);
    // THE NRD PATH (SetNrdInputs): the variants that also write NRD's inputs, and none of our denoisers below.
    const bool nrd = m_nrdMode && !m_rawMode && m_psoNrd && m_nrdIn[0];
    if (nrd)
    {
        list->SetPipelineState(m_psoNrd.Get());
        list->SetComputeRootDescriptorTable(6, gpuSlot(kSlotSunNrdUav));
    }

    m_timestamps.Stamp(list, 0);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    m_timestamps.Stamp(list, 1);

    // AMBIENT OCCLUSION, on the same acceleration structure and the same
    // surface reconstruction, into its own output. Timed together with the
    // shadow: one number for "what the ray tracing costs".
    const bool doAo = ao && m_psoAo && m_aoResult;
    // A frame without AO leaves its history describing an older camera: drop
    // it, so switching AO back on converges from fresh rather than smearing a
    // reprojection several frames stale.
    if (!doAo) m_aoHistValid = false;
    if (doAo)
    {
        list->SetPipelineState(nrd && m_nrdAo ? m_psoAoNrd.Get() : m_psoAo.Get());
        list->SetComputeRootDescriptorTable(1, gpuSlot(kSlotAoOut));
        if (nrd && m_nrdAo) list->SetComputeRootDescriptorTable(6, gpuSlot(kSlotAoNrdUav));
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    }
    m_timestamps.Stamp(list, 2);

    // LOCAL LIGHTS: one ray per pixel to one of them (rt_lights.hlsl), into their own output.
    if (!doLights) m_lightHistValid = false;
    if (doLights)
    {
        list->SetPipelineState(nrd ? m_psoLightsNrd.Get() : m_psoLights.Get());
        list->SetComputeRootDescriptorTable(1, gpuSlot(kSlotLightOut));
        if (nrd) list->SetComputeRootDescriptorTable(6, gpuSlot(kSlotLightNrdUav));
        list->SetComputeRootShaderResourceView(3, lights->LightsVa());
        list->SetComputeRootShaderResourceView(4, lights->ClustersVa());
        list->SetComputeRootShaderResourceView(5, lights->IndicesVa());
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    }
    m_timestamps.Stamp(list, 3);

    // ---- denoise, once per feature ------------------------------------------
    DenoiseCB dc = {};
    {
        float inv[16];
        bool ok = unjitteredViewProj && Invert(unjitteredViewProj, inv);
        if (ok) MatMul(inv, m_prevViewProj, dc.reproj);
        dc.size[0] = width; dc.size[1] = height;
        dc.blend = 0.85f;
        dc.valid = ok ? 1.0f : 0.0f;
        dc.motionOn = motion ? 1.0f : 0.0f;
        // Where a moved object was hit: enough of the past to keep the
        // car's own shadow from boiling, little enough that a stale one is
        // gone in two or three frames (0.25^3 < 2%).
        dc.blendMoved = 0.25f;
        // A pixel still on the screen: three times the samples (see rt_denoise.hlsl's STILL).
        dc.blendStill = 0.95f;
        // The sun's and the lamps' samples are stratified over 4x4 tiles (StratU, StratDisc), as the AO's.
        dc.period4 = 1.0f;
        if (unjitteredViewProj) memcpy(m_prevViewProj, unjitteredViewProj, sizeof(m_prevViewProj));
    }
    m_tracedAo = doAo;
    m_tracedLights = doLights;
    if (m_rawMode)
    {
        // RR denoises the lit colour (SetRawOnly): ours keep no history meanwhile.
        m_filtered = m_filteredAo = m_filteredLights = nullptr;
        m_histValid = m_aoHistValid = m_lightHistValid = false;
    }
    else if (nrd)
    {
        // NrdVisibility denoises the raw traces and NRD's inputs (NrdInput): ours keep no history meanwhile, and start
        // over when they are back - but for the AO when NRD takes the shadows only (SetNrdInputs's `ao`).
        m_filtered = m_filteredLights = nullptr;
        m_histValid = m_lightHistValid = false;
        if (m_nrdAo) m_aoHistValid = false;
        DenoiseAll(list, dc, false, doAo && !m_nrdAo, false);
    }
    else
        DenoiseAll(list, dc, true, doAo, doLights);
    m_timestamps.Stamp(list, 4);
    m_timestamps.Resolve(list, 0, 5);

    D3D12_RESOURCE_BARRIER toCopy = Transition(m_counters.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &toCopy);
    list->CopyBufferRegion(m_countersReadback.Get(), (UINT64)FrameSlot() * 4 * sizeof(uint32_t), m_counters.Get(), 0,
                           4 * sizeof(uint32_t));
    m_countersPending[FrameSlot()] = true;
    D3D12_RESOURCE_BARRIER back = Transition(m_counters.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &back);

    D3D12_RESOURCE_BARRIER post[4] = {
        Transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_DEPTH_WRITE),
        Transition(normal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    UINT nPost = 2;
    if (motion) post[nPost++] = Transition(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (plant)  post[nPost++] = Transition(plant, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(nPost, post);
}

// Our denoisers over this frame's traces (Trace, unless NRD takes them): the sun's, the AO's and its wide pass, the
// local lights' and theirs.
void Rt::DenoiseAll(ID3D12GraphicsCommandList* list, const DenoiseCB& dc, bool doSun, bool doAo, bool doLights)
{
    // NGG_RT_NO_HISTORY=1: the sun's and the AO's denoise without their history nor the AO's wide pass - this frame's
    // 5x5 alone: grainy, but unbiased, the reference a ghost (anything a history carries that this frame's rays do not
    // say) is measured against (tools/car-ghost).
    static const bool noHistory = [] { const char* e = std::getenv("NGG_RT_NO_HISTORY"); return e && *e && *e != '0'; }();
    DenoiseCB dcSun = dc;
    if (noHistory) dcSun.valid = 0.0f;
    if (doSun)
        Denoise(list, dcSun, m_result.Get(), kSlotPair0, kSlotPair1, kSlotHistUav0, kSlotHistUav1,
                m_hist, m_histState, m_histValid, m_filtered);
    DenoiseCB dcAo = dc;
    // AoSample's stratification. And the history stays 0.85: 0.92 was smoother
    // standing still but lagged - noisier and further from a 32-ray reference -
    // once the car (and the camera) drove.
    dcAo.period4 = 1.0f;
    // Where its rays hit an object on the move, the AO keeps only what they allow (Allow, kAoPenumbra) - not, as the
    // sun's, a quarter at least: behind a driving car that quarter alone left the road 7 grey darker than its rays,
    // the bumper's and the exhaust's ghosts still there (tools/car-ghost). What the cut leaves grainy, the wide pass
    // smooths (frame-to-frame change behind the car +9%).
    dcAo.blendMoved = 0.0f;
    if (noHistory) dcAo.valid = 0.0f;
    if (doAo)
    {
        Denoise(list, dcAo, m_aoResult.Get(), kSlotAoPair0, kSlotAoPair1, kSlotAoHistUav0, kSlotAoHistUav1,
                m_aoHist, m_aoHistState, m_aoHistValid, m_filteredAo);
        if (m_psoAoWide && m_aoWide && !noHistory)
            Widen(list, dcAo, m_aoHist, m_aoHistState, kSlotAoPair0, kSlotAoPair1, m_aoWide.Get(), m_aoWideState,
                  kSlotAoWideUav, m_filteredAo);
    }
    else
        m_filteredAo = nullptr;
    // The local lights' fraction: denoised as the sun's visibility is (the same constants: it is the same kind of
    // signal, a binary coin per pixel whose mean is smooth).
    if (doLights)
        Denoise(list, dc, m_lightResult.Get(), kSlotLightPair0, kSlotLightPair1, kSlotLightHistUav0,
                kSlotLightHistUav1, m_lightHist, m_lightHistState, m_lightHistValid, m_filteredLights);
    // And widened where their history was cut, as the AO is: under the Garage's turning car, the floor's
    // spotlit shadow - one ray a pixel among three lamps, its rays' hits right above it allowing little
    // history (Allow) - stayed grainy, and those penumbrae are wider than the pass's 13 pixels anyway.
    if (doLights && m_psoAoWide && m_lightWide)
        Widen(list, dc, m_lightHist, m_lightHistState, kSlotLightPair0, kSlotLightPair1, m_lightWide.Get(),
              m_lightWideState, kSlotLightWideUav, m_filteredLights);
    else
        m_filteredLights = nullptr;
}

// Spatial + temporal filter of one binary-per-frame ray result; see
// rt_denoise.hlsl. Generic over which pair of history textures it works on, so the
// shadow and the AO share the shader, the root signature and the reasoning.
void Rt::Denoise(ID3D12GraphicsCommandList* list, const DenoiseCB& base, ID3D12Resource* raw,
                 UINT pair0, UINT pair1, UINT uav0, UINT uav1,
                 Microsoft::WRL::ComPtr<ID3D12Resource>* hist, D3D12_RESOURCE_STATES* histState,
                 bool& histValid, ID3D12Resource*& filtered)
{
    const int write = (int)((m_frame - 1) & 1), read = write ^ 1;   // m_frame already advanced
    D3D12_RESOURCE_BARRIER pre[2] = {
        Transition(raw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(hist[write].Get(), histState[write], D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(2, pre);
    histState[write] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // Readable by pixel shaders too: TAA compares this frame's AO with it (AoPrevDenoised).
    const D3D12_RESOURCE_STATES readState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (histState[read] != readState)
    {
        D3D12_RESOURCE_BARRIER b = Transition(hist[read].Get(), histState[read], readState);
        list->ResourceBarrier(1, &b);
        histState[read] = readState;
    }

    DenoiseCB dc = base;
    dc.valid = (base.valid > 0.5f && histValid) ? 1.0f : 0.0f;

    auto gpu = [&](UINT slot) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = GpuBase();   // this frame slot's half (Publish)
        h.ptr += (UINT64)slot * m_stride;
        return h;
    };
    list->SetComputeRootSignature(m_denoiseRootSig.Get());
    list->SetPipelineState(m_psoDenoise.Get());
    list->SetComputeRootDescriptorTable(0, gpu(read == 0 ? pair0 : pair1));
    list->SetComputeRootDescriptorTable(1, gpu(kSlotDepth));
    list->SetComputeRootDescriptorTable(2, gpu(write == 0 ? uav0 : uav1));
    list->SetComputeRoot32BitConstants(3, sizeof(DenoiseCB) / 4, &dc, 0);
    list->SetComputeRootDescriptorTable(4, gpu(kSlotMotion));
    list->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER post[2] = {
        Transition(raw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(hist[write].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(2, post);
    histState[write] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    filtered = hist[write].Get();
    histValid = true;
}

// rt_ao_wide.hlsl over this frame's denoised result (the history just written, of
// the pair pair0/pair1), into `wide`, which then is what the scene reads.
void Rt::Widen(ID3D12GraphicsCommandList* list, const DenoiseCB& dc,
               Microsoft::WRL::ComPtr<ID3D12Resource>* hists, D3D12_RESOURCE_STATES* histState,
               UINT pair0, UINT pair1, ID3D12Resource* wide, D3D12_RESOURCE_STATES& wideState, UINT uav,
               ID3D12Resource*& filtered)
{
    const int write = (int)((m_frame - 1) & 1);
    ID3D12Resource* hist = hists[write].Get();
    D3D12_RESOURCE_BARRIER pre[2] = {
        Transition(hist, histState[write], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(wide, wideState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(wideState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ? 1 : 2, pre);

    auto gpu = [&](UINT slot) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = GpuBase();   // this frame slot's half (Publish)
        h.ptr += (UINT64)slot * m_stride;
        return h;
    };
    list->SetComputeRootSignature(m_denoiseRootSig.Get());
    list->SetPipelineState(m_psoAoWide.Get());
    // The pair whose t1 is the history just written.
    list->SetComputeRootDescriptorTable(0, gpu(write == 0 ? pair0 : pair1));
    list->SetComputeRootDescriptorTable(1, gpu(kSlotDepth));
    list->SetComputeRootDescriptorTable(2, gpu(uav));
    list->SetComputeRoot32BitConstants(3, sizeof(DenoiseCB) / 4, &dc, 0);
    list->SetComputeRootDescriptorTable(4, gpu(kSlotMotion));
    list->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER post[2] = {
        Transition(hist, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, histState[write]),
        Transition(wide, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(2, post);
    wideState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    filtered = wide;
}

void Rt::ReadStats()
{
    const uint32_t s = FrameSlot();
    uint32_t* c = nullptr;
    D3D12_RANGE all = { (SIZE_T)s * 4 * sizeof(uint32_t), (SIZE_T)(s + 1) * 4 * sizeof(uint32_t) };
    if (m_countersReadback && m_countersPending[s] &&
        SUCCEEDED(m_countersReadback->Map(0, &all, reinterpret_cast<void**>(&c))))
    {
        m_countersPending[s] = false;
        c += 4 * s;
        m_traced = c[0];
        m_shadowed = c[1];
        m_leafHits = c[2];
        m_leafMisses = c[3];
        D3D12_RANGE none = { 0, 0 };
        m_countersReadback->Unmap(0, &none);
    }

    if (UINT64 t[5]; m_timestampHz && m_timestamps.Take(t))
    {
        const double k = 1000.0 / (double)m_timestampHz;
        if (t[3] > t[0]) m_lastMs = k * (double)(t[3] - t[0]);
        m_aoMs      = t[2] >= t[1] ? k * (double)(t[2] - t[1]) : 0.0;
        m_lightsMs  = t[3] >= t[2] ? k * (double)(t[3] - t[2]) : 0.0;
        m_denoiseMs = t[4] >= t[3] ? k * (double)(t[4] - t[3]) : 0.0;
    }
}

void Rt::Publish(ID3D12Device* dev)
{
    if (!dev || !m_heap || !m_gpuHeap) return;
    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_gpuHeap->GetCPUDescriptorHandleForHeapStart();
    dst.ptr += (SIZE_T)FrameSlot() * m_heapCount * m_stride;
    dev->CopyDescriptorsSimple(m_heapCount, dst, m_heap->GetCPUDescriptorHandleForHeapStart(),
                               D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

D3D12_GPU_DESCRIPTOR_HANDLE Rt::GpuBase() const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_gpuHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)FrameSlot() * m_heapCount * m_stride;
    return h;
}

} // namespace core
} // namespace ngg
