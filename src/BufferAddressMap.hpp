#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <NoGraphicsAPI/bit.hpp>
#include <assert.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace gpu::detail
{

inline uint64 buffer_atomic_load(const uint64* value, bool acquire = false) noexcept
{
#if defined(_MSC_VER) && !defined(__clang__)
    // The MSVC backend targets x86-64, where aligned 64-bit accesses need only compiler ordering.
    const uint64 result = *reinterpret_cast<const volatile uint64*>(value);
    if (acquire) _ReadWriteBarrier();
    return result;
#else
    return __atomic_load_n(value, acquire ? __ATOMIC_ACQUIRE : __ATOMIC_RELAXED);
#endif
}

inline void buffer_atomic_store(uint64* destination, uint64 value, bool release = false) noexcept
{
#if defined(_MSC_VER) && !defined(__clang__)
    if (release) _ReadWriteBarrier();
    *reinterpret_cast<volatile uint64*>(destination) = value;
#else
    __atomic_store_n(destination, value, release ? __ATOMIC_RELEASE : __ATOMIC_RELAXED);
#endif
}

inline void buffer_atomic_fence(bool release) noexcept
{
#if defined(_MSC_VER) && !defined(__clang__)
    (void)release;
    _ReadWriteBarrier();
#else
    __atomic_thread_fence(release ? __ATOMIC_RELEASE : __ATOMIC_ACQUIRE);
#endif
}

inline bool buffer_atomic_exchange(uint64* destination, uint64* expected, uint64 value) noexcept
{
#if defined(_MSC_VER) && !defined(__clang__)
    const uint64 previous = static_cast<uint64>(_InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(destination),
                                                                              static_cast<long long>(value), static_cast<long long>(*expected)));
    if (previous == *expected) return true;
    *expected = previous;
    return false;
#else
    return __atomic_compare_exchange_n(destination, expected, value, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
#endif
}

struct BufferRecord
{
    uint64 base = 0;
    uint64 size = 0;
    uint64 buffer = 0;
};

struct BufferAddressMap
{
    BufferRecord records[64] = {};
    uint64 indices[64] = {};
    uint64 sequence = 0;
    uint64 count = 0;
    uint64 occupied = 0;

    uint64 begin_update() noexcept
    {
        uint64 previous = buffer_atomic_load(&sequence);
        for (;;)
        {
            if (!(previous & 1) && buffer_atomic_exchange(&sequence, &previous, previous + 1))
            {
                // Readers observing changed metadata must also observe this writer's odd sequence.
                buffer_atomic_fence(true);
                return previous;
            }
            previous = buffer_atomic_load(&sequence);
        }
    }

    BufferRecord* insert(uint64 base, uint64 size, uint64 buffer) noexcept
    {
        const uint64 previous = begin_update();
        assert(occupied != ~uint64{0});
        const uint32 slot = static_cast<uint32>(~occupied) ? count_trailing_zeros(static_cast<uint32>(~occupied))
                                                          : 32 + count_trailing_zeros(static_cast<uint32>(~occupied >> 32));
        occupied |= uint64{1} << slot;
        buffer_atomic_store(&records[slot].base, base);
        buffer_atomic_store(&records[slot].size, size);
        buffer_atomic_store(&records[slot].buffer, buffer);
        uint64 index = buffer_atomic_load(&count);
        buffer_atomic_store(&count, index + 1);
        while (index && buffer_atomic_load(&records[buffer_atomic_load(&indices[index - 1])].base) > base)
        {
            buffer_atomic_store(&indices[index], buffer_atomic_load(&indices[index - 1]));
            --index;
        }
        buffer_atomic_store(&indices[index], slot);
        buffer_atomic_store(&sequence, previous + 2, true);
        return &records[slot];
    }

    void remove(BufferRecord* record) noexcept
    {
        const uint64 previous = begin_update();
        const uint64 size = buffer_atomic_load(&count);
        uint64 index = 0;
        while (index < size && &records[buffer_atomic_load(&indices[index])] != record) ++index;
        assert(index < size);
        for (; index + 1 < size; ++index) buffer_atomic_store(&indices[index], buffer_atomic_load(&indices[index + 1]));
        buffer_atomic_store(&count, size - 1);
        occupied &= ~(uint64{1} << (record - records));
        buffer_atomic_store(&sequence, previous + 2, true);
    }

    uint64 resolve(GpuRange range, uint64* offset) const noexcept
    {
        const uint64 address = reinterpret_cast<uintptr>(range.gpu);
        for (;;)
        {
            const uint64 previous = buffer_atomic_load(&sequence, true);
            if (previous & 1) continue;
            uint32 first = 0;
            uint32 last = static_cast<uint32>(buffer_atomic_load(&count));
            while (first < last)
            {
                const uint32 middle = first + (last - first) / 2;
                const BufferRecord* record = &records[buffer_atomic_load(&indices[middle])];
                if (buffer_atomic_load(&record->base) <= address) first = middle + 1;
                else last = middle;
            }
            uint64 base = 0;
            uint64 size = 0;
            uint64 buffer = 0;
            if (first)
            {
                const BufferRecord* record = &records[buffer_atomic_load(&indices[first - 1])];
                base = buffer_atomic_load(&record->base);
                size = buffer_atomic_load(&record->size);
                buffer = buffer_atomic_load(&record->buffer);
            }
            buffer_atomic_fence(false);
            if (buffer_atomic_load(&sequence) != previous) continue;
            assert(first && address >= base && address - base <= size && range.size <= size - (address - base));
            (void)size;
            *offset = address - base;
            return buffer;
        }
    }
};

} // namespace gpu::detail
