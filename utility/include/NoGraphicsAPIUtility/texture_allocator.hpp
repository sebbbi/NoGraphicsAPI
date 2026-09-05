#pragma once

#include <NoGraphicsAPIUtility/heap_allocator.hpp>

namespace gpu
{

struct PlacedTexture
{
    Texture* texture;
    uint32_t token;
};

class TextureAllocator
{
public:
    // max_textures must be 1..HeapAllocator::maximum_allocation_count.
    TextureAllocator(Device* device, const TextureHeap& heap, uint32_t max_textures) noexcept;

    TextureAllocator(const TextureAllocator&) = delete;
    TextureAllocator& operator=(const TextureAllocator&) = delete;
    TextureAllocator(TextureAllocator&&) = delete;
    TextureAllocator& operator=(TextureAllocator&&) = delete;

    // An empty result reports exhausted heap space. Free once with the same allocator after all views and GPU use have finished.
    [[nodiscard]] PlacedTexture allocate(const TextureDesc& desc) noexcept;
    void free(PlacedTexture& texture) noexcept;

private:
    Device* device_ = nullptr;
    TextureHeap heap_{};
    HeapAllocator::RangeAllocator ranges_;
};

} // namespace gpu
