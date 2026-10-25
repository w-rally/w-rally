#include "atmosphere_ms.h"
#include "log.h"
#include "atmosphere_hlsl.h"

#include <d3dcompiler.h>
#include <string>
#include <string.h>
#include <math.h>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

const DXGI_FORMAT kMsFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
const D3D12_RESOURCE_STATES kReadState =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Prepended with kAtmosphereHlsl for the constants and AtmRaySphere. This is a
// DIFFERENT integral from AtmScatter - second-order, isotropic, from an
// arbitrary point - not a second copy of it.
const char* kMsSrc = R"(
cbuffer MsCB : register(b0)
{
    float gTurbidity;
    float gGroundAlbedo;
    float gEnabled;
    float gPad;
};

RWTexture2D<float4> gOut : register(u0);

// Sun transmittance from an ARBITRARY point (AtmSunTransmittance is fixed at
// the camera). Zero when the planet is in the way.
float3 SunTransmittanceFrom(float3 p, float3 sunDir, float3 betaM)
{
    float g0, g1;
    if (AtmRaySphere(p, sunDir, kRg, g0, g1) && g0 > 0.0) return float3(0, 0, 0);
    float t0, t1;
    AtmRaySphere(p, sunDir, kRa, t0, t1);
    const int steps = 8;
    const float seg = max(t1, 0.0) / steps;
    float odR = 0.0, odM = 0.0;
    [loop] for (int i = 0; i < steps; ++i)
    {
        const float h = max(length(p + sunDir * (seg * (i + 0.5))) - kRg, 0.0);
        odR += exp(-h / kHr) * seg;
        odM += exp(-h / kHm) * seg;
    }
    return exp(-(kBetaR * odR + betaM * 1.1 * odM));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    gOut.GetDimensions(w, h);
    if (any(id.xy >= uint2(w, h))) return;
    if (gEnabled < 0.5) { gOut[id.xy] = float4(0, 0, 0, 1); return; }

    // Texel centres: mu_s across [-1, 1], altitude down [0, top].
    const float muS = ((id.x + 0.5) / w) * 2.0 - 1.0;
    const float alt = ((id.y + 0.5) / h) * (kRa - kRg);
    const float3 x = float3(0.0, kRg + alt, 0.0);
    const float3 sunDir = float3(sqrt(saturate(1.0 - muS * muS)), muS, 0.0);
    const float3 betaM = kBetaM * max(gTurbidity, 0.0);
    const float isoPhase = 1.0 / (4.0 * kPI);

    // 64 directions over the sphere, stratified in cos(theta) and phi - equal
    // solid angle per cell, so the sphere integral with the isotropic pdf
    // 1/(4 PI) is a plain average.
    float3 L2 = 0.0, fms = 0.0;
    const int nT = 8, nP = 8;
    [loop] for (int a = 0; a < nT; ++a)
    {
        const float ct = 1.0 - 2.0 * (a + 0.5) / nT;
        const float st = sqrt(saturate(1.0 - ct * ct));
        [loop] for (int b = 0; b < nP; ++b)
        {
            const float ph = 2.0 * kPI * (b + 0.5) / nP;
            const float3 d = float3(st * cos(ph), ct, st * sin(ph));

            float t0, t1;
            AtmRaySphere(x, d, kRa, t0, t1);
            float tMax = max(t1, 0.0);
            float g0, g1;
            const bool ground = AtmRaySphere(x, d, kRg, g0, g1) && g0 > 0.0;
            if (ground) tMax = g0;

            const int n = 20;
            const float ds = tMax / n;
            float3 tau = 0.0, Lp = 0.0, Fp = 0.0;
            [loop] for (int i = 0; i < n; ++i)
            {
                const float3 p = x + d * (ds * (i + 0.5));
                const float hh = max(length(p) - kRg, 0.0);
                const float dR = exp(-hh / kHr), dM = exp(-hh / kHm);
                const float3 sigS = kBetaR * dR + betaM * dM;
                const float3 sigT = kBetaR * dR + betaM * 1.1 * dM;
                const float3 T = exp(-(tau + sigT * ds * 0.5));   // x -> step midpoint
                Lp += T * sigS * SunTransmittanceFrom(p, sunDir, betaM) * isoPhase * ds;
                Fp += T * sigS * ds;
                tau += sigT * ds;
            }
            if (ground)
            {
                // The ground is a Lambertian reflector lit by the attenuated
                // sun: radiance albedo/PI * E. Part of the second bounce.
                const float3 pg = x + d * tMax;
                const float3 ng = normalize(pg);
                Lp += exp(-tau) * SunTransmittanceFrom(pg + ng * 1.0, sunDir, betaM)
                    * saturate(dot(ng, sunDir)) * gGroundAlbedo / kPI;
            }
            L2 += Lp;
            fms += Fp;
        }
    }
    L2  /= float(nT * nP);
    fms /= float(nT * nP);

    // Every further order is the previous one scattered by the same fraction:
    // L2 * (1 + f + f^2 + ...) = L2 / (1 - f).
    gOut[id.xy] = float4(L2 / (1.0 - min(fms, 0.99)), 1.0);
}
)";

struct MsCB
{
    float turbidity, groundAlbedo, enabled, pad;
};

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

float HalfToFloat(uint16_t v)
{
    const uint32_t s = (v >> 15) & 1, e = (v >> 10) & 31, m = v & 1023;
    const float f = (e == 0) ? ldexpf((float)m, -24) : ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -f : f;
}

} // namespace

D3D12_DESCRIPTOR_RANGE AtmosphereMs::MsSrvRange()
{
    D3D12_DESCRIPTOR_RANGE r = {};
    r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r.NumDescriptors = 1;
    r.BaseShaderRegister = 0;
    r.RegisterSpace = 1;
    r.OffsetInDescriptorsFromTableStart = 0;
    return r;
}

D3D12_STATIC_SAMPLER_DESC AtmosphereMs::MsSampler(D3D12_SHADER_VISIBILITY visibility)
{
    D3D12_STATIC_SAMPLER_DESC s = {};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderRegister = 0;
    s.RegisterSpace = 1;
    s.ShaderVisibility = visibility;
    return s;
}

bool AtmosphereMs::Create(ID3D12Device* dev)
{
    if (!dev) return false;

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(MsCB) / 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters = params;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&m_rootSig))))
    {
        Log("atmosphere-ms: root signature failed");
        return false;
    }

    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    const std::string src = std::string(kAtmosphereHlsl) + kMsSrc;
    ComPtr<ID3DBlob> cs;
    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_atm_ms", nullptr, nullptr,
                          "CSMain", "cs_5_1", flags, 0, &cs, &err)))
    {
        Log("atmosphere-ms: compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_rootSig.Get();
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_pso))))
    {
        Log("atmosphere-ms: pipeline creation failed");
        return false;
    }

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kSize;
    desc.Height = kSize;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kMsFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // Starts zeroed (committed), i.e. single scattering, until the first Update.
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kReadState,
                                            nullptr, IID_PPV_ARGS(&m_lut))))
    {
        m_pso.Reset();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_uavHeap))))
    {
        m_pso.Reset();
        return false;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.Format = kMsFormat;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(m_lut.Get(), nullptr, &uv,
                                   m_uavHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_HEAP_PROPERTIES rb = {};
    rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&m_readback))))
        m_readback.Reset();    // diagnostics only

    return true;
}

void AtmosphereMs::CreateSrv(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE dest) const
{
    if (!dev || !m_lut) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = kMsFormat;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(m_lut.Get(), &sv, dest);
}

void AtmosphereMs::Update(ID3D12GraphicsCommandList* list, float turbidity, float groundAlbedo,
                          bool enabled)
{
    if (!Ready() || !list) return;

    // Last computation's probe texel. The frame is synchronous, so a copy
    // recorded in an earlier frame has landed.
    if (m_probePending && m_readback)
    {
        const uint16_t* h = nullptr;
        D3D12_RANGE r = { 0, 8 };
        if (SUCCEEDED(m_readback->Map(0, &r, (void**)&h)))
        {
            for (int c = 0; c < 3; ++c) m_probe[c] = HalfToFloat(h[c]);
            D3D12_RANGE none = { 0, 0 };
            m_readback->Unmap(0, &none);
            m_probeValid = true;
            m_probePending = false;
            Log("atmosphere-ms: Psi_ms(%.0f m, mu_s %.3f) = %.5f %.5f %.5f",
                ((kProbeY + 0.5f) / kSize) * 60000.0f, ((kProbeX + 0.5f) / kSize) * 2.0f - 1.0f,
                m_probe[0], m_probe[1], m_probe[2]);
        }
    }

    if (m_valid && turbidity == m_turbidity && groundAlbedo == m_groundAlbedo &&
        enabled == m_enabled)
        return;

    ID3D12Device* dev = nullptr;
    list->GetDevice(IID_PPV_ARGS(&dev));
    if (!dev) return;
    MsCB cb = { turbidity, groundAlbedo, enabled ? 1.0f : 0.0f, 0.0f };

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_lut.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = kReadState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(1, &b);

    ID3D12DescriptorHeap* heaps[] = { m_uavHeap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_rootSig.Get());
    list->SetPipelineState(m_pso.Get());
    list->SetComputeRoot32BitConstants(0, sizeof(cb) / 4, &cb, 0);
    list->SetComputeRootDescriptorTable(1, m_uavHeap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(kSize / 8, kSize / 8, 1);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    if (m_readback)
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = m_readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = kMsFormat;
        dst.PlacedFootprint.Footprint.Width = 1;
        dst.PlacedFootprint.Footprint.Height = 1;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = 256;
        src.pResource = m_lut.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box = { kProbeX, kProbeY, 0, kProbeX + 1, kProbeY + 1, 1 };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        m_probePending = true;
    }
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = kReadState;
    list->ResourceBarrier(1, &b);

    dev->Release();

    m_valid = true;
    m_turbidity = turbidity;
    m_groundAlbedo = groundAlbedo;
    m_enabled = enabled;
    Log("atmosphere-ms: table %s (turbidity %.2f, ground albedo %.3f)",
        enabled ? "computed" : "zeroed - single scattering", turbidity, groundAlbedo);
}

} // namespace core
} // namespace ngg
