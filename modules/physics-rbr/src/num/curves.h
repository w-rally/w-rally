// The rate helpers of the engine and drive code (float quads by index).
#pragma once
#include <cstdint>

#include "math.h"
#include "x87.h"

namespace phys::num {

// 0x52e8a0  cdecl(p, a, b, c): p[1] = a / b + p[1]; p[2] = p[0] == 0 (ordered) ? 0 :
// ((p[0] / p[3]) * p[1]) * c.
void AccumulateRate(float* p, float a, float b, float c);

// 0x52e630  cdecl(a, b, c) over two float quads: s0 = a0 + b0, s3 = b3 + a3, q = ((b1 b0 + a0 a1) /
// s0) / s3, r = s3 / a3, m = (q s0) c (SSE, floats); k = float(((r r) / sqrt(r)) * m) (x87);
// a0 = s0, a2 = k, a1 = (a3 k) / (s0 c).
void CombinePair(float* a, const float* b, float c);

} // namespace phys::num
