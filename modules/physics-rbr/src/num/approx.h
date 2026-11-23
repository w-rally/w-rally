// Deterministic replacements for the approximate SSE instructions (decision D6, design 1.1.7).
//
// rsqrtss/rcpss results are vendor-specific (Intel and AMD differ), so the original game's
// physics already differs between machines. The user chose determinism: the physics computes
// these from a fixed table, identical on every machine, and the oracle routes the original's own
// rsqrtss through the same function so the reference stays bit-comparable.
//
// Structure (verified exhaustively with phys-approx-probe on the table's source CPU, and the same
// as the published reverse engineering of Intel's): the result for a positive normal input is a
// table entry indexed by the exponent's parity and the top k mantissa bits, rescaled by the
// exponent. Special inputs: +-0 and denormals -> +-inf, +inf -> +0, NaN -> the same NaN quieted,
// negative normals and -inf -> the default NaN 0xffc00000.
//
// The definition is chosen at build time with the CMake option PHYS_RSQRT:
//   AMD    (default, used for testing) table captured on AMD Zen 4 - 8192 entries (parity + 12 bits)
//   INTEL  table captured on Intel Haswell - 2048 entries (parity + 10 bits)
//   NATIVE the running CPU's own rsqrtss - matches the unmodified original on that machine, but is
//          NOT deterministic across vendors
// Tables come from phys-approx-probe captures via phys/tools/gen_approx_table.py; the special-case
// rules above were identical on both CPUs.
#pragma once
#include <cstdint>

namespace phys::num {

// Bits of rsqrtss(x) per the selected definition.
uint32_t RsqrtApproxBits(uint32_t x);

// Which CPU the table was captured on.
const char* RsqrtApproxSource();

// Bits of rcpss(x), and of each rcpps lane (the same on the CPUs measured). The car's collision meshes are built with
// them (data/sgc.cpp: D3DX's matrix inverse and transform). A table of the top 12 mantissa bits captured on AMD Zen 4
// (tables/rcp_table_amd.inc, tools/approx-table), rescaled by the exponent; +-0 and denormals -> +-inf, +-inf -> +-0,
// NaN -> the same NaN quieted, a result below the normals -> +-0. Verified on its source CPU on all 2^32 inputs.
// Intel's estimates differ (an i7-4790K's for 3 of 8 sample inputs), which made the golden replay, recorded on AMD,
// fail there. The AMD table serves PHYS_RSQRT AMD and INTEL alike (no Intel capture yet); NATIVE uses the running
// CPU's rcpss.
uint32_t RcpApproxBits(uint32_t x);
float RcpApprox(float x);

// Which CPU the rcp table was captured on.
const char* RcpApproxSource();

} // namespace phys::num
