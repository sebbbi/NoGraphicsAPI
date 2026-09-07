#include <NoGraphicsAPIUtility/bump_allocator.hpp>
#include <NoGraphicsAPIUtility/fixed_function.hpp>
#include <NoGraphicsAPIUtility/heap_allocator.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>
#include <NoGraphicsAPI/bit.hpp>
#include <NoGraphicsAPI/traits.hpp>

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

template<typename T>
T&& declval() noexcept;

static_assert(!__is_constructible(gpu::HeapAllocator, const gpu::HeapAllocator&));
static_assert(__is_nothrow_constructible(gpu::HeapAllocator, gpu::HeapAllocator&&));
static_assert(!__is_constructible(gpu::BumpAllocator, const gpu::BumpAllocator&));
static_assert(__is_nothrow_constructible(gpu::BumpAllocator, gpu::BumpAllocator&&));
static_assert(gpu::HeapAllocator::alignment == 16 && gpu::BumpAllocator::alignment == 16);
static_assert(gpu::HeapAllocator::maximum_size == uint64{~uint32{0}} * gpu::HeapAllocator::alignment);
static_assert(__is_aggregate(gpu::PlacedTexture) && __is_standard_layout(gpu::PlacedTexture) &&
              __is_trivial(gpu::PlacedTexture) && __is_trivially_copyable(gpu::PlacedTexture) &&
              __is_constructible(gpu::PlacedTexture, const gpu::PlacedTexture&));
static_assert(__is_aggregate(gpu::HeapAllocation<gpu::byte>) && __is_standard_layout(gpu::HeapAllocation<gpu::byte>) &&
              __is_trivially_copyable(gpu::HeapAllocation<gpu::byte>));
static_assert(gpu::detail::is_same_v<decltype(declval<gpu::GpuCpuRange<uint32>>().cpu), uint32*>);
static_assert(gpu::detail::is_same_v<decltype(declval<gpu::GpuCpuRange<uint32>>().gpu), uint32*>);
static_assert(gpu::detail::is_same_v<decltype(declval<gpu::HeapAllocator&>().allocate<uint32>(1)), gpu::HeapAllocation<uint32>>);
static_assert(gpu::detail::is_same_v<decltype(declval<gpu::BumpAllocator&>().allocate<uint32>(1)), gpu::GpuCpuRange<uint32>>);
static_assert(gpu::detail::is_same_v<decltype(declval<gpu::BumpAllocator&>().allocate_atomic<uint32>(1)), gpu::GpuCpuRange<uint32>>);
static_assert(!__is_constructible(gpu::FixedFunction<128>, const gpu::FixedFunction<128>&));
static_assert(!__is_constructible(gpu::FixedFunction<128>, gpu::FixedFunction<128>&&));

namespace
{

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            fprintf(stderr, "allocator check failed at line %d: %s\n", __LINE__, #condition); \
            return false; \
        } \
    } while (false)

template<typename T>
bool empty(gpu::GpuCpuRange<T> allocation) noexcept
{
    return !allocation.cpu && !allocation.gpu && allocation.size == 0;
}

template<typename T>
bool empty(gpu::HeapAllocation<T> allocation) noexcept
{
    return empty(allocation.range) && allocation.token == ~uint32{0};
}

bool check_gpu_cpu_range_defaults() noexcept
{
    constexpr gpu::GpuCpuRange<gpu::byte> allocation{};
    constexpr gpu::HeapAllocation<gpu::byte> heap_allocation{};
    constexpr gpu::PlacedTexture placed_texture{};
    CHECK(!allocation.cpu && !allocation.gpu && allocation.size == 0);
    CHECK(empty(heap_allocation));
    CHECK(!placed_texture.texture && placed_texture.token == 0);
    return true;
}

bool check_bit_operations() noexcept
{
    CHECK(gpu::detail::count_leading_zeros(0) == 32 && gpu::detail::count_trailing_zeros(0) == 32 && gpu::detail::popcount(0) == 0);
    CHECK(gpu::detail::count_leading_zeros(~uint32{0}) == 0 && gpu::detail::count_trailing_zeros(~uint32{0}) == 0);
    CHECK(gpu::detail::popcount(~uint32{0}) == 32 && gpu::detail::popcount(0xaaaaaaaau) == 16 && gpu::detail::popcount(0x55555555u) == 16);
    CHECK(gpu::detail::count_leading_zeros(0x08000800u) == 4 && gpu::detail::count_trailing_zeros(0x08000800u) == 11);
    CHECK(gpu::detail::popcount(0x08000800u) == 2 && gpu::detail::popcount(0x81008001u) == 4);
    for (uint32 bit = 0; bit != 32; ++bit)
    {
        CHECK(gpu::detail::count_leading_zeros(1u << bit) == 31 - bit && gpu::detail::count_trailing_zeros(1u << bit) == bit);
        CHECK(gpu::detail::popcount(1u << bit) == 1 && gpu::detail::popcount(~(1u << bit)) == 31);
    }
    return true;
}

struct FullFixedFunctionCallback
{
    byte padding[128 - sizeof(uint32*)]{};
    uint32* invocation_count = nullptr;

    void operator()() noexcept
    {
        ++*invocation_count;
    }
};

static_assert(sizeof(FullFixedFunctionCallback) == 128);

struct alignas(gpu::maximum_alignment) CountingCallback
{
    uint32* invocation_count = nullptr;
    uint32 increment = 2;

    void operator()() noexcept
    {
        assert(reinterpret_cast<uintptr>(this) % alignof(CountingCallback) == 0);
        *invocation_count += increment++;
    }
};

bool check_fixed_function() noexcept
{
    uint32 invocation_count = 0;
    gpu::FixedFunction<128> callback;
    callback.set(FullFixedFunctionCallback{.invocation_count = &invocation_count});
    callback();
    callback.clear();
    callback.set(CountingCallback{.invocation_count = &invocation_count});
    callback();
    callback();
    callback.clear();
    callback.set(FullFixedFunctionCallback{.invocation_count = &invocation_count});
    callback();
    CHECK(invocation_count == 7);
    return true;
}

bool check_heap_allocator_basics() noexcept
{
    alignas(64) byte cpu[4096]{};
    alignas(64) byte gpu_address[4096]{};
    gpu::HeapAllocator allocator({.cpu = cpu, .gpu = gpu_address, .size = sizeof(cpu)}, 8);
    const gpu::HeapAllocation<gpu::byte> first = allocator.allocate(1);
    const gpu::HeapAllocation<gpu::byte> second = allocator.allocate(17);
    const gpu::HeapAllocation<gpu::byte> third = allocator.allocate(32);
    CHECK(first.range.cpu == cpu && first.range.gpu == gpu_address && first.range.size == 1);
    CHECK(second.range.cpu == cpu + 16 && second.range.gpu == gpu_address + 16 && second.range.size == 17);
    CHECK(third.range.cpu == cpu + 48 && third.range.gpu == gpu_address + 48 && third.range.size == 32);
    CHECK(first.token != second.token && first.token != third.token && second.token != third.token);

    allocator.free(second);
    allocator.free(first);
    allocator.free(third);
    const gpu::HeapAllocation<gpu::byte> whole = allocator.allocate(sizeof(cpu));
    CHECK(whole.range.cpu == cpu && whole.range.gpu == gpu_address && whole.range.size == sizeof(cpu));
    CHECK(empty(allocator.allocate(16)));
    allocator.free(whole);

    allocator.reset();
    CHECK(empty(allocator.allocate(sizeof(cpu) + 1)));

    alignas(16) byte partial[17]{};
    gpu::HeapAllocator partial_allocator({.cpu = partial, .size = sizeof(partial)}, 2);
    CHECK(partial_allocator.allocate(1).range.cpu == partial);
    CHECK(empty(partial_allocator.allocate(1)));
    return true;
}

bool check_heap_allocator_address_domains() noexcept
{
    alignas(64) byte cpu[128]{};
    alignas(64) byte gpu_address[128]{};
    gpu::HeapAllocator cpu_only({.cpu = cpu, .size = sizeof(cpu)}, 1);
    const gpu::HeapAllocation<gpu::byte> cpu_allocation = cpu_only.allocate(1);
    CHECK(cpu_allocation.range.cpu == cpu && !cpu_allocation.range.gpu && cpu_allocation.range.size == 1);
    cpu_only.free(cpu_allocation);

    gpu::HeapAllocator gpu_only({.gpu = gpu_address, .size = sizeof(gpu_address)}, 1);
    const gpu::HeapAllocation<gpu::byte> gpu_allocation = gpu_only.allocate(1);
    CHECK(!gpu_allocation.range.cpu && gpu_allocation.range.gpu == gpu_address && gpu_allocation.range.size == 1);
    gpu_only.free(gpu_allocation);
    return true;
}

bool check_heap_allocator_limit_and_move() noexcept
{
    alignas(16) byte cpu[128]{};
    alignas(16) byte gpu_address[128]{};
    gpu::HeapAllocator source({.cpu = cpu, .gpu = gpu_address, .size = sizeof(cpu)}, 2);
    const gpu::HeapAllocation<gpu::byte> first = source.allocate(16);
    const gpu::HeapAllocation<gpu::byte> second = source.allocate(16);
    CHECK(!empty(first) && !empty(second));
    CHECK(empty(source.allocate(16)));
    source.free(first);
    const gpu::HeapAllocation<gpu::byte> replacement = source.allocate(16);
    CHECK(replacement.range.gpu == first.range.gpu && replacement.token == first.token);

    gpu::HeapAllocator moved(static_cast<gpu::HeapAllocator&&>(source));
    source.reset();
    CHECK(empty(source.allocate(16)));
    moved.free(replacement);
    moved.free(second);
    CHECK(moved.allocate(sizeof(cpu)).range.gpu == gpu_address);

    alignas(16) byte other_cpu[64]{};
    alignas(16) byte other_gpu[64]{};
    gpu::HeapAllocator destination({.cpu = other_cpu, .gpu = other_gpu, .size = sizeof(other_cpu)}, 1);
    destination = static_cast<gpu::HeapAllocator&&>(moved);
    moved.reset();
    CHECK(empty(moved.allocate(16)));
    destination.reset();
    CHECK(destination.allocate(sizeof(cpu)).range.gpu == gpu_address);
    return true;
}

bool ranges_overlap(gpu::HeapAllocation<gpu::byte> lhs, gpu::HeapAllocation<gpu::byte> rhs) noexcept
{
    const uintptr lhs_begin = reinterpret_cast<uintptr>(lhs.range.gpu);
    const uintptr rhs_begin = reinterpret_cast<uintptr>(rhs.range.gpu);
    const uint64 lhs_size = (lhs.range.size + 15) & ~uint64{15};
    const uint64 rhs_size = (rhs.range.size + 15) & ~uint64{15};
    return lhs_begin < rhs_begin + rhs_size && rhs_begin < lhs_begin + lhs_size;
}

bool check_typed_allocations() noexcept
{
    alignas(16) byte cpu[64]{};
    alignas(16) byte gpu_address[64]{};
    gpu::HeapAllocator heap_allocator({.cpu = cpu, .gpu = gpu_address, .size = sizeof(cpu)}, 1);
    const gpu::HeapAllocation<uint32> heap_allocation = heap_allocator.allocate<uint32>(3);
    CHECK(heap_allocation.range.cpu == reinterpret_cast<uint32*>(cpu) &&
          heap_allocation.range.gpu == reinterpret_cast<uint32*>(gpu_address) && heap_allocation.range.size == 12);
    CHECK(gpu::gpu_range(heap_allocation.range).gpu == gpu_address && gpu::gpu_range(heap_allocation.range).size == 12);
    heap_allocator.free(heap_allocation);

    gpu::BumpAllocator bump_allocator({.cpu = cpu, .gpu = gpu_address, .size = sizeof(cpu)});
    const gpu::GpuCpuRange<uint16> bump_allocation = bump_allocator.allocate<uint16>(5);
    CHECK(bump_allocation.cpu == reinterpret_cast<uint16*>(cpu) &&
          bump_allocation.gpu == reinterpret_cast<uint16*>(gpu_address) && bump_allocation.size == 10);
    return true;
}

bool check_heap_allocator_fragmentation() noexcept
{
    constexpr uint32 allocation_count = 64;
    alignas(16) byte gpu_address[4096]{};
    gpu::HeapAllocator allocator({.gpu = gpu_address, .size = sizeof(gpu_address)}, allocation_count);
    gpu::HeapAllocation<gpu::byte> allocations[allocation_count]{};
    uint32 random = 0x12345678u;

    for (uint32 iteration = 0; iteration != 20000; ++iteration)
    {
        random = random * 1664525u + 1013904223u;
        const uint32 slot = random % allocation_count;
        if (!empty(allocations[slot]))
        {
            allocator.free(allocations[slot]);
            allocations[slot] = {};
            continue;
        }

        random = random * 1664525u + 1013904223u;
        const uint64 size = (random % 97u) + 1;
        allocations[slot] = allocator.allocate(size);
        if (empty(allocations[slot]))
            continue;

        CHECK(allocations[slot].range.size == size);
        const uintptr allocation_begin = reinterpret_cast<uintptr>(allocations[slot].range.gpu);
        const uintptr storage_begin = reinterpret_cast<uintptr>(gpu_address);
        CHECK(allocation_begin >= storage_begin && allocation_begin + allocations[slot].range.size <= storage_begin + sizeof(gpu_address));
        for (uint32 other = 0; other != allocation_count; ++other)
        {
            if (other != slot && !empty(allocations[other]))
                CHECK(!ranges_overlap(allocations[slot], allocations[other]));
        }
    }

    for (gpu::HeapAllocation<gpu::byte> allocation : allocations)
    {
        if (!empty(allocation))
            allocator.free(allocation);
    }
    CHECK(allocator.allocate(sizeof(gpu_address)).range.gpu == gpu_address);
    return true;
}

bool check_bump_allocator() noexcept
{
    alignas(16) byte cpu[64]{};
    alignas(16) byte gpu_address[64]{};
    gpu::BumpAllocator allocator({.cpu = cpu, .gpu = gpu_address, .size = sizeof(cpu)});
    const gpu::GpuCpuRange<gpu::byte> first = allocator.allocate(1);
    const gpu::GpuCpuRange<gpu::byte> second = allocator.allocate(17);
    const gpu::GpuCpuRange<gpu::byte> third = allocator.allocate(16);
    CHECK(first.cpu == cpu && first.gpu == gpu_address && first.size == 1);
    CHECK(second.cpu == cpu + 16 && second.gpu == gpu_address + 16 && second.size == 17);
    CHECK(third.cpu == cpu + 48 && third.gpu == gpu_address + 48 && third.size == 16);
    CHECK(empty(allocator.allocate(1)));

    allocator.reset();
    gpu::BumpAllocator moved(static_cast<gpu::BumpAllocator&&>(allocator));
    CHECK(moved.allocate(sizeof(cpu)).gpu == gpu_address);

    alignas(16) byte partial[17]{};
    gpu::BumpAllocator partial_allocator({.cpu = partial, .size = sizeof(partial)});
    CHECK(partial_allocator.allocate(1).cpu == partial);
    CHECK(partial_allocator.allocate(1).cpu == partial + 16);
    CHECK(empty(partial_allocator.allocate(1)));
    return true;
}

struct AtomicBumpThreadContext
{
    gpu::BumpAllocator* allocator = nullptr;
    gpu::GpuCpuRange<gpu::byte>* allocations = nullptr;
    uint32 allocation_count = 0;
};

#if defined(_WIN32)
DWORD WINAPI allocate_atomic_ranges(void* argument) noexcept
#else
void* allocate_atomic_ranges(void* argument) noexcept
#endif
{
    AtomicBumpThreadContext* context = static_cast<AtomicBumpThreadContext*>(argument);
    for (uint32 index = 0; index != context->allocation_count; ++index)
        context->allocations[index] = context->allocator->allocate_atomic(1);
    return 0;
}

bool check_atomic_bump_allocator() noexcept
{
    constexpr uint32 thread_count = 8;
    constexpr uint32 allocations_per_thread = 64;
    constexpr uint32 allocation_count = thread_count * allocations_per_thread;
    alignas(16) byte cpu[allocation_count * gpu::BumpAllocator::alignment]{};
    alignas(16) byte gpu_address[sizeof(cpu)]{};
    gpu::GpuCpuRange<gpu::byte> allocations[allocation_count]{};
    AtomicBumpThreadContext contexts[thread_count]{};
#if defined(_WIN32)
    HANDLE threads[thread_count]{};
#else
    pthread_t threads[thread_count]{};
#endif
    gpu::BumpAllocator allocator({.cpu = cpu, .gpu = gpu_address, .size = sizeof(cpu)});

    uint32 started_count = 0;
    for (; started_count != thread_count; ++started_count)
    {
        contexts[started_count] = {
            .allocator = &allocator,
            .allocations = allocations + started_count * allocations_per_thread,
            .allocation_count = allocations_per_thread,
        };
#if defined(_WIN32)
        threads[started_count] = CreateThread(nullptr, 0, allocate_atomic_ranges, contexts + started_count, 0, nullptr);
        if (!threads[started_count])
            break;
#else
        if (pthread_create(threads + started_count, nullptr, allocate_atomic_ranges, contexts + started_count) != 0)
            break;
#endif
    }
    bool joined = true;
    for (uint32 thread_index = 0; thread_index != started_count; ++thread_index)
    {
#if defined(_WIN32)
        if (WaitForSingleObject(threads[thread_index], INFINITE) != WAIT_OBJECT_0)
            joined = false;
        CloseHandle(threads[thread_index]);
#else
        if (pthread_join(threads[thread_index], nullptr) != 0)
            joined = false;
#endif
    }
    CHECK(started_count == thread_count && joined);

    for (uint32 index = 0; index != allocation_count; ++index)
    {
        const uintptr cpu_offset = reinterpret_cast<uintptr>(allocations[index].cpu) - reinterpret_cast<uintptr>(cpu);
        const uintptr gpu_offset = reinterpret_cast<uintptr>(allocations[index].gpu) - reinterpret_cast<uintptr>(gpu_address);
        CHECK(allocations[index].size == 1 && cpu_offset == gpu_offset && cpu_offset % gpu::BumpAllocator::alignment == 0 && cpu_offset < sizeof(cpu));
        for (uint32 other = 0; other != index; ++other)
            CHECK(allocations[index].cpu != allocations[other].cpu && allocations[index].gpu != allocations[other].gpu);
    }
    CHECK(empty(allocator.allocate_atomic(1)));
    allocator.reset();
    CHECK(allocator.allocate_atomic(sizeof(cpu)).cpu == cpu);
    return true;
}

bool check_maximum_heap_range() noexcept
{
    byte* gpu_address = reinterpret_cast<byte*>(gpu::HeapAllocator::alignment);
    gpu::HeapAllocator heap_allocator({.gpu = gpu_address, .size = gpu::HeapAllocator::maximum_size}, 1);
    const gpu::HeapAllocation<gpu::byte> heap_allocation = heap_allocator.allocate(gpu::HeapAllocator::maximum_size);
    CHECK(heap_allocation.range.gpu == gpu_address && heap_allocation.range.size == gpu::HeapAllocator::maximum_size);
    heap_allocator.free(heap_allocation);
    return true;
}

} // namespace

int main()
{
    if (!check_gpu_cpu_range_defaults() || !check_bit_operations() || !check_fixed_function() ||
        !check_heap_allocator_basics() || !check_heap_allocator_address_domains() ||
        !check_heap_allocator_limit_and_move() || !check_typed_allocations() || !check_heap_allocator_fragmentation() || !check_bump_allocator() ||
        !check_atomic_bump_allocator() || !check_maximum_heap_range())
        return 1;
    return 0;
}
