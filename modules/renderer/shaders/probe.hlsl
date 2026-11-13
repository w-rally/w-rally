// The reflection probe's resolve and downsample (core/probe.cpp).

cbuffer ProbeCB : register(b0)
{
    uint gSize;    // the destination mip's size
    uint3 gPad;
};
Texture2DArray<float4> gFaces : register(t0);
Texture2D<float4>      gEnv   : register(t1);
RWTexture2D<float4>    gDst   : register(u0);
RWTexture2D<float4>    gSrc   : register(u1);
SamplerState           gLin   : register(s0);

// As AtmOctEncode / AtmOctDecode (atmosphere_hlsl.h): the scene samples this chain with the environment's mapping.
float2 OctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 o = n.xz;
    if (n.y < 0.0) o = (1.0 - abs(float2(o.y, o.x))) * float2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0);
    return o * 0.5 + 0.5;
}
float3 OctDecode(float2 f)
{
    f = f * 2.0 - 1.0;
    float3 n = float3(f.x, 1.0 - abs(f.x) - abs(f.y), f.y);
    float t = saturate(-n.y);
    n.x += (n.x >= 0.0) ? -t : t;
    n.z += (n.z >= 0.0) ? -t : t;
    return normalize(n);
}

static const float3 kF[6] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };
static const float3 kU[6] = { float3(0, 1, 0), float3(0, 1, 0), float3(0, 0, -1), float3(0, 0, 1), float3(0, 1, 0), float3(0, 1, 0) };

// Mip 0: each texel's direction from the face it falls on (the face's own projection: x / z right, y / z up), the sky
// where that face saw nothing (alpha: how much of the texel is the world).
[numthreads(8, 8, 1)]
void CSResolve(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= gSize)) return;
    const float3 d = OctDecode((float2(id) + 0.5) / float(gSize));
    const float3 a = abs(d);
    const uint f = (a.x >= a.y && a.x >= a.z) ? (d.x > 0.0 ? 0u : 1u)
                 : (a.y >= a.z)               ? (d.y > 0.0 ? 2u : 3u)
                                              : (d.z > 0.0 ? 4u : 5u);
    const float3 R = cross(kU[f], kF[f]);
    const float z = dot(d, kF[f]);
    const float2 uv = float2(0.5 + 0.5 * dot(d, R) / z, 0.5 - 0.5 * dot(d, kU[f]) / z);
    const float4 c = gFaces.SampleLevel(gLin, float3(uv, f), 0.0);
    const float3 sky = gEnv.SampleLevel(gLin, OctEncode(d), 0.0).rgb;
    gDst[id] = float4(c.rgb + (1.0 - saturate(c.a)) * sky, 1.0);
}

// Mip n from mip n - 1: a 2x2 box, as the environment's chain.
[numthreads(8, 8, 1)]
void CSDown(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= gSize)) return;
    const uint2 s = id * 2;
    gDst[id] = 0.25 * (gSrc[s] + gSrc[s + uint2(1, 0)] + gSrc[s + uint2(0, 1)] + gSrc[s + uint2(1, 1)]);
}
