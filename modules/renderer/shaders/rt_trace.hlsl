// RT sun shadows (core/rt.cpp).
//
// ---- sun shadow: one ray towards a random point on the sun's disc ---------
#include "rt_common.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    float3 p, N;
    uint plant;
    if (!SurfaceAt(tid.xy, p, N, plant)) { gOut[tid.xy] = 1.0; return; }

    // Normal-offset origin, the normal flipped towards the sun so a two-sided
    // leaf card is pushed to whichever side the ray leaves from. The offset
    // grows with distance because depth precision does.
    if (dot(N, gSunTowards) < 0.0) N = -N;
    float offset = gBias * (1.0 + length(p - gCamPos) * 0.002);

    // SOFT SHADOWS: the sun is a DISC. A random point on it per pixel per
    // frame; the denoiser turns the binary answers into the penumbra. Uniform
    // over the disc (sqrt on the radius), or the centre is over-sampled.
    float3 dir = gSunTowards;
    if (gSunAngle > 0.0)
    {
        float3 T, B; Basis(dir, T, B);
        float2 r  = StratDisc(tid.xy, gFrame);
        float rad = sqrt(r.x) * tan(gSunAngle);
        float ang = r.y * 6.28318530;
        dir = normalize(dir + T * (rad * cos(ang)) + B * (rad * sin(ang)));
    }

    RayDesc ray;
    ray.Origin    = p + N * offset;
    ray.Direction = dir;
    ray.TMin      = 0.0;
    ray.TMax      = gTMax;

    float2 mover;
    bool hit = Occluded(ray, true, plant, mover);
    // The sun's disc: the penumbra per metre of the hit's distance.
    gOut[tid.xy] = EncodeRaw(hit ? 0.0 : 1.0, Allow(mover, 2.0 * tan(gSunAngle)));
    InterlockedAdd(gCounters[0], 1);
    if (hit) InterlockedAdd(gCounters[1], 1);
}
