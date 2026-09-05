#pragma once

#include <NoGraphicsAPIUtility/heap_allocator.hpp>

#include <cstdint>
#include <limits>

namespace gpu
{

struct PlacedTexture
{
    Texture* texture = nullptr;

    PlacedTexture() noexcept = default;
    PlacedTexture(PlacedTexture&& other) noexcept;
    PlacedTexture& operator=(PlacedTexture&& other) noexcept;

    PlacedTexture(const PlacedTexture&) = delete;
    PlacedTexture& operator=(const PlacedTexture&) = delete;

private:
    std::uint32_t allocation_ = std::numeric_limits<std::uint32_t>::max();

    friend class TextureAllocator;
};

class TextureAllocator
{
public:
    TextureAllocator(Device* device, const TextureHeap& heap, std::uint32_t max_textures) noexcept;

    TextureAllocator(const TextureAllocator&) = delete;
    TextureAllocator& operator=(const TextureAllocator&) = delete;
    TextureAllocator(TextureAllocator&&) = delete;
    TextureAllocator& operator=(TextureAllocator&&) = delete;

    // An empty result reports exhausted heap space. Free with the same allocator after all views and GPU use have finished.
    [[nodiscard]] PlacedTexture allocate(const TextureDesc& desc) noexcept;
    void free(PlacedTexture& texture) noexcept;

private:
    Device* device_ = nullptr;
    TextureHeap heap_{};
    HeapAllocator::RangeAllocator ranges_;
};

} // namespace gpu
