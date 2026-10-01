#include "shader_code.h"

int main()
{
    const gpu::DeviceInit initialized = gpu::create_device({.timestamp_query_count = 0});
    if (initialized.error == gpu::Error::unsupported) return 77;
    if (initialized.error != gpu::Error::none) return 1;
    gpu::Device* device = initialized.device;
    const gpu::Span<byte> vertex = load_test_shader(NOGRAPHICSAPI_CONTINUATION_VERTEX_SPV);
    const gpu::Span<byte> fragment = load_test_shader(NOGRAPHICSAPI_CONTINUATION_FRAGMENT_SPV);
    if (!vertex.data || !fragment.data) return 1;
    gpu::PSO* pso = gpu::create_graphics_pso(device, {
        .vertex = {.code = {vertex.data, vertex.size}, .entry_point = "vertexMain"},
        .fragment = {.code = {fragment.data, fragment.size}, .entry_point = "fragmentMain"},
        .color_targets = {{.format = gpu::Format::rgba8_unorm}},
    });
    free(vertex.data);
    free(fragment.data);
    if (!pso) return 1;
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    if (!pool) return 1;
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    if (!timeline) return 1;
    const gpu::GpuHeap upload = gpu::create_gpu_heap(device, 512);
    if (!upload.owner) return 1;
    const gpu::GpuHeap arguments = gpu::create_gpu_heap(device, 512, gpu::MemoryType::gpu_only);
    if (!arguments.owner) return 1;
    const gpu::GpuHeap readback = gpu::create_gpu_heap(device, 64 + 8 * 8 * 4, gpu::MemoryType::readback);
    if (!readback.owner) return 1;
    memset(upload.range.cpu, 0, upload.range.size);
    const uint32 indices[]{99, 0, 1, 2};
    memcpy(upload.range.cpu + 32, indices, sizeof(indices));
    const uint32 indirect[]{3, 1, 0, 0};
    memcpy(upload.range.cpu + 160, indirect, sizeof(indirect));
    const uint32 indexed[]{3, 1, 1, 0, 0};
    memcpy(upload.range.cpu + 288, indexed, sizeof(indexed));
    const gpu::TextureDesc desc{.extent = {.x = 8, .y = 8, .z = 1},
        .usage = gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source};
    const gpu::TextureHeap heap = gpu::create_texture_heap(device, gpu::get_texture_size_align(device, desc).size);
    if (!heap.owner) return 1;
    gpu::CommandBuffer* commands = gpu::begin_commands(pool);
    gpu::Texture* texture = gpu::create_texture(commands, desc, heap, 0);
    if (!texture) return 1;
    gpu::RenderView* view = gpu::create_render_view(texture);
    if (!view) return 1;
    gpu::copy_memory(commands, {.gpu = upload.range.gpu + 32, .size = 288}, {.gpu = arguments.range.gpu + 32, .size = 288});
    gpu::end_commands(commands);
    gpu::submit(device, {.commands = {commands}, .completion = {.semaphore = timeline, .value = 1}});
    gpu::wait_timeline({.semaphore = timeline, .value = 1});
    bool valid = true;
    for (uint32 mode = 0; mode < 6; ++mode)
    {
        memset(readback.range.cpu, 0xa5, readback.range.size);
        gpu::reset_command_pool(pool);
        commands = gpu::begin_commands(pool);
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write | gpu::Access::transfer_read,
            gpu::Stage::index_input | gpu::Stage::indirect | gpu::Stage::color_output,
            gpu::Access::index_read | gpu::Access::indirect_read | gpu::Access::color_write);
        gpu::begin_render_pass(commands, {.colors = {{.render_view = view, .load = gpu::LoadOp::clear}}});
        gpu::bind_pso(commands, pso);
        const gpu::GpuRange index_range{.gpu = arguments.range.gpu + 32, .size = sizeof(indices)};
        switch (mode)
        {
        case 0: gpu::draw(commands, nullptr, 3); break;
        case 1: gpu::draw_indexed(commands, nullptr, index_range, gpu::IndexType::uint32, 3, 1, 1); break;
        case 2: gpu::draw_indirect(commands, nullptr, {.gpu = arguments.range.gpu + 128, .size = 48}, 2, 32); break;
        case 3: gpu::draw_indexed_indirect(commands, nullptr, index_range, gpu::IndexType::uint32,
            {.gpu = arguments.range.gpu + 256, .size = 52}, 2, 32); break;
        case 4: gpu::draw_indirect(commands, nullptr, {.gpu = arguments.range.gpu + 160, .size = sizeof(indirect)}); break;
        case 5: gpu::draw_indexed_indirect(commands, nullptr, index_range, gpu::IndexType::uint32,
            {.gpu = arguments.range.gpu + 288, .size = sizeof(indexed)}); break;
        }
        gpu::end_render_pass(commands);
        gpu::barrier(commands, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::transfer, gpu::Access::transfer_read);
        gpu::copy_texture_to_memory(commands, texture, {.gpu = readback.range.gpu + 64, .size = 8 * 8 * 4});
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
        gpu::end_commands(commands);
        gpu::submit(device, {.commands = {commands}, .completion = {.semaphore = timeline, .value = mode + 2}});
        gpu::wait_timeline({.semaphore = timeline, .value = mode + 2});
        for (uint32 i = 0; i < 64; ++i) valid = readback.range.cpu[i] == 0xa5 && valid;
        for (uint32 i = 64; i < readback.range.size; i += 4)
            valid = readback.range.cpu[i] == 255 && readback.range.cpu[i + 1] == 0 &&
                    readback.range.cpu[i + 2] == 0 && readback.range.cpu[i + 3] == 255 && valid;
    }
    gpu::wait_idle(device);
    gpu::destroy_render_view(view);
    gpu::destroy_texture(texture);
    gpu::destroy_texture_heap(heap);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(arguments);
    gpu::destroy_gpu_heap(upload);
    gpu::destroy_command_pool(pool);
    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_pso(pso);
    gpu::destroy_device(device);
    printf("Indexed/indirect draw offsets, strides and GPU-only arguments: %s\n", valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
