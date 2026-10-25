// Deterministic sine and cosine for the original's x87 fsin / fcos (decision D6, design 1.1).
//
// fsin/fcos are not specified to the bit: Intel and AMD compute them differently (Intel's are
// notoriously inaccurate near multiples of pi), they ignore the 24-bit precision control and
// return a 64-bit-significand result. As with rsqrtss, determinism wins: phys::classic defines
// them by one algorithm - reduction by pi/2 as musl's __rem_pio2f (25-bit head + tail, accurate
// for |x| < 2^28*pi/2) and the fdlibm kernels on [-pi/4, pi/4] - computed with SSE2 double intrinsics only,
// so every build and machine gives the same bits. The oracle routes the original's fsin/fcos
// through the same functions. Accuracy is within about an ulp of double; how often the result
// rounded to float differs from this CPU's fsin/fcos is measured by phys-approx-probe --trig.
//
// Special inputs as x87: NaN -> the NaN quieted, +-inf -> the x87 "indefinite" NaN, |x| >= 2^63 ->
// x unchanged (architectural). Between ~4.2e8 and 2^63 the result is deterministic but not
// accurate (fsin's argument in the physics is an accumulating wheel angle, 11,626 rad after 50 s).
#pragma once

namespace phys::num {

double SinX87(double x);   // fsin
double CosX87(double x);   // fcos

// 0x53f3b0  float -> fld dword; fsin; ret (cdecl, the x87 result left in st0)
inline double SinOfFloat(float x) { return SinX87(static_cast<double>(x)); }
// 0x53f3a0  float -> fld dword; fcos; ret
inline double CosOfFloat(float x) { return CosX87(static_cast<double>(x)); }

} // namespace phys::num
