#include <NoGraphicsAPIUtility/bump_allocator.hpp>

#include <assert.h>

#if defined(_MSC_VER) && !defined(__clang__)
extern "C" __int64 __iso_volatile_load64(const volatile __int64*);
extern "C" __int64 _InterlockedCompareExchange64(volatile __int64*, __int64, __int64);
#pragma intrinsic(__iso_volatile_load64)
#pragma intrinsic(_InterlockedCompareExchange64)
#else
static_assert(__atomic_always_lock_free(sizeof(uint64), nullptr), "atomic bump allocation requires lock-free 64-bit atomics");
#endif

namespace gpu
{

namespace
{

byte* offset_pointer(byte* pointer, uint64 offset) noexcept
{
    if (!pointer)
        return nullptr;
    return reinterpret_cast<byte*>(reinterpret_cast<uintptr>(pointer) + offset);
}

} // namespace

BumpAllocator::BumpAllocator(GpuCpuRange<byte> storage) noexcept
    : storage_(storage)
{
    assert(storage.size != 0);
    assert(storage.cpu || storage.gpu);
    assert((!storage.cpu || reinterpret_cast<uintptr>(storage.cpu) % alignment == 0) &&
           (!storage.gpu || reinterpret_cast<uintptr>(storage.gpu) % alignment == 0));
}

BumpAllocator::BumpAllocator(BumpAllocator&& other) noexcept
    : storage_(other.storage_), offset_(other.offset_)
{
    other.storage_ = {};
    other.offset_ = 0;
}

BumpAllocator& BumpAllocator::operator=(BumpAllocator&& other) noexcept
{
    if (this == &other)
        return *this;

    storage_ = other.storage_;
    offset_ = other.offset_;
    other.storage_ = {};
    other.offset_ = 0;
    return *this;
}

GpuCpuRange<byte> BumpAllocator::allocate(uint64 byte_size) noexcept
{
    assert(byte_size != 0);
    if (byte_size > storage_.size - offset_)
        return {};

    const GpuCpuRange<byte> allocation{
        .cpu = offset_pointer(storage_.cpu, offset_),
        .gpu = offset_pointer(storage_.gpu, offset_),
        .size = byte_size,
    };
    const uint64 remaining = storage_.size - offset_;
    const uint64 aligned_size = (byte_size + alignment - 1) & ~(alignment - 1);
    offset_ += aligned_size < remaining ? aligned_size : remaining;
    return allocation;
}

GpuCpuRange<byte> BumpAllocator::allocate_atomic(uint64 byte_size) noexcept
{
    assert(byte_size != 0);
#if defined(_MSC_VER) && !defined(__clang__)
    uint64 allocation_offset = static_cast<uint64>(__iso_volatile_load64(reinterpret_cast<volatile __int64*>(&offset_)));
#else
    uint64 allocation_offset = __atomic_load_n(&offset_, __ATOMIC_RELAXED);
#endif
    for (;;)
    {
        if (byte_size > storage_.size - allocation_offset)
            return {};

        const uint64 remaining = storage_.size - allocation_offset;
        const uint64 aligned_size = (byte_size + alignment - 1) & ~(alignment - 1);
        const uint64 next_offset = allocation_offset + (aligned_size < remaining ? aligned_size : remaining);
#if defined(_MSC_VER) && !defined(__clang__)
        const uint64 previous_offset = static_cast<uint64>(_InterlockedCompareExchange64(reinterpret_cast<volatile __int64*>(&offset_),
                                                                                         static_cast<__int64>(next_offset),
                                                                                         static_cast<__int64>(allocation_offset)));
        if (previous_offset == allocation_offset)
#else
        if (__atomic_compare_exchange_n(&offset_, &allocation_offset, next_offset, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
#endif
        {
            return {
                .cpu = offset_pointer(storage_.cpu, allocation_offset),
                .gpu = offset_pointer(storage_.gpu, allocation_offset),
                .size = byte_size,
            };
        }
#if defined(_MSC_VER) && !defined(__clang__)
        allocation_offset = previous_offset;
#endif
    }
}

void BumpAllocator::reset() noexcept
{
    offset_ = 0;
}

} // namespace gpu
