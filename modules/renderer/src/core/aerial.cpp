#include "aerial.h"
#include "log.h"
#include "atmosphere_hlsl.h"

#include <d3dcompiler.h>
#include <string>
#include <string.h>
#include <math.h>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

const DXGI_FORMAT kLutFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

const char* kAerialSrc = R"(
cbuffer AerialCB : register(b0)
{
    row_major float4x4 gInvViewProj;   // unjittered, reverse-Z
    float3 gSunTowards;
    float  gSunIntensity;
    float  gTurbidity;
    float  gMaxDist;                   // radial metres where w = 1
    float  gCloud;                     // weather (5.0q)
    float  gFogBeta;
    float  gFogH;
    float  gGround;
    float2 gPad;
};

RWTexture3D<float4> gInscatter     : register(u0);
RWTexture3D<float4> gTransmittance : register(u1);

// One thread per froxel, each an independent AtmScatter over [0, d]. A
// cumulative march (slice n continuing from slice n-1) would be cheaper still,
// but it would be a SECOND implementation of the integral, and the whole table
// already costs less than a sliver of what the per-pixel march did.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    AtmSetWeather(gCloud, gFogBeta, gFogH, gGround);
    uint3 size;
    gInscatter.GetDimensions(size.x, size.y, size.z);
    if (any(id >= size)) return;

    // The cell's centre, in the same uv the scene pass derives from
    // SV_Position: x right, y DOWN.
    float2 uv  = (float2(id.xy) + 0.5) / float2(size.xy);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    // Two points on the cell's ray. Reverse-Z: 1 is the near plane, and 0.5
    // is safely in front of the far plane for any projection we are handed.
    float4 a = mul(float4(ndc, 1.0, 1.0), gInvViewProj);
    float4 b = mul(float4(ndc, 0.5, 1.0), gInvViewProj);
    float3 dir = normalize(b.xyz / b.w - a.xyz / a.w);

    // Quadratic slices: texel centre w = (z+0.5)/N holds d = maxDist * w^2,
    // so the scene pass's w = sqrt(d / maxDist) lands on texel centres exactly.
    float w = (float(id.z) + 0.5) / float(size.z);
    float d = gMaxDist * w * w;

    float3 tr;
    // 8x3 - the SAME quadrature as the per-pixel reference path, deliberately.
    // 16x4 was tried first and cost 0.60 ms for no difference that survived
    // the tonemap; matching the reference also means an A/B against it now
    // measures the table's interpolation error and nothing else.
    float3 ins = AtmScatter(dir, gSunTowards, d, gTurbidity, gSunIntensity, 8, 3, tr);
    gInscatter[id]     = float4(ins, 1.0);
    gTransmittance[id] = float4(tr, 1.0);
}
)";

struct AerialCB
{
    float invViewProj[16];
    float sunTowards[3];
    float sunIntensity;
    float turbidity;
    float maxDist;
    float cloudCover, fogBeta, fogHeight, groundAlbedo;
    float pad[2];
};
static_assert(sizeof(AerialCB) % 16 == 0, "AerialCB must be whole float4s - see scene.cpp");

ComPtr<ID3D12Resource> MakeUpload(ID3D12Device* dev, const void* data, size_t bytes)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (bytes + 255) & ~size_t(255);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> res;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&res))))
        return nullptr;

    void* p = nullptr;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(res->Map(0, &none, &p))) return nullptr;
    memcpy(p, data, bytes);
    res->Unmap(0, nullptr);
    return res;
}

// General 4x4 inverse - the view-projection is not affine. The same routine as
// rt.cpp and post.cpp, kept local for the reason given there: a header that
// exists only to share twelve lines costs more than the lines.
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

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

} // namespace

bool AerialLut::Create(ID3D12Device* dev)
{
    if (!dev) return false;

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 2;                 // u0, u1
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_DESCRIPTOR_RANGE msRange = AtmosphereMs::MsSrvRange();
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &msRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_STATIC_SAMPLER_DESC msSamp = AtmosphereMs::MsSampler(D3D12_SHADER_VISIBILITY_ALL);

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &msSamp;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    {
        Log("aerial: root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&m_rootSig))))
        return false;

    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    // The shared atmosphere, exactly as the sky pass and the environment get it.
    const std::string src = std::string(kAtmosphereHlsl) + kAerialSrc;

    ComPtr<ID3DBlob> cs;
    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_aerial", nullptr, nullptr,
                          "CSMain", "cs_5_1", flags, 0, &cs, &err)))
    {
        Log("aerial: compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_rootSig.Get();
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_pso))))
    {
        Log("aerial: pipeline creation failed");
        return false;
    }

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = kSlices;
    desc.MipLevels = 1;
    desc.Format = kLutFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource** targets[2] = { m_inscatter.GetAddressOf(), m_transmittance.GetAddressOf() };
    for (ID3D12Resource** t : targets)
    {
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(t))))
        {
            Log("aerial: volume creation failed");
            m_pso.Reset();
            return false;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 3;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_uavHeap))))
    {
        m_pso.Reset();
        return false;
    }
    const UINT stride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_stride = stride;

    D3D12_CPU_DESCRIPTOR_HANDLE h = m_uavHeap->GetCPUDescriptorHandleForHeapStart();
    ID3D12Resource* volumes[2] = { m_inscatter.Get(), m_transmittance.Get() };
    for (ID3D12Resource* r : volumes)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
        uv.Format = kLutFormat;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uv.Texture3D.WSize = kSlices;
        dev->CreateUnorderedAccessView(r, nullptr, &uv, h);
        h.ptr += stride;
    }

    Log("aerial: ready - %d x %d x %d froxels", kWidth, kHeight, kSlices);
    return true;
}

void AerialLut::CreateSrvs(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE dest,
                           UINT stride) const
{
    if (!dev || !Ready()) return;
    ID3D12Resource* volumes[2] = { m_inscatter.Get(), m_transmittance.Get() };
    for (ID3D12Resource* r : volumes)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = kLutFormat;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture3D.MipLevels = 1;
        dev->CreateShaderResourceView(r, &sv, dest);
        dest.ptr += stride;
    }
}

void AerialLut::Record(ID3D12GraphicsCommandList* list, const float* viewProj,
                       const float* sunTowards, float sunIntensity, float turbidity,
                       float maxDist, const AtmosphereMs* ms, const float weather[4])
{
    if (!Ready() || !list || !viewProj) return;

    AerialCB cb = {};
    if (!Invert(viewProj, cb.invViewProj)) return;

    ID3D12Device* dev = nullptr;
    list->GetDevice(IID_PPV_ARGS(&dev));
    if (!dev) return;

    for (int i = 0; i < 3; ++i) cb.sunTowards[i] = sunTowards[i];
    cb.sunIntensity = sunIntensity;
    cb.turbidity    = turbidity;
    cb.maxDist      = maxDist;
    if (weather)
    {
        cb.cloudCover = weather[0]; cb.fogBeta = weather[1];
        cb.fogHeight = weather[2];  cb.groundAlbedo = weather[3];
    }
    ComPtr<ID3D12Resource> cbRes = MakeUpload(dev, &cb, sizeof(cb));

    // The MS table in slot 2, rewritten each frame (a write, not an allocation).
    D3D12_CPU_DESCRIPTOR_HANDLE msh = m_uavHeap->GetCPUDescriptorHandleForHeapStart();
    msh.ptr += 2 * (SIZE_T)m_stride;
    if (ms && ms->Ready())
        ms->CreateSrv(dev, msh);
    else
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nv = {};
        nv.Format = kLutFormat;
        nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(nullptr, &nv, msh);
    }
    dev->Release();
    if (!cbRes) return;

    D3D12_RESOURCE_BARRIER b[2] = {
        Transition(m_inscatter.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(m_transmittance.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(2, b);

    ID3D12DescriptorHeap* heaps[] = { m_uavHeap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_rootSig.Get());
    list->SetPipelineState(m_pso.Get());
    list->SetComputeRootConstantBufferView(0, cbRes->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(1, m_uavHeap->GetGPUDescriptorHandleForHeapStart());
    {
        D3D12_GPU_DESCRIPTOR_HANDLE mg = m_uavHeap->GetGPUDescriptorHandleForHeapStart();
        mg.ptr += 2 * (UINT64)m_stride;
        list->SetComputeRootDescriptorTable(2, mg);
    }
    list->Dispatch((kWidth + 7) / 8, (kHeight + 7) / 8, kSlices);

    for (D3D12_RESOURCE_BARRIER& t : b)
        std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
    list->ResourceBarrier(2, b);

    m_keepAlive = cbRes;
}

} // namespace core
} // namespace ngg
