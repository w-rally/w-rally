#include "sky.h"
#include "log.h"
#include "pipeline_cache.h"
#include "shaders.h"

#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

struct SkyCB
{
    float camX[3];  float tanX;
    float camY[3];  float tanY;
    float camZ[3];  float pad0;
    float sunDir[3];       // towards the sun (negated from the params on upload)
    float sunIntensity;
    float turbidity;
    float groundAlbedo;
    float sunDiscCos;
    // HLSL will not let the float2 below straddle a register, so it starts a
    // new one - and this struct must say so. Without this pad the jitter was
    // read one slot off (x from jitterY, y from padding) for as long as the
    // sky has been jittered, and the weather after it was read as its
    // neighbour: fog HEIGHT as fog extinction turned the clear sky to fog.
    float padJ;
    // TAA jitter, in NDC. The sky must be offset by exactly what the geometry
    // was, or the world is anti-aliased against a static background and the
    // horizon crawls where the two meet.
    float jitterX;
    float jitterY;
    float cloudCover;      // weather (5.0q)
    float fogBeta;
    float fogHeight;
    // The night (design §5.4).
    float nightGlow;
    float lightIsMoon;
    float starIrradiance;
    float moonDir[3];      float moonRadiance;
    float trueSun[3];      float moonDiscCos;
    float eq0[3];          float pixelAngle;
    float eq1[3];          float pad3;
    float eq2[3];          float pad4;
    float light2[3];       float light2Intensity;
};
static_assert(offsetof(SkyCB, jitterX) == 16 * 5, "SkyCB: gJitter starts register 5 (HLSL packing)");
static_assert(sizeof(SkyCB) % 16 == 0, "SkyCB must be whole float4s");

} // namespace

bool Sky::Create(ID3D12Device* dev, DXGI_FORMAT rtvFormat)
{
    if (!m_ms.Create(dev)) Log("sky: multiple-scattering table unavailable - single scattering");

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_DESCRIPTOR_RANGE msRange = AtmosphereMs::MsSrvRange();
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &msRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_STATIC_SAMPLER_DESC msSamp = AtmosphereMs::MsSampler(D3D12_SHADER_VISIBILITY_PIXEL);

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &msSamp;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    {
        Log("sky: root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(CreateRootSignature(dev, 0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&m_rootSig))))
        return false;

    // sky.hlsl: the shared atmosphere (atmosphere.hlsli) first, then this pass's own code.
    ComPtr<ID3DBlob> vs = LoadShader("sky.hlsl", "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> ps = LoadShader("sky.hlsl", "PSMain", "ps_5_1");
    if (!vs || !ps) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_rootSig.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;        // drawn before the world
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = rtvFormat;
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count = 1;

    if (FAILED(CreatePipeline(dev, &pd, IID_PPV_ARGS(&m_pso))))
    {
        Log("sky: pipeline state creation failed");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)))) return false;
    // Written once: an unwritten descriptor reads zero, which here would
    // quietly mean "single scattering" rather than an error - so if the table
    // is missing, a null SRV says the same thing deliberately.
    if (m_ms.Ready())
        m_ms.CreateSrv(dev, m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    else
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nv = {};
        nv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(nullptr, &nv, m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    }
    Log("sky: ready");
    return true;
}

void Sky::Record(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                 const float* view, const float* proj, UINT width, UINT height,
                 const SkyParams& p)
{
    if (!m_pso) return;

    SkyCB cb = {};

    // The camera basis lives in the COLUMNS of a row-vector view matrix - the
    // same fact CameraPosFromView depends on in scene.cpp.
    cb.camX[0] = view[0]; cb.camX[1] = view[4]; cb.camX[2] = view[8];
    cb.camY[0] = view[1]; cb.camY[1] = view[5]; cb.camY[2] = view[9];
    cb.camZ[0] = view[2]; cb.camZ[1] = view[6]; cb.camZ[2] = view[10];

    // proj[0] = 1/(aspect*tan(fovY/2)), proj[5] = 1/tan(fovY/2).
    cb.tanX = (proj[0] != 0.0f) ? 1.0f / proj[0] : 1.0f;
    cb.tanY = (proj[5] != 0.0f) ? 1.0f / proj[5] : 1.0f;

    // Params carry the direction light TRAVELS; the model wants the direction
    // TOWARDS the sun. Negated here, once, rather than at every call site.
    float len = sqrtf(p.sunDir[0]*p.sunDir[0] + p.sunDir[1]*p.sunDir[1] + p.sunDir[2]*p.sunDir[2]);
    if (len < 1e-6f) len = 1.0f;
    cb.sunDir[0] = -p.sunDir[0] / len;
    cb.sunDir[1] = -p.sunDir[1] / len;
    cb.sunDir[2] = -p.sunDir[2] / len;

    cb.sunIntensity = p.sunIntensity;
    cb.turbidity    = p.turbidity;
    cb.groundAlbedo = p.groundAlbedo;
    cb.jitterX      = m_jitterX;
    cb.jitterY      = m_jitterY;
    cb.cloudCover   = p.cloudCover;
    cb.fogBeta      = FogBeta(p);
    cb.fogHeight    = p.fogHeight;
    // The sun subtends ~0.53 degrees. cos of the half-angle, widened by the
    // params so it can be exaggerated for visibility while debugging.
    cb.sunDiscCos   = cosf(0.00465f * (p.sunDiscSize > 0.0f ? p.sunDiscSize : 1.0f));

    // The night (design §5.4). The moon subtends ~0.52 degrees, as the sun does.
    cb.nightGlow      = p.nightGlow;
    cb.lightIsMoon    = p.lightIsMoon;
    cb.starIrradiance = p.starIrradiance;
    cb.moonRadiance   = p.moonRadiance;
    cb.moonDiscCos    = cosf(0.00452f);
    for (int k = 0; k < 3; ++k)
    {
        cb.moonDir[k] = p.moonTowards[k];
        cb.trueSun[k] = p.trueSunTowards[k];
        cb.eq0[k] = p.worldToEquatorial[k];
        cb.eq1[k] = p.worldToEquatorial[3 + k];
        cb.eq2[k] = p.worldToEquatorial[6 + k];
        cb.light2[k] = p.light2Towards[k];
    }
    cb.light2Intensity = p.light2Intensity;
    // A pixel's angle: what a star's gaussian is as wide as.
    cb.pixelAngle = 2.0f * cb.tanY / (float)(height > 0 ? height : 1);

    // The table for this composition, before anything samples it. Recomputes
    // only when turbidity, ground albedo or the switch changed.
    m_ms.Update(list, p.turbidity, p.groundAlbedo, m_msOn);

    ID3D12Device* dev = nullptr;
    list->GetDevice(IID_PPV_ARGS(&dev));
    if (!dev) return;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(SkyCB);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> cbRes;
    HRESULT hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&cbRes));
    dev->Release();
    if (FAILED(hr)) return;

    void* mapped = nullptr;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(cbRes->Map(0, &none, &mapped))) return;
    memcpy(mapped, &cb, sizeof(cb));
    cbRes->Unmap(0, nullptr);

    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)width, (LONG)height };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);

    list->SetGraphicsRootSignature(m_rootSig.Get());
    list->SetPipelineState(m_pso.Get());
    list->SetGraphicsRootConstantBufferView(0, cbRes->GetGPUVirtualAddress());
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    m_frameCB = cbRes;   // outlive GPU execution; the frame loop waits
}

} // namespace core
} // namespace ngg
