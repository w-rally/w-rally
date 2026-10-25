// Small x87 helpers of the original that return their result unrounded in st0 (cdecl). The value
// keeps the x87's wide exponent; callers continue on it, so the classic port returns X87.
#pragma once
#include <cstdint>
#include <cstring>

#include "f32.h"
#include "x87.h"

namespace phys::num {

// 0x52e350  ((a - b) * c) * d    fld; fsub; fmul; fmul
inline X87 SubMulMul(float a, float b, float c, float d) { return ((X87(a) - X87(b)) * X87(c)) * X87(d); }

// 0x52e370  -((a * b) * c)       fld; fmul; fmul; fchs
inline X87 NegMulMul(float a, float b, float c) { return Neg((X87(a) * X87(b)) * X87(c)); }

// 0x52e590  x * 100000       fld; fmul dword
inline X87 Mul100000(float x) { return X87(x) * X87(100000.0f); }

// 0x536950  stdcall(table, x, lo, hi, n): curve of n points, 16 bytes each, value at +0xc.
// q = ((x - lo) * float(n - 1)) / (hi - lo) (SSE), i = FloorS32(q) - despite the name a
// truncation: cvttss2si for q in [-2^31, 2^31), 0x7fffffff above, else (below, NaN) a warning
// and 0x80000000. i > n - 2: last value; i < 0: first value; else, on the x87 with f = q - i:
// (1 - f) * y[i] + f * y[i + 1], left in st0.
X87 CurveSample(const uint8_t* table, float x, float lo, float hi, int32_t n);

// 0x52e5a0  cdecl(x): 0 >= x (ordered): 0; else sqrt(sqrt(x)) (x87, st0).
inline X87 Root4(float x) { return jmp::B(0.0f, x) ? Sqrt(Sqrt(X87(x))) : X87(0.0f); }

// 0x52e5e0  cdecl(x), x87: with a = 2/7, b = 9/7: 1 > x: (b - x a) x; else (r = 1 / x)
// 1 / (r (b - a r)). Continuous at 1 (value 1, slope 5/7) - a soft curve.
X87 SoftCurve(float x);

// 0x52e6d0  cdecl(p, a, b): the inverse of CombinePair (math.h). p[0] > a: d = p[0] - a,
// r = 1 / (p[0] / d) (SSE, float), p[0] = d, k = float(((r * r) / sqrt(r)) * p[2]) (x87), p[2] = k,
// p[1] = (p[3] * k) / (d * b). Else (also NaN) p[0] = p[2] = p[1] = 1e-5.
void SplitPair(float* p, float a, float b);

// 0x52e760  cdecl(p, a, b, c) -> st0: take from a lump p = {amount, rate, T, k}. p[2] > b: r = p[2] / b,
// f = r < 1 ? (9/7 - r * 2/7) * r : 1 / ((9/7 - ir * 2/7) * ir) with ir = 1 / r (also for NaN r),
// h = (1 - 1 / f) * p[0]. h > a: SplitPair(p, a, c) (inlined in the original), returns a; else h > 0:
// SplitPair(p, h, c), returns h. Otherwise 0.
X87 LumpTake(float* p, float a, float b, float c);

// 0x50d580  stdcall(table, x, lo, hi, n): CurveSample over a plain float array (4-byte points).
X87 FloatCurveSample(const float* table, float x, float lo, float hi, int32_t n);

// 0x503ca0  stdcall(x, ring) -> st0: moving average. ring = {n, i, values[n]}: i + 1 > n - 1 wraps to 0,
// values[i] = x, s = sum of the n values (SSE, value + s in order), returns s / float(n) (x87: fild
// as unsigned, fdivr - not rounded to a float).
X87 RingAverage(float x, uint8_t* ring);

// 0x5325f0  stdcall(t, table, n) -> st0: q = float(x87: float(n as unsigned) * t); i = the FloorS32-style
// conversion of q (0 <= q < 2^31 or -2^31 <= q < 0: cvttss2si; q >= 2^31: 0x7fffffff; below or NaN: Warn,
// 0x80000000), capped at n (unsigned); f = q - float(i) (x87); returns (1 - f) * table[i] + f * table[i + 1].
X87 TableLerpScaled(float t, const float* table, uint32_t n);

// 0x50d6f0  stdcall(x, out): three blend weights at out[3..5] and a flag at out[2]. x == 0
// (ordered): out[2] = 1, rest 0. Else y = 2x; x > 0.5: (y - 1, 1 - (y - 1), 0); else (0, y, 1 - y);
// out[2] = 0 (stores in the original's order).
void TriBlend(float x, float* out);

// 0x503e00  stdcall: interpolate table t at q = U(a - b) * U(d) / U(c - b) (U: int32 loaded as
// unsigned, see FildUnsigned): i = _ftol(q), f = q - U(i), result ((1 - f) * t[i]) + (f * t[i+1]),
// all x87, left in st0.
X87 TableLerpU(const float* t, int32_t a, int32_t b, int32_t c, int32_t d);

} // namespace phys::num

