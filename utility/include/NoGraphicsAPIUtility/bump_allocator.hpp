#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <cassert>
#include <cstdint>
#include <limits>

namespace gpu
{

class BumpAllocator
{
public:
    static constexpr std::uint64_t alignment = 16;

    // Storage must be nonempty, with at least one 16-byte-aligned address domain present.
    explicit BumpAllocator(GpuCpuRange<byte> storage) noexcept;

    BumpAllocator(const BumpAllocator&) = delete;
    BumpAllocator& operator=(const BumpAllocator&) = delete;
    BumpAllocator(BumpAllocator&& other) noexcept;
    BumpAllocator& operator=(BumpAllocator&& other) noexcept;

    // The request must be nonzero. Reservations are rounded up to 16 bytes; an empty allocation reports exhausted storage.
    [[nodiscard]] GpuCpuRange<byte> allocate(std::uint64_t byte_size) noexcept;

    template<typename T>
    [[nodiscard]] GpuCpuRange<T> allocate(std::uint64_t element_count) noexcept
    {
        static_assert(alignof(T) <= alignment);
        if (element_count > std::numeric_limits<std::uint64_t>::max() / sizeof(T))
        {
            assert(false && "typed allocation byte size overflows uint64_t");
            return {};
        }
        const GpuCpuRange<byte> allocation = allocate(element_count * sizeof(T));
        return {.cpu = reinterpret_cast<T*>(allocation.cpu), .gpu = reinterpret_cast<T*>(allocation.gpu), .size = allocation.size};
    }

    // Reset invalidates every previous allocation.
    void reset() noexcept;

private:
    GpuCpuRange<byte> storage_{};
    std::uint64_t offset_ = 0;
};

} // namespace gpu
