// The local lights (core/lights.h): the light records, the clusters and their irradiance. Included by the scene pass
// (NGG_LIGHTS_T0..T2 = t12..t14) and by the RT local-light shadows (rt_lights.hlsl).

// ---- local lights (lights.h) ------------------------------------------------
// The includer defines NGG_LIGHTS_T0/T1/T2, the registers of the three buffers (the scene and the ray tracer bind
// them in different places).
struct Light
{
    float3 pos;    float invRange2;  // 1 / range^2
    float3 colour; float radius;     // tint x intensity, irradiance units at 1 m
    float3 dir;    float cosOuter;
    float3 up;     float spotScale;  // a beam's up; 1 / (cos inner - cos outer)
    uint   type;   uint flags;  uint lobeCount; float pad;
    float4 lobeA[4];                 // a beam's lobes: candela, 1/tan(half-width), 1/tan(half-height), sharpness/2
    float4 lobeB[4];                 // and their centres: tan(yaw), tan(pitch)
};
StructuredBuffer<Light> gLights     : register(NGG_LIGHTS_T0);
StructuredBuffer<uint2> gClusters   : register(NGG_LIGHTS_T1);   // per cluster: first index, count
StructuredBuffer<uint>  gLightIndex : register(NGG_LIGHTS_T2);
static const uint kLightShadows = 1u;
static const uint kLightSpot = 1u, kLightBeam = 2u;
static const uint kClustersX = 16u, kClustersY = 9u, kClustersZ = 24u;

// The cluster of a pixel at a view depth. params: tile width and height in pixels, the slices' near plane, their
// log scale (slices per natural-log unit of depth).
uint LightCluster(float2 px, float viewZ, float4 params)
{
    const uint cx = min((uint)(px.x / params.x), kClustersX - 1u);
    const uint cy = min((uint)(px.y / params.y), kClustersY - 1u);
    const float s = log(max(viewZ, params.z) / params.z) * params.w;
    const uint cz = min((uint)max(s, 0.0), kClustersZ - 1u);
    return (cz * kClustersY + cy) * kClustersX + cx;
}

// A light's record but its lobes, READ AT ONCE: every field from the buffer by index - never through a copy of the
// struct, which indexed by a variable (its lobes) is an indexable temporary in scratch memory, ~1.5 ms per light at
// 1080p on the development GPU - and every one of them used whatever the light's type (LightIrradiance selects, it
// does not branch), so the compiler issues them as one clause (one round trip, not four), and with a wave-uniform
// index as scalar loads (the scene pass's light loop, design §5.5).
struct LightRec
{
    float3 pos;    float invRange2;
    float3 colour; float radius;
    float3 dir;    float cosOuter;
    float3 up;     float spotScale;
    uint   type;   uint flags;  uint lobeCount;
};
LightRec LightLoad(uint li)
{
    LightRec L;
    L.pos = gLights[li].pos;           L.invRange2 = gLights[li].invRange2;
    L.colour = gLights[li].colour;     L.radius = gLights[li].radius;
    L.dir = gLights[li].dir;           L.cosOuter = gLights[li].cosOuter;
    L.up = gLights[li].up;             L.spotScale = gLights[li].spotScale;
    L.type = gLights[li].type;         L.flags = gLights[li].flags;
    L.lobeCount = gLights[li].lobeCount;
    return L;
}

// What light `li` (its record L) gives at p, as an irradiance on a surface facing it (rgb, before visibility and the
// cosine), and the direction towards it. Inverse square, softened inside the emitter's radius and faded to nothing at
// its range (a window, so the clusters' cut-off never shows); a spot's cone; a beam's lobes.
float3 LightIrradiance(uint li, LightRec L, float3 p, out float3 l, out float dist)
{
    // Per light per pixel, so the divisions the light alone decides are made on the CPU (invRange2, spotScale, the
    // lobes' 1/tan) and the distance and direction come from one rsqrt.
    const float3 d = L.pos - p;
    const float d2 = dot(d, d);
    const float inv = rsqrt(max(d2, 1e-8));
    dist = d2 * inv;
    l = d * inv;
    const float x = d2 * L.invRange2;
    const float w = saturate(1.0 - x * x);
    float atten = w * w / max(d2, L.radius * L.radius);
    const float cd = dot(-l, L.dir);
    const float s = saturate((cd - L.cosOuter) * L.spotScale);   // smoothstep(cosOuter, cosInner, cd)
    atten *= L.type == kLightSpot ? s * s * (3.0 - 2.0 * s) : 1.0;
    // A BEAM (design §5.5): the sum of its lobes, each an elliptical peak in the lamp's frame, measured in the
    // tangent plane (across and up over along) - read only where it is used, the lobes from the buffer by index.
    if (L.type == kLightBeam)
    {
        float beam = 0.0;
        if (cd > 1e-3)
        {
            const float3 toP = -l;
            const float2 t = float2(dot(toP, cross(L.up, L.dir)), dot(toP, L.up)) / cd;
            [loop] for (uint k = 0; k < L.lobeCount; ++k)
            {
                const float4 a = gLights[li].lobeA[k];
                const float2 q = (t - gLights[li].lobeB[k].xy) * a.yz;
                beam += a.x * exp2(-pow(max(dot(q, q), 1e-12), a.w));
            }
        }
        atten *= beam;
    }
    return L.colour * atten;
}
float3 LightIrradiance(uint li, float3 p, out float3 l, out float dist)
{
    return LightIrradiance(li, LightLoad(li), p, l, dist);
}
