#include <NoGraphicsAPIUtility/bump_allocator.hpp>
#include <NoGraphicsAPIUtility/heap_allocator.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <utility>

static_assert(!std::is_copy_constructible_v<gpu::HeapAllocator>);
static_assert(std::is_nothrow_move_constructible_v<gpu::HeapAllocator>);
static_assert(!std::is_copy_constructible_v<gpu::BumpAllocator>);
static_assert(std::is_nothrow_move_constructible_v<gpu::BumpAllocator>);
static_assert(gpu::HeapAllocator::alignment == 16 && gpu::BumpAllocator::alignment == 16);
static_assert(gpu::HeapAllocator::maximum_size == std::uint64_t{std::numeric_limits<std::uint32_t>::max()} * gpu::HeapAllocator::alignment);
static_assert(std::is_aggregate_v<gpu::PlacedTexture> && std::is_standard_layout_v<gpu::PlacedTexture> &&
              std::is_trivial_v<gpu::PlacedTexture> && std::is_trivially_copyable_v<gpu::PlacedTexture> &&
              std::is_copy_constructible_v<gpu::PlacedTexture>);
static_assert(std::is_aggregate_v<gpu::HeapAllocation<gpu::byte>> && std::is_standard_layout_v<gpu::HeapAllocation<gpu::byte>> &&
              std::is_trivially_copyable_v<gpu::HeapAllocation<gpu::byte>>);
static_assert(std::is_same_v<decltype(std::declval<gpu::GpuCpuRange<std::uint32_t>>().cpu), std::uint32_t*>);
static_assert(std::is_same_v<decltype(std::declval<gpu::GpuCpuRange<std::uint32_t>>().gpu), std::uint32_t*>);
static_assert(std::is_same_v<decltype(std::declval<gpu::HeapAllocator&>().allocate<std::uint32_t>(1)), gpu::HeapAllocation<std::uint32_t>>);
static_assert(std::is_same_v<decltype(std::declval<gpu::BumpAllocator&>().allocate<std::uint32_t>(1)), gpu::GpuCpuRange<std::uint32_t>>);

namespace
{

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::fprintf(stderr, "allocator check failed at line %d: %s\n", __LINE__, #condition); \
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
    return empty(allocation.range) && allocation.token == std::numeric_limits<std::uint32_t>::max();
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

bool check_heap_allocator_basics() noexcept
{
    alignas(64) std::byte cpu[4096]{};
    alignas(64) std::byte gpu_address[4096]{};
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

    alignas(16) std::byte partial[17]{};
    gpu::HeapAllocator partial_allocator({.cpu = partial, .size = sizeof(partial)}, 2);
    CHECK(partial_allocator.allocate(1).range.cpu == partial);
    CHECK(empty(partial_allocator.allocate(1)));
    return true;
}

bool check_heap_allocator_address_domains() noexcept
{
    alignas(64) std::byte cpu[128]{};
    alignas(64) std::byte gpu_address[128]{};
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
    alignas(16) std::byte cpu[128]{};
    alignas(16) std::byte gpu_address[128]{};
    gpu::HeapAllocator source({.cpu = cpu, .gpu = gpu_address, .size = sizeof(cpu)}, 2);
    const gpu::HeapAllocation<gpu::byte> first = source.allocate(16);
    const gpu::HeapAllocation<gpu::byte> second = source.allocate(16);
    CHECK(!empty(first) && !empty(second));
    CHECK(empty(source.allocate(16)));
    source.free(first);
    const gpu::HeapAllocation<gpu::byte> replacement = source.allocate(16);
    CHECK(replacement.range.gpu == first.range.gpu && replacement.token == first.token);

    gpu::HeapAllocator moved(std::move(source));
    source.reset();
    CHECK(empty(source.allocate(16)));
    moved.free(replacement);
    moved.free(second);
    CHECK(moved.allocate(sizeof(cpu)).range.gpu == gpu_address);

    alignas(16) std::byte other_cpu[64]{};
    alignas(16) std::byte other_gpu[64]{};
    gpu::HeapAllocator destination({.cpu = other_cpu, .gpu = other_gpu, .size = sizeof(other_cpu)}, 1);
    destination = std::move(moved);
    moved.reset();
    CHECK(empty(moved.allocate(16)));
    destination.reset();
    CHECK(destination.allocate(sizeof(cpu)).range.gpu == gpu_address);
    return true;
}

bool ranges_overlap(gpu::HeapAllocation<gpu::byte> lhs, gpu::HeapAllocation<gpu::byte> rhs) noexcept
{
    const std::uintptr_t lhs_begin = reinterpret_cast<std::uintptr_t>(lhs.range.gpu);
    const std::uintptr_t rhs_begin = reinterpret_cast<std::uintptr_t>(rhs.range.gpu);
    const std::uint64_t lhs_size = (lhs.range.size + 15) & ~UINT64_C(15);
    const std::uint64_t rhs_size = (rhs.range.size + 15) & ~UINT64_C(15);
    return lhs_begin < rhs_begin + rhs_size && rhs_begin < lhs_begin + lhs_size;
}

bool check_typed_allocations() noexcept
{
    alignas(16) std::byte cpu[64]{};
    alignas(16) std::byte gpu_address[64]{};
    gpu::HeapAllocator heap_allocator({.cpu = cpu, .gpu = gpu_address, .size = sizeof(cpu)}, 1);
    const gpu::HeapAllocation<std::uint32_t> heap_allocation = heap_allocator.allocate<std::uint32_t>(3);
    CHECK(heap_allocation.range.cpu == reinterpret_cast<std::uint32_t*>(cpu) &&
          heap_allocation.range.gpu == reinterpret_cast<std::uint32_t*>(gpu_address) && heap_allocation.range.size == 12);
    CHECK(gpu::gpu_range(heap_allocation.range).gpu == gpu_address && gpu::gpu_range(heap_allocation.range).size == 12);
    heap_allocator.free(heap_allocation);

    gpu::BumpAllocator bump_allocator({.cpu = cpu, .gpu = gpu_address, .size = sizeof(cpu)});
    const gpu::GpuCpuRange<std::uint16_t> bump_allocation = bump_allocator.allocate<std::uint16_t>(5);
    CHECK(bump_allocation.cpu == reinterpret_cast<std::uint16_t*>(cpu) &&
          bump_allocation.gpu == reinterpret_cast<std::uint16_t*>(gpu_address) && bump_allocation.size == 10);
    return true;
}

bool check_heap_allocator_fragmentation() noexcept
{
    constexpr std::uint32_t allocation_count = 64;
    alignas(16) std::byte gpu_address[4096]{};
    gpu::HeapAllocator allocator({.gpu = gpu_address, .size = sizeof(gpu_address)}, allocation_count);
    gpu::HeapAllocation<gpu::byte> allocations[allocation_count]{};
    std::uint32_t random = 0x12345678u;

    for (std::uint32_t iteration = 0; iteration != 20000; ++iteration)
    {
        random = random * 1664525u + 1013904223u;
        const std::uint32_t slot = random % allocation_count;
        if (!empty(allocations[slot]))
        {
            allocator.free(allocations[slot]);
            allocations[slot] = {};
            continue;
        }

        random = random * 1664525u + 1013904223u;
        const std::uint64_t size = (random % 97u) + 1;
        allocations[slot] = allocator.allocate(size);
        if (empty(allocations[slot]))
            continue;

        CHECK(allocations[slot].range.size == size);
        const std::uintptr_t allocation_begin = reinterpret_cast<std::uintptr_t>(allocations[slot].range.gpu);
        const std::uintptr_t storage_begin = reinterpret_cast<std::uintptr_t>(gpu_address);
        CHECK(allocation_begin >= storage_begin && allocation_begin + allocations[slot].range.size <= storage_begin + sizeof(gpu_address));
        for (std::uint32_t other = 0; other != allocation_count; ++other)
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
    alignas(16) std::byte cpu[64]{};
    alignas(16) std::byte gpu_address[64]{};
    gpu::BumpAllocator allocator({.cpu = cpu, .gpu = gpu_address, .size = sizeof(cpu)});
    const gpu::GpuCpuRange<gpu::byte> first = allocator.allocate(1);
    const gpu::GpuCpuRange<gpu::byte> second = allocator.allocate(17);
    const gpu::GpuCpuRange<gpu::byte> third = allocator.allocate(16);
    CHECK(first.cpu == cpu && first.gpu == gpu_address && first.size == 1);
    CHECK(second.cpu == cpu + 16 && second.gpu == gpu_address + 16 && second.size == 17);
    CHECK(third.cpu == cpu + 48 && third.gpu == gpu_address + 48 && third.size == 16);
    CHECK(empty(allocator.allocate(1)));

    allocator.reset();
    gpu::BumpAllocator moved(std::move(allocator));
    CHECK(moved.allocate(sizeof(cpu)).gpu == gpu_address);

    alignas(16) std::byte partial[17]{};
    gpu::BumpAllocator partial_allocator({.cpu = partial, .size = sizeof(partial)});
    CHECK(partial_allocator.allocate(1).cpu == partial);
    CHECK(partial_allocator.allocate(1).cpu == partial + 16);
    CHECK(empty(partial_allocator.allocate(1)));
    return true;
}

bool check_maximum_heap_range() noexcept
{
    std::byte* gpu_address = reinterpret_cast<std::byte*>(gpu::HeapAllocator::alignment);
    gpu::HeapAllocator heap_allocator({.gpu = gpu_address, .size = gpu::HeapAllocator::maximum_size}, 1);
    const gpu::HeapAllocation<gpu::byte> heap_allocation = heap_allocator.allocate(gpu::HeapAllocator::maximum_size);
    CHECK(heap_allocation.range.gpu == gpu_address && heap_allocation.range.size == gpu::HeapAllocator::maximum_size);
    heap_allocator.free(heap_allocation);
    return true;
}

} // namespace

int main()
{
    if (!check_gpu_cpu_range_defaults() || !check_heap_allocator_basics() || !check_heap_allocator_address_domains() ||
        !check_heap_allocator_limit_and_move() || !check_typed_allocations() || !check_heap_allocator_fragmentation() || !check_bump_allocator() ||
        !check_maximum_heap_range())
        return 1;
    return 0;
}
