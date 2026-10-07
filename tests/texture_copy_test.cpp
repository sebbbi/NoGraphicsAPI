#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <stdio.h>
#include <string.h>
using namespace gpu;

static bool check(Device* device, TimelineSemaphore* timeline, uint64& value, TextureType type, Format format)
{
    const bool compressed = format == Format::bc7_unorm;
    const bool volume = type == TextureType::three_d;
    const TextureDesc desc{.type = type, .extent = {.x = 8, .y = 8, .z = volume ? 4u : 1u}, .layer_count = volume ? 1u : type == TextureType::cube ? 6u : 3u,
        .format = format, .usage = TextureUsage::transfer_source | TextureUsage::transfer_destination};
    const SizeAlign required = get_texture_size_align(device, desc);
    CommandPool* pool = create_command_pool(device);
    CommandBuffer* commands = begin_commands(pool);
    const TextureHeap heap = create_texture_heap(device, required.size);
    Texture* texture = create_texture(commands, desc, heap, 0);
    const GpuHeap upload = create_gpu_heap(device, 4096);
    const GpuHeap readback = create_gpu_heap(device, 4096, MemoryType::readback);
    for (uint32 i = 0; i < 4096; ++i) upload.range.cpu[i] = byte(i * 17 + i / 16);
    memset(readback.range.cpu, 0xa5, 4096);
    const uint32 row_pitch = compressed ? 48 : 16;
    const uint32 slice_pitch = compressed ? 144 : 64;
    const uint32 row_bytes = compressed ? 32 : 3;
    const TextureCopyDesc copy{.base_slice = volume ? 0u : 1u, .slice_count = volume ? 1u : 2u,
        .offset = {.x = compressed ? 0u : 1u, .y = compressed ? 0u : 1u, .z = volume ? 1u : 0u},
        .extent = {.x = compressed ? 8u : 3u, .y = compressed ? 8u : 2u, .z = volume ? 2u : 1u},
        .row_pitch_bytes = row_pitch, .slice_pitch_bytes = slice_pitch};
    copy_memory_to_texture(commands, gpu_range(upload), texture, copy);
    // NVIDIA 596.99 needs a timeline wait before address-based image readback; see docs/known-driver-issues.md.
    end_commands(commands);
    submit(device, {.commands = {commands}, .completion = {.semaphore = timeline, .value = ++value}});
    wait_timeline({.semaphore = timeline, .value = value});
    commands = begin_commands(pool);
    barrier(commands, Stage::transfer, Access::transfer_write, Stage::transfer, Access::transfer_read);
    copy_texture_to_memory(commands, texture, gpu_range(readback), copy);
    end_commands(commands);
    submit(device, {.commands = {commands}, .completion = {.semaphore = timeline, .value = ++value}});
    wait_timeline({.semaphore = timeline, .value = value});
    bool valid = true;
    for (uint32 i = 0; i < 4096; ++i)
    {
        const uint32 slice = i / slice_pitch;
        const uint32 in_slice = i % slice_pitch;
        const bool copied = slice < 2 && in_slice / row_pitch < 2 && in_slice % row_pitch < row_bytes;
        const byte expected = copied ? upload.range.cpu[i] : byte(0xa5);
        if (readback.range.cpu[i] != expected)
        {
            fprintf(stderr, "Texture copy type=%u format=%u at byte %u: got %u, expected %u.\n",
                    uint32(type), uint32(format), i, readback.range.cpu[i], expected);
            valid = false;
            break;
        }
    }
    destroy_command_pool(pool);
    destroy_gpu_heap(upload);
    destroy_gpu_heap(readback);
    destroy_texture(texture);
    destroy_texture_heap(heap);
    return valid;
}
int main()
{
    const DeviceInit initialized = create_device({.timestamp_query_count = 0});
    if (initialized.error == Error::unsupported) return 77;
    if (initialized.error != Error::none) return 1;
    Device* device = initialized.device;
    TimelineSemaphore* timeline = create_timeline_semaphore(device);
    uint64 value = 0;
    const bool valid = check(device, timeline, value, TextureType::three_d, Format::r8_unorm) &&
        check(device, timeline, value, TextureType::two_d_array, Format::r8_unorm) &&
        check(device, timeline, value, TextureType::cube, Format::r8_unorm) &&
        (!supports_texture_format(device, Format::bc7_unorm, TextureUsage::transfer_source | TextureUsage::transfer_destination) ||
         check(device, timeline, value, TextureType::two_d_array, Format::bc7_unorm));
    wait_idle(device);
    destroy_timeline_semaphore(timeline);
    destroy_device(device);
    printf("3D/array/cube/BC7 pitch roundtrips: %s\n", valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
