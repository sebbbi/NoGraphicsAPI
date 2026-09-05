#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <cassert>
#include <limits>

namespace gpu
{

class BumpAllocator
{
public:
    static constexpr uint64_t alignment = 16;

    // Storage must be nonempty, expose at least one address, and align every exposed address to 16 bytes.
    explicit BumpAllocator(GpuCpuRange<byte> storage) noexcept;

    BumpAllocator(const BumpAllocator&) = delete;
    BumpAllocator& operator=(const BumpAllocator&) = delete;
    BumpAllocator(BumpAllocator&& other) noexcept;
    BumpAllocator& operator=(BumpAllocator&& other) noexcept;

    // The request must be nonzero. Reservations are rounded up to 16 bytes; an empty allocation reports exhausted storage.
    [[nodiscard]] GpuCpuRange<byte> allocate(uint64_t byte_size) noexcept;

    template<typename T>
    [[nodiscard]] GpuCpuRange<T> allocate(uint64_t element_count) noexcept
    {
        static_assert(alignof(T) <= alignment);
        if (element_count > std::numeric_limits<uint64_t>::max() / sizeof(T))
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
    uint64_t offset_ = 0;
};

} // namespace gpu
