// The aerial-perspective volume (core/aerial.cpp).
#include "atmosphere.hlsli"

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
