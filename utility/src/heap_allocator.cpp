#include <NoGraphicsAPIUtility/heap_allocator.hpp>

#include <NoGraphicsAPI/bit.hpp>
#include <assert.h>
#include <string.h>

namespace gpu
{

namespace
{

constexpr uint32 mantissa_bits = 3;
constexpr uint32 mantissa_value = 1u << mantissa_bits;
constexpr uint32 mantissa_mask = mantissa_value - 1;
constexpr uint32 invalid_bin = 0xffffffffu;

uint32 size_to_bin_round_down(uint32 size) noexcept
{
    if (size < mantissa_value)
        return size;

    const uint32 mantissa_start_bit = 31u - detail::count_leading_zeros(size) - mantissa_bits;
    return ((mantissa_start_bit + 1) << mantissa_bits) | ((size >> mantissa_start_bit) & mantissa_mask);
}

uint32 find_lowest_set_bit(uint32 bits, uint32 first_bit) noexcept
{
    if (first_bit >= 32)
        return invalid_bin;

    bits &= ~uint32{0} << first_bit;
    return bits == 0 ? invalid_bin : detail::count_trailing_zeros(bits);
}

byte* offset_pointer(byte* pointer, uint64 offset) noexcept
{
    if (!pointer)
        return nullptr;
    return reinterpret_cast<byte*>(reinterpret_cast<uintptr>(pointer) + offset);
}

uint64 validate_storage(GpuCpuRange<byte> storage) noexcept
{
    assert(storage.cpu || storage.gpu);
    assert(storage.size <= HeapAllocator::maximum_size);
    assert((!storage.cpu || reinterpret_cast<uintptr>(storage.cpu) % HeapAllocator::alignment == 0) &&
           (!storage.gpu || reinterpret_cast<uintptr>(storage.gpu) % HeapAllocator::alignment == 0));
    return storage.size;
}

} // namespace

HeapAllocator::RangeAllocator::RangeAllocator(uint64 byte_size, uint32 allocation_limit, uint64 allocation_element_size) noexcept
    : element_size(allocation_element_size)
{
    assert(element_size != 0 && (element_size & (element_size - 1)) == 0 && byte_size >= element_size);
    assert(allocation_limit != 0 && allocation_limit <= maximum_allocation_count);
    const uint64 element_capacity = byte_size / element_size;
    assert(element_capacity <= 0xffffffffu);
    capacity = static_cast<uint32>(element_capacity);
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
    memcpy(used_leaf_bins, other.used_leaf_bins, sizeof(used_leaf_bins));
    memcpy(bin_indices, other.bin_indices, sizeof(bin_indices));
    nodes = other.nodes;
    free_nodes = other.free_nodes;

    other.nodes = nullptr;
    other.free_nodes = nullptr;
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
    const uint32 bin_index = size_to_bin_round_down(node.size);
    const uint32 top_bin_index = bin_index / bins_per_leaf;
    const uint32 leaf_bin_index = bin_index % bins_per_leaf;

    node.used = false;
    node.bin_previous = unused_node;
    node.bin_next = bin_indices[bin_index];
    if (node.bin_next != unused_node)
        nodes[node.bin_next].bin_previous = node_index;
    bin_indices[bin_index] = node_index;
    used_leaf_bins[top_bin_index] |= static_cast<uint8>(1u << leaf_bin_index);
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
        const uint32 bin_index = size_to_bin_round_down(node.size);
        const uint32 top_bin_index = bin_index / bins_per_leaf;
        const uint32 leaf_bin_index = bin_index % bins_per_leaf;

        assert(bin_indices[bin_index] == node_index);
        bin_indices[bin_index] = node.bin_next;
        if (node.bin_next != unused_node)
            nodes[node.bin_next].bin_previous = unused_node;
        else
        {
            used_leaf_bins[top_bin_index] &= static_cast<uint8>(~(1u << leaf_bin_index));
            if (used_leaf_bins[top_bin_index] == 0)
                used_top_bins &= ~(1u << top_bin_index);
        }
    }

    node.bin_previous = unused_node;
    node.bin_next = unused_node;
}

HeapAllocator::Range HeapAllocator::RangeAllocator::allocate(uint64 byte_size) noexcept
{
    assert(byte_size != 0);
    if (!nodes || allocation_count == max_allocations)
        return {};

    const uint64 requested_elements = 1 + (byte_size - 1) / element_size;
    if (requested_elements > capacity)
        return {};
    const uint32 size = static_cast<uint32>(requested_elements);
    const uint32 approximate_bin_index = size_to_bin_round_down(size);
    NodeIndex node_index = bin_indices[approximate_bin_index];
    while (node_index != unused_node && nodes[node_index].size < size)
        node_index = nodes[node_index].bin_next;

    if (node_index == unused_node)
    {
        const uint32 minimum_bin_index = approximate_bin_index + 1;
        uint32 top_bin_index = minimum_bin_index / bins_per_leaf;
        uint32 leaf_bin_index = invalid_bin;
        if (top_bin_index < top_bin_count)
            leaf_bin_index = find_lowest_set_bit(used_leaf_bins[top_bin_index], minimum_bin_index % bins_per_leaf);

        if (leaf_bin_index == invalid_bin)
        {
            top_bin_index = find_lowest_set_bit(used_top_bins, top_bin_index + 1);
            if (top_bin_index == invalid_bin)
                return {};
            leaf_bin_index = detail::count_trailing_zeros(static_cast<uint32>(used_leaf_bins[top_bin_index]));
        }
        node_index = bin_indices[top_bin_index * bins_per_leaf + leaf_bin_index];
    }
    Node& node = nodes[node_index];
    const uint32 original_size = node.size;
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

    return {.offset = node.offset, .token = node_index};
}

void HeapAllocator::RangeAllocator::free(NodeIndex token) noexcept
{
    assert(nodes && token < node_capacity);
    Node& node = nodes[token];
    assert(node.used);

    if (node.neighbor_previous != unused_node && !nodes[node.neighbor_previous].used)
    {
        const NodeIndex previous_index = node.neighbor_previous;
        Node& previous = nodes[previous_index];
        remove_free_node(previous_index);
        node.offset = previous.offset;
        node.size += previous.size;
        node.neighbor_previous = previous.neighbor_previous;
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
        nodes[node.neighbor_previous].neighbor_next = token;
    if (node.neighbor_next != unused_node)
        nodes[node.neighbor_next].neighbor_previous = token;

    insert_free_node(token);
    --allocation_count;
}

void HeapAllocator::RangeAllocator::reset() noexcept
{
    if (!nodes)
        return;
    assert(free_nodes && capacity != 0 && max_allocations != 0);
    allocation_count = 0;
    free_node_count = node_capacity;
    used_top_bins = 0;
    memset(used_leaf_bins, 0, sizeof(used_leaf_bins));
    for (uint32 index = 0; index != leaf_bin_count; ++index)
        bin_indices[index] = unused_node;
    for (NodeIndex index = 0; index != node_capacity; ++index)
    {
        nodes[index] = {};
        free_nodes[index] = index;
    }

    const NodeIndex node_index = acquire_node();
    nodes[node_index].size = capacity;
    insert_free_node(node_index);
}

HeapAllocator::HeapAllocator(GpuCpuRange<byte> storage, uint32 max_allocations) noexcept
    : storage_(storage), ranges_(validate_storage(storage), max_allocations, alignment)
{
}

HeapAllocation<byte> HeapAllocator::allocate(uint64 byte_size) noexcept
{
    const Range allocation = ranges_.allocate(byte_size);
    if (allocation.offset == unused_node)
        return {};
    const uint64 byte_offset = uint64{allocation.offset} * ranges_.element_size;
    return {
        .range = {
            .cpu = offset_pointer(storage_.cpu, byte_offset),
            .gpu = offset_pointer(storage_.gpu, byte_offset),
            .size = byte_size,
        },
        .token = allocation.token,
    };
}

void HeapAllocator::reset() noexcept
{
    ranges_.reset();
}

} // namespace gpu
