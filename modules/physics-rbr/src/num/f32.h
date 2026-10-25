// F32 - a float whose every operation is ONE scalar SSE instruction (design 1.1).
//
// Why: 32-bit MSVC computes a float expression on the x87 unit when its result is returned
// (the x86 ABI returns floats in st0) - found on Dot3, where x87's wider exponent range turned
// the original's +inf/NaN into finite values and +0 into -0. No MSVC switch forbids that. Code
// written with intrinsics cannot be moved to x87 by any compiler, so all phys::classic
// arithmetic goes through F32: + - * / sqrt rsqrt (min/max/compares as the original needs them),
// each the _ss instruction the original used. Operand order follows the source (D5: the compiler may still
// commute + and *, which only changes NaN payloads).
//
// Storage stays plain float (game layout); F32 is for computing. Leaving SSE is explicit: .f().
#pragma once
#include <xmmintrin.h>

#include <cstdint>
#include <cstring>

#include "approx.h"

namespace phys::num {

struct F32 {
    __m128 v;

    F32() : v(_mm_setzero_ps()) {}
    F32(float f) : v(_mm_set_ss(f)) {}   // NOLINT: implicit on purpose, F32 stands in for float
    explicit F32(__m128 x) : v(x) {}
    float f() const { return _mm_cvtss_f32(v); }

    friend F32 operator+(F32 a, F32 b) { return F32(_mm_add_ss(a.v, b.v)); }
    friend F32 operator-(F32 a, F32 b) { return F32(_mm_sub_ss(a.v, b.v)); }
    friend F32 operator*(F32 a, F32 b) { return F32(_mm_mul_ss(a.v, b.v)); }
    friend F32 operator/(F32 a, F32 b) { return F32(_mm_div_ss(a.v, b.v)); }
    friend F32 operator-(F32 a) { return F32(_mm_xor_ps(a.v, _mm_set_ss(-0.0f))); }   // xorps sign flip
    F32& operator+=(F32 b) { return *this = *this + b; }
    F32& operator-=(F32 b) { return *this = *this - b; }
    F32& operator*=(F32 b) { return *this = *this * b; }
};

inline F32 Sqrt(F32 a) { return F32(_mm_sqrt_ss(a.v)); }   // sqrtss: exact (IEEE)
// rsqrtss replacement: the original's rsqrtss is an approximation whose bits differ between CPU
// vendors; D6 makes it a fixed table, identical on every machine (approx.h).
inline F32 RsqrtApprox(F32 a)
{
    const float in = a.f();
    uint32_t b;
    std::memcpy(&b, &in, 4);
    b = RsqrtApproxBits(b);
    float out;
    std::memcpy(&out, &b, 4);
    return F32(out);
}

// Four lanes of a packed SSE op (mulps, addps, ...) done as four scalar F32 ops. Per lane they are
// the same IEEE operations, but MSVC reorders and folds packed intrinsics (swaps addps/mulps
// operands, drops "+ 0"), which changes NaN payloads; it keeps scalar ss intrinsics as written.
struct F32x4 {
    F32 v[4];

    static F32x4 Load(const void* p)
    {
        F32x4 r;
        for (int i = 0; i < 4; ++i) {
            float f;
            std::memcpy(&f, static_cast<const uint8_t*>(p) + 4 * i, 4);
            r.v[i] = F32(f);
        }
        return r;
    }
    static F32x4 Splat(F32 s) { return F32x4{{s, s, s, s}}; }
    void Store(void* p) const
    {
        for (int i = 0; i < 4; ++i) {
            const float f = v[i].f();
            std::memcpy(static_cast<uint8_t*>(p) + 4 * i, &f, 4);
        }
    }
    friend F32x4 operator+(const F32x4& a, const F32x4& b) { return {{a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]}}; }
    friend F32x4 operator*(const F32x4& a, const F32x4& b) { return {{a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]}}; }
};

} // namespace phys::num
