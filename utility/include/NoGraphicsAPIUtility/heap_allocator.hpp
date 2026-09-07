#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

namespace gpu
{

class TextureAllocator;

template<typename T>
struct HeapAllocation
{
    GpuCpuRange<T> range{}; // Size is in bytes.
    uint32 token = 0xffffffffu;
};

class HeapAllocator
{
public:
    static constexpr uint64 alignment = 16;
    static constexpr uint64 maximum_size = uint64{0xffffffffu} * alignment;
    static constexpr uint32 maximum_allocation_count = (0xffffffffu - 1) / 2;

    // Storage must expose at least one address, align every exposed address to 16 bytes, and contain 16..maximum_size bytes.
    // max_allocations must be 1..maximum_allocation_count.
    // A trailing partial element is unused.
    HeapAllocator(GpuCpuRange<byte> storage, uint32 max_allocations) noexcept;
    HeapAllocator(HeapAllocator&&) noexcept = default;
    HeapAllocator& operator=(HeapAllocator&&) noexcept = default;
    ~HeapAllocator() = default;

    HeapAllocator(const HeapAllocator&) = delete;
    HeapAllocator& operator=(const HeapAllocator&) = delete;

    // The request must be nonzero. An allocation with an empty range reports exhausted storage.
    [[nodiscard]] HeapAllocation<byte> allocate(uint64 byte_size) noexcept;

    template<typename T>
    [[nodiscard]] HeapAllocation<T> allocate(uint64 element_count) noexcept
    {
        static_assert(alignof(T) <= alignment);
        const HeapAllocation<byte> allocation = allocate(element_count * sizeof(T));
        return {
            .range = {
                .cpu = reinterpret_cast<T*>(allocation.range.cpu),
                .gpu = reinterpret_cast<T*>(allocation.range.gpu),
                .size = allocation.range.size,
            },
            .token = allocation.token,
        };
    }

    // The value must be an allocation returned by this allocator and not already freed.
    // Reset invalidates every allocation.
    template<typename T>
    void free(HeapAllocation<T> allocation) noexcept
    {
        ranges_.free(allocation.token);
    }

    void reset() noexcept;

private:
    friend class TextureAllocator;

    using NodeIndex = uint32;

    static constexpr NodeIndex unused_node = 0xffffffffu;
    static constexpr uint32 top_bin_count = 32;
    static constexpr uint32 bins_per_leaf = 8;
    static constexpr uint32 leaf_bin_count = top_bin_count * bins_per_leaf;

    struct Range
    {
        uint32 offset = unused_node;
        NodeIndex token = unused_node;
    };

    struct RangeAllocator
    {
        struct Node
        {
            uint32 offset = 0;
            uint32 size = 0;
            NodeIndex bin_previous = unused_node;
            NodeIndex bin_next = unused_node;
            NodeIndex neighbor_previous = unused_node;
            NodeIndex neighbor_next = unused_node;
            bool used = false;
        };

        RangeAllocator(uint64 byte_size, uint32 max_allocations, uint64 element_size) noexcept;
        RangeAllocator(RangeAllocator&& other) noexcept;
        RangeAllocator& operator=(RangeAllocator&& other) noexcept;
        ~RangeAllocator();

        RangeAllocator(const RangeAllocator&) = delete;
        RangeAllocator& operator=(const RangeAllocator&) = delete;

        [[nodiscard]] Range allocate(uint64 byte_size) noexcept;
        void free(NodeIndex token) noexcept;
        void reset() noexcept;

        void move_from(RangeAllocator& other) noexcept;
        [[nodiscard]] NodeIndex acquire_node() noexcept;
        void release_node(NodeIndex node_index) noexcept;
        void insert_free_node(NodeIndex node_index) noexcept;
        void remove_free_node(NodeIndex node_index) noexcept;

        uint64 element_size = 0;
        uint32 capacity = 0;
        uint32 max_allocations = 0;
        uint32 allocation_count = 0;
        uint32 node_capacity = 0;
        uint32 free_node_count = 0;
        uint32 used_top_bins = 0;
        uint8 used_leaf_bins[top_bin_count]{};
        NodeIndex bin_indices[leaf_bin_count]{};
        Node* nodes = nullptr;
        NodeIndex* free_nodes = nullptr;
    };

    GpuCpuRange<byte> storage_{};
    RangeAllocator ranges_;
};

} // namespace gpu
