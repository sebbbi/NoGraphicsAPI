#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

namespace gpu
{

class BumpAllocator
{
public:
    static constexpr uint64 alignment = 16;

    // Storage must be nonempty, expose at least one address, and align every exposed address to 16 bytes.
    explicit BumpAllocator(GpuCpuRange<byte> storage) noexcept;

    BumpAllocator(const BumpAllocator&) = delete;
    BumpAllocator& operator=(const BumpAllocator&) = delete;
    BumpAllocator(BumpAllocator&& other) noexcept;
    BumpAllocator& operator=(BumpAllocator&& other) noexcept;

    // The request must be nonzero. Reservations are rounded up to 16 bytes; an empty allocation reports exhausted storage.
    [[nodiscard]] GpuCpuRange<byte> allocate(uint64 byte_size) noexcept;

    // Intended for concurrent bump allocation from worker threads. Successful concurrent calls to allocate_atomic() return disjoint ranges.
    // The request must be nonzero. Reservations are rounded up to 16 bytes; an empty allocation reports exhausted storage.
    // Every other operation, including allocate(), reset(), move construction/assignment, and destruction, requires exclusive access.
    // None may execute concurrently with allocate() or allocate_atomic().
    [[nodiscard]] GpuCpuRange<byte> allocate_atomic(uint64 byte_size) noexcept;

    template<typename T>
    [[nodiscard]] GpuCpuRange<T> allocate(uint64 element_count) noexcept
    {
        static_assert(alignof(T) <= alignment);
        const GpuCpuRange<byte> allocation = allocate(element_count * sizeof(T));
        return {.cpu = reinterpret_cast<T*>(allocation.cpu), .gpu = reinterpret_cast<T*>(allocation.gpu), .size = allocation.size};
    }

    template<typename T>
    [[nodiscard]] GpuCpuRange<T> allocate_atomic(uint64 element_count) noexcept
    {
        static_assert(alignof(T) <= alignment);
        const GpuCpuRange<byte> allocation = allocate_atomic(element_count * sizeof(T));
        return {.cpu = reinterpret_cast<T*>(allocation.cpu), .gpu = reinterpret_cast<T*>(allocation.gpu), .size = allocation.size};
    }

    // Reset invalidates every previous allocation.
    void reset() noexcept;

private:
    GpuCpuRange<byte> storage_{};
    alignas(8) uint64 offset_ = 0;
};

} // namespace gpu
