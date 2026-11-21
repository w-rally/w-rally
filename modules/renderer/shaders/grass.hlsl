// GRASS (core/grass.h, NGG design §2.6 "Grass"): the field round the camera and the spawn of its blades. The blades
// themselves are drawn by scene.hlsl (VSGrass, the GRASS variants), so they light as the rest of the world does.
//
// The chunk's texels are read straight from its bytes (gChunk: the NGGF payload as the file holds it), by the offsets
// its records carry: masks RG8 (R density, G dryness) rows top first, sampled with the ground texture's own UV,
// wrapping; paint tiles RGBA8 (R density scale, G height scale x64, B type override + 1, A 0), i along +x, rows +z.

// ---------------------------------------------------------------------------------------------------------------------
// THE FIELD: the terrain drawn from above into the clipmap (Grass::DrawField).
// ---------------------------------------------------------------------------------------------------------------------
cbuffer FieldCB : register(b0)
{
    row_major float4x4 gFieldViewProj;   // world -> the clipmap's clip space (y to depth: higher is nearer)
    uint  gFieldFlags;                   // Scene::kDrawSingleStage (256): no second layer
    uint  gBaseMaskOff, gBaseMaskSize, gBaseMaskType;        // the base layer's mask (offset ~0: none); size w | h << 16
    uint  gDetailMaskOff, gDetailMaskSize, gDetailMaskType;  // the detail layer's
    float gFactorR, gFactorG, gFactorB;                      // the material's base colour factor
    int   gGridX0, gGridZ0;              // the paint's tile grid: its first tile
    uint  gGridW, gGridH;                // and its size, in tiles
    float gCellSize;                     // metres per paint texel
    uint  gTileCells;                    // texels along a tile's side
    float gOffsetX, gOffsetZ;            // the variant's offset (its tiles move with its vertices)
};
Texture2D           gBase   : register(t0);   // the material's five slots (the scene's heap): base colour,
Texture2D           gDetail : register(t1);   // detail
ByteAddressBuffer   gChunk  : register(t5);
StructuredBuffer<uint> gGrid : register(t6);  // per tile of the grid: its texels' offset in the chunk, ~0 neutral
SamplerState        gSampWrap : register(s0);

struct FieldIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 col : COLOR0; };
struct FieldOut { float4 pos : SV_Position; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 col : COLOR0; float3 wpos : TEXCOORD2; };

FieldOut VSField(FieldIn i)
{
    FieldOut o;
    o.pos = mul(float4(i.pos, 1.0), gFieldViewProj);
    o.uv0 = i.uv0;
    o.uv1 = all(isfinite(i.uv1)) ? i.uv1 : i.uv0;
    o.col = i.col;
    o.wpos = i.pos;
    return o;
}

// Two bytes of the chunk at byte `at` (2-aligned): an RG8 texel.
float2 ChunkRG8(uint at)
{
    const uint w = gChunk.Load(at & ~3u);
    const uint v = (at & 2u) ? (w >> 16) : (w & 0xFFFFu);
    return float2(v & 255u, v >> 8) / 255.0;
}

// A mask, bilinear, wrapping, by the texture's UV (the mask covers the texture texel for texel at its own size).
float2 MaskAt(uint off, uint size, float2 uv)
{
    if (off == 0xFFFFFFFFu) return float2(0.0, 0.0);
    const uint w = size & 0xFFFFu, h = size >> 16;
    const float2 p = uv * float2(w, h) - 0.5;
    const float2 f = frac(p);
    const int2 b = (int2)floor(p);
    float2 r[4];
    [unroll] for (uint k = 0; k < 4u; ++k)
    {
        const int2 q = b + int2(k & 1u, k >> 1);
        const uint x = (uint)(((q.x % (int)w) + (int)w) % (int)w), y = (uint)(((q.y % (int)h) + (int)h) % (int)h);
        r[k] = ChunkRG8(off + (y * w + x) * 2u);
    }
    return lerp(lerp(r[0], r[1], f.x), lerp(r[2], r[3], f.x), f.y);
}

// One paint texel by its global index (gi along +x, gj along +z, from the variant's origin): RGBA8 as 0..255; neutral
// (255, 64, 0, 0) where no tile is.
float4 PaintTexel(int gi, int gj)
{
    const int n = (int)gTileCells;
    const int tx = (int)floor((float)gi / n), tz = (int)floor((float)gj / n);
    const int gx = tx - gGridX0, gz = tz - gGridZ0;
    if (gx < 0 || gz < 0 || gx >= (int)gGridW || gz >= (int)gGridH) return float4(255.0, 64.0, 0.0, 0.0);
    const uint off = gGrid[(uint)gz * gGridW + (uint)gx];
    if (off == 0xFFFFFFFFu) return float4(255.0, 64.0, 0.0, 0.0);
    const uint li = (uint)(gi - tx * n), lj = (uint)(gj - tz * n);
    const uint v = gChunk.Load(off + (lj * gTileCells + li) * 4u);
    return float4(v & 255u, (v >> 8) & 255u, (v >> 16) & 255u, v >> 24);
}

struct FieldTargets
{
    float  height  : SV_Target0;   // R32_FLOAT: the ground's world y
    float4 colour  : SV_Target1;   // sRGB: the ground's albedo (the main pass's blend), a the mask's dryness
    float4 field   : SV_Target2;   // r density 0..1 (masks x paint), g height scale / 4, b type / 255, a 1: covered
};
FieldTargets PSField(FieldOut i)
{
    const bool single = (gFieldFlags & 256u) != 0u;
    const float4 t0 = gBase.Sample(gSampWrap, i.uv0);
    const float4 t1 = single ? t0 : gDetail.Sample(gSampWrap, i.uv1);
    const float w = single ? 0.0 : saturate(t1.a * i.col.a);   // the main pass's weight (scene.hlsl Shade)

    const float2 mb = MaskAt(gBaseMaskOff, gBaseMaskSize, i.uv0);
    const float2 md = single ? float2(0.0, 0.0) : MaskAt(gDetailMaskOff, gDetailMaskSize, i.uv1);
    const float2 m = lerp(mb, md, w);
    // The dominant layer's type; a layer without a mask lends the other's (it grows nothing itself).
    uint type = (w < 0.5 || gDetailMaskOff == 0xFFFFFFFFu) ? gBaseMaskType : gDetailMaskType;
    if (gBaseMaskOff == 0xFFFFFFFFu) type = gDetailMaskType;

    // The paint, bilinear (its type override nearest).
    const float2 pp = float2(i.wpos.x - gOffsetX, i.wpos.z - gOffsetZ) / gCellSize - 0.5;
    const int2 pb = (int2)floor(pp);
    const float2 pf = pp - pb;
    const float4 p00 = PaintTexel(pb.x, pb.y), p10 = PaintTexel(pb.x + 1, pb.y);
    const float4 p01 = PaintTexel(pb.x, pb.y + 1), p11 = PaintTexel(pb.x + 1, pb.y + 1);
    const float4 paint = lerp(lerp(p00, p10, pf.x), lerp(p01, p11, pf.x), pf.y);
    const float4 pn = (pf.y < 0.5) ? (pf.x < 0.5 ? p00 : p10) : (pf.x < 0.5 ? p01 : p11);
    if (pn.b > 0.5) type = (uint)(pn.b + 0.5) - 1u;

    FieldTargets o;
    o.height = i.wpos.y;
    o.colour = float4(lerp(t0.rgb, t1.rgb, w) * float3(gFactorR, gFactorG, gFactorB), m.y);
    o.field = float4(m.x * paint.r / 255.0, saturate(paint.g / 64.0 / 4.0), (float)min(type, 255u) / 255.0, 1.0);
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// THE SPAWN: one thread per 0.5 m patch of the world within reach (Grass::Update).
// ---------------------------------------------------------------------------------------------------------------------
cbuffer SpawnCB : register(b0)
{
    float  gClipX0, gClipZ0;      // world xz of the clipmap's texel (0, 0) corner
    float  gClipSize;             // its side, metres
    float  gReach;                // grass gone by here, metres
    float3 gEye;
    float  gFullTo;               // whole density to here
    int    gPatchX0, gPatchZ0;    // the dispatch's first patch, in the world's patches
    uint   gPatchesSide;
    float  gDensityScale;         // NGG_GRASS_DENSITY
    float  gLod0, gLod1;          // metres: 7-triangle blades to gLod0, 3-triangle to gLod1, cards beyond
    uint   gTypeCount;
    float  gMaxDensity;           // the densest type's blades per m2 (candidates per m2 = this x gDensityScale)
    float4 gPlanes[6];            // the view frustum, world space (inside: dot >= 0)
    uint   gCap0, gCap1, gCap2;   // instances per LOD
    uint   gSpawnPad;
};
struct GrassTypeGpu { float heightMin, heightMax, density, width, dryness, spread, wind, seedHeads; };
Texture2D<float>  gFieldHeight : register(t0);
Texture2D<float4> gFieldColour : register(t1);
Texture2D<float4> gField       : register(t2);
StructuredBuffer<GrassTypeGpu> gTypes : register(t3);
RWByteAddressBuffer gInst : register(u0);   // 32-byte instances (scene.hlsl GrassIn)
RWByteAddressBuffer gArgs : register(u1);   // kLods DRAW records (16 bytes), then the raw counts at 48
SamplerState gSampClampSpawn : register(s0);

static const float kPatch = 0.5;
static const uint  kClump = 8u;     // blades a card stands for

uint Pcg(uint v)
{
    const uint s = v * 747796405u + 2891336453u;
    const uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float U01(uint h) { return (float)(h >> 8) * (1.0 / 16777216.0); }

float2 ClipUv(float2 xz) { return (xz - float2(gClipX0, gClipZ0)) / gClipSize; }

// The blades of a patch are the same every frame for the same patch: candidate k sits at a hashed point and is kept
// when its stratified threshold (k + jitter) / n is under its keep probability - the fade times the density there over
// the densest type's. So as the camera moves away a patch loses its blades one by one, and none moves.
[numthreads(8, 8, 1)]
void CSSpawn(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gPatchesSide || id.y >= gPatchesSide) return;
    const int2 patch = int2(gPatchX0 + (int)id.x, gPatchZ0 + (int)id.y);
    const float2 c = (float2(patch) + 0.5) * kPatch;
    const float2 d2 = c - gEye.xz;
    const float flat = length(d2);
    if (flat > gReach + kPatch) return;
    const float2 cuv = ClipUv(c);
    if (any(cuv < 0.0) || any(cuv > 1.0)) return;

    // Anything here at all? The patch's corners and centre.
    const float hc = gFieldHeight.SampleLevel(gSampClampSpawn, cuv, 0);
    float some = 0.0;
    [unroll] for (uint q = 0; q < 5u; ++q)
    {
        const float2 o = q == 4u ? float2(0.0, 0.0) : (float2(q & 1u, q >> 1) - 0.5) * kPatch;
        const float4 f = gField.SampleLevel(gSampClampSpawn, ClipUv(c + o), 0);
        some = max(some, f.a > 0.5 ? f.r : 0.0);
    }
    if (some <= 0.0) return;

    // In view: a sphere round the patch's grass (the tallest a blade gets, leaning).
    const float3 centre = float3(c.x, hc + 0.75, c.y);
    const float radius = kPatch * 0.75 + 1.5;
    [unroll] for (uint p = 0; p < 6u; ++p)
        if (dot(gPlanes[p].xyz, centre) + gPlanes[p].w < -radius) return;

    const float dist = length(centre - gEye);
    const float fade = 1.0 - smoothstep(gFullTo, gReach, dist);
    if (fade <= 0.0) return;
    const uint lod = dist < gLod0 ? 0u : dist < gLod1 ? 1u : 2u;
    // Fewer blades, wider: the cover fades slower than the count.
    const float widen = clamp(rsqrt(max(fade, 0.08)), 1.0, 2.5);

    const uint seed = Pcg((uint)patch.x * 73856093u ^ (uint)patch.y * 19349663u);
    const float candidates = gMaxDensity * gDensityScale * kPatch * kPatch / (lod == 2u ? (float)kClump : 1.0);
    const uint n = (uint)max(1.0, ceil(candidates));
    const uint loop = min(n, (uint)ceil((float)n * fade));

    // Two passes over the same candidates: count, reserve, then write - one atomic per patch.
    uint count = 0;
    for (uint k = 0; k < loop; ++k)
    {
        const uint h0 = Pcg(seed ^ (k * 0x9E3779B9u));
        const uint h1 = Pcg(h0);
        const float2 at = c + (float2(U01(h1), U01(Pcg(h1))) - 0.5) * kPatch;
        const float4 f = gField.SampleLevel(gSampClampSpawn, ClipUv(at), 0);
        if (f.a < 0.5 || f.r <= 0.0) continue;
        const uint t = min((uint)(f.b * 255.0 + 0.5), gTypeCount - 1u);
        const float keep = fade * f.r * gTypes[t].density / gMaxDensity;
        if (((float)k + U01(h0)) / (float)n < keep) ++count;
    }
    if (count == 0u) return;
    uint base;
    gArgs.InterlockedAdd(48u + lod * 4u, count, base);
    const uint cap = lod == 0u ? gCap0 : lod == 1u ? gCap1 : gCap2;
    if (base >= cap) return;
    const uint start = lod == 0u ? 0u : lod == 1u ? gCap0 : gCap0 + gCap1;
    uint slot = start + base;
    const uint end = start + min(cap, base + count);

    for (uint k2 = 0; k2 < loop && slot < end; ++k2)
    {
        const uint h0 = Pcg(seed ^ (k2 * 0x9E3779B9u));
        const uint h1 = Pcg(h0);
        const uint h2 = Pcg(h1);
        const float2 at = c + (float2(U01(h1), U01(h2)) - 0.5) * kPatch;
        const float2 uv = ClipUv(at);
        const float4 f = gField.SampleLevel(gSampClampSpawn, uv, 0);
        if (f.a < 0.5 || f.r <= 0.0) continue;
        const uint t = min((uint)(f.b * 255.0 + 0.5), gTypeCount - 1u);
        const GrassTypeGpu ty = gTypes[t];
        const float keep = fade * f.r * ty.density / gMaxDensity;
        if (((float)k2 + U01(h0)) / (float)n >= keep) continue;

        const uint h3 = Pcg(h2), h4 = Pcg(h3), h5 = Pcg(h4);
        const float y = gFieldHeight.SampleLevel(gSampClampSpawn, uv, 0);
        const float4 col = gFieldColour.SampleLevel(gSampClampSpawn, uv, 0);
        const float hs = f.g * 4.0;                                   // the paint's height scale
        const float height = lerp(ty.heightMin, ty.heightMax, U01(h3)) * hs;
        if (height < 0.02) continue;
        // A card stands for a clump: as wide as kClump blades' spread.
        const float width = (lod == 2u ? 0.35 : ty.width * widen) * (0.8 + 0.4 * U01(h4));
        const float yaw = U01(h5) * 6.2831853;
        const float3 cs = sqrt(saturate(col.rgb));                    // kept as its square root: 8 bits in the darks
        const uint colour = (uint)(cs.r * 255.0 + 0.5) | ((uint)(cs.g * 255.0 + 0.5) << 8) |
                            ((uint)(cs.b * 255.0 + 0.5) << 16) | ((uint)(saturate(col.a) * 255.0 + 0.5) << 24);
        const bool seedHead = ty.seedHeads > 0.5 && U01(Pcg(h5)) < 0.3;
        const uint bits = lod | ((uint)(saturate(ty.spread) * 255.0 + 0.5) << 2) |
                          ((uint)(saturate(ty.dryness) * 255.0 + 0.5) << 10) |
                          ((uint)(saturate(ty.wind) * 255.0 + 0.5) << 18) | (seedHead ? (1u << 26) : 0u);
        const uint at32 = slot * 32u;
        gInst.Store4(at32, uint4(asuint(at.x), asuint(y), asuint(at.y), asuint(height)));
        gInst.Store4(at32 + 16u, uint4(colour, f32tof16(width) | (f32tof16(yaw) << 16), h5, bits));
        ++slot;
    }
}

// After the spawn: each draw's instance count is its count, capped (the counts stay for the readout).
[numthreads(1, 1, 1)]
void CSClamp(uint3 id : SV_DispatchThreadID)
{
    const uint caps[3] = { gCap0, gCap1, gCap2 };
    [unroll] for (uint l = 0; l < 3u; ++l)
        gArgs.Store(l * 16u + 4u, min(gArgs.Load(48u + l * 4u), caps[l]));
}
