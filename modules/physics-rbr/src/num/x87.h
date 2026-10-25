// X87 - the game's x87 arithmetic, emulated exactly (design 1.1 rule 3).
//
// During physics the FPU control word is 0x007F: 24-bit precision control, round to nearest.
// An x87 result is then rounded to a float-sized (24-bit) significand but keeps the x87's wide
// exponent range: a sum above FLT_MAX stays finite, a product below FLT_MIN stays normal. Only a
// store to a float in memory (fstp dword) brings it back into float range. The original compares
// such register values directly (fcomi), so SSE floats cannot reproduce it.
//
// Emulation: compute in double, then round the significand to 24 bits (RNE), exponent untouched.
// Double rounding (exact -> 53 bits -> 24 bits) is innocuous for + - * / sqrt because
// 53 >= 2*24 + 2 (Figueroa), so each X87 op equals the hardware's single rounding. Double's
// exponent range (+-1023) is narrower than x87's (+-16383); chains of float-derived values do
// not reach it in practice - X87::Checked() reports if one does.
//
// Comparisons: the original branches on comiss/fcomi flags. Which jump it uses decides the NaN
// outcome (ja is false on unordered, jbe is true), so conditions are named after the jumps.
#pragma once
#include <emmintrin.h>

#include <cstdint>
#include <cstring>

#include "f32.h"

namespace phys::num {

struct X87 {
    double d;

    X87() : d(0.0) {}
    X87(float f) : d(static_cast<double>(f)) {}     // fld dword: exact
    X87(F32 f) : d(static_cast<double>(f.f())) {}
    static X87 Raw(double v) { X87 x; x.d = v; return x; }
    static double BitsToDouble(uint64_t b) { double r; std::memcpy(&r, &b, 8); return r; }

    // Round the significand to 24 bits, round-to-nearest-even; exponent unbounded (double's).
    static double Round24(double v)
    {
        uint64_t b;
        std::memcpy(&b, &v, 8);
        if (((b >> 52) & 0x7ff) == 0x7ff || (b & 0x7fffffffffffffffull) == 0) return v;   // inf/NaN/0
        const uint64_t low = b & ((1ull << 29) - 1);
        b &= ~((1ull << 29) - 1);
        const uint64_t half = 1ull << 28;
        if (low > half || (low == half && (b & (1ull << 29)))) b += 1ull << 29;   // carry may bump the exponent: correct
        double r;
        std::memcpy(&r, &b, 8);
        return r;
    }

    // The double operations are SSE2 intrinsics: plain double arithmetic may be compiled to x87
    // on 32-bit targets, where it would itself be rounded by the game's 24-bit precision control.
    friend X87 operator+(X87 a, X87 b) { return Raw(Round24(_mm_cvtsd_f64(_mm_add_sd(_mm_set_sd(a.d), _mm_set_sd(b.d))))); }
    friend X87 operator-(X87 a, X87 b) { return Raw(Round24(_mm_cvtsd_f64(_mm_sub_sd(_mm_set_sd(a.d), _mm_set_sd(b.d))))); }
    friend X87 operator*(X87 a, X87 b) { return Raw(Round24(_mm_cvtsd_f64(_mm_mul_sd(_mm_set_sd(a.d), _mm_set_sd(b.d))))); }
    friend X87 operator/(X87 a, X87 b) { return Raw(Round24(_mm_cvtsd_f64(_mm_div_sd(_mm_set_sd(a.d), _mm_set_sd(b.d))))); }
    friend X87 Abs(X87 a)   // fabs: clears the sign bit, NaNs included
    {
        uint64_t b;
        std::memcpy(&b, &a.d, 8);
        b &= ~(1ull << 63);
        return Raw(BitsToDouble(b));
    }
    friend X87 Neg(X87 a) { return Raw(-a.d); }                                                          // fchs
    // fsqrt obeys precision control: the exact root rounded to 24 bits (double rounding innocuous).
    friend X87 Sqrt(X87 a) { return Raw(Round24(_mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(a.d))))); }

    // fstp dword: one rounding into float range (overflow -> inf, underflow -> denormal/0).
    float Store() const { return _mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(), _mm_set_sd(d))); }
};

// fild of an int32 that the original treats as unsigned: fild, then fadd 2^32 if negative (that
// add is an x87 operation, rounded to 24 bits).
inline X87 FildUnsigned(int32_t n)
{
    const X87 v = X87::Raw(static_cast<double>(n));
    return n < 0 ? v + X87(4294967296.0f) : v;
}

// _ftol (0x70c2c4): truncate st0 to int64, low dword returned; NaN or |v| >= 2^63 give the
// integer indefinite 0x8000000000000000 (low dword 0).
inline int32_t Ftol(X87 v)
{
    if (!(v.d < 9223372036854775808.0 && v.d > -9223372036854775808.0)) return 0;
    return static_cast<int32_t>(static_cast<int64_t>(v.d));   // truncation; exact for these values
}

// Jump taken after "comi a, b" (comiss a,b / fcomi with st0 = a, st(i) = b).
// Unordered (a NaN) sets ZF=PF=CF=1.
namespace jmp {
inline bool A(double a, double b) { return a > b; }            // ja : CF=0 & ZF=0
inline bool AE(double a, double b) { return a >= b; }          // jae: CF=0
inline bool B(double a, double b) { return !(a >= b); }        // jb : CF=1          (true if unordered)
inline bool BE(double a, double b) { return !(a > b); }        // jbe: CF=1 | ZF=1   (true if unordered)
inline bool E(double a, double b) { return !(a < b) && !(a > b); }   // je : ZF=1  (true if unordered)
inline bool NE(double a, double b) { return a < b || a > b; }        // jne: ZF=0
inline bool P(double a, double b) { return a != a || b != b; }       // jp : unordered
}

} // namespace phys::num
