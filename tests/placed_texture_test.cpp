#include <NoGraphicsAPIUtility/texture_allocator.hpp>

#include <cstdint>
#include <type_traits>
#include <utility>

static_assert(!std::is_copy_constructible_v<gpu::PlacedTexture>);
static_assert(std::is_nothrow_move_constructible_v<gpu::PlacedTexture>);
static_assert(!std::is_move_constructible_v<gpu::TextureAllocator>);

namespace
{

constexpr int skipped = 77;

}

int main()
{
    const gpu::DeviceInit device_init = gpu::create_device();
    if (device_init.error == gpu::Error::unsupported)
        return skipped;
    if (device_init.error != gpu::Error::none)
        return 1;

    gpu::Device* device = device_init.device;
    const gpu::TextureDesc desc{};
    const gpu::SizeAlign size_align = gpu::get_texture_size_align(device, desc);
    const std::uint64_t element_size = gpu::get_device_caps(device).texture_heap_alignment;
    if (element_size == 0 || size_align.size == 0 || size_align.align == 0 || element_size < size_align.align || element_size % size_align.align != 0)
    {
        gpu::destroy_device(device);
        return 1;
    }

    const std::uint64_t heap_size = (size_align.size + element_size - 1) / element_size * element_size;
    gpu::TextureHeap texture_heap = gpu::create_texture_heap(device, heap_size);
    gpu::TextureAllocator allocator(device, texture_heap, 1);

    gpu::PlacedTexture texture = allocator.allocate(desc);
    gpu::PlacedTexture exhausted = allocator.allocate(desc);
    if (!texture.texture || exhausted.texture)
    {
        allocator.free(exhausted);
        allocator.free(texture);
        gpu::destroy_texture_heap(texture_heap);
        gpu::destroy_device(device);
        return 1;
    }

    gpu::PlacedTexture moved(std::move(texture));
    if (texture.texture || !moved.texture)
    {
        allocator.free(moved);
        gpu::destroy_texture_heap(texture_heap);
        gpu::destroy_device(device);
        return 1;
    }
    texture = std::move(moved);
    if (moved.texture || !texture.texture)
    {
        allocator.free(texture);
        gpu::destroy_texture_heap(texture_heap);
        gpu::destroy_device(device);
        return 1;
    }

    allocator.free(texture);
    texture = allocator.allocate(desc);
    if (!texture.texture)
    {
        gpu::destroy_texture_heap(texture_heap);
        gpu::destroy_device(device);
        return 1;
    }

    allocator.free(texture);
    gpu::destroy_texture_heap(texture_heap);
    gpu::destroy_device(device);
    return 0;
}
