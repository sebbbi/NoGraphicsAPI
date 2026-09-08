#pragma once

#include <NoGraphicsAPI/types.h>

#if defined(_MSC_VER) && !defined(__clang__)
extern "C" unsigned char _BitScanReverse(unsigned long*, unsigned long);
extern "C" unsigned char _BitScanForward(unsigned long*, unsigned long);
#pragma intrinsic(_BitScanReverse)
#pragma intrinsic(_BitScanForward)
#endif

namespace gpu::detail
{

inline uint32 count_leading_zeros(uint32 value) noexcept
{
    if (value == 0) return 32;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long index;
    _BitScanReverse(&index, value);
    return 31u - index;
#else
    return static_cast<uint32>(__builtin_clz(value));
#endif
}

inline uint32 count_trailing_zeros(uint32 value) noexcept
{
    if (value == 0) return 32;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long index;
    _BitScanForward(&index, value);
    return index;
#else
    return static_cast<uint32>(__builtin_ctz(value));
#endif
}

inline uint32 popcount(uint32 value) noexcept
{
    // Keep the core library usable on x86-64 CPUs without POPCNT.
    value -= (value >> 1) & 0x55555555u;
    value = (value & 0x33333333u) + ((value >> 2) & 0x33333333u);
    value = (value + (value >> 4)) & 0x0f0f0f0fu;
    return (value * 0x01010101u) >> 24;
}

} // namespace gpu::detail
