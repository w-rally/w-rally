// The environment probe: the sky into the cube, its downsample and irradiance (core/env.cpp).
#include "atmosphere.hlsli"

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
    float  gNightGlow;       // design §5.4: the moonless sky's own radiance
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
