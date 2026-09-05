#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <cassert>
#include <cstdint>
#include <limits>

namespace gpu
{

class TextureAllocator;

class HeapAllocator
{
public:
    static constexpr std::uint64_t alignment = 16;
    static constexpr std::uint64_t maximum_size = std::uint64_t{std::numeric_limits<std::uint32_t>::max()} * alignment;
    static constexpr std::uint32_t maximum_allocation_count = (std::numeric_limits<std::uint32_t>::max() - 1) / 2;

    // Storage must expose at least one 16-byte-aligned address and contain 16..maximum_size bytes. A trailing partial element is unused.
    HeapAllocator(GpuCpuRange<byte> storage, std::uint32_t max_allocations) noexcept;
    HeapAllocator(HeapAllocator&&) noexcept = default;
    HeapAllocator& operator=(HeapAllocator&&) noexcept = default;
    ~HeapAllocator() = default;

    HeapAllocator(const HeapAllocator&) = delete;
    HeapAllocator& operator=(const HeapAllocator&) = delete;

    // The request must be nonzero. An empty range reports exhausted storage.
    [[nodiscard]] GpuCpuRange<byte> allocate(std::uint64_t byte_size) noexcept;

    template<typename T>
    [[nodiscard]] GpuCpuRange<T> allocate(std::uint64_t element_count) noexcept
    {
        static_assert(alignof(T) <= alignment);
        if (element_count > std::numeric_limits<std::uint64_t>::max() / sizeof(T))
        {
            assert(false && "typed allocation byte size overflows uint64_t");
            return {};
        }
        const GpuCpuRange<byte> allocation = allocate(element_count * sizeof(T));
        return {.cpu = reinterpret_cast<T*>(allocation.cpu), .gpu = reinterpret_cast<T*>(allocation.gpu), .size = allocation.size};
    }

    // The range must be an allocation returned by this allocator and not already freed. Lookup is linear in the current allocation count.
    // Reset invalidates every allocation.
    template<typename T>
    void free(GpuCpuRange<T> range) noexcept
    {
        free_bytes({.cpu = reinterpret_cast<byte*>(range.cpu), .gpu = reinterpret_cast<byte*>(range.gpu), .size = range.size});
    }

    void reset() noexcept;

private:
    friend class TextureAllocator;

    using NodeIndex = std::uint32_t;

    static constexpr NodeIndex unused_node = std::numeric_limits<NodeIndex>::max();
    static constexpr std::uint32_t top_bin_count = 32;
    static constexpr std::uint32_t bins_per_leaf = 8;
    static constexpr std::uint32_t leaf_bin_count = top_bin_count * bins_per_leaf;

    struct Range
    {
        std::uint32_t offset = unused_node;
        NodeIndex metadata = unused_node;
    };

    struct RangeAllocator
    {
        struct Node
        {
            std::uint32_t offset = 0;
            std::uint32_t size = 0;
            NodeIndex bin_previous = unused_node;
            NodeIndex bin_next = unused_node;
            NodeIndex neighbor_previous = unused_node;
            NodeIndex neighbor_next = unused_node;
            bool used = false;
        };

        RangeAllocator(std::uint64_t byte_size, std::uint32_t max_allocations, std::uint64_t element_size) noexcept;
        RangeAllocator(RangeAllocator&& other) noexcept;
        RangeAllocator& operator=(RangeAllocator&& other) noexcept;
        ~RangeAllocator();

        RangeAllocator(const RangeAllocator&) = delete;
        RangeAllocator& operator=(const RangeAllocator&) = delete;

        [[nodiscard]] Range allocate(std::uint64_t byte_size) noexcept;
        void free(NodeIndex metadata) noexcept;
        void reset() noexcept;

        void move_from(RangeAllocator& other) noexcept;
        [[nodiscard]] NodeIndex acquire_node() noexcept;
        void release_node(NodeIndex node_index) noexcept;
        void insert_free_node(NodeIndex node_index) noexcept;
        void remove_free_node(NodeIndex node_index) noexcept;

        std::uint64_t element_size = 0;
        std::uint32_t capacity = 0;
        std::uint32_t max_allocations = 0;
        std::uint32_t allocation_count = 0;
        std::uint32_t node_capacity = 0;
        std::uint32_t free_node_count = 0;
        std::uint32_t used_top_bins = 0;
        std::uint8_t used_leaf_bins[top_bin_count]{};
        NodeIndex bin_indices[leaf_bin_count]{};
        Node* nodes = nullptr;
        NodeIndex* free_nodes = nullptr;
        NodeIndex first_node = unused_node;
    };

    void free_bytes(GpuCpuRange<byte> range) noexcept;

    GpuCpuRange<byte> storage_{};
    RangeAllocator ranges_;
};

} // namespace gpu
