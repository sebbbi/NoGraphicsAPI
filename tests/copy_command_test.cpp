// Exercises the address-based copy commands end to end and checks the bytes that arrive.
// On a device without VK_KHR_device_address_commands these run through the core copy commands,
// where an address is resolved to a buffer and an offset, so a wrong offset shows up here as
// wrong data rather than as a silent mistake.

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <cstdint>

namespace
{

constexpr int skipped = 77;
constexpr std::uint32_t texture_width = 16;
constexpr std::uint32_t texture_height = 8;
constexpr std::uint64_t texel_count = texture_width * texture_height;
constexpr std::uint64_t texture_bytes = texel_count * 4;

}

int main()
{
    const gpu::DeviceInit device_init = gpu::create_device();
    if (device_init.error == gpu::Error::unsupported)
        return skipped;
    if (device_init.error != gpu::Error::none)
        return 1;

    gpu::Device* device = device_init.device;
    gpu::GpuHeap source = gpu::create_gpu_heap(device, texture_bytes, gpu::MemoryType::cpu_visible);
    gpu::GpuHeap staging = gpu::create_gpu_heap(device, texture_bytes, gpu::MemoryType::cpu_visible);
    gpu::GpuHeap readback = gpu::create_gpu_heap(device, texture_bytes, gpu::MemoryType::readback);
    if (!source.range.cpu || !staging.range.cpu || !readback.range.cpu)
    {
        gpu::destroy_device(device);
        return 1;
    }

    // A pattern where every byte differs from its neighbours, so a copy landing at the wrong
    // offset cannot produce the expected result by accident.
    std::uint8_t* source_bytes = reinterpret_cast<std::uint8_t*>(source.range.cpu);
    for (std::uint64_t index = 0; index < texture_bytes; ++index)
        source_bytes[index] = static_cast<std::uint8_t>(index * 7 + 13);
    std::uint8_t* readback_bytes = reinterpret_cast<std::uint8_t*>(readback.range.cpu);
    for (std::uint64_t index = 0; index < texture_bytes; ++index)
        readback_bytes[index] = 0;

    const gpu::SizeAlign size_align = gpu::get_texture_size_align(device, {
        .extent = {.x = texture_width, .y = texture_height, .z = 1},
        .format = gpu::Format::rgba8_unorm,
        .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_source | gpu::TextureUsage::transfer_destination,
    });
    gpu::TextureHeap texture_heap = gpu::create_texture_heap(device, size_align.size);
    gpu::Texture* texture = gpu::create_texture(device, {
        .extent = {.x = texture_width, .y = texture_height, .z = 1},
        .format = gpu::Format::rgba8_unorm,
        .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_source | gpu::TextureUsage::transfer_destination,
    }, texture_heap, 0);
    if (!texture)
    {
        gpu::destroy_texture_heap(texture_heap);
        gpu::destroy_device(device);
        return 1;
    }

    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    gpu::CommandBuffer* commands = gpu::begin_commands(device);

    // source -> staging, buffer to buffer.
    gpu::copy_memory(commands, gpu_range(source), gpu_range(staging));
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::transfer, gpu::Access::transfer_read);

    // staging -> texture -> readback, buffer to image and back.
    gpu::copy_memory_to_texture(commands, gpu_range(staging), texture, {});
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::transfer, gpu::Access::transfer_read);
    gpu::copy_texture_to_memory(commands, texture, gpu_range(readback), {});
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);

    gpu::submit({commands}, {.semaphore = timeline, .value = 1});
    gpu::wait_timeline({.semaphore = timeline, .value = 1});

    bool valid = true;
    for (std::uint64_t index = 0; index < texture_bytes; ++index)
        valid &= readback_bytes[index] == source_bytes[index];

    gpu::destroy_texture(texture);
    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_texture_heap(texture_heap);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(staging);
    gpu::destroy_gpu_heap(source);
    gpu::destroy_device(device);
    return valid ? 0 : 1;
}
