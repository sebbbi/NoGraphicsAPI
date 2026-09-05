#include <NoGraphicsAPIUtility/texture_allocator.hpp>

#include <cassert>

namespace
{

std::uint64_t texture_element_size(gpu::Device* device) noexcept
{
    assert(device);
    return gpu::get_device_caps(device).texture_heap_alignment;
}

} // namespace

namespace gpu
{

PlacedTexture::PlacedTexture(PlacedTexture&& other) noexcept
    : texture(other.texture), allocation_(other.allocation_)
{
    other.texture = nullptr;
    other.allocation_ = std::numeric_limits<std::uint32_t>::max();
}

PlacedTexture& PlacedTexture::operator=(PlacedTexture&& other) noexcept
{
    if (this == &other)
        return *this;

    assert(!texture && "move-assigned PlacedTexture must be empty");
    if (texture)
        return *this;
    texture = other.texture;
    allocation_ = other.allocation_;
    other.texture = nullptr;
    other.allocation_ = std::numeric_limits<std::uint32_t>::max();
    return *this;
}

TextureAllocator::TextureAllocator(Device* device, const TextureHeap& heap, std::uint32_t max_textures) noexcept
    : device_(device), heap_(heap), ranges_(heap.size, max_textures, texture_element_size(device))
{
    assert(heap.owner);
}

PlacedTexture TextureAllocator::allocate(const TextureDesc& desc) noexcept
{
    const SizeAlign size_align = get_texture_size_align(device_, desc);
    const bool valid_alignment = size_align.size != 0 && size_align.align != 0 &&
                                 ranges_.element_size >= size_align.align && ranges_.element_size % size_align.align == 0;
    assert(valid_alignment && "texture heap alignment is incompatible with this texture");
    if (!valid_alignment)
        return {};

    const HeapAllocator::Range range = ranges_.allocate(size_align.size);
    if (range.offset == HeapAllocator::unused_node)
        return {};

    PlacedTexture result{};
    result.texture = create_texture(device_, desc, heap_, std::uint64_t{range.offset} * ranges_.element_size);
    result.allocation_ = range.metadata;
    return result;
}

void TextureAllocator::free(PlacedTexture& texture) noexcept
{
    if (!texture.texture)
        return;

    destroy_texture(texture.texture);
    ranges_.free(texture.allocation_);
    texture.texture = nullptr;
    texture.allocation_ = std::numeric_limits<std::uint32_t>::max();
}

} // namespace gpu
