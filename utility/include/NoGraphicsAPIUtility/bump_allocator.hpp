#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <atomic>

namespace gpu
{

class BumpAllocator
{
public:
    static constexpr uint64_t alignment = 16;
    static_assert(std::atomic_ref<uint64_t>::is_always_lock_free, "atomic bump allocation requires lock-free 64-bit atomics");

    // Storage must be nonempty, expose at least one address, and align every exposed address to 16 bytes.
    explicit BumpAllocator(GpuCpuRange<byte> storage) noexcept;

    BumpAllocator(const BumpAllocator&) = delete;
    BumpAllocator& operator=(const BumpAllocator&) = delete;
    BumpAllocator(BumpAllocator&& other) noexcept;
    BumpAllocator& operator=(BumpAllocator&& other) noexcept;

    // The request must be nonzero. Reservations are rounded up to 16 bytes; an empty allocation reports exhausted storage.
    [[nodiscard]] GpuCpuRange<byte> allocate(uint64_t byte_size) noexcept;

    // The request must be nonzero. Reservations are rounded up to 16 bytes; an empty allocation reports exhausted storage.
    // Concurrent allocate_atomic calls return disjoint ranges. They must not race allocate, reset, move, or destruction.
    [[nodiscard]] GpuCpuRange<byte> allocate_atomic(uint64_t byte_size) noexcept;

    template<typename T>
    [[nodiscard]] GpuCpuRange<T> allocate(uint64_t element_count) noexcept
    {
        static_assert(alignof(T) <= alignment);
        const GpuCpuRange<byte> allocation = allocate(element_count * sizeof(T));
        return {.cpu = reinterpret_cast<T*>(allocation.cpu), .gpu = reinterpret_cast<T*>(allocation.gpu), .size = allocation.size};
    }

    template<typename T>
    [[nodiscard]] GpuCpuRange<T> allocate_atomic(uint64_t element_count) noexcept
    {
        static_assert(alignof(T) <= alignment);
        const GpuCpuRange<byte> allocation = allocate_atomic(element_count * sizeof(T));
        return {.cpu = reinterpret_cast<T*>(allocation.cpu), .gpu = reinterpret_cast<T*>(allocation.gpu), .size = allocation.size};
    }

    // Reset invalidates every previous allocation.
    void reset() noexcept;

private:
    GpuCpuRange<byte> storage_{};
    alignas(std::atomic_ref<uint64_t>::required_alignment) uint64_t offset_ = 0;
};

} // namespace gpu
