#include <NoGraphicsAPIUtility/heap_allocator.hpp>

#include <bit>
#include <cassert>
#include <cstring>

namespace
{

constexpr std::uint32_t mantissa_bits = 3;
constexpr std::uint32_t mantissa_value = 1u << mantissa_bits;
constexpr std::uint32_t mantissa_mask = mantissa_value - 1;
constexpr std::uint32_t bins_per_leaf = 8;
constexpr std::uint32_t invalid_bin = std::numeric_limits<std::uint32_t>::max();

std::uint32_t size_to_bin_round_down(std::uint32_t size) noexcept
{
    if (size < mantissa_value)
        return size;

    const std::uint32_t mantissa_start_bit = 31u - std::countl_zero(size) - mantissa_bits;
    return ((mantissa_start_bit + 1) << mantissa_bits) | ((size >> mantissa_start_bit) & mantissa_mask);
}

std::uint32_t find_lowest_set_bit(std::uint32_t bits, std::uint32_t first_bit) noexcept
{
    if (first_bit >= 32)
        return invalid_bin;

    if (first_bit != 0)
        bits &= ~((1u << first_bit) - 1);
    return bits == 0 ? invalid_bin : std::countr_zero(bits);
}

std::byte* offset_pointer(std::byte* pointer, std::uint64_t offset) noexcept
{
    if (!pointer)
        return nullptr;
    return reinterpret_cast<std::byte*>(reinterpret_cast<std::uintptr_t>(pointer) + offset);
}

std::uint64_t element_count(std::uint64_t byte_size, std::uint64_t element_size) noexcept
{
    return 1 + (byte_size - 1) / element_size;
}

std::uint64_t validate_storage(gpu::GpuCpuRange<gpu::byte> storage) noexcept
{
    assert(storage.cpu || storage.gpu);
    assert(storage.size <= gpu::HeapAllocator::maximum_size);
    assert((!storage.cpu || reinterpret_cast<std::uintptr_t>(storage.cpu) % gpu::HeapAllocator::alignment == 0) &&
           (!storage.gpu || reinterpret_cast<std::uintptr_t>(storage.gpu) % gpu::HeapAllocator::alignment == 0));
    return storage.size <= gpu::HeapAllocator::maximum_size ? storage.size : gpu::HeapAllocator::maximum_size;
}

} // namespace

namespace gpu
{

HeapAllocator::RangeAllocator::RangeAllocator(std::uint64_t byte_size, std::uint32_t allocation_limit, std::uint64_t allocation_element_size) noexcept
    : element_size(allocation_element_size)
{
    assert(element_size != 0 && (element_size & (element_size - 1)) == 0 && byte_size >= element_size);
    assert(allocation_limit != 0 && allocation_limit <= maximum_allocation_count);
    const std::uint64_t element_capacity = byte_size / element_size;
    assert(element_capacity <= std::numeric_limits<std::uint32_t>::max());
    capacity = static_cast<std::uint32_t>(element_capacity <= std::numeric_limits<std::uint32_t>::max()
                                              ? element_capacity
                                              : std::numeric_limits<std::uint32_t>::max());
    max_allocations = allocation_limit;
    node_capacity = allocation_limit * 2 + 1;
    nodes = new Node[node_capacity];
    free_nodes = new NodeIndex[node_capacity];
    reset();
}

HeapAllocator::RangeAllocator::RangeAllocator(RangeAllocator&& other) noexcept
{
    move_from(other);
}

HeapAllocator::RangeAllocator& HeapAllocator::RangeAllocator::operator=(RangeAllocator&& other) noexcept
{
    if (this == &other)
        return *this;

    delete[] nodes;
    delete[] free_nodes;
    move_from(other);
    return *this;
}

HeapAllocator::RangeAllocator::~RangeAllocator()
{
    delete[] nodes;
    delete[] free_nodes;
}

void HeapAllocator::RangeAllocator::move_from(RangeAllocator& other) noexcept
{
    element_size = other.element_size;
    capacity = other.capacity;
    max_allocations = other.max_allocations;
    allocation_count = other.allocation_count;
    node_capacity = other.node_capacity;
    free_node_count = other.free_node_count;
    used_top_bins = other.used_top_bins;
    std::memcpy(used_leaf_bins, other.used_leaf_bins, sizeof(used_leaf_bins));
    std::memcpy(bin_indices, other.bin_indices, sizeof(bin_indices));
    nodes = other.nodes;
    free_nodes = other.free_nodes;
    first_node = other.first_node;

    other.element_size = 0;
    other.capacity = 0;
    other.max_allocations = 0;
    other.allocation_count = 0;
    other.node_capacity = 0;
    other.free_node_count = 0;
    other.used_top_bins = 0;
    other.nodes = nullptr;
    other.free_nodes = nullptr;
    other.first_node = unused_node;
}

HeapAllocator::NodeIndex HeapAllocator::RangeAllocator::acquire_node() noexcept
{
    assert(free_node_count != 0);
    return free_nodes[--free_node_count];
}

void HeapAllocator::RangeAllocator::release_node(NodeIndex node_index) noexcept
{
    assert(free_node_count < node_capacity);
    nodes[node_index] = {};
    free_nodes[free_node_count++] = node_index;
}

void HeapAllocator::RangeAllocator::insert_free_node(NodeIndex node_index) noexcept
{
    Node& node = nodes[node_index];
    const std::uint32_t bin_index = size_to_bin_round_down(node.size);
    const std::uint32_t top_bin_index = bin_index / bins_per_leaf;
    const std::uint32_t leaf_bin_index = bin_index % bins_per_leaf;

    node.used = false;
    node.bin_previous = unused_node;
    node.bin_next = bin_indices[bin_index];
    if (node.bin_next != unused_node)
        nodes[node.bin_next].bin_previous = node_index;
    bin_indices[bin_index] = node_index;
    used_leaf_bins[top_bin_index] |= static_cast<std::uint8_t>(1u << leaf_bin_index);
    used_top_bins |= 1u << top_bin_index;
}

void HeapAllocator::RangeAllocator::remove_free_node(NodeIndex node_index) noexcept
{
    Node& node = nodes[node_index];
    assert(!node.used);

    if (node.bin_previous != unused_node)
    {
        nodes[node.bin_previous].bin_next = node.bin_next;
        if (node.bin_next != unused_node)
            nodes[node.bin_next].bin_previous = node.bin_previous;
    }
    else
    {
        const std::uint32_t bin_index = size_to_bin_round_down(node.size);
        const std::uint32_t top_bin_index = bin_index / bins_per_leaf;
        const std::uint32_t leaf_bin_index = bin_index % bins_per_leaf;

        assert(bin_indices[bin_index] == node_index);
        bin_indices[bin_index] = node.bin_next;
        if (node.bin_next != unused_node)
            nodes[node.bin_next].bin_previous = unused_node;
        else
        {
            used_leaf_bins[top_bin_index] &= static_cast<std::uint8_t>(~(1u << leaf_bin_index));
            if (used_leaf_bins[top_bin_index] == 0)
                used_top_bins &= ~(1u << top_bin_index);
        }
    }

    node.bin_previous = unused_node;
    node.bin_next = unused_node;
}

HeapAllocator::Range HeapAllocator::RangeAllocator::allocate(std::uint64_t byte_size) noexcept
{
    assert(nodes && byte_size != 0);
    if (!nodes || byte_size == 0 || allocation_count == max_allocations)
        return {};

    const std::uint64_t requested_elements = element_count(byte_size, element_size);
    if (requested_elements > capacity)
        return {};
    const std::uint32_t size = static_cast<std::uint32_t>(requested_elements);
    const std::uint32_t approximate_bin_index = size_to_bin_round_down(size);
    NodeIndex node_index = bin_indices[approximate_bin_index];
    while (node_index != unused_node && nodes[node_index].size < size)
        node_index = nodes[node_index].bin_next;

    const std::uint32_t minimum_bin_index = approximate_bin_index + 1;
    std::uint32_t top_bin_index = minimum_bin_index / bins_per_leaf;
    std::uint32_t leaf_bin_index = invalid_bin;

    if (node_index == unused_node && top_bin_index < top_bin_count && (used_top_bins & (1u << top_bin_index)) != 0)
        leaf_bin_index = find_lowest_set_bit(used_leaf_bins[top_bin_index], minimum_bin_index % bins_per_leaf);

    if (node_index == unused_node && leaf_bin_index == invalid_bin)
    {
        top_bin_index = find_lowest_set_bit(used_top_bins, top_bin_index + 1);
        if (top_bin_index == invalid_bin)
            return {};
        leaf_bin_index = std::countr_zero(static_cast<std::uint32_t>(used_leaf_bins[top_bin_index]));
    }

    if (node_index == unused_node)
        node_index = bin_indices[top_bin_index * bins_per_leaf + leaf_bin_index];
    Node& node = nodes[node_index];
    const std::uint32_t original_size = node.size;
    const NodeIndex original_next_neighbor = node.neighbor_next;
    assert(original_size >= size);
    remove_free_node(node_index);

    node.size = size;
    node.used = true;
    ++allocation_count;

    if (original_size != size)
    {
        const NodeIndex remainder_index = acquire_node();
        Node& remainder = nodes[remainder_index];
        remainder.offset = node.offset + size;
        remainder.size = original_size - size;
        remainder.neighbor_previous = node_index;
        remainder.neighbor_next = original_next_neighbor;
        if (original_next_neighbor != unused_node)
            nodes[original_next_neighbor].neighbor_previous = remainder_index;
        node.neighbor_next = remainder_index;
        insert_free_node(remainder_index);
    }

    return {.offset = node.offset, .metadata = node_index};
}

void HeapAllocator::RangeAllocator::free(NodeIndex metadata) noexcept
{
    const bool valid_metadata = nodes && metadata < node_capacity;
    assert(valid_metadata);
    if (!valid_metadata)
        return;

    Node& node = nodes[metadata];
    assert(node.used);
    if (!node.used)
        return;

    if (node.neighbor_previous != unused_node && !nodes[node.neighbor_previous].used)
    {
        const NodeIndex previous_index = node.neighbor_previous;
        Node& previous = nodes[previous_index];
        remove_free_node(previous_index);
        node.offset = previous.offset;
        node.size += previous.size;
        node.neighbor_previous = previous.neighbor_previous;
        if (first_node == previous_index)
            first_node = metadata;
        release_node(previous_index);
    }

    if (node.neighbor_next != unused_node && !nodes[node.neighbor_next].used)
    {
        const NodeIndex next_index = node.neighbor_next;
        Node& next = nodes[next_index];
        remove_free_node(next_index);
        node.size += next.size;
        node.neighbor_next = next.neighbor_next;
        release_node(next_index);
    }

    if (node.neighbor_previous != unused_node)
        nodes[node.neighbor_previous].neighbor_next = metadata;
    if (node.neighbor_next != unused_node)
        nodes[node.neighbor_next].neighbor_previous = metadata;

    insert_free_node(metadata);
    --allocation_count;
}

void HeapAllocator::RangeAllocator::reset() noexcept
{
    assert(nodes && free_nodes && capacity != 0 && max_allocations != 0);
    allocation_count = 0;
    free_node_count = node_capacity;
    used_top_bins = 0;
    std::memset(used_leaf_bins, 0, sizeof(used_leaf_bins));
    for (std::uint32_t index = 0; index != leaf_bin_count; ++index)
        bin_indices[index] = unused_node;
    for (NodeIndex index = 0; index != node_capacity; ++index)
    {
        nodes[index] = {};
        free_nodes[index] = index;
    }

    first_node = acquire_node();
    nodes[first_node].size = capacity;
    insert_free_node(first_node);
}

HeapAllocator::HeapAllocator(GpuCpuRange<byte> storage, std::uint32_t max_allocations) noexcept
    : storage_(storage), ranges_(validate_storage(storage), max_allocations, alignment)
{
}

GpuCpuRange<byte> HeapAllocator::allocate(std::uint64_t byte_size) noexcept
{
    const Range range = ranges_.allocate(byte_size);
    if (range.offset == unused_node)
        return {};
    const std::uint64_t byte_offset = std::uint64_t{range.offset} * ranges_.element_size;
    return {
        .cpu = offset_pointer(storage_.cpu, byte_offset),
        .gpu = offset_pointer(storage_.gpu, byte_offset),
        .size = byte_size,
    };
}

void HeapAllocator::free_bytes(GpuCpuRange<byte> range) noexcept
{
    const void* base = storage_.gpu ? storage_.gpu : storage_.cpu;
    const void* pointer = storage_.gpu ? range.gpu : range.cpu;
    const std::uintptr_t base_address = reinterpret_cast<std::uintptr_t>(base);
    const std::uintptr_t range_address = reinterpret_cast<std::uintptr_t>(pointer);
    const bool valid_address = pointer && range_address >= base_address;
    const std::uint64_t byte_offset = valid_address ? range_address - base_address : 0;
    const bool valid_range = valid_address && range.size != 0 && byte_offset < storage_.size &&
                             range.size <= storage_.size - byte_offset && byte_offset % ranges_.element_size == 0;
    const bool valid_pointers = valid_range && range.cpu == offset_pointer(storage_.cpu, byte_offset) &&
                                range.gpu == offset_pointer(storage_.gpu, byte_offset);
    assert(valid_pointers);
    if (!valid_pointers)
        return;

    const std::uint32_t offset = static_cast<std::uint32_t>(byte_offset / ranges_.element_size);
    const std::uint32_t size = static_cast<std::uint32_t>(element_count(range.size, ranges_.element_size));
    NodeIndex node_index = ranges_.first_node;
    while (node_index != unused_node && ranges_.nodes[node_index].offset < offset)
        node_index = ranges_.nodes[node_index].neighbor_next;
    const bool found = node_index != unused_node && ranges_.nodes[node_index].used &&
                       ranges_.nodes[node_index].offset == offset && ranges_.nodes[node_index].size == size;
    assert(found);
    if (!found)
        return;
    ranges_.free(node_index);
}

void HeapAllocator::reset() noexcept
{
    ranges_.reset();
}

} // namespace gpu
