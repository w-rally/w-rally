// The sky pass (core/sky.cpp).
#include "atmosphere.hlsli"

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
