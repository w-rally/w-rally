#include "native/driveline.h"

#include <algorithm>
#include <cmath>
#include <xmmintrin.h>

#include "num/f32.h"
#include "num/bits.h"
#include "native/unreached.h"

namespace phys::native {

using num::F32;
using num::F32x4;
using num::Vec4;
using num::X87;
using num::FromBits;
namespace jmp = num::jmp;

namespace {
F32x4 MulAdd4(const F32x4& d, const F32x4& src, float s) { return src * F32x4::Splat(F32(s)) + d; }
} // namespace

void DriveLine::Load(const DriveLineSample* s, uint32_t n)
{
    samples.assign(s, s + n);
    samples.push_back(DriveLineSample{});
}

const DriveLineSample& DriveLine::Sample(uint32_t j) const
{
    if (j >= Count()) {
        Unreached("a drive line sample past its end (the original's assertion)");
        j = 0;
    }
    return samples[j];
}

void DriveLine::ResetPoint(float t, Vec4& point, Vec4& dir) const
{
    const int32_t i = _mm_cvttss_si32(_mm_set_ss(t));
    const float frac = (X87(t) - X87::Raw(static_cast<double>(i))).Store();   // fild, fsubr
    Key(i, frac, point);
    const size_t j = std::min(static_cast<size_t>(i), samples.size() - 1), k = std::min(j + 1, samples.size() - 1);
    num::Lerp4(samples[j].direction, samples[k].direction, frac, dir);
    num::Normalize3Approx4(dir);
}

void DriveLine::Key(int32_t index, float t, Vec4& out) const
{
    const DriveLineSample* k = samples.data() + index;
    const F32x4 p0 = F32x4::Load(&k[0].position), t0 = F32x4::Load(&k[0].direction), p1 = F32x4::Load(&k[1].position),
                t1 = F32x4::Load(&k[1].direction);
    const F32x4 zero = F32x4::Splat(F32(0.0f));
    F32x4 a = MulAdd4(zero, p0, 2.0f);
    a = MulAdd4(a, p1, -2.0f);
    a = MulAdd4(a, t0, 1.0f);
    a = MulAdd4(a, t1, 1.0f);
    F32x4 b = MulAdd4(zero, p0, -3.0f);
    b = MulAdd4(b, p1, 3.0f);
    b = MulAdd4(b, t0, -2.0f);
    b = MulAdd4(b, t1, -1.0f);
    const float t2 = (F32(t) * F32(t)).f();
    F32x4 o = MulAdd4(p0, t0, t);
    o = MulAdd4(o, b, t2);
    o = MulAdd4(o, a, (F32(t2) * F32(t)).f());
    o.Store(&out);
}

void DriveLine::KeyAt(float x, Vec4& out) const
{
    const float two31 = 2147483648.0f;
    int32_t i = 0;
    F32 f = 0.0f;
    auto truncate = [&] {   // cvttss2si; a negative result gives (0, 0)
        const int32_t t = _mm_cvtt_ss2si(_mm_set_ss(x));
        if (t >= 0) i = t, f = F32(x) - F32(_mm_cvtsi32_ss(_mm_setzero_ps(), t));
    };
    if (!jmp::B(x, 0.0f) && jmp::A(two31, x)) {
        truncate();
    } else if (!jmp::B(x, two31)) {
        i = 0x7fffffff;
        f = F32(x) - F32(_mm_cvtsi32_ss(_mm_setzero_ps(), i));
    } else if (jmp::AE(x, -two31)) {
        truncate();
    } else {
        Unreached("FloorS32 of a NaN drive line parameter");
    }
    if (static_cast<uint32_t>(i) + 1 >= samples.size()) Unreached("a drive line key past its end");
    Key(i, f.f(), out);
}

float DriveLine::Distance(float t) const
{
    const X87 fl = X87::Raw(std::floor(static_cast<double>(t)));
    const uint32_t i = static_cast<uint32_t>(num::Ftol(fl));
    const float frac = (X87(t) - num::FildUnsigned(static_cast<int32_t>(i))).Store();
    const uint32_t count = Count();
    if (i >= count) {
        Unreached("a drive line distance past its end (the original's assertion)");
        return 0.0f;
    }
    F32 d = F32(samples[i].distance);
    if (i < count - 1) d = (F32(samples[i + 1].distance) - d) * F32(frac) + d;
    return X87(d).Store();
}

X87 DriveLine::Project(const float* p, float t) const
{
    const uint32_t n = Count();
    if (n == 0) return X87(0.0f);
    const int32_t i = _mm_cvttss_si32(_mm_set_ss(t));
    Vec4 at;
    if (i < 0) at = Sample(static_cast<uint32_t>(i)).position;
    else if (i >= static_cast<int32_t>(n)) at = Sample(n - 1).position;
    else KeyAt(t, at);
    Vec4 d = {(F32(at.x) - F32(p[0])).f(), (F32(at.y) - F32(p[1])).f(), (F32(at.z) - F32(p[2])).f(), 0.0f};   // w: stack
    int32_t e = num::Ftol((X87(num::Length3(d)) - X87(6.0f)) * X87(0.5f));
    if (e <= 0) e = 1;
    int32_t lo = static_cast<int32_t>(static_cast<uint32_t>(i) - static_cast<uint32_t>(e));
    int32_t hi = static_cast<int32_t>(static_cast<uint32_t>(i) + static_cast<uint32_t>(e));
    if (lo < 0) lo = 0;
    if (hi >= static_cast<int32_t>(n)) hi = static_cast<int32_t>(n) - 1;
    // The nearest sample in [lo, hi] (squared distance; the first of equals).
    auto dist2 = [&](const DriveLineSample& s) {
        const F32 d1 = F32(p[1]) - F32(s.position.y), d0 = F32(p[0]) - F32(s.position.x), d2 = F32(p[2]) - F32(s.position.z);
        return (d2 * d2 + d1 * d1) + d0 * d0;
    };
    int32_t k = lo;
    F32 best = dist2(Sample(static_cast<uint32_t>(lo)));
    for (int32_t j = lo + 1; j <= hi; ++j) {
        const F32 dd = dist2(Sample(static_cast<uint32_t>(j)));
        if (jmp::A(best.f(), dd.f())) {
            best = dd;
            k = j;
        }
    }
    // The segment: [k, k + 1], or [k - 1, k] when p lies behind sample k (along its direction) or k is the last.
    if (k != 0) {
        if (k == static_cast<int32_t>(n) - 1) {
            --k;
        } else {
            const DriveLineSample& s = Sample(static_cast<uint32_t>(k));
            const F32 dx = F32(p[0]) - F32(s.position.x), dy = F32(p[1]) - F32(s.position.y), dz = F32(p[2]) - F32(s.position.z);
            const F32 dot = (F32(s.direction.y) * dy + F32(s.direction.z) * dz) + F32(s.direction.x) * dx;
            if (jmp::A(0.0f, dot.f())) --k;
        }
    }
    if (k + 1 >= static_cast<int32_t>(n)) return num::FildUnsigned(static_cast<int32_t>(n) - 1);
    // Bisection on (p - curve(m)) . tangent(m) over [0, 1]: until |dot| <= 0.01 or the tenth step.
    const Vec4& t0 = samples[k].direction;
    const Vec4& t1 = samples[k + 1].direction;
    const F32 zero = 0.0f;
    float a = 0.0f, b = 1.0f, m;
    uint32_t steps = 0;
    for (;;) {
        m = ((X87(b) + X87(a)) * X87(0.5f)).Store();
        Vec4 c;
        Key(k, m, c);
        const F32 rx = F32(p[0]) - F32(c.x), ry = F32(p[1]) - F32(c.y), rz = F32(p[2]) - F32(c.z);
        Vec4 tg;
        num::Lerp4(t0, t1, m, tg);
        const F32 dot = (F32(tg.z) * rz + F32(tg.y) * ry) + rx * F32(tg.x);
        const F32 ad = !jmp::BE(dot.f(), 0.0f) ? dot : zero - dot;
        if (jmp::AE(FromBits(0x3c23d70a), ad.f())) break;   // 0.01
        if (steps++ > 8) break;
        if (!jmp::BE(dot.f(), 0.0f)) a = m;
        else b = m;
    }
    return X87::Raw(static_cast<double>(k)) + X87(m);
}

} // namespace phys::native
