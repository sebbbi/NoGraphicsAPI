#include <NoGraphicsAPIUtility/texture_allocator.hpp>

#include <cassert>

namespace gpu
{

namespace
{

uint64_t texture_element_size(Device* device) noexcept
{
    assert(device);
    return get_device_caps(device).texture_heap_alignment;
}

} // namespace

TextureAllocator::TextureAllocator(Device* device, const TextureHeap& heap, uint32_t max_textures) noexcept
    : device_(device), heap_(heap), ranges_(heap.size, max_textures, texture_element_size(device))
{
    assert(heap.owner);
}

PlacedTexture TextureAllocator::allocate(const TextureDesc& desc) noexcept
{
    const SizeAlign size_align = get_texture_size_align(device_, desc);
    const HeapAllocator::Range range = ranges_.allocate(size_align.size);
    if (range.offset == HeapAllocator::unused_node)
        return {};

    return {
        .texture = create_texture(device_, desc, heap_, uint64_t{range.offset} * ranges_.element_size),
        .token = range.token,
    };
}

void TextureAllocator::free(PlacedTexture& texture) noexcept
{
    if (!texture.texture)
        return;

    ranges_.free(texture.token);
    destroy_texture(texture.texture);
    texture = {};
}

} // namespace gpu
