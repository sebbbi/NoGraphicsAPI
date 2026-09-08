#include <NoGraphicsAPIUtility/texture_allocator.hpp>

#include <assert.h>

namespace gpu
{

TextureAllocator::TextureAllocator(Device* device, const TextureHeap& heap, uint32 max_textures) noexcept
    : device_(device), heap_(heap), ranges_(heap.size, max_textures, get_device_caps(device).texture_heap_alignment)
{
    assert(heap.owner);
}

PlacedTexture TextureAllocator::allocate(const TextureDesc& desc) noexcept
{
    const HeapAllocator::Range range = ranges_.allocate(get_texture_size_align(device_, desc).size);
    if (range.offset == HeapAllocator::unused_node)
        return {};

    return {
        .texture = create_texture(device_, desc, heap_, uint64{range.offset} * ranges_.element_size),
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
