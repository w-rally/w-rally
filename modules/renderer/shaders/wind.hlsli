// THE WIND on an instanced model's vertex (ngg_format.h kModelWind, NGG design §2.6): a float32 port of Wallaby's
// tools/vegetation/plantgen/wind.py `displace`, the reference - its docstring states the formula in full, and the
// CTest wind-reference evaluates this file against its golden table (tests/data/wind_golden.json).
//
//   main bend   lean = bend s^2 gust (0.75 + 0.25 sin(2 pi (f0 t + php))) (h / height)^2 along the wind, then the
//               vertex's distance from the base restored (the trunk bends, it does not stretch); f0 = 2.2 / height
//   branch      w 0.25 m s gust (0.6 along the wind (0.5 + 0.5 sin(2 pi (fb t + ph + php)))
//                              + 0.4 up sin(2 pi (1.3 fb t + 1.7 ph + php))),           fb = 0.3 (1 + 0.4 ph)
//   flutter     fl 0.025 m s gust sin(2 pi (ff t + 7.3 ph + pos . (0.7, 1.3, 0.9))) along the normal,
//                                                                                        ff = 2.5 (1 + 0.5 ph)
// with s = speed / 10, gust = 1 + gustiness (0.5 + 0.5 sin(2 pi (0.12 t - 0.02 pos . dir))), php = WindPhase(base).
//
// FLOAT32, AND WHERE IT LOSES. A stage puts bases thousands of metres out and the time runs to an hour (3600 s, where
// the caller wraps it), so a straight port would take sin() of 13,500 cycles in float - a millisecond of phase. So
// every cycle count is reduced to its fraction BEFORE it is summed:
//   - a frequency times the time: WindCycles splits the time into whole seconds and a fraction (exact), and the
//     frequency into its top 12 bits and the rest, so the big product is exact and its fraction too;
//   - the frequencies are kept as sums of exact constants times the phase (2.5 t + 1.25 ph t, not ff t);
//   - a position's dot product takes each coordinate's own fraction (frac(0.7 x) + ...).
// What remains is the float rounding of the INPUTS: the packed uv1.y holds the phase to ~1e-5 beside a flutter of
// 100, and since the phase also sets the branch's frequency, a vertex's motion after an hour is that of a phase a
// hair off - constant, so nothing jitters, but up to ~2.4 mm from the reference evaluated on the unrounded value.
// wind-reference therefore measures this port against the reference on the SAME float32 inputs (0.1 mm on this
// machine), and reports the distance to the golden table's own outputs beside it.
//
// `precise` keeps the compiler from refolding the splits (ah t + al t back into a t).
#ifndef NGG_WIND_HLSLI
#define NGG_WIND_HLSLI

static const float kWindTau = 6.28318530717958647;
static const float kWindBranchM = 0.25;     // a branch tip's sway at weight 1, 10 m/s (wind.py BRANCH_M)
static const float kWindFlutterM = 0.025;   // a leaf cluster tip's flutter at weight 1, 10 m/s (wind.py FLUTTER_M)

// PCG (Jarcho & Olano): wind.py's _pcg, integer arithmetic, so the GPU agrees with Python bit for bit.
uint WindHash(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// A placement's own phase, 0..1: its base's two ground coordinates to the centimetre (two's complement) through PCG.
// wind.py placement_phase (round() is round-half-even on both sides).
float WindPhase(float3 base)
{
    const uint qa = asuint((int)round(base.x * 100.0));
    const uint qb = asuint((int)round(base.z * 100.0));
    return (float)(WindHash(qa ^ WindHash(qb)) & 0xFFFFFFu) / 16777216.0;
}

// frac(a * (ti + tf)) for whole seconds ti (0..4095, exact in float) and their fraction tf, kept exact for the product's
// big part: a's top 12 significant bits times ti is exact in 24, and so is its fraction.
float WindCycles(float a, float ti, float tf)
{
    precise const float ah = asfloat(asuint(a) & 0xFFFFF000u);
    precise const float al = a - ah;
    precise const float big = frac(ah * ti);
    precise const float small = al * ti + a * tf;
    return frac(big + frac(small));
}

float WindSin(float cycles) { return sin(kWindTau * frac(cycles)); }

// The wind for a frame: wind.x/.y the direction it blows towards along the ground (x, z; unit length), .z the speed
// (m/s at 10 m), .w the gustiness. t: seconds, 0..3600.
struct WindFrame { float4 wind; float t; };

// The bent vertex RELATIVE TO THE BASE (base + this is its place in the world).
//   base    the placement's base, in the stage's frame (the hash and the gust wave read it; never camera-relative)
//   v       the placed vertex minus the base (the placement's linear part times the model-space position)
//   nrm     its placed normal, unit
//   attr    its uv1: x the branch weight, y phase + flutter hundredths (ngg_format.h kModelWind); (0, 0) for the main
//           bend alone (a billboard)
//   h       its height above the base in model space times the placement's vertical scale
//   height  the model's height times the vertical scale; bend its windBend times the vertical scale
float3 WindBend(float3 base, float3 v, float3 nrm, float2 attr, float h, float height, float bend, WindFrame f)
{
    const float3 d = float3(f.wind.x, 0.0, f.wind.y);
    const float s = f.wind.z / 10.0;
    const float ti = floor(f.t), tf = f.t - ti;
    // unpack: the phase in the fraction, the flutter in hundredths
    const float whole = floor(attr.y);
    const float ph = attr.y - whole;
    const float fl = whole / 100.0;
    const float php = WindPhase(base);

    // the gust wave rolling across the stage: 0.12 t - 0.02 (pos . dir)
    precise const float gc = WindCycles(0.12, ti, tf) - frac(0.02 * dot(base, d)) - 0.02 * dot(v, d);
    const float gust = 1.0 + f.wind.w * (0.5 + 0.5 * WindSin(gc));

    // main bend: lean the whole plant, keeping each vertex's distance from the base
    const float f0 = 2.2 / max(height, 0.5);
    const float lean = bend * s * s * gust * (0.75 + 0.25 * WindSin(WindCycles(f0, ti, tf) + php));
    const float hf = saturate(h / max(height, 1e-6));
    const float len = length(v);
    float3 bent = v + d * (lean * hf * hf);
    bent *= len / max(length(bent), 1e-9);

    // branch sway: along the wind (fb t + ph + php) and up and down (1.3 fb t + 1.7 ph + php), fb = 0.3 + 0.12 ph
    const float ab = attr.x * kWindBranchM * s * gust;
    const float ca = WindCycles(0.3, ti, tf) + WindCycles(0.12 * ph, ti, tf) + ph + php;
    const float cb = WindCycles(0.39, ti, tf) + WindCycles(0.156 * ph, ti, tf) + frac(1.7 * ph) + php;
    const float along = 0.6 * (0.5 + 0.5 * WindSin(ca));
    const float bob = 0.4 * WindSin(cb);
    const float3 branch = ab * (d * along + float3(0.0, bob, 0.0));

    // leaf flutter along the normal: ff t = 2.5 t + ph t + 0.25 ph t; pos . (0.7, 1.3, 0.9) by each coordinate's fraction
    const float3 k = float3(0.7, 1.3, 0.9);
    precise const float pk = frac(k.x * base.x) + frac(k.y * base.y) + frac(k.z * base.z) + dot(k, v);
    const float cf = WindCycles(2.5, ti, tf) + WindCycles(ph, ti, tf) + WindCycles(0.25 * ph, ti, tf) + frac(7.3 * ph) + pk;
    const float3 flut = (fl * kWindFlutterM * s * gust * WindSin(cf)) * nrm;

    return bent + branch + flut;
}

#endif
