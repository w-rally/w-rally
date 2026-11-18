// DLSS Ray Reconstruction's own passes (core/rr_visibility.cpp), at the render size.
//
// CSDecode, before the main pass: RR denoises the LIT colour, so the main pass must light with the ray tracer's raw,
// undenoised visibility. The raw traces (Rt::Result, RawAo, RawLights; R16F) carry the moved-object flag in their
// sign (rt_common.hlsli EncodeRaw: -(1 + v + 2 k), k the history the rays allow, in sevenths) - here the plain
// visibility v for the main pass's three slots, and the MOVED MASK: how little history the pixel's rays allow where
// they hit a moving object (1 - k / 7; 0 untouched), the most of the three - our "no ghost behind the car" (NGG
// design 4.5) for RR's bias-current-colour mask, with NGG_RR_BIAS=1 only: RR keeps that road clean on its own.
//
// CSGuides, after it: the main pass's surface targets (the GBUF variants: Scene::SetGbufferWanted) into RR's guides -
//   diffuse albedo   albedo x (1 - metallic): what the main pass's diffuse terms multiply
//   specular albedo  EnvBRDFApprox(f0, roughness, N.V), as the main pass weighs the environment's reflection; under a
//                    clear coat the coat's own term added (kind 3); none on a plant's card (kind 2: no specular lobe)
//   normal+roughness world normal in xyz, linear roughness in w (RR's Roughness_Mode_Packed)
//   bias mask        0.25 where moving glass is in front (DLSS SR's reactive mask); the moved mask with NGG_RR_BIAS=1
// No surface (the sky, kind 0): albedo 1 (nothing to take out), no specular, the normal towards the camera.

#include "brdf.hlsli"

Texture2D<float>  gRawSun    : register(t0);   // decode
Texture2D<float>  gRawAo     : register(t1);
Texture2D<float>  gRawLights : register(t2);
Texture2D<float4> gGbufN     : register(t3);   // guides: R10G10B10A2 octahedral normal, roughness, kind / 3
Texture2D<float4> gGbufA     : register(t4);   //         RGBA8 sRGB (read linear): albedo, metallic
Texture2D<float>  gDepth     : register(t5);   //         reverse-Z, jittered
Texture2D<float2> gGlassMv   : register(t6);   //         Scene::GlassMotionBuffer: x < 6 where moving glass is in front
Texture2D<float>  gMovedIn   : register(t7);   //         CSDecode's moved mask

RWTexture2D<float>  gSun      : register(u0);  // decode
RWTexture2D<float>  gAo       : register(u1);
RWTexture2D<float>  gLights   : register(u2);
RWTexture2D<float>  gMoved    : register(u3);
RWTexture2D<float4> gDiffuse  : register(u4);  // guides
RWTexture2D<float4> gSpecular : register(u5);
RWTexture2D<float4> gNormal   : register(u6);
RWTexture2D<float>  gBias     : register(u7);

cbuffer K : register(b0)
{
    row_major float4x4 gInvViewProj;   // jittered: the surface as the prepass drew it
    float3 gCamPos;
    float  gAoOn;
    uint2  gSize;
    float  gLightsOn;
    float  gGlassOn;
    float  gBiasScale;                 // 1: the moved mask in RR's bias (NGG_RR_BIAS=1, A/B; off by default)
    float  gGuideMode;                 // NGG_RR_GUIDES (A/B): 0 as above, 1 no specular albedo, 2 diffuse albedo white
    float2 gPad;
};

// EnvBRDFApprox, as scene.hlsl has it (Karis' fit to the split-sum term).
float3 EnvBRDF(float3 f0, float roughness, float ndv)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4( 1.0,  0.0425,  1.040, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// The G-buffer's octahedral normal (atmosphere.hlsli AtmOctDecode, which this file does not include: it declares
// the atmosphere's resources).
float3 OctDecode(float2 f)
{
    f = f * 2.0 - 1.0;
    float3 n = float3(f.x, 1.0 - abs(f.x) - abs(f.y), f.y);
    const float t = saturate(-n.y);
    n.x += (n.x >= 0.0) ? -t : t;
    n.z += (n.z >= 0.0) ? -t : t;
    return normalize(n);
}

// The raw value and how little history its rays allow (0 untouched).
float Decode(float r, out float moved)
{
    moved = 0.0;
    if (r >= 0.0) return r;
    r = -r - 1.0;                        // v + 2 k
    const float k = floor(r * 0.5);
    moved = 1.0 - k / 7.0;
    return r - 2.0 * k;
}

[numthreads(8, 8, 1)]
void CSDecode(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    const int3 px = int3(tid.xy, 0);
    float m0, m1 = 0.0, m2 = 0.0;
    gSun[tid.xy] = Decode(gRawSun.Load(px), m0);
    gAo[tid.xy] = gAoOn > 0.5 ? Decode(gRawAo.Load(px), m1) : 1.0;
    gLights[tid.xy] = gLightsOn > 0.5 ? Decode(gRawLights.Load(px), m2) : 1.0;
    gMoved[tid.xy] = max(m0, max(m1, m2));
}

[numthreads(8, 8, 1)]
void CSGuides(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    const int3 px = int3(tid.xy, 0);
    const float4 n = gGbufN.Load(px);
    const float kind = round(n.w * 3.0);
    const float depth = gDepth.Load(px);
    float bias = gMovedIn.Load(px) * gBiasScale;
    if (gGlassOn > 0.5 && gGlassMv.Load(px).x < 6.0) bias = max(bias, 0.25);
    gBias[tid.xy] = saturate(bias);
    if (kind < 0.5 || depth <= 0.0)
    {
        gDiffuse[tid.xy] = float4(1.0, 1.0, 1.0, 0.0);
        gSpecular[tid.xy] = float4(0.0, 0.0, 0.0, 0.0);
        gNormal[tid.xy] = float4(0.0, 1.0, 0.0, 1.0);
        return;
    }
    const float4 am = gGbufA.Load(px);
    const float3 N = OctDecode(n.xy);
    float roughness = n.z, specK = 0.0;
    if (kind > 2.5) GbufRoughUnpack(n.z, roughness, specK);
    const float2 uv = (float2(tid.xy) + 0.5) / float2(gSize);
    const float4 wp = mul(float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0), gInvViewProj);
    const float3 V = normalize(gCamPos - wp.xyz / wp.w);
    const float ndv = saturate(abs(dot(N, V)));
    float3 spec = float3(0.0, 0.0, 0.0);
    if (kind < 1.5 || kind > 2.5)
    {
        spec = EnvBRDF(lerp(float3(0.04, 0.04, 0.04), am.rgb, am.a), roughness, ndv);
        if (kind > 2.5) spec += EnvBRDF(kCoatF0.xxx, kCoatRoughness, ndv);
    }
    gDiffuse[tid.xy] = float4(gGuideMode > 1.5 ? float3(1.0, 1.0, 1.0) : am.rgb * (1.0 - am.a), 0.0);
    gSpecular[tid.xy] = float4(gGuideMode > 0.5 && gGuideMode < 1.5 ? float3(0.0, 0.0, 0.0) : spec, 0.0);
    gNormal[tid.xy] = float4(N, saturate(roughness));
}
