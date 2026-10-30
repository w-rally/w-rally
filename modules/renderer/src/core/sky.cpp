#include "sky.h"
#include "log.h"
#include "atmosphere_hlsl.h"

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
    // The night (design 5.4, N1b).
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

const char* kSkySrc = R"(
cbuffer SkyCB : register(b0)
{
    float3 gCamX; float gTanX;
    float3 gCamY; float gTanY;
    float3 gCamZ; float gPad0;
    float3 gSunDir;          // TOWARDS the sun
    float  gSunIntensity;
    float  gTurbidity;
    float  gGroundAlbedo;
    float  gSunDiscCos;
    float2 gJitter;
    float  gCloud;           // weather (5.0q)
    float  gFogBeta;
    float  gFogH;
    float  gNightGlow;       // the night (design 5.4, N1b)
    float  gLightIsMoon;
    float  gStarIrradiance;
    float3 gMoonDir;         float gMoonRadiance;
    float3 gTrueSun;         float gMoonDiscCos;
    float3 gEq0;             float gPixelAngle;
    float3 gEq1;             float gPad3;
    float3 gEq2;             float gPad4;
    float3 gLight2;          float gLight2I;   // the other body's sky (design 5.4)
};

// The atmosphere's extinction of a point source (a star, the moon) along `ray`: ~0.2 magnitudes per air mass in V,
// the air mass ~1/sin(elevation) with a floor at the horizon, then the cloud deck and the fog as the sun gets them.
float PointExtinction(float3 ray, float3 dir)
{
    const float airmass = 1.0 / max(ray.y + 0.025, 0.025);
    return exp(-0.18 * airmass) * AtmCloudSun() * AtmFogSunT(dir);
}

float StarHash(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

// The stars, procedural: the equatorial sphere cut into cube-face cells, ~6 % of them holding a star at a random
// place, its magnitude drawn so that brighter stars are rarer as the sky's counts are (N(<m) ~ 10^(0.5 m): ~9,000
// to magnitude 6). Each is a gaussian a pixel wide holding its whole flux, so a star is as bright at any
// resolution. Fixed to the celestial sphere: they turn with the sidereal time, about the pole.
float3 Stars(float3 ray)
{
    const float3 e = float3(dot(gEq0, ray), dot(gEq1, ray), dot(gEq2, ray));
    const float3 a = abs(e);
    float face; float2 uv;
    if (a.x >= a.y && a.x >= a.z) { face = e.x > 0.0 ? 0.0 : 1.0; uv = e.yz / a.x; }
    else if (a.y >= a.z)          { face = e.y > 0.0 ? 2.0 : 3.0; uv = e.xz / a.y; }
    else                          { face = e.z > 0.0 ? 4.0 : 5.0; uv = e.xy / a.z; }
    const float kCells = 160.0;
    const float2 g = (uv * 0.5 + 0.5) * kCells;
    const float2 cell = floor(g);
    const float3 key = float3(cell, face * 1000.0 + 7.0);
    if (StarHash(key) > 0.06) return float3(0.0, 0.0, 0.0);
    // Its place in the cell (away from the edges, so its gaussian stays in it) and its direction.
    const float2 at = cell + 0.2 + 0.6 * float2(StarHash(key + 11.0), StarHash(key + 23.0));
    const float2 suv = at / kCells * 2.0 - 1.0;
    float3 d;
    if (face < 1.5)      d = float3(face < 0.5 ? 1.0 : -1.0, suv.x, suv.y);
    else if (face < 3.5) d = float3(suv.x, face < 2.5 ? 1.0 : -1.0, suv.y);
    else                 d = float3(suv.x, suv.y, face < 4.5 ? 1.0 : -1.0);
    d = normalize(d);
    const float m = max(6.0 + 2.0 * log10(max(StarHash(key + 37.0), 1e-4)), -1.5);
    const float flux = gStarIrradiance * pow(10.0, -0.4 * m);
    const float sigma = gPixelAngle * 0.7;
    const float r2 = dot(e - d, e - d);
    const float3 tint = lerp(float3(1.0, 0.82, 0.66), float3(0.82, 0.9, 1.0), StarHash(key + 53.0));
    return tint * flux * exp(-r2 / (2.0 * sigma * sigma)) / (6.2831853 * sigma * sigma);
}

struct VSOut { float4 pos : SV_Position; float2 ndc : TEXCOORD0; };

// Fullscreen triangle from the vertex id - no vertex buffer, no input layout.
VSOut VSMain(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.ndc = o.pos.xy;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    // Camera basis + the projection's tangents give the world ray directly, so
    // no matrix inversion is needed and the reverse-Z flip cannot affect it.
    // Geometry was shifted by +gJitter in NDC, so the pixel now sitting at ndc
    // shows the direction that truly lies at ndc - gJitter. Subtracting it here
    // is what keeps the sky in the same sub-pixel frame as the world.
    AtmSetWeather(gCloud, gFogBeta, gFogH, gGroundAlbedo);
    AtmSetNight(gNightGlow);
    AtmSetLight2(gLight2, gLight2I);
    float2 ndc = i.ndc - gJitter;
    float3 ray = normalize(gCamZ + gCamX * (ndc.x * gTanX) + gCamY * (ndc.y * gTanY));

    // Scatter plus the below-horizon ground blend, from the SHARED function -
    // the environment map calls the same one, so the sky the world reflects and
    // the sky behind it cannot drift apart at the horizon.
    float3 col = AtmSkyRadiance(ray, gSunDir, gTurbidity, gSunIntensity,
                                gGroundAlbedo, 16, 6);

    // Sun disc, attenuated by the same atmosphere the sky was. It lives HERE
    // and not in AtmSkyRadiance: the environment map feeds a specular term that
    // already has its own sun lobe.
    float cosSun = dot(ray, gSunDir);
    // The disc dims behind the cloud deck and inside fog - the same factors
    // the direct sun on the ground gets. Clear air leaves it as it was.
    // At night gSunDir is the MOON (the light, design 5.4): no sun disc then - the moon has its own, below.
    if (cosSun > gSunDiscCos && gLightIsMoon < 0.5)
        col += gSunIntensity * 0.02 * smoothstep(gSunDiscCos, 1.0, cosSun)
             * AtmCloudSun() * AtmFogSunT(gSunDir);

    // The MOON's disc (design 5.4, N1b), by day too: a sphere lit by the real sun, so its phase draws itself -
    // the lit part is where the disc's surface faces the sun. Earthshine keeps the dark part just visible.
    bool behindMoon = false;
    if (gMoonRadiance > 0.0 && gMoonDir.y > -0.02)
    {
        const float cosMoon = dot(ray, gMoonDir);
        if (cosMoon > gMoonDiscCos - 0.0001)
        {
            const float sinR = sqrt(max(1.0 - gMoonDiscCos * gMoonDiscCos, 1e-12));
            const float3 side = normalize(abs(gMoonDir.y) < 0.99 ? cross(float3(0.0, 1.0, 0.0), gMoonDir)
                                                                 : float3(1.0, 0.0, 0.0));
            const float3 upm = cross(gMoonDir, side);
            const float2 q = float2(dot(ray, side), dot(ray, upm)) / sinR;
            const float r2 = dot(q, q);
            if (r2 < 1.0)
            {
                const float3 n = side * q.x + upm * q.y - gMoonDir * sqrt(1.0 - r2);
                const float lit = saturate(dot(n, gTrueSun)) + 0.002;
                const float edge = saturate((1.0 - sqrt(r2)) * sinR / max(gPixelAngle, 1e-6));   // a pixel's AA
                col += gMoonRadiance * lit * edge * float3(1.0, 0.96, 0.9) * PointExtinction(ray, gMoonDir);
                behindMoon = true;
            }
        }
    }
    // The stars: there by day too, physically - the sky outshines them a hundred-million-fold until the night's
    // exposure brings them out. Not behind the moon, and not below the horizon.
    if (gStarIrradiance > 0.0 && !behindMoon && ray.y > -0.02)
        col += Stars(ray) * PointExtinction(ray, ray);

    // Linear HDR out, exactly like the scene pass. Exposure, the tonemap curve
    // and the sRGB encode all live in PostFx now - having them here was what
    // made the sky respond to the exposure control while surfaces did not.
    return float4(col, 1.0);
}
)";

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
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&m_rootSig))))
        return false;

    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    // Shared atmosphere first, then this pass's own code.
    std::string src = std::string(kAtmosphereHlsl) + kSkySrc;

    ComPtr<ID3DBlob> vs, ps;
    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_sky", nullptr, nullptr,
                          "VSMain", "vs_5_1", flags, 0, &vs, &err)))
    {
        Log("sky: VS compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_sky", nullptr, nullptr,
                          "PSMain", "ps_5_1", flags, 0, &ps, &err)))
    {
        Log("sky: PS compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

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

    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pso))))
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

    // The night (design 5.4, N1b). The moon subtends ~0.52 degrees, as the sun does.
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
