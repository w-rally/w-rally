#include "env.h"
#include "log.h"
#include "atmosphere_hlsl.h"

#include <d3dcompiler.h>
#include <string>
#include <math.h>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

const DXGI_FORMAT kEnvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

const char* kEnvSrc = R"(
cbuffer EnvCB : register(b0)
{
    float3 gSunDir;          // TOWARDS the sun
    float  gSunIntensity;
    float  gTurbidity;
    float  gGroundAlbedo;
    float2 gSrcTexel;        // 1/width, 1/height of the SOURCE mip
    float  gCloud;           // weather (5.0q)
    float  gFogBeta;
    float  gFogH;
    float  gNightGlow;       // design 5.4: the moonless sky's own radiance
    float3 gLight2;          // ... and the other body's sky
    float  gLight2I;
};

Texture2D    gSrc  : register(t0);
SamplerState gSamp : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}

// Mip 0: the atmosphere, once per texel. 4,096 marches for the whole frame,
// against one per pixel before this existed.
float4 PSSky(VSOut i) : SV_Target
{
    AtmSetWeather(gCloud, gFogBeta, gFogH, gGroundAlbedo);
    AtmSetNight(gNightGlow);
    AtmSetLight2(gLight2, gLight2I);
    float3 dir = AtmOctDecode(i.uv);
    float3 col = AtmSkyRadiance(dir, gSunDir, gTurbidity, gSunIntensity,
                                gGroundAlbedo, 16, 4);
    return float4(col, 1.0);
}

// Irradiance for the normal at this texel: the cosine-weighted integral of the
// sky radiance (mip 0) over the UPPER hemisphere, by quadrature - 16 rings of
// 32 directions. dOmega = sin(theta) dtheta dphi, integrated exactly per ring
// (cos th0 - cos th1) so the rings near the horizon, which carry the most
// solid angle, are not under-weighted.
//
// 512 taps for each of 256 texels, once per frame: 131k bilinear fetches, a
// fraction of what one frame of the per-pixel march used to cost.
float4 PSIrradiance(VSOut i) : SV_Target
{
    const float3 N = AtmOctDecode(i.uv);
    const int nt = 16, np = 32;
    const float dth = kPI * 0.5 / nt;
    const float dph = 2.0 * kPI / np;
    float3 E = float3(0.0, 0.0, 0.0);
    [loop] for (int a = 0; a < nt; ++a)
    {
        const float th0 = a * dth, th1 = th0 + dth;
        const float th  = th0 + 0.5 * dth;
        const float w   = cos(th0) - cos(th1);
        const float st = sin(th), ct = cos(th);
        [loop] for (int b = 0; b < np; ++b)
        {
            const float ph = (b + 0.5) * dph;
            const float3 d = float3(st * cos(ph), ct, st * sin(ph));
            const float c = dot(N, d);
            if (c > 0.0)
                E += gSrc.SampleLevel(gSamp, AtmOctEncode(d), 0).rgb * (c * w);
        }
    }
    return float4(E * dph, 1.0);
}

// Mip n from mip n-1. Four bilinear taps at the source's texel centres, which
// is a 2x2 box - the cheap stand-in for a GGX-weighted prefilter, and the one
// most engines actually ship.
float4 PSDown(VSOut i) : SV_Target
{
    float2 o = gSrcTexel * 0.5;
    float3 c = gSrc.SampleLevel(gSamp, i.uv + float2(-o.x, -o.y), 0).rgb
             + gSrc.SampleLevel(gSamp, i.uv + float2( o.x, -o.y), 0).rgb
             + gSrc.SampleLevel(gSamp, i.uv + float2(-o.x,  o.y), 0).rgb
             + gSrc.SampleLevel(gSamp, i.uv + float2( o.x,  o.y), 0).rgb;
    return float4(c * 0.25, 1.0);
}
)";

struct EnvCB
{
    float sunDir[3];
    float sunIntensity;
    float turbidity;
    float groundAlbedo;
    float srcTexel[2];
    float cloudCover, fogBeta, fogHeight, nightGlow;
    float light2[3], light2Intensity;
};
static_assert(sizeof(EnvCB) % 16 == 0, "EnvCB must be whole float4s - see scene.cpp");

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, UINT sub,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = sub;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

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

} // namespace

bool Environment::Create(ID3D12Device* dev)
{
    if (!dev) return false;

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_DESCRIPTOR_RANGE msRange = AtmosphereMs::MsSrvRange();
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &msRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp[2] = {};
    samp[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp[0].AddressU = samp[0].AddressV = samp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp[0].ShaderRegister = 0;
    samp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samp[1] = AtmosphereMs::MsSampler(D3D12_SHADER_VISIBILITY_PIXEL);

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 2;
    rs.pStaticSamplers = samp;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    {
        Log("env: root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&m_rootSig))))
        return false;

    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    // The shared atmosphere, exactly as the sky pass gets it.
    const std::string src = std::string(kAtmosphereHlsl) + kEnvSrc;

    ComPtr<ID3DBlob> vs, psSky, psDown, psIrr;
    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_env", nullptr, nullptr,
                          "VSMain", "vs_5_1", flags, 0, &vs, &err)))
    {
        Log("env: VS compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_env", nullptr, nullptr,
                          "PSSky", "ps_5_1", flags, 0, &psSky, &err)))
    {
        Log("env: sky compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_env", nullptr, nullptr,
                          "PSDown", "ps_5_1", flags, 0, &psDown, &err)))
    {
        Log("env: downsample compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_env", nullptr, nullptr,
                          "PSIrradiance", "ps_5_1", flags, 0, &psIrr, &err)))
    {
        Log("env: irradiance compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_rootSig.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = kEnvFormat;
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count = 1;

    pd.PS = { psSky->GetBufferPointer(), psSky->GetBufferSize() };
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoSky))))
    {
        Log("env: sky pipeline creation failed");
        return false;
    }
    pd.PS = { psDown->GetBufferPointer(), psDown->GetBufferSize() };
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoDown))))
    {
        Log("env: downsample pipeline creation failed");
        return false;
    }
    pd.PS = { psIrr->GetBufferPointer(), psIrr->GetBufferSize() };
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoIrr))))
    {
        Log("env: irradiance pipeline creation failed");
        return false;
    }

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kSize;
    desc.Height = kSize;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = kMips;
    desc.Format = kEnvFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                            nullptr, IID_PPV_ARGS(&m_tex))))
    {
        Log("env: texture creation failed");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kMips;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)))) return false;
    m_rtvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kMips + 1;          // + the multiple-scattering table
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)))) return false;
    m_srvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (UINT m = 0; m < kMips; ++m)
    {
        D3D12_RENDER_TARGET_VIEW_DESC rv = {};
        rv.Format = kEnvFormat;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rv.Texture2D.MipSlice = m;
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)m * m_rtvStride;
        dev->CreateRenderTargetView(m_tex.Get(), &rv, h);

        // ONE mip per SRV, not the whole chain: the downsample must read
        // exactly the level below, and a view over the full chain would let
        // SampleLevel(0) read whatever the sampler chose instead.
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = kEnvFormat;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MostDetailedMip = m;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE sh = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        sh.ptr += (SIZE_T)m * m_srvStride;
        dev->CreateShaderResourceView(m_tex.Get(), &sv, sh);
    }

    // The irradiance map and its RTV.
    {
        D3D12_RESOURCE_DESC id = desc;
        id.Width = kIrrSize;
        id.Height = kIrrSize;
        id.MipLevels = 1;
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &id,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(&m_irr))))
        {
            Log("env: irradiance texture creation failed");
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC rh = {};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = 1;
        if (FAILED(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&m_irrRtvHeap)))) return false;
        dev->CreateRenderTargetView(m_irr.Get(), nullptr,
                                    m_irrRtvHeap->GetCPUDescriptorHandleForHeapStart());

        // One texel's worth of readback, at the 256-byte pitch a texture copy
        // requires.
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
                                                IID_PPV_ARGS(&m_irrReadback))))
            m_irrReadback.Reset();      // diagnostics only - not fatal
    }

    Log("env: ready - %d x %d octahedral, %d roughness levels, %d x %d irradiance",
        kSize, kSize, kMips, kIrrSize, kIrrSize);
    return true;
}

void Environment::CreateIrradianceSrv(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE dest) const
{
    if (!dev || !m_irr) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = kEnvFormat;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(m_irr.Get(), &sv, dest);
}

void Environment::CreateSrv(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE dest) const
{
    if (!dev || !m_tex) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = kEnvFormat;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = kMips;
    dev->CreateShaderResourceView(m_tex.Get(), &sv, dest);
}

void Environment::Record(ID3D12GraphicsCommandList* list, const SkyParams& p,
                         const AtmosphereMs* ms)
{
    if (!Ready() || !list) return;

    ID3D12Device* dev = nullptr;
    list->GetDevice(IID_PPV_ARGS(&dev));
    if (!dev) return;

    // The MS table's descriptor, in the slot after the mips. Rewritten each
    // frame - a write, not an allocation - so it can never point at a table
    // that has since been recreated. Null SRV (= single scattering) without one.
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)kMips * m_srvStride;
        if (ms && ms->Ready())
            ms->CreateSrv(dev, h);
        else
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC nv = {};
            nv.Format = kEnvFormat;
            nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nv.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(nullptr, &nv, h);
        }
    }

    // Last frame's upward irradiance (diagnostics). The frame is synchronous.
    if (m_irrPending && m_irrReadback)
    {
        const uint16_t* h = nullptr;
        D3D12_RANGE r = { 0, 8 };
        if (SUCCEEDED(m_irrReadback->Map(0, &r, (void**)&h)))
        {
            auto half = [](uint16_t v) {
                const uint32_t s = (v >> 15) & 1, e = (v >> 10) & 31, m = v & 1023;
                float f = (e == 0) ? ldexpf((float)m, -24)
                                   : ldexpf((float)(m | 1024), (int)e - 25);
                return s ? -f : f;
            };
            for (int c = 0; c < 3; ++c) m_irrUp[c] = half(h[c]);
            D3D12_RANGE none = { 0, 0 };
            m_irrReadback->Unmap(0, &none);
            m_irrUpValid = true;
        }
        m_irrPending = false;
    }

    EnvCB cb = {};
    // Params carry the direction light TRAVELS; the model wants the direction
    // TOWARDS the sun. Same negation as Sky::Record and Scene::Record, for the
    // same reason - it is done once per consumer rather than at every call site.
    {
        const float* sd = p.sunDir;
        float len = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
        if (len < 1e-6f) len = 1.0f;
        cb.sunDir[0] = -sd[0] / len;
        cb.sunDir[1] = -sd[1] / len;
        cb.sunDir[2] = -sd[2] / len;
    }
    cb.sunIntensity = p.sunIntensity;
    cb.turbidity    = p.turbidity;
    cb.groundAlbedo = p.groundAlbedo;
    cb.cloudCover   = p.cloudCover;
    cb.fogBeta      = FogBeta(p);
    cb.fogHeight    = p.fogHeight;
    cb.nightGlow    = p.nightGlow;
    for (int k = 0; k < 3; ++k) cb.light2[k] = p.light2Towards[k];
    cb.light2Intensity = p.light2Intensity;

    list->SetGraphicsRootSignature(m_rootSig.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    {
        D3D12_GPU_DESCRIPTOR_HANDLE mh = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        mh.ptr += (UINT64)kMips * m_srvStride;
        list->SetGraphicsRootDescriptorTable(2, mh);
    }

    // The constant buffers must outlive GPU execution; the frame is synchronous
    // (see the note at the end of Scene::Record) so holding them to the next
    // Record is enough. One per mip because srcTexel differs.
    ComPtr<ID3D12Resource> cbs[kMips];

    for (UINT m = 0; m < kMips; ++m)
    {
        const UINT size = kSize >> m;
        if (m == 0)
        {
            D3D12_RESOURCE_BARRIER b = Transition(m_tex.Get(), 0,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &b);
        }
        else
        {
            // Previous mip becomes readable, this one becomes the target.
            D3D12_RESOURCE_BARRIER b[2];
            b[0] = Transition(m_tex.Get(), m - 1,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            b[1] = Transition(m_tex.Get(), m,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(2, b);
        }

        const UINT srcSize = (m == 0) ? kSize : (kSize >> (m - 1));
        cb.srcTexel[0] = 1.0f / (float)srcSize;
        cb.srcTexel[1] = 1.0f / (float)srcSize;
        cbs[m] = MakeUpload(dev, &cb, sizeof(cb));
        if (!cbs[m]) break;

        D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)size, (float)size, 0.0f, 1.0f };
        D3D12_RECT sc = { 0, 0, (LONG)size, (LONG)size };
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)m * m_rtvStride;
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        list->SetPipelineState((m == 0) ? m_psoSky.Get() : m_psoDown.Get());
        list->SetGraphicsRootConstantBufferView(0, cbs[m]->GetGPUVirtualAddress());
        if (m > 0)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE sh = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
            sh.ptr += (UINT64)(m - 1) * m_srvStride;
            list->SetGraphicsRootDescriptorTable(1, sh);
        }
        else
        {
            // Nothing is read, but the table must still be bound: an unbound
            // descriptor table is a validation error, and leaving last frame's
            // is the kind of thing that works until it does not.
            list->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
        }
        list->DrawInstanced(3, 1, 0, 0);
    }

    // Last mip back to readable; everything else already is.
    D3D12_RESOURCE_BARRIER b = Transition(m_tex.Get(), kMips - 1,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &b);

    // Irradiance, from mip 0 - which every barrier above has left readable.
    if (m_psoIrr && m_irr)
    {
        D3D12_RESOURCE_BARRIER ib = Transition(m_irr.Get(), 0,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &ib);

        cb.srcTexel[0] = cb.srcTexel[1] = 1.0f / (float)kSize;
        m_irrKeepAlive = MakeUpload(dev, &cb, sizeof(cb));
        if (m_irrKeepAlive)
        {
            D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)kIrrSize, (float)kIrrSize, 0.0f, 1.0f };
            D3D12_RECT sc = { 0, 0, kIrrSize, kIrrSize };
            list->RSSetViewports(1, &vp);
            list->RSSetScissorRects(1, &sc);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_irrRtvHeap->GetCPUDescriptorHandleForHeapStart();
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->SetPipelineState(m_psoIrr.Get());
            list->SetGraphicsRootConstantBufferView(0, m_irrKeepAlive->GetGPUVirtualAddress());
            list->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
            list->DrawInstanced(3, 1, 0, 0);
        }

        // The texel nearest straight up, for IrradianceUp. Octahedral up is the
        // centre of the map; with an even size that is a texel corner, so
        // (size/2, size/2) is 5.6 degrees off vertical - which the audit's
        // reference integral accounts for by evaluating the same direction.
        ib.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ib.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &ib);
        if (m_irrReadback)
        {
            D3D12_TEXTURE_COPY_LOCATION dst = {}, srcLoc = {};
            dst.pResource = m_irrReadback.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint.Format = kEnvFormat;
            dst.PlacedFootprint.Footprint.Width = 1;
            dst.PlacedFootprint.Footprint.Height = 1;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = 256;
            srcLoc.pResource = m_irr.Get();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.SubresourceIndex = 0;
            D3D12_BOX box = { kIrrSize / 2, kIrrSize / 2, 0, kIrrSize / 2 + 1, kIrrSize / 2 + 1, 1 };
            list->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, &box);
            m_irrPending = true;
        }
        ib.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ib.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &ib);
    }

    dev->Release();
    for (UINT m = 0; m < kMips; ++m) m_keepAlive[m] = cbs[m];
}

} // namespace core
} // namespace ngg
