#include "approx.h"

#if defined(PHYS_RSQRT_NATIVE)
#include <xmmintrin.h>

#include <cstring>
#endif

// Which definition of rsqrtss: CMake option PHYS_RSQRT (AMD | INTEL | NATIVE), see approx.h.

namespace phys::num {

#if defined(PHYS_RSQRT_NATIVE)

uint32_t RsqrtApproxBits(uint32_t x)
{
    float f;
    std::memcpy(&f, &x, 4);
    f = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(f)));
    std::memcpy(&x, &f, 4);
    return x;
}

const char* RsqrtApproxSource()
{
    return "native rsqrtss of the running CPU (NOT deterministic across CPU vendors)";
}

#else

namespace {
#if defined(PHYS_RSQRT_INTEL)
#include "tables/rsqrt_table_intel.inc"
#elif defined(PHYS_RSQRT_AMD)
#include "tables/rsqrt_table_amd.inc"
#else
#error "PHYS_RSQRT must select AMD, INTEL or NATIVE"
#endif
} // namespace

uint32_t RsqrtApproxBits(uint32_t x)
{
    const uint32_t exp = (x >> 23) & 0xff;
    const bool negative = (x >> 31) != 0;
    if (exp == 0xff) {
        if (x & 0x007fffffu) return x | 0x00400000u;   // NaN: quieted, payload kept
        return negative ? 0xffc00000u : 0u;             // -inf: default NaN; +inf: +0
    }
    if (exp == 0) return negative ? 0xff800000u : 0x7f800000u;   // +-0, denormals: +-inf
    if (negative) return 0xffc00000u;                             // negative normals

    // Table at the reference exponents 127/128 ([1,4)), then x * 4^n -> r / 2^n.
    const int32_t rel = static_cast<int32_t>(exp) - 127;
    const uint32_t parity = static_cast<uint32_t>(rel & 1);
    const int32_t shift = -((rel - static_cast<int32_t>(parity)) / 2);   // rel - parity is even: exact
    const uint32_t key = (parity << kRsqrtTableBits) | ((x & 0x007fffffu) >> (23 - kRsqrtTableBits));
    const uint32_t ref = kRsqrtTable[key];
    const uint32_t re = static_cast<uint32_t>(static_cast<int32_t>((ref >> 23) & 0xff) + shift);
    return (ref & 0x807fffffu) | (re << 23);   // stays normal for every normal input
}

const char* RsqrtApproxSource()
{
    return kRsqrtTableSource;
}

#endif

} // namespace phys::num
