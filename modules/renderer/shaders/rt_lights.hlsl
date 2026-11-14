// RT local-light shadows (core/rt.cpp). LIGHTS_REF=1|2: a reference, for checking (NGG_LIGHTS_REF).
//
// ---- local lights: one ray to one light, chosen by what it gives (lights.h) -
// The RATIO ESTIMATOR's stochastic half: among the shadowed lights of this pixel's cluster, one chosen with a
// probability in proportion to what it gives here (its irradiance's luminance times the cosine - the same terms the
// scene pass sums), by a sum over the cluster and an inverse-CDF pick on a stratified number; then one ray to a random point on its emitter. The 0/1 is
// then an unbiased estimate of the shadowed fraction of the scene pass's sum, and the denoiser averages it as it does
// the sun's. No light here: 1, which the scene multiplies into nothing.
#include "rt_common.hlsli"

#define NGG_LIGHTS_T0 t11
#define NGG_LIGHTS_T1 t12
#define NGG_LIGHTS_T2 t13
#include "lights.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    float3 p, N;
    uint plant;
#ifdef NRD_INPUTS
    gNrdOut[tid.xy] = kNrdFp16Max;     // unoccluded, unless the one ray below says otherwise
#endif
    if (!SurfaceAt(tid.xy, p, N, plant)) { gOut[tid.xy] = 1.0; return; }

    const float viewZ = dot(p - gCamPos, gCamFwd);
    const uint2 c = gClusters[LightCluster(float2(tid.xy) + 0.5, viewZ, gClusterParams)];
#if LIGHTS_REF == 2
    // NGG_LIGHTS_REF=2: the estimator itself, 16 picks per pixel averaged - what the one pick converges to.
    {
        float vSum = 0.0;
        float allow = -1.0;
        for (uint s = 0; s < 16u; ++s)
        {
            float total = 0.0;
            uint chosen = 0xFFFFFFFFu;
            for (uint k = 0; k < c.y; ++k)
            {
                const uint li = gLightIndex[c.x + k];
                if ((gLights[li].flags & kLightShadows) == 0u) continue;
                float3 l;
                float dist;
                const float3 E = LightIrradiance(li, p, l, dist);
                const float w = dot(E, float3(0.2126, 0.7152, 0.0722)) * (plant != 0u ? 1.0 : saturate(dot(N, l)));
                if (w <= 0.0) continue;
                total += w;
                if (Rand2(tid.xy, gFrame * 17u + s, 101u + k).x * total < w) chosen = li;
            }
            if (chosen == 0xFFFFFFFFu) { vSum += 1.0; continue; }
            const float3 lpos = gLights[chosen].pos;
            const float3 toL = lpos - p;
            const float3 d0 = toL / max(length(toL), 1e-4);
            float3 T, B; Basis(d0, T, B);
            const float2 r = Rand2(tid.xy, gFrame * 17u + s, 7u);
            const float rad = sqrt(r.x) * gLights[chosen].radius, ang = r.y * 6.28318530;
            const float3 target = lpos + T * (rad * cos(ang)) + B * (rad * sin(ang));
            float3 Nn = N;
            if (dot(Nn, d0) < 0.0) Nn = -Nn;
            RayDesc ray;
            ray.Origin = p + Nn * (gBias * (1.0 + length(p - gCamPos) * 0.002));
            const float3 dv = target - ray.Origin;
            const float len = length(dv);
            ray.Direction = dv / max(len, 1e-4);
            ray.TMin = 0.0;
            ray.TMax = max(len - 0.02, 0.0);
            float2 mover;
            const bool hit = Occluded(ray, false, plant, mover);
            const float a = Allow(mover, 2.0 * gLights[chosen].radius / max(len, 1e-3));
            if (a >= 0.0) allow = allow < 0.0 ? a : min(allow, a);
            vSum += hit ? 0.0 : 1.0;
        }
        gOut[tid.xy] = EncodeRaw(vSum / 16.0, allow);
        return;
    }
#elif defined(LIGHTS_REF)
    // NGG_LIGHTS_REF=1: THE REFERENCE - a ray to EVERY light, the exact weighted visibility. The one-ray estimate below
    // is checked against it (design §5.5: the lamp lab, the test lamps' shadows).
    {
        float sumW = 0.0, sumV = 0.0;
        float allow = -1.0;
        for (uint k = 0; k < c.y; ++k)
        {
            const uint li = gLightIndex[c.x + k];
            if ((gLights[li].flags & kLightShadows) == 0u) continue;
            float3 l;
            float dist;
            const float3 E = LightIrradiance(li, p, l, dist);
            const float w = dot(E, float3(0.2126, 0.7152, 0.0722)) * (plant != 0u ? 1.0 : saturate(dot(N, l)));
            if (w <= 0.0) continue;
            const float3 lpos = gLights[li].pos;
            const float3 toL = lpos - p;
            const float3 d0 = toL / max(length(toL), 1e-4);
            float3 T, B; Basis(d0, T, B);
            const float2 r = Rand2(tid.xy, gFrame, 7u + k);
            const float rad = sqrt(r.x) * gLights[li].radius, ang = r.y * 6.28318530;
            const float3 target = lpos + T * (rad * cos(ang)) + B * (rad * sin(ang));
            float3 Nn = N;
            if (dot(Nn, d0) < 0.0) Nn = -Nn;
            RayDesc ray;
            ray.Origin = p + Nn * (gBias * (1.0 + length(p - gCamPos) * 0.002));
            const float3 dv = target - ray.Origin;
            const float len = length(dv);
            ray.Direction = dv / max(len, 1e-4);
            ray.TMin = 0.0;
            ray.TMax = max(len - 0.02, 0.0);
            float2 mover;
            const bool hit = Occluded(ray, false, plant, mover);
            const float a = Allow(mover, 2.0 * gLights[li].radius / max(len, 1e-3));
            if (a >= 0.0) allow = allow < 0.0 ? a : min(allow, a);
            sumW += w;
            sumV += hit ? 0.0 : w;
        }
        gOut[tid.xy] = EncodeRaw(sumW > 0.0 ? sumV / sumW : 1.0, allow);
        return;
    }
#endif
    // Each lamp in proportion to what it gives here: their sum, then the one
    // whose share of it holds this pixel's stratified number (StratU).
    float total = 0.0;
    for (uint k = 0; k < c.y; ++k)
    {
        const uint li = gLightIndex[c.x + k];
        if ((gLights[li].flags & kLightShadows) == 0u) continue;
        float3 l;
        float dist;
        const float3 E = LightIrradiance(li, p, l, dist);
        // A plant's card takes light from both sides (its shading wraps), any other surface from its front.
        total += max(dot(E, float3(0.2126, 0.7152, 0.0722)) * (plant != 0u ? 1.0 : saturate(dot(N, l))), 0.0);
    }
    if (total <= 0.0) { gOut[tid.xy] = 1.0; return; }
    float pick = StratU(tid.xy, gFrame) * total;
    uint chosen = 0xFFFFFFFFu;
    for (uint k = 0; k < c.y && pick >= 0.0; ++k)
    {
        const uint li = gLightIndex[c.x + k];
        if ((gLights[li].flags & kLightShadows) == 0u) continue;
        float3 l;
        float dist;
        const float3 E = LightIrradiance(li, p, l, dist);
        const float w = dot(E, float3(0.2126, 0.7152, 0.0722)) * (plant != 0u ? 1.0 : saturate(dot(N, l)));
        if (w <= 0.0) continue;
        chosen = li;
        pick -= w;
    }
    if (chosen == 0xFFFFFFFFu) { gOut[tid.xy] = 1.0; return; }

    const float3 lpos = gLights[chosen].pos;
    const float lradius = gLights[chosen].radius;
    // A random point on the emitter, a disc of its radius facing this surface: the penumbra, per pixel per frame.
    const float3 toL = lpos - p;
    const float3 d0 = toL / max(length(toL), 1e-4);
    float3 T, B; Basis(d0, T, B);
    const float2 r = StratDisc(tid.xy, gFrame);
    const float rad = sqrt(r.x) * lradius, ang = r.y * 6.28318530;
    const float3 target = lpos + T * (rad * cos(ang)) + B * (rad * sin(ang));

    if (dot(N, d0) < 0.0) N = -N;
    const float offset = gBias * (1.0 + length(p - gCamPos) * 0.002);
    RayDesc ray;
    ray.Origin = p + N * offset;
    const float3 dv = target - ray.Origin;
    const float len = length(dv);
    ray.Direction = dv / max(len, 1e-4);
    ray.TMin = 0.0;
    ray.TMax = max(len - 0.02, 0.0);    // short of the emitter itself
    float2 mover;
    float t;
    const bool hit = OccludedT(ray, false, plant, mover, t);
    // The emitter's disc as seen from here: the penumbra per metre of the hit's distance.
    gOut[tid.xy] = EncodeRaw(hit ? 0.0 : 1.0, Allow(mover, 2.0 * lradius / max(len, 1e-3)));
#ifdef NRD_INPUTS
    // SIGMA's IN_PENUMBRA, the local light's (NRD.hlsli SIGMA_FrontEnd_PackPenumbra): the radius of the penumbra the
    // emitter's disc (diameter 2 r) casts at an occluder t from here and len - t from the lamp. The lamp is the one this
    // pixel picked, so neighbours' penumbrae may be other lamps' - the ratio estimator's 0/1 as the default denoiser
    // takes it.
    if (hit) gNrdOut[tid.xy] = min(lradius * t / max(len - t, 1e-6), kNrdPenumbraMax);
#endif
}
