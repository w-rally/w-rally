#include "curves.h"

#include <cstring>
#include <initializer_list>

#include "f32.h"
#include "trig.h"
#include "x87math.h"

namespace phys::num {

void AccumulateRate(float* p, float a, float b, float c)
{
    const float p0 = p[0];
    const float v = (F32(a) / F32(b) + F32(p[1])).f();
    p[1] = v;
    // ucomiss p0, 0; lahf; test ah, 0x44; jnp: 0 when equal and ordered.
    p[2] = p0 == 0.0f ? 0.0f : (((F32(p0) / F32(p[3])) * F32(v)) * F32(c)).f();
}

void CombinePair(float* a, const float* b, float c)
{
    const F32 s0 = F32(a[0]) + F32(b[0]);
    const F32 s3 = F32(b[3]) + F32(a[3]);
    const F32 q = ((F32(b[1]) * F32(b[0]) + F32(a[0]) * F32(a[1])) / s0) / s3;
    const float r = (s3 / F32(a[3])).f();
    const float m = ((q * s0) * F32(c)).f();
    const X87 rr = X87(r) * X87(r);
    a[0] = s0.f();
    const float k = ((rr / Sqrt(X87(r))) * X87(m)).Store();
    const F32 a3k = F32(a[3]) * F32(k);
    a[2] = k;
    a[1] = (a3k / (s0 * F32(c))).f();
}

} // namespace phys::num
