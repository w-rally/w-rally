// RT ambient occlusion (core/rt.cpp).
//
// ---- ambient occlusion: cosine-weighted rays over the hemisphere ----------
#include "rt_common.hlsli"

// STRATIFIED over the screen. A 4x4 tile of pixels takes the 16 cells of a
// 4x4 grid on the sample square, one each - (x + 2y, 2x + y) mod 4 is a
// bijection, and any 2x2 block of it already has four different azimuths and
// four different elevations - with a white jitter inside the cell. The
// denoiser's AO window spans exactly one period each way (kDenoiseSrc), so on
// an even surface its mean is a 16-sample STRATIFIED estimate rather than 25
// independent coin tosses: most of the variance of white noise is the luck
// of where the samples fall, and this takes the luck out. Every frame the
// whole grid is shifted on the square (toroidally, by the R2 sequence), which
// keeps the cells apart and walks each pixel's sample over the hemisphere.
float2 AoSample(uint2 px, uint frame, uint i, uint n)
{
    const uint2  cell = uint2((px.x + 2u * px.y) & 3u, (2u * px.x + px.y) & 3u);
    const float2 j    = Rand2(px, frame, i + 1u);
    // n rays split their cell along the azimuth, so they stay apart too.
    const float2 u    = (float2(cell) + float2((float(i) + j.x) / float(n), j.y)) * 0.25;
    return frac(u + frac(float(frame) * float2(0.7548776662, 0.5698402910)));
}

// HOW FAST THE OCCLUSION CHANGES across the ground about an occluder at a
// distance t, per metre of t: what Allow takes as an AO ray's "penumbra". It
// was the whole distance (1.0), and behind a driving car - 17 cm a frame at
// 18 km/h, its bumper some 0.3-1 m from the road its rays leave - that let
// the road keep over half its history: the car's occlusion where it had been,
// the road 10-15 grey darker than its rays said, half a metre behind the car;
// and where the exhaust or the bumper had just uncovered the road, which had
// no history to lag, it came out right - lighter, in their shape: the GHOST
// BEHIND THE CAR, a copy of the bumper and the exhaust a frame of travel
// behind them (view 13, TAA off; tools/car-ghost). The occlusion under a car
// falls off far faster than its distance: 0.15 leaves a driving car's road
// nothing to lag (its weight under gBlendMoved, the AO's 0), and the Garage's
// turning car, its sill a few millimetres a frame, still over two thirds.
static const float kAoPenumbra = 0.15;

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    float3 p, N;
    uint plant;
    if (!SurfaceAt(tid.xy, p, N, plant))
    {
        gOut[tid.xy] = 1.0;
#ifdef NRD_INPUTS
        gNrdOut[tid.xy] = 1.0;
#endif
        return;
    }

    // The prepass normal already faces the camera, which is the side the
    // ambient light arrives from as far as this pixel is concerned.
    float offset = gBias * (1.0 + length(p - gCamPos) * 0.002);
    // A PLANT's card is not a surface facing one way: its lighting uses the
    // plant's volume normal, and a hemisphere about the card's plane - mostly
    // horizontal - sends half its rays into the ground. That ground is not
    // missing sky; the ambient term already lights the card from it (E_ground
    // * down, scene.cpp). It made grass ~7% darker than under the cascades,
    // which have no AO. So a plant looks UP: the sky it is occluded from is
    // the sky, by its neighbours and anything over it. The origin still
    // leaves the card along its own plane's normal.
    const float3 axis = plant != 0u ? float3(0.0, 1.0, 0.0) : N;
    float3 T, B; Basis(axis, T, B);

    // COSINE-WEIGHTED hemisphere: rays are distributed the way the diffuse
    // ambient integral weights them, so the average of hit/miss IS the
    // occluded fraction of the ambient term with no extra weighting. The
    // radius is where this stops being "occlusion" and starts being "global
    // illumination": an occluder ten metres away blocks sky but a wall that
    // far away also reflects light we are not simulating, so beyond the radius
    // the honest answer is "unknown", which is what unoccluded means here.
    float open = 0.0;
    float hitDist = 0.0;      // NRD: the rays' normalized hit distances, summed
    float allow = -1.0;       // the least any ray allows (Allow), -1 none touched
    for (uint i = 0; i < gAoRays; ++i)
    {
        float2 r = AoSample(tid.xy, gFrame, i, gAoRays);
        float phi = r.x * 6.28318530;
        float cosT = sqrt(1.0 - r.y);            // cosine-weighted
        float sinT = sqrt(r.y);
        float3 dir = T * (cos(phi) * sinT) + B * (sin(phi) * sinT) + axis * cosT;

        RayDesc ray;
        ray.Origin    = p + N * offset;
        ray.Direction = dir;
        ray.TMin      = 0.0;
        ray.TMax      = gAoRadius;
        float2 mover;
        float t;
        const bool hit = OccludedT(ray, false, plant, mover, t);
        if (!hit) open += 1.0;
        // REBLUR's normalized hit distance (NRD.hlsli REBLUR_FrontEnd_GetNormHitDist with the hit distance parameters
        // nrd_visibility.cpp sets: A = the radius, B ~ 0, so the normalization is the radius itself): 1 for a miss. What
        // REBLUR takes only with NGG_NRD_AO=distance; by default it takes the open fraction (nrd_pack.hlsl).
        hitDist += hit ? saturate(t / gAoRadius) : 1.0;
        // An occluder this far off changes the occlusion over a fraction of
        // that across the ground: its "penumbra" (kAoPenumbra).
        const float a = Allow(mover, kAoPenumbra);
        if (a >= 0.0) allow = allow < 0.0 ? a : min(allow, a);
    }
    gOut[tid.xy] = EncodeRaw(open / max(1.0, float(gAoRays)), allow);
#ifdef NRD_INPUTS
    // REBLUR_DIFFUSE_OCCLUSION's IN_DIFF_HITDIST: the mean over this pixel's rays.
    gNrdOut[tid.xy] = hitDist / max(1.0, float(gAoRays));
#endif
}
