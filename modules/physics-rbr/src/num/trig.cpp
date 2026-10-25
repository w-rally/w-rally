#include "trig.h"

#include <emmintrin.h>

#include <cstdint>
#include <cstring>

// Kernel coefficients: fdlibm (Sun Microsystems, "Permission to use, copy, modify, and distribute
// this software is freely granted, provided that this notice is preserved").

namespace phys::num {

namespace {

// One SSE2 operation each - never x87, whatever the target (see f32.h).
inline double Add(double a, double b) { return _mm_cvtsd_f64(_mm_add_sd(_mm_set_sd(a), _mm_set_sd(b))); }
inline double Sub(double a, double b) { return _mm_cvtsd_f64(_mm_sub_sd(_mm_set_sd(a), _mm_set_sd(b))); }
inline double Mul(double a, double b) { return _mm_cvtsd_f64(_mm_mul_sd(_mm_set_sd(a), _mm_set_sd(b))); }

uint64_t Bits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
double FromBits(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }

constexpr double kInvPio2 = 6.36619772367581382433e-01;
// Reduction for float-sized arguments (musl __rem_pio2f): pi/2 = head + tail, head has 25 bits, so
// n * head is exact for |n| < 2^28 and x - n * head is exact; the tail term adds ~2^-53 relative error.
constexpr double kPio2Head = 1.57079631090164184570e+00;   // first 25 bits of pi/2
constexpr double kPio2Tail = 1.58932547735281966916e-08;   // pi/2 - head

double KernelSin(double x)
{
    constexpr double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                     S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                     S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
    const double z = Mul(x, x);
    const double v = Mul(z, x);
    const double r = Add(S2, Mul(z, Add(S3, Mul(z, Add(S4, Mul(z, Add(S5, Mul(z, S6))))))));
    return Add(x, Mul(v, Add(S1, Mul(z, r))));
}

double KernelCos(double x)
{
    constexpr double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                     C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                     C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
    const double z = Mul(x, x);
    const double r = Mul(z, Add(C1, Mul(z, Add(C2, Mul(z, Add(C3, Mul(z, Add(C4, Mul(z, Add(C5, Mul(z, C6)))))))))));
    const double hz = Mul(0.5, z);
    const double w = Sub(1.0, hz);
    return Add(w, Add(Sub(Sub(1.0, w), hz), Mul(z, r)));
}

// x = n * pi/2 + r, |r| <= ~pi/4. Returns the quadrant n mod 4. Accurate for |x| < 2^28 * pi/2
// (~4.2e8); beyond that n is not exact and the result is deterministic but not accurate.
int Reduce(double x, double& r)
{
    const double q = Mul(x, kInvPio2);
    const bool big = !(q < 2147483647.0 && q > -2147483647.0);
    const double fn = big ? 0.0 : static_cast<double>(_mm_cvtsd_si32(_mm_set_sd(q)));   // round to nearest
    r = big ? x : Sub(Sub(x, Mul(fn, kPio2Head)), Mul(fn, kPio2Tail));
    return static_cast<int>(static_cast<int64_t>(fn) & 3);
}

bool Special(double x, double& out)
{
    const uint64_t b = Bits(x);
    if (((b >> 52) & 0x7ff) >= 1023 + 63 && ((b >> 52) & 0x7ff) != 0x7ff) {
        out = x;   // |x| >= 2^63: fsin/fcos leave the operand unchanged (and set C2) on every x87
        return true;
    }
    if (((b >> 52) & 0x7ff) != 0x7ff) return false;
    out = (b & 0xfffffffffffffull) ? FromBits(b | (1ull << 51))        // NaN: quieted
                                   : FromBits(0xfff8000000000000ull);   // +-inf: x87 indefinite
    return true;
}

} // namespace

double SinX87(double x)
{
    double out;
    if (Special(x, out)) return out;
    double r;
    switch (Reduce(x, r)) {
    case 0: return KernelSin(r);
    case 1: return KernelCos(r);
    case 2: return -KernelSin(r);
    default: return -KernelCos(r);
    }
}

double CosX87(double x)
{
    double out;
    if (Special(x, out)) return out;
    double r;
    switch (Reduce(x, r)) {
    case 0: return KernelCos(r);
    case 1: return -KernelSin(r);
    case 2: return -KernelCos(r);
    default: return KernelSin(r);
    }
}

} // namespace phys::num
