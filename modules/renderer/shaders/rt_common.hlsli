// The ray-traced visibility's common part (core/rt.cpp): the registers, the scene's geometry and the ray helpers.

RaytracingAccelerationStructure gScene        : register(t0);
Texture2D<float>                gDepth        : register(t1);
Texture2D<float4>               gNormal       : register(t6);   // prepass, SNORM8
// For the any-hit test only. Raw views over the SAME buffers the rasteriser
// draws from, so a leaf is cut out by exactly the pixels that were drawn.
ByteAddressBuffer               gVerts        : register(t2);
ByteAddressBuffer               gAlphaIndices : register(t3);
StructuredBuffer<uint>          gTriMaterial  : register(t4);
// The editor's dynamic pool, for instances of the meshes it added.
ByteAddressBuffer               gDynVerts     : register(t7);
// Per TLAS instance (indexed by InstanceID): where its alpha triangles start
// in gAlphaIndices/gTriMaterial, what to add to its indices, which vertex
// buffer they index, and its material when the mesh's triangles do not say.
// moved: the instance's transform changed since last frame (see Occluded);
// was0..was3 then its LAST frame's object-to-world rows (row vectors), so a
// ray's hit point can be put where it was.
struct Inst { uint triBase; uint vertexBase; uint vb; uint material; uint moved;
              float3 was0; float3 was1; float3 was2; float3 was3; };
StructuredBuffer<Inst>          gInst         : register(t8);
// Foliage: the plant each alpha triangle belongs to (~0 = none), and the
// prepass's per-pixel plant (+ 1, 0 = none) - scene.cpp FindPlants.
StructuredBuffer<uint>          gTriPlant     : register(t9);
Texture2D<uint>                 gPlantId      : register(t10);
struct Mat { uint tex; float cutoff; };
StructuredBuffer<Mat>           gMaterials    : register(t5);
Texture2D                       gTextures[]   : register(t0, space1);
SamplerState                    gSamp         : register(s0);
RWTexture2D<float>              gOut          : register(u0);
RWStructuredBuffer<uint>        gCounters     : register(u1);
#ifdef NRD_INPUTS
// THE NRD PATH (core/nrd_visibility.cpp; the -DNRD_INPUTS=1 variants): besides the raw 0/1 in gOut - kept, for the moved-object
// flags it carries - each trace writes what NRD denoises: SIGMA's penumbra for the sun and the lamps, REBLUR's
// normalized hit distance for the AO. Its own table (root parameter 6), so the default variants are unchanged.
RWTexture2D<float>              gNrdOut       : register(u2);
// NRD's "unoccluded" (NRD.hlsli NRD_FP16_MAX) and the largest penumbra radius it takes.
static const float kNrdFp16Max = 65504.0;
static const float kNrdPenumbraMax = 32768.0;
#endif

cbuffer K : register(b0)
{
    row_major float4x4 gInvViewProj;
    float3 gSunTowards;
    float  gTMax;
    uint2  gSize;
    float  gBias;
    uint   gStride;         // bytes per vertex; uv0 at +24, colour at +40
    float3 gCamPos;
    float  gSunAngle;       // the sun's angular RADIUS in radians; 0 = a point
    uint   gFrame;          // decorrelates the samples frame to frame
    float  gAoRadius;       // metres; occluders beyond this do not count
    uint   gAoRays;         // rays per pixel per frame
    uint   gPad2;
    float3 gCamFwd;         // the local lights' clusters: their depth axis (lights.h)
    float  gLightsOn;
    float4 gClusterParams;  // tile w, h (px), slices' near plane, their log scale
};

// A cheap, good integer hash (PCG-style). White noise per pixel per frame;
// the denoiser does the averaging.
uint Hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float2 Rand2(uint2 pixel, uint frame, uint salt)
{
    uint h = Hash(pixel.x * 1973u + pixel.y * 9277u + frame * 26699u + salt * 7919u);
    return float2((h & 0xFFFFu) / 65535.0, (h >> 16) / 65535.0);
}

// STRATIFIED over the screen, as the AO's samples are (AoSample): every 4x4
// tile of pixels takes each of 16 strata once, with a white jitter inside its
// stratum, the whole pattern shifted every frame. White noise let a 5x5
// window draw its samples unevenly - one lamp of three for twelve of its
// pixels and another for five, half the sun's disc for most of them - and
// that luck was the blotches the denoiser blurred but kept: under the
// Garage's car, its floor. Stratified, a window's mean has little luck left.
// StratU: one number (which lamp, Pick), its stratum the tile's (x + 2y,
// 2x + y) cell in 16ths - any 2x2 block of it spans the four quarters.
float StratU(uint2 px, uint frame)
{
    const uint k = ((px.x + 2u * px.y) & 3u) + 4u * ((2u * px.x + px.y) & 3u);
    return frac((float(k) + Rand2(px, frame, 101u).x) / 16.0 + float(frame) * 0.6180339887);
}
// StratDisc: a point on a light's disc (the square, mapped by the caller); a
// second bijection of the tile, (x + 3y, 3x + 2y), so which lamp a pixel
// takes says nothing of where on it the ray goes.
float2 StratDisc(uint2 px, uint frame)
{
    const uint2 cell = uint2((px.x + 3u * px.y) & 3u, (3u * px.x + 2u * px.y) & 3u);
    const float2 j = Rand2(px, frame, 7u);
    return frac((float2(cell) + j) * 0.25 + frac(float(frame) * float2(0.7548776662, 0.5698402910)));
}

// Surface position from depth and the frame's JITTERED matrix, and the
// prepass normal. Shared by every ray type, because a ray origin that differs
// between them by half a pixel would be a different bias problem per feature.
// `plant`: the plant the pixel is a card of, + 1; 0 for anything else. Its
// normal is then the card's own plane (PSPrepass), w = 0.5.
bool SurfaceAt(uint2 px, out float3 p, out float3 N, out uint plant)
{
    plant = 0u;
    float depth = gDepth.Load(int3(px, 0));
    if (depth <= 0.0) return false;                      // reverse-Z far plane: sky
    float2 uv  = (float2(px) + 0.5) / float2(gSize);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = mul(float4(ndc, depth, 1.0), gInvViewProj);
    p = wp.xyz / wp.w;
    const float4 n4 = gNormal.Load(int3(px, 0));
    N = n4.xyz;
    plant = (n4.w > 0.25 && n4.w < 0.75) ? gPlantId.Load(int3(px, 0)) : 0u;
    if (dot(N, N) < 0.25) N = float3(0, 1, 0);
    N = normalize(N);
    return true;
}

// A basis around a direction, for scattering rays about it.
void Basis(float3 d, out float3 T, out float3 B)
{
    float3 up = abs(d.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    T = normalize(cross(up, d));
    B = cross(d, T);
}

// THE ANY-HIT TEST, shared. Opaque geometry (BLAS geometry 0) commits on its
// own; alpha-tested geometry (geometry 1) surfaces as a candidate and is
// accepted or refused by the rasteriser's own test - texture alpha times
// vertex alpha against the material's cutoff - so a leaf blocks exactly the
// pixels it was drawn with. Returns true if anything blocks the ray, and in
// `mover` - if what blocked it is an object that moved this frame - how far
// the point the ray hit moved since last frame and how far along the ray it
// is, metres; x < 0 when it did not move (Allow).
//
// FROM A PLANT (`fromPlant` = its index + 1), that plant's own cards do not
// count. A tree here is 2-3 crossed flat cards, and a card's ray towards the
// sun crosses the other cards of the same tree, so visibility flipped card by
// card: one lit, the one crossing it in shadow - a split no real canopy has.
// The cascades never showed it because the foliage caster bias (400) is
// deeper than a tree. Other plants, and everything opaque, still shadow it -
// EXACTLY its own plant, by id: a first version skipped every plant whose box
// held the ray's start, and grass under a tree is inside the tree's box, so it
// lost the tree's shadow and glowed. What shading the plant's own volume gets
// comes from the PIXEL shader (FoliageSelfShadow), the same on every card.
// How far from a legacy card its rays ignore other cards, metres: past a grass tuft (~0.5 m) and the near cards of a
// tree, short of the canopy over the grass.
static const float kLegacyCardSkip = 1.5;
// `hitT`: how far along the ray the hit is (metres), for NRD's penumbra and hit distance. ACCEPT_FIRST_HIT: the first
// occluder found, not always the nearest - the same ray as the default path's, at the same cost.
bool OccludedT(RayDesc ray, bool count, uint fromPlant, out float2 mover, out float hitT)
{
    mover = float2(-1.0, 0.0);
    hitT = 0.0;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // From a legacy card (~0, scene.cpp PSPrepass): the cards within kLegacyCardSkip of the start do not count -
    // the card's own tuft or tree, whose crossing cards split it card by card - while cards farther off (a
    // canopy over the grass) still shadow it, as everything opaque does.
    q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed())
    {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        if (fromPlant == 0xFFFFFFFFu && q.CandidateTriangleRayT() < kLegacyCardSkip) continue;

        // Which mesh, and where its alpha triangles live: per INSTANCE now,
        // because the world, a moved object and an editor-added mesh are
        // separate BLASes sharing one index buffer and one material table.
        const Inst  inst = gInst[q.CandidateInstanceID()];
        const uint  tri  = inst.triBase + q.CandidatePrimitiveIndex();
        if (fromPlant != 0u && gTriPlant[tri] + 1u == fromPlant)
        {
            if (count) InterlockedAdd(gCounters[3], 1);
            continue;
        }
        const uint3 idx  = gAlphaIndices.Load3(tri * 12) + inst.vertexBase;
        const float2 b   = q.CandidateTriangleBarycentrics();
        const float3 w   = float3(1.0 - b.x - b.y, b.x, b.y);

        // What a plant casts (scene.cpp Plant::shadow, in its vertices' uv1.y above kPlantShadowPack; a plant's
        // vertex has a volume normal, shorter than 0.55): grass nothing - its sun and AO shadows are its cards'
        // outlines - and a bush half its hits, which the denoise turns into a lighter, softer shadow.
        {
            const uint at = idx.x * gStride;
            const float3 pn = asfloat(inst.vb == 0 ? gVerts.Load3(at + 12) : gDynVerts.Load3(at + 12));
            if (dot(pn, pn) < 0.3025)
            {
                const float packed = max(asfloat(inst.vb == 0 ? gVerts.Load(at + 36) : gDynVerts.Load(at + 36)), 0.0);
                const float mode = floor(packed / 1000.0);
                if (mode > 0.5 && mode < 1.5) continue;
                if (mode > 1.5 && frac(sin(dot(ray.Origin + ray.Direction * 7.0 + float(tri) * 0.001,
                                                float3(12.9898, 78.233, 37.719))) * 43758.5453) < 0.5)
                    continue;
            }
        }

        float2 uv0, uv1, uv2;
        float  a0, a1, a2;
        if (inst.vb == 0)
        {
            uv0 = asfloat(gVerts.Load2(idx.x * gStride + 24));
            uv1 = asfloat(gVerts.Load2(idx.y * gStride + 24));
            uv2 = asfloat(gVerts.Load2(idx.z * gStride + 24));
            a0 = float(gVerts.Load(idx.x * gStride + 40) >> 24);
            a1 = float(gVerts.Load(idx.y * gStride + 40) >> 24);
            a2 = float(gVerts.Load(idx.z * gStride + 40) >> 24);
        }
        else
        {
            uv0 = asfloat(gDynVerts.Load2(idx.x * gStride + 24));
            uv1 = asfloat(gDynVerts.Load2(idx.y * gStride + 24));
            uv2 = asfloat(gDynVerts.Load2(idx.z * gStride + 24));
            a0 = float(gDynVerts.Load(idx.x * gStride + 40) >> 24);
            a1 = float(gDynVerts.Load(idx.y * gStride + 40) >> 24);
            a2 = float(gDynVerts.Load(idx.z * gStride + 40) >> 24);
        }
        const float2 tuv = uv0 * w.x + uv1 * w.y + uv2 * w.z;
        const float  va  = (a0 * w.x + a1 * w.y + a2 * w.z) / 255.0;

        const Mat m = gMaterials[inst.material != 0xFFFFFFFFu ? inst.material : gTriMaterial[tri]];
        const float ta = gTextures[NonUniformResourceIndex(m.tex)].SampleLevel(gSamp, tuv, 2.0).a;
        if (ta * va >= m.cutoff)
        {
            q.CommitNonOpaqueTriangleHit();
            if (count) InterlockedAdd(gCounters[2], 1);
        }
        else if (count) InterlockedAdd(gCounters[3], 1);
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
    hitT = q.CommittedRayT();
    const Inst hit = gInst[q.CommittedInstanceID()];
    if (hit.moved != 0)
    {
        const float  t   = q.CommittedRayT();
        const float3 o   = q.CommittedObjectRayOrigin() + q.CommittedObjectRayDirection() * t;
        const float3 was = o.x * hit.was0 + o.y * hit.was1 + o.z * hit.was2 + hit.was3;
        mover = float2(length(ray.Origin + ray.Direction * t - was), t);
    }
    return true;
}
bool Occluded(RayDesc ray, bool count, uint fromPlant, out float2 mover)
{
    float t;
    return OccludedT(ray, count, fromPlant, mover, t);
}

// HOW MUCH HISTORY a pixel may keep when its ray hit a moved object (`mover`,
// from Occluded), as a history weight; -1 when it hit none. Its history holds
// the shadow where it was: with weight b the shadow on the ground lags its
// occluder by b / (1 - b) frames of the occluder's travel. What that costs is
// the lag against how SOFT the shadow is there - its penumbra, the hit's
// distance times the light's angular size (`penumbra`, per metre): a lag well
// inside the penumbra moves the visibility by little, while one frame's own
// coin tosses are off by far more. So the weight is whatever keeps the lag
// within kLagShare of the penumbra. A car driving lags a contact shadow by its
// whole width in one frame: the weight goes to nothing, and the denoiser keeps
// gBlendMoved, as it always did - no trail. A car on the Garage's turntable
// moves its sill some 3-7 mm a frame under a soft spotlight's shadow and keeps
// most of it: at a quarter of history the floor around it boiled every frame
// it turned (the lights' frame-to-frame noise 6x the standing car's). 0.3:
// 0.15 left half as much noise again; full history (0.85) a visible lag line
// along the sill.
static const float kLagShare = 0.3;
float Allow(float2 mover, float penumbra)
{
    if (mover.x < 0.0) return -1.0;
    const float w = kLagShare * mover.y * penumbra;
    return w / (w + mover.x + 1e-6);
}

// The raw result, and whether a ray of it hit a moved object: R16_FLOAT,
// negative for "touched" - the value then -(1 + v + 2 k), k the history weight
// it allows (Allow) in sevenths, rounded down. The denoiser decodes it.
float EncodeRaw(float v, float allow)
{
    return allow < 0.0 ? v : -(1.0 + v + 2.0 * floor(saturate(allow) * 7.0));
}
