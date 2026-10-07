#include <NoGraphicsAPIUtility/texture_allocator.hpp>

#include <assert.h>

namespace gpu
{

TextureAllocator::TextureAllocator(Device* device, const TextureHeap& heap, uint32 max_textures) noexcept
    : device_(device), heap_(heap), ranges_(heap.size, max_textures, get_device_caps(device).texture_heap_alignment)
{
    assert(heap.owner);
}

PlacedTexture TextureAllocator::allocate(CommandBuffer* commands, const TextureDesc& desc) noexcept
{
    const SizeAlign requirements = get_texture_size_align(device_, desc);
    if (!requirements.size) return {};
    const HeapAllocator::Range range = ranges_.allocate(requirements.size);
    if (range.offset == HeapAllocator::unused_node)
        return {};
    Texture* texture = create_texture(commands, desc, heap_, uint64{range.offset} * ranges_.element_size);
    if (!texture)
    {
        ranges_.free(range.token);
        return {};
    }
    return {
        .texture = texture,
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
