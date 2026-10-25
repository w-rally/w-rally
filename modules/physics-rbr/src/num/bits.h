// A float from its bits, and back (memcpy: no aliasing, NaN payloads kept).
#pragma once
#include <cstdint>
#include <cstring>

namespace phys::num {

inline float FromBits(uint32_t b)
{
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

inline uint32_t ToBits(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

} // namespace phys::num
