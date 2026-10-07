#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <stdio.h>

using namespace gpu;

namespace
{
constexpr uint32 width = 256;
constexpr uint32 height = 192;
constexpr uint32 passes = 8;
constexpr uint64 readback_stride = uint64(width) * height * 8;

void record_alias(CommandBuffer* commands, Texture* const* textures, RenderView* const* views, const GpuHeap& readback, uint32 pass) noexcept
{
    const uint32 mode = pass & 1u;
    const bool alternate = (pass & 2u) != 0;
    activate_texture_alias(commands, textures[mode]);
    begin_render_pass(commands, {.colors = {{.render_view = views[mode], .load = LoadOp::clear,
        .clear = mode ? ClearColor{.x = alternate ? 0.5f : 2.5f, .y = alternate ? 1.0f : 12.5f, .z = 0.5f, .w = 1.0f}
                      : ClearColor{.x = alternate ? 0.0f : 1.0f, .y = alternate ? 1.0f : 0.0f, .z = 1.0f, .w = 1.0f}}}});
    end_render_pass(commands);
    barrier(commands, Stage::color_output, Access::color_write, Stage::transfer, Access::transfer_read);
    copy_texture_to_memory(commands, textures[mode], {.gpu = readback.range.gpu + pass * readback_stride, .size = readback_stride},
        {.extent = {.x = width, .y = height, .z = 1}});
}

bool check_alias(const GpuHeap& readback, uint32 pass) noexcept
{
    const bool alternate = (pass & 2u) != 0;
    const byte* data = readback.range.cpu + pass * readback_stride;
    for (uint32 pixel = 0; pixel < width * height; ++pixel)
    {
        bool valid;
        if (pass & 1u)
        {
            const uint16* color = reinterpret_cast<const uint16*>(data) + pixel * 4;
            valid = color[0] == (alternate ? 0x3800 : 0x4100) && color[1] == (alternate ? 0x3c00 : 0x4a40)
                && color[2] == 0x3800 && color[3] == 0x3c00;
        }
        else
        {
            const byte* color = data + pixel * 4;
            valid = color[0] == 255 && color[1] == (alternate ? 255 : 0) && color[2] == (alternate ? 0 : 255) && color[3] == 255;
        }
        if (!valid)
        {
            fprintf(stderr, "Aliased texture pass %u pixel %u has unexpected color.\n", pass, pixel);
            return false;
        }
    }
    return true;
}
}

int main()
{
    const DeviceInit initialized = create_device({.timestamp_query_count = 0});
    if (initialized.error == Error::unsupported) return 77;
    if (initialized.error != Error::none) return 1;
    Device* device = initialized.device;
    const TextureDesc descriptions[2]{
        {.extent = {.x = width, .y = height, .z = 1}, .format = Format::bgra8_srgb,
            .usage = TextureUsage::sampled | TextureUsage::color_attachment | TextureUsage::transfer_source, .aliasable = true},
        {.extent = {.x = width, .y = height, .z = 1}, .format = Format::rgba16_float,
            .usage = TextureUsage::sampled | TextureUsage::color_attachment | TextureUsage::transfer_source, .aliasable = true},
    };
    const SizeAlign sdr = get_texture_size_align(device, descriptions[0]);
    const SizeAlign hdr = get_texture_size_align(device, descriptions[1]);
    const uint64 offset = sdr.align > hdr.align ? sdr.align : hdr.align;
    const TextureHeap heap = create_texture_heap(device, offset + (sdr.size > hdr.size ? sdr.size : hdr.size));
    const GpuHeap readback = create_gpu_heap(device, passes * readback_stride, MemoryType::readback);
    TextureDescriptorHeap* descriptors = create_texture_descriptor_heap(device, 2);
    CommandPool* pool = create_command_pool(device);
    TimelinePoint completion{.semaphore = create_timeline_semaphore(device)};
    CommandBuffer* commands = begin_commands(pool);
    Texture* textures[2]{};
    RenderView* views[2]{};
    for (uint32 mode = 0; mode < 2; ++mode)
    {
        textures[mode] = create_texture(commands, descriptions[mode], heap, offset);
        views[mode] = create_render_view(textures[mode]);
        write_texture_descriptor(descriptors, mode, textures[mode], TextureDescriptorType::sampled);
    }
    // Alternate without CPU waits first, then repeat handoffs between idle submissions using the same handles and descriptors.
    for (uint32 pass = 0; pass < passes; ++pass) record_alias(commands, textures, views, readback, pass);
    barrier(commands, Stage::transfer, Access::transfer_write, Stage::host, Access::host_read);
    end_commands(commands);
    submit(device, {.commands = {commands}, .completion = {.semaphore = completion.semaphore, .value = ++completion.value}});
    wait_timeline(completion);
    bool valid = true;
    for (uint32 pass = 0; pass < passes; ++pass) valid = check_alias(readback, pass) && valid;
    for (uint32 pass = 0; pass < passes; ++pass)
    {
        wait_idle(device);
        reset_command_pool(pool);
        commands = begin_commands(pool);
        record_alias(commands, textures, views, readback, pass);
        barrier(commands, Stage::transfer, Access::transfer_write, Stage::host, Access::host_read);
        end_commands(commands);
        submit(device, {.commands = {commands}, .completion = {.semaphore = completion.semaphore, .value = ++completion.value}});
        wait_timeline(completion);
        valid = check_alias(readback, pass) && valid;
    }
    wait_idle(device);
    destroy_command_pool(pool);
    destroy_texture_descriptor_heap(descriptors);
    for (uint32 mode = 0; mode < 2; ++mode)
    {
        destroy_render_view(views[mode]);
        destroy_texture(textures[mode]);
    }
    destroy_texture_heap(heap);
    destroy_gpu_heap(readback);
    destroy_timeline_semaphore(completion.semaphore);
    destroy_device(device);
    printf("Resident SDR/HDR texture aliases, batched and idle handoffs: %s\n", valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
