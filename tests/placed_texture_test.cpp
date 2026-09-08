#include <NoGraphicsAPIUtility/delete_queue.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>

static_assert(__is_aggregate(gpu::PlacedTexture));
static_assert(__is_standard_layout(gpu::PlacedTexture));
static_assert(__is_trivial(gpu::PlacedTexture));
static_assert(__is_trivially_copyable(gpu::PlacedTexture));
static_assert(!__is_constructible(gpu::TextureAllocator, gpu::TextureAllocator&&));

namespace
{

constexpr int skipped = 77;

struct DeferredTextureFree
{
    gpu::TextureAllocator* allocator = nullptr;
    gpu::PlacedTexture* texture = nullptr;

    void operator()() noexcept
    {
        allocator->free(*texture);
    }
};

struct DeferredCount
{
    uint32* count = nullptr;
    uint32 increment = 1;

    void operator()() noexcept
    {
        *count += increment;
    }
};

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
    const uint64 element_size = gpu::get_device_caps(device).texture_heap_alignment;
    if (element_size == 0 || size_align.size == 0 || size_align.align == 0 || element_size < size_align.align || element_size % size_align.align != 0)
    {
        gpu::destroy_device(device);
        return 1;
    }

    const uint64 heap_size = (size_align.size + element_size - 1) / element_size * element_size;
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

    allocator.free(texture);
    texture = allocator.allocate(desc);
    if (!texture.texture)
    {
        gpu::destroy_texture_heap(texture_heap);
        gpu::destroy_device(device);
        return 1;
    }

    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    bool valid = true;
    uint32 callback_count = 0;
    {
        gpu::DeleteQueue delete_queue(timeline, 2);
        gpu::CommandBuffer* commands = gpu::begin_commands(device);
        delete_queue.defer(1, DeferredTextureFree{.allocator = &allocator, .texture = &texture});
        delete_queue.defer(~uint64{0}, DeferredCount{.count = &callback_count});

        delete_queue.tick();
        exhausted = allocator.allocate(desc);
        if (exhausted.texture)
        {
            valid = false;
            allocator.free(exhausted);
        }

        gpu::submit({commands}, {.semaphore = timeline, .value = 1});
        gpu::wait_timeline({.semaphore = timeline, .value = 1});
        exhausted = allocator.allocate(desc);
        if (exhausted.texture)
        {
            valid = false;
            allocator.free(exhausted);
        }

        delete_queue.tick();
        texture = allocator.allocate(desc);
        valid &= texture.texture != nullptr && callback_count == 0;

        commands = gpu::begin_commands(device);
        gpu::submit({commands}, {.semaphore = timeline, .value = 2});
        gpu::wait_idle(device);
        delete_queue.drain();
        delete_queue.defer(2, DeferredCount{.count = &callback_count});
        delete_queue.defer(2, DeferredCount{.count = &callback_count});
        delete_queue.tick();
        valid &= callback_count == 3;
        allocator.free(texture);
    }

    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_texture_heap(texture_heap);
    gpu::destroy_device(device);
    return valid ? 0 : 1;
}
