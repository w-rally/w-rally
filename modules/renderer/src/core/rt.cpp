#include "rt.h"
#include "log.h"
#include "shaders.h"

#include <string>
#include <cstdlib>
#include <algorithm>
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

ComPtr<ID3D12Resource> MakeUploadBuffer(ID3D12Device* dev, const void* data, size_t bytes)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes ? bytes : 4;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&r))))
        return nullptr;
    void* p = nullptr;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(r->Map(0, &none, &p))) return nullptr;
    memcpy(p, data, bytes);
    r->Unmap(0, nullptr);
    return r;
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
       kSlotTextures = 38 };

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

    D3D12_ROOT_PARAMETER params[6] = {};
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
    rs.NumParameters = 6;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    {
        Log("rt: root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(dev.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                               IID_PPV_ARGS(&m_rootSig))))
        return false;

    ComPtr<ID3DBlob> cs = LoadShader("rt_trace.hlsl", "CSMain", "cs_6_5");
    if (!cs) return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_rootSig.Get();
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    if (FAILED(dev.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_pso))))
    {
        Log("rt: compute pipeline creation failed - the DXIL was probably rejected");
        return false;
    }

    ComPtr<ID3DBlob> aocs = LoadShader("rt_ao.hlsl", "CSMain", "cs_6_5");
    if (aocs)
    {
        pd.CS = { aocs->GetBufferPointer(), aocs->GetBufferSize() };
        if (FAILED(dev.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_psoAo))))
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
        if (FAILED(dev.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_psoLights))))
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
            FAILED(dev.device->CreateRootSignature(0, dsig->GetBufferPointer(), dsig->GetBufferSize(),
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
        if (FAILED(dev.device->CreateComputePipelineState(&dpd, IID_PPV_ARGS(&m_psoDenoise))))
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
            if (FAILED(dev.device->CreateComputePipelineState(&dpd, IID_PPV_ARGS(&m_psoAoWide))))
                m_psoAoWide.Reset();
        }
        if (!m_psoAoWide) Log("rt: AO wide pass pipeline creation failed - the AO goes unwidened");
    }

    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 5;     // start, after the shadow, after the AO, after the local lights, after the denoise
    dev.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_timestamps));
    m_timestampReadback = MakeBuffer(dev.device.Get(), 5 * sizeof(UINT64),
                                     D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
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
    m_countersReadback = MakeBuffer(dev.device.Get(), 4 * sizeof(uint32_t),
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

    Log("rt: ready - inline RayQuery, tier %d.%d, SM %d.%d",
        dev.rtTier / 10, dev.rtTier % 10, dev.shaderModel >> 4, dev.shaderModel & 0xF);
    return true;
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

bool Rt::Rebuild(Device& dev, const Scene::RtShared& shared,
                 std::vector<Scene::RtMeshData>& newMeshes,
                 const std::vector<uint64_t>& liveKeys,
                 const std::vector<Scene::RtInstanceDesc>& instances)
{
    if (!Ready() || !shared.staticVb) return false;
    ComPtr<ID3D12Device5> dev5;
    if (FAILED(dev.device.As(&dev5))) return false;

    LARGE_INTEGER t0, t1, hz;
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&hz);

    // ---- drop the BLASes nothing uses any more
    for (auto it = m_blas.begin(); it != m_blas.end(); )
    {
        if (std::find(liveKeys.begin(), liveKeys.end(), it->first) == liveKeys.end())
            it = m_blas.erase(it);
        else
            ++it;
    }

    // ---- build the new ones, in one list. Each has its own scratch, so they
    // need no barriers between them; all scratch dies after the wait.
    std::vector<ComPtr<ID3D12Resource>> keep;      // uploads + scratch, alive until EndAndWait
    UINT64 blasBytes = 0;
    uint32_t builtTris = 0;
    ComPtr<ID3D12GraphicsCommandList4> list4;
    if (!newMeshes.empty())
    {
        if (!dev.Begin()) return false;
        if (FAILED(dev.list.As(&list4))) { dev.EndAndWait(); return false; }

        for (Scene::RtMeshData& m : newMeshes)
        {
            ComPtr<ID3D12Resource> ibo = m.opaque.empty() ? nullptr :
                MakeUploadBuffer(dev.device.Get(), m.opaque.data(), m.opaque.size() * 4);
            ComPtr<ID3D12Resource> iba = m.alpha.empty() ? nullptr :
                MakeUploadBuffer(dev.device.Get(), m.alpha.data(), m.alpha.size() * 4);
            keep.push_back(ibo); keep.push_back(iba);

            // Opaque geometry commits on its own; alpha-tested geometry surfaces
            // as a candidate for the any-hit test. PrimitiveIndex counts per
            // geometry, which is what lets the alpha one index from 0.
            D3D12_RAYTRACING_GEOMETRY_DESC geos[2] = {};
            UINT n = 0;
            auto describe = [&](ID3D12Resource* ib, size_t count, bool opaque) {
                if (!ib || !count) return;
                D3D12_RAYTRACING_GEOMETRY_DESC& g = geos[n++];
                g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                g.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                g.Triangles.VertexBuffer.StartAddress = m.vb->GetGPUVirtualAddress() + m.vbOffset;
                g.Triangles.VertexBuffer.StrideInBytes = shared.stride;
                g.Triangles.VertexCount = m.vertexCount;
                g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                g.Triangles.IndexBuffer = ib->GetGPUVirtualAddress();
                g.Triangles.IndexCount = (UINT)count;
                g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            };
            describe(ibo.Get(), m.opaque.size(), true);
            describe(iba.Get(), m.alpha.size(), false);
            if (!n) continue;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
            in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            // PREFER_FAST_TRACE: a BLAS is built when its mesh appears and then
            // traced for as long as it exists - moving it is the TLAS's job.
            in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            in.NumDescs = n;
            in.pGeometryDescs = geos;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
            dev5->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);

            BlasEntry e;
            e.as = MakeBuffer(dev.device.Get(), info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            // COMMON: buffers are created in it whatever is asked, and promote
            // to UNORDERED_ACCESS on first use - asking for UAV only earned a
            // validation warning per rebuild.
            ComPtr<ID3D12Resource> scratch = MakeBuffer(dev.device.Get(), info.ScratchDataSizeInBytes,
                              D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (!e.as || !scratch) { Log("rt: BLAS allocation failed"); continue; }
            keep.push_back(scratch);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
            bd.Inputs = in;
            bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
            bd.DestAccelerationStructureData = e.as->GetGPUVirtualAddress();
            list4->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);

            e.alpha.swap(m.alpha);
            e.alphaMat.swap(m.alphaMat);
            e.alphaPlant.swap(m.alphaPlant);
            if (e.alphaPlant.size() != e.alphaMat.size()) e.alphaPlant.assign(e.alphaMat.size(), 0xFFFFFFFFu);
            e.vbSelect = m.vbSelect;
            e.vertexBase = m.vertexBase;
            blasBytes += info.ResultDataMaxSizeInBytes;
            builtTris += (uint32_t)((m.opaque.size() + e.alpha.size()) / 3);
            m_blas[m.key] = std::move(e);
        }
        dev.EndAndWait();
    }

    // ---- the shared alpha index buffer and per-triangle materials: every
    // live mesh's alpha triangles end to end, each BLAS knowing where its own
    // start. Rebuilt whenever the mesh set changes, which is not per drag.
    std::vector<uint32_t> alpha, alphaMat, alphaPlant;
    for (uint64_t k : liveKeys)
    {
        auto it = m_blas.find(k);
        if (it == m_blas.end()) continue;
        it->second.triBase = (uint32_t)(alphaMat.size());
        alpha.insert(alpha.end(), it->second.alpha.begin(), it->second.alpha.end());
        alphaMat.insert(alphaMat.end(), it->second.alphaMat.begin(), it->second.alphaMat.end());
        alphaPlant.insert(alphaPlant.end(), it->second.alphaPlant.begin(), it->second.alphaPlant.end());
    }
    if (alpha.empty()) { alpha.assign(3, 0); alphaMat.assign(1, 0); alphaPlant.assign(1, 0xFFFFFFFFu); }   // never an empty view
    m_triPlant = MakeUploadBuffer(dev.device.Get(), alphaPlant.data(), alphaPlant.size() * 4);
    m_ibAlpha = MakeUploadBuffer(dev.device.Get(), alpha.data(), alpha.size() * 4);
    m_triMaterial = MakeUploadBuffer(dev.device.Get(), alphaMat.data(), alphaMat.size() * 4);
    m_materials = shared.materials;
    m_vertexStride = shared.stride;
    m_staticVb = shared.staticVb;
    m_dynVb = shared.dynVb;

    // ---- the descriptor heap, sized for THIS scene's textures
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSlotTextures + (UINT)(shared.textures.empty() ? 1 : shared.textures.size());
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    m_heap.Reset();
    if (FAILED(dev.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap)))) return false;
    m_width = m_height = 0;        // the output UAVs live in the new heap: recreate on first Trace

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

    const bool ok = UpdateInstances(dev, instances);

    QueryPerformanceCounter(&t1);
    Log("rt: %zu BLAS(es) built (%u triangles, %.1f MB) in %.0f ms; %zu live, %zu instance(s), "
        "%zu alpha-tested triangles for the any-hit test, %zu textures",
        newMeshes.size(), builtTris, blasBytes / 1048576.0,
        1000.0 * double(t1.QuadPart - t0.QuadPart) / double(hz.QuadPart),
        m_blas.size(), instances.size(), alphaMat.size(), shared.textures.size());
    return ok;
}

// The TLAS and the instance table only. What a drag costs: a handful of
// instances, rebuilt whole - FAST_BUILD, because it is rebuilt far more often
// than any single version of it is traced.
bool Rt::UpdateInstances(Device& dev, const std::vector<Scene::RtInstanceDesc>& instances)
{
    if (!Ready() || !m_heap) return false;
    ComPtr<ID3D12Device5> dev5;
    if (FAILED(dev.device.As(&dev5))) return false;

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs;
    struct InstInfo { uint32_t triBase, vertexBase, vb, material, moved; float was[12]; };
    std::vector<InstInfo> info;
    for (const Scene::RtInstanceDesc& d : instances)
    {
        auto it = m_blas.find(d.mesh);
        if (it == m_blas.end() || !it->second.as) continue;
        D3D12_RAYTRACING_INSTANCE_DESC id = {};
        ToInstanceTransform(d.xform, id.Transform);
        id.InstanceID = (UINT)descs.size();          // indexes the table below
        id.InstanceMask = Scene::kRtMaskAll | (d.focus ? Scene::kRtMaskFocus : 0u);   // (the rays here: 0xFF, all)
        id.Flags = d.forceOpaque ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE
                                 : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        id.AccelerationStructure = it->second.as->GetGPUVirtualAddress();
        descs.push_back(id);
        InstInfo in = { it->second.triBase, it->second.vertexBase, it->second.vbSelect, d.material,
                        d.moved ? 1u : 0u };
        for (int r = 0; r < 4; ++r)                  // the rows' xyz (row vectors)
            for (int c = 0; c < 3; ++c) in.was[r * 3 + c] = d.prevXform[r * 4 + c];
        info.push_back(in);
    }
    if (descs.empty()) { m_tlas.Reset(); return false; }

    m_instances = MakeUploadBuffer(dev.device.Get(), descs.data(), descs.size() * sizeof(descs[0]));
    m_instInfo  = MakeUploadBuffer(dev.device.Get(), info.data(), info.size() * sizeof(info[0]));
    if (!m_instances || !m_instInfo) return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top = {};
    top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    top.NumDescs = (UINT)descs.size();
    top.InstanceDescs = m_instances->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo = {};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&top, &topInfo);

    if (!m_tlas || m_tlas->GetDesc().Width < topInfo.ResultDataMaxSizeInBytes)
        m_tlas = MakeBuffer(dev.device.Get(), topInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!m_scratch || m_scratch->GetDesc().Width < topInfo.ScratchDataSizeInBytes)
        m_scratch = MakeBuffer(dev.device.Get(), topInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_COMMON,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!m_tlas || !m_scratch) return false;

    ComPtr<ID3D12GraphicsCommandList4> list4;
    if (!dev.Begin()) return false;
    if (FAILED(dev.list.As(&list4))) { dev.EndAndWait(); return false; }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td = {};
    td.Inputs = top;
    td.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
    td.DestAccelerationStructureData = m_tlas->GetGPUVirtualAddress();
    list4->BuildRaytracingAccelerationStructure(&td, 0, nullptr);
    dev.EndAndWait();

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
        sd.Buffer.NumElements = (UINT)info.size();
        sd.Buffer.StructureByteStride = sizeof(InstInfo);
        dev.device->CreateShaderResourceView(m_instInfo.Get(), &sd, slot(kSlotInst));
    }
    m_instanceCount = (uint32_t)descs.size();
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

    m_result.Reset();
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&m_result))))
        return false;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.Format = DXGI_FORMAT_R16_FLOAT;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)kSlotOut * m_stride;
    dev->CreateUnorderedAccessView(m_result.Get(), nullptr, &uv, h);

    // Raw visibility as an SRV, for the denoiser - twice, at the head of each
    // [raw, hist] pair.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC rs = {};
        rs.Format = DXGI_FORMAT_R16_FLOAT;
        rs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        rs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        rs.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE a = m_heap->GetCPUDescriptorHandleForHeapStart();
        a.ptr += (SIZE_T)kSlotPair0 * m_stride;
        dev->CreateShaderResourceView(m_result.Get(), &rs, a);
        a = m_heap->GetCPUDescriptorHandleForHeapStart();
        a.ptr += (SIZE_T)kSlotPair1 * m_stride;
        dev->CreateShaderResourceView(m_result.Get(), &rs, a);
    }

    // The filtered history, two of them, RGBA16F: filtered visibility, the
    // NDC depth it was computed at - so next frame can tell whether a
    // reprojected sample landed on the same surface - and two flags (a ray
    // hit a moved object; the pixel is a moved object's), see rt_denoise.hlsl.
    D3D12_RESOURCE_DESC hd2 = d;
    hd2.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    for (int i = 0; i < 2; ++i)
    {
        m_hist[i].Reset();
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &hd2,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(&m_hist[i]))))
            return false;
        m_histState[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        D3D12_SHADER_RESOURCE_VIEW_DESC hs = {};
        hs.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        hs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        hs.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE sh = m_heap->GetCPUDescriptorHandleForHeapStart();
        sh.ptr += (SIZE_T)((i == 0 ? kSlotPair0 : kSlotPair1) + 1) * m_stride;
        dev->CreateShaderResourceView(m_hist[i].Get(), &hs, sh);

        D3D12_UNORDERED_ACCESS_VIEW_DESC hu = {};
        hu.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE uh = m_heap->GetCPUDescriptorHandleForHeapStart();
        uh.ptr += (SIZE_T)(i == 0 ? kSlotHistUav0 : kSlotHistUav1) * m_stride;
        dev->CreateUnorderedAccessView(m_hist[i].Get(), nullptr, &hu, uh);
    }
    m_histValid = false;

    // ---- the same again for ambient occlusion ------------------------------
    m_aoResult.Reset();
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&m_aoResult))))
        return false;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC au = {};
        au.Format = DXGI_FORMAT_R16_FLOAT;
        au.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE ah = m_heap->GetCPUDescriptorHandleForHeapStart();
        ah.ptr += (SIZE_T)kSlotAoOut * m_stride;
        dev->CreateUnorderedAccessView(m_aoResult.Get(), nullptr, &au, ah);

        D3D12_UNORDERED_ACCESS_VIEW_DESC acv = {};
        acv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        acv.Buffer.NumElements = 4;
        acv.Buffer.StructureByteStride = sizeof(uint32_t);
        ah = m_heap->GetCPUDescriptorHandleForHeapStart();
        ah.ptr += (SIZE_T)kSlotAoCounters * m_stride;
        dev->CreateUnorderedAccessView(m_aoCounters.Get(), nullptr, &acv, ah);

        D3D12_SHADER_RESOURCE_VIEW_DESC rs = {};
        rs.Format = DXGI_FORMAT_R16_FLOAT;
        rs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        rs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        rs.Texture2D.MipLevels = 1;
        ah = m_heap->GetCPUDescriptorHandleForHeapStart();
        ah.ptr += (SIZE_T)kSlotAoPair0 * m_stride;
        dev->CreateShaderResourceView(m_aoResult.Get(), &rs, ah);
        ah = m_heap->GetCPUDescriptorHandleForHeapStart();
        ah.ptr += (SIZE_T)kSlotAoPair1 * m_stride;
        dev->CreateShaderResourceView(m_aoResult.Get(), &rs, ah);
    }
    for (int i = 0; i < 2; ++i)
    {
        m_aoHist[i].Reset();
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &hd2,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(&m_aoHist[i]))))
            return false;
        m_aoHistState[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        D3D12_SHADER_RESOURCE_VIEW_DESC hs = {};
        hs.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        hs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        hs.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE sh = m_heap->GetCPUDescriptorHandleForHeapStart();
        sh.ptr += (SIZE_T)((i == 0 ? kSlotAoPair0 : kSlotAoPair1) + 1) * m_stride;
        dev->CreateShaderResourceView(m_aoHist[i].Get(), &hs, sh);

        D3D12_UNORDERED_ACCESS_VIEW_DESC hu = {};
        hu.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE uh = m_heap->GetCPUDescriptorHandleForHeapStart();
        uh.ptr += (SIZE_T)(i == 0 ? kSlotAoHistUav0 : kSlotAoHistUav1) * m_stride;
        dev->CreateUnorderedAccessView(m_aoHist[i].Get(), nullptr, &hu, uh);
    }
    m_aoHistValid = false;

    // ---- and again for the local lights (no wide pass) ----------------------
    m_lightResult.Reset();
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&m_lightResult))))
        return false;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC lu = {};
        lu.Format = DXGI_FORMAT_R16_FLOAT;
        lu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE lh = m_heap->GetCPUDescriptorHandleForHeapStart();
        lh.ptr += (SIZE_T)kSlotLightOut * m_stride;
        dev->CreateUnorderedAccessView(m_lightResult.Get(), nullptr, &lu, lh);

        D3D12_UNORDERED_ACCESS_VIEW_DESC lcv = {};
        lcv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        lcv.Buffer.NumElements = 4;
        lcv.Buffer.StructureByteStride = sizeof(uint32_t);
        lh = m_heap->GetCPUDescriptorHandleForHeapStart();
        lh.ptr += (SIZE_T)kSlotLightCounters * m_stride;
        dev->CreateUnorderedAccessView(m_lightCounters.Get(), nullptr, &lcv, lh);

        D3D12_SHADER_RESOURCE_VIEW_DESC rs = {};
        rs.Format = DXGI_FORMAT_R16_FLOAT;
        rs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        rs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        rs.Texture2D.MipLevels = 1;
        lh = m_heap->GetCPUDescriptorHandleForHeapStart();
        lh.ptr += (SIZE_T)kSlotLightPair0 * m_stride;
        dev->CreateShaderResourceView(m_lightResult.Get(), &rs, lh);
        lh = m_heap->GetCPUDescriptorHandleForHeapStart();
        lh.ptr += (SIZE_T)kSlotLightPair1 * m_stride;
        dev->CreateShaderResourceView(m_lightResult.Get(), &rs, lh);
    }
    for (int i = 0; i < 2; ++i)
    {
        m_lightHist[i].Reset();
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &hd2,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(&m_lightHist[i]))))
            return false;
        m_lightHistState[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        D3D12_SHADER_RESOURCE_VIEW_DESC hs = {};
        hs.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        hs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        hs.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE sh = m_heap->GetCPUDescriptorHandleForHeapStart();
        sh.ptr += (SIZE_T)((i == 0 ? kSlotLightPair0 : kSlotLightPair1) + 1) * m_stride;
        dev->CreateShaderResourceView(m_lightHist[i].Get(), &hs, sh);

        D3D12_UNORDERED_ACCESS_VIEW_DESC hu = {};
        hu.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE uh = m_heap->GetCPUDescriptorHandleForHeapStart();
        uh.ptr += (SIZE_T)(i == 0 ? kSlotLightHistUav0 : kSlotLightHistUav1) * m_stride;
        dev->CreateUnorderedAccessView(m_lightHist[i].Get(), nullptr, &hu, uh);
    }
    m_lightHistValid = false;

    // The AO's wide pass output: visibility only, read by the scene like the
    // histories (its SRV follows this format).
    m_aoWide.Reset();
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&m_aoWide))))
        return false;
    m_aoWideState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC wu = {};
        wu.Format = DXGI_FORMAT_R16_FLOAT;
        wu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE wh = m_heap->GetCPUDescriptorHandleForHeapStart();
        wh.ptr += (SIZE_T)kSlotAoWideUav * m_stride;
        dev->CreateUnorderedAccessView(m_aoWide.Get(), nullptr, &wu, wh);
    }
    // And the local lights' (the same pass, the same format).
    m_lightWide.Reset();
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&m_lightWide))))
        return false;
    m_lightWideState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC wu = {};
        wu.Format = DXGI_FORMAT_R16_FLOAT;
        wu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE wh = m_heap->GetCPUDescriptorHandleForHeapStart();
        wh.ptr += (SIZE_T)kSlotLightWideUav * m_stride;
        dev->CreateUnorderedAccessView(m_lightWide.Get(), nullptr, &wu, wh);
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC cv = {};
    cv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    cv.Buffer.NumElements = 4;
    cv.Buffer.StructureByteStride = sizeof(uint32_t);
    h = m_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)kSlotCounters * m_stride;
    dev->CreateUnorderedAccessView(m_counters.Get(), nullptr, &cv, h);

    m_width = width;
    m_height = height;
    m_resultReadable = false;
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
    if ((width != m_width || height != m_height) && !CreateTargets(dev.device.Get(), width, height))
        return;

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


    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_rootSig.Get());
    list->SetPipelineState(m_pso.Get());
    auto gpuSlot = [&](UINT slot) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)slot * m_stride;
        return h;
    };
    list->SetComputeRootDescriptorTable(0, m_heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootDescriptorTable(1, gpuSlot(kSlotOut));
    list->SetComputeRoot32BitConstants(2, sizeof(TraceCB) / 4, &cb, 0);

    if (m_timestamps) list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    if (m_timestamps) list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

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
        list->SetPipelineState(m_psoAo.Get());
        list->SetComputeRootDescriptorTable(1, gpuSlot(kSlotAoOut));
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    }
    if (m_timestamps) list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);

    // LOCAL LIGHTS: one ray per pixel to one of them (rt_lights.hlsl), into their own output.
    if (!doLights) m_lightHistValid = false;
    if (doLights)
    {
        list->SetPipelineState(m_psoLights.Get());
        list->SetComputeRootDescriptorTable(1, gpuSlot(kSlotLightOut));
        list->SetComputeRootShaderResourceView(3, lights->LightsVa());
        list->SetComputeRootShaderResourceView(4, lights->ClustersVa());
        list->SetComputeRootShaderResourceView(5, lights->IndicesVa());
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    }
    if (m_timestamps) list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);

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
    // NGG_RT_NO_HISTORY=1: the sun's and the AO's denoise without their history nor the AO's wide pass - this frame's
    // 5x5 alone: grainy, but unbiased, the reference a ghost (anything a history carries that this frame's rays do not
    // say) is measured against (tools/car-ghost).
    static const bool noHistory = [] { const char* e = std::getenv("NGG_RT_NO_HISTORY"); return e && *e && *e != '0'; }();
    DenoiseCB dcSun = dc;
    if (noHistory) dcSun.valid = 0.0f;
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
    if (m_timestamps)
    {
        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 4);
        list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 5,
                               m_timestampReadback.Get(), 0);
    }

    D3D12_RESOURCE_BARRIER toCopy = Transition(m_counters.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &toCopy);
    list->CopyResource(m_countersReadback.Get(), m_counters.Get());
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
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_heap->GetGPUDescriptorHandleForHeapStart();
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
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_heap->GetGPUDescriptorHandleForHeapStart();
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
    if (!m_countersReadback) return;

    uint32_t* c = nullptr;
    D3D12_RANGE all = { 0, 4 * sizeof(uint32_t) };
    if (SUCCEEDED(m_countersReadback->Map(0, &all, reinterpret_cast<void**>(&c))))
    {
        m_traced = c[0];
        m_shadowed = c[1];
        m_leafHits = c[2];
        m_leafMisses = c[3];
        D3D12_RANGE none = { 0, 0 };
        m_countersReadback->Unmap(0, &none);
    }

    if (!m_timestampReadback || !m_timestampHz) return;
    UINT64* t = nullptr;
    D3D12_RANGE ts = { 0, 5 * sizeof(UINT64) };
    if (SUCCEEDED(m_timestampReadback->Map(0, &ts, reinterpret_cast<void**>(&t))))
    {
        const double k = 1000.0 / (double)m_timestampHz;
        if (t[3] > t[0]) m_lastMs = k * (double)(t[3] - t[0]);
        m_aoMs      = t[2] >= t[1] ? k * (double)(t[2] - t[1]) : 0.0;
        m_lightsMs  = t[3] >= t[2] ? k * (double)(t[3] - t[2]) : 0.0;
        m_denoiseMs = t[4] >= t[3] ? k * (double)(t[4] - t[3]) : 0.0;
        D3D12_RANGE none = { 0, 0 };
        m_timestampReadback->Unmap(0, &none);
    }
}

} // namespace core
} // namespace ngg
