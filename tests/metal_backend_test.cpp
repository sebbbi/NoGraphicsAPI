#include "shader_code.h"

#include <stdio.h>
#include <string.h>

namespace
{

struct Root
{
    uint32* output;
    uint32 texture_index;
    uint32 sampler_index;
    uint32 value;
    uint32 storage_index;
};

bool test_heaps_and_roots(gpu::Device* device, gpu::TimelineSemaphore* timeline)
{
    const gpu::Span<byte> code = load_test_shader(NOGRAPHICSAPI_METAL_HEAP_SHADER);
    if (!code.data) return false;
    gpu::PSO* pso = gpu::create_compute_pso(device, {
        .code = {code.data, code.size},
        .entry_point = "heapTest",
        .threadgroup_size = {.x = 4, .y = 1, .z = 1},
    });
    free(code.data);
    if (!pso) return false;
    const gpu::TextureDesc sampled_desc{
        .extent = {.x = 2, .y = 2, .z = 1},
        .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination,
    };
    const gpu::TextureDesc storage_desc{
        .extent = {.x = 2, .y = 2, .z = 1},
        .format = gpu::Format::rgba32_float,
        .usage = gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source,
    };
    const gpu::SizeAlign sampled_size = gpu::get_texture_size_align(device, sampled_desc);
    const gpu::SizeAlign storage_size = gpu::get_texture_size_align(device, storage_desc);
    const uint64 storage_offset = (sampled_size.size + storage_size.align - 1) / storage_size.align * storage_size.align;
    const uint64 storage_stride = (storage_size.size + storage_size.align - 1) / storage_size.align * storage_size.align;
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    gpu::CommandBuffer* commands = gpu::begin_commands(pool);
    const gpu::TextureHeap heap = gpu::create_texture_heap(device, storage_offset + storage_stride + storage_size.size);
    gpu::Texture* sampled = gpu::create_texture(commands, sampled_desc, heap, 0);
    gpu::Texture* storage = gpu::create_texture(commands, storage_desc, heap, storage_offset);
    gpu::Texture* second_storage = gpu::create_texture(commands, storage_desc, heap, storage_offset + storage_stride);
    gpu::TextureDescriptorHeap* source_views = gpu::create_texture_descriptor_heap(device, 4);
    gpu::TextureDescriptorHeap* views = gpu::create_texture_descriptor_heap(device, 32);
    gpu::SamplerDescriptorHeap* source_samplers = gpu::create_sampler_descriptor_heap(device, 4);
    gpu::SamplerDescriptorHeap* samplers = gpu::create_sampler_descriptor_heap(device, 32);
    gpu::write_texture_descriptor(source_views, 1, sampled, gpu::TextureDescriptorType::sampled);
    gpu::write_texture_descriptor(source_views, 2, storage, gpu::TextureDescriptorType::storage);
    gpu::copy_texture_descriptors(source_views, 1, views, 9, 2);
    gpu::write_texture_descriptor(views, 11, second_storage, gpu::TextureDescriptorType::storage);
    gpu::write_sampler_descriptor(source_samplers, 3, {.min_filter = gpu::Filter::nearest, .mag_filter = gpu::Filter::nearest});
    gpu::copy_sampler_descriptors(source_samplers, 3, samplers, 12, 1);
    gpu::destroy_texture_descriptor_heap(source_views);
    gpu::destroy_sampler_descriptor_heap(source_samplers);

    const gpu::GpuHeap upload = gpu::create_gpu_heap(device, 256);
    const gpu::GpuHeap output = gpu::create_gpu_heap(device, 256, gpu::MemoryType::gpu_only);
    const gpu::GpuHeap readback = gpu::create_gpu_heap(device, 512, gpu::MemoryType::readback);
    const uint8 pixels[16]{17, 51, 119, 255, 17, 51, 119, 255, 17, 51, 119, 255, 17, 51, 119, 255};
    memcpy(upload.range.cpu, pixels, sizeof(pixels));
    const uint32 indirect[3]{1, 1, 1};
    memcpy(upload.range.cpu + 64, indirect, sizeof(indirect));
    memset(readback.range.cpu, 0, readback.range.size);
    gpu::copy_memory_to_texture(commands, {.gpu = upload.range.gpu, .size = sizeof(pixels)}, sampled);
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::compute, gpu::Access::shader_read);
    gpu::set_texture_descriptor_heap(commands, views);
    gpu::set_sampler_descriptor_heap(commands, samplers);
    gpu::bind_pso(commands, pso);
    Root root{.output = reinterpret_cast<uint32*>(output.range.gpu), .texture_index = 9, .sampler_index = 12, .value = 23, .storage_index = 10};
    memcpy(upload.range.cpu + 96, &root, sizeof(root));
    gpu::dispatch(commands, upload.range.gpu + 96, {.x = 1, .y = 1, .z = 1});
    // Disjoint writes need no barrier; each dispatch points to a separate stable root record.
    root.output += 4;
    root.value = 101;
    root.storage_index = 11;
    memcpy(upload.range.cpu + 128, &root, sizeof(root));
    gpu::dispatch_indirect(commands, upload.range.gpu + 128, {.gpu = upload.range.gpu + 64, .size = sizeof(indirect)});
    memset(&root, 0, sizeof(root));
    gpu::barrier(commands, gpu::Stage::compute, gpu::Access::shader_write, gpu::Stage::transfer, gpu::Access::transfer_read);
    gpu::copy_memory(commands, gpu::gpu_range(output), {.gpu = readback.range.gpu, .size = 256});
    gpu::copy_texture_to_memory(commands, storage, {.gpu = readback.range.gpu + 256, .size = 64});
    gpu::copy_texture_to_memory(commands, second_storage, {.gpu = readback.range.gpu + 320, .size = 64});
    gpu::end_commands(commands);
    gpu::submit(device, {.commands = {commands}, .completion = {.semaphore = timeline, .value = 1}});
    gpu::wait_timeline({.semaphore = timeline, .value = 1});
    bool valid = true;
    const uint32* result = reinterpret_cast<const uint32*>(readback.range.cpu);
    const float* stored = reinterpret_cast<const float*>(readback.range.cpu + 256);
    for (uint32 index = 0; index != 4; ++index)
    {
        valid &= result[index] == 40 + index && result[4 + index] == 118 + index;
        valid &= stored[index * 4] > 0.066f && stored[index * 4] < 0.067f && stored[index * 4 + 3] == 1.0f;
        valid &= stored[16 + index * 4] > 0.066f && stored[16 + index * 4] < 0.067f && stored[16 + index * 4 + 3] == 1.0f;
    }
    if (!valid) fprintf(stderr, "Metal heap/root/indirect test failed: %u %u %u %u; %u %u %u %u\n",
                        result[0], result[1], result[2], result[3], result[4], result[5], result[6], result[7]);
    gpu::destroy_command_pool(pool);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(output);
    gpu::destroy_gpu_heap(upload);
    gpu::destroy_sampler_descriptor_heap(samplers);
    gpu::destroy_texture_descriptor_heap(views);
    gpu::destroy_texture(second_storage);
    gpu::destroy_texture(storage);
    gpu::destroy_texture(sampled);
    gpu::destroy_texture_heap(heap);
    gpu::destroy_pso(pso);
    return valid;
}

bool test_draws(gpu::Device* device, gpu::TimelineSemaphore* timeline)
{
    const gpu::TextureDesc desc{
        .extent = {.x = 8, .y = 8, .z = 1},
        .usage = gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
    };
    const gpu::Span<byte> code = load_test_shader(NOGRAPHICSAPI_METAL_DRAW_SHADER);
    if (!code.data) return false;
    gpu::PSO* pso = gpu::create_graphics_pso(device, {
        .vertex = {.code = {code.data, code.size}, .entry_point = "vertexMain"},
        .fragment = {.code = {code.data, code.size}, .entry_point = "fragmentMain"},
        .color_targets = {{.format = gpu::Format::rgba8_unorm}},
    });
    free(code.data);
    if (!pso) return false;
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    gpu::CommandBuffer* commands = gpu::begin_commands(pool);
    const gpu::TextureHeap heap = gpu::create_texture_heap(device, gpu::get_texture_size_align(device, desc).size);
    gpu::Texture* color = gpu::create_texture(commands, desc, heap, 0);
    gpu::RenderView* view = gpu::create_render_view(color);
    const gpu::GpuHeap arguments = gpu::create_gpu_heap(device, 256);
    const gpu::GpuHeap readback = gpu::create_gpu_heap(device, 1024, gpu::MemoryType::readback);
    const uint32 indices[3]{0, 1, 2};
    const uint32 indirect[4]{3, 1, 0, 0};
    const uint32 indexed_indirect[5]{3, 1, 0, 0, 0};
    memcpy(arguments.range.cpu, indices, sizeof(indices));
    memcpy(arguments.range.cpu + 64, indirect, sizeof(indirect));
    memcpy(arguments.range.cpu + 128, indexed_indirect, sizeof(indexed_indirect));
    const gpu::GpuRange index_range{.gpu = arguments.range.gpu, .size = sizeof(indices)};
    for (uint32 mode = 0; mode != 4; ++mode)
    {
        gpu::begin_render_pass(commands, {.colors = {{.render_view = view, .load = gpu::LoadOp::clear}}});
        gpu::bind_pso(commands, pso);
        gpu::ClearColor root{.x = 1.0f, .y = 0.0f, .z = 0.0f, .w = 1.0f};
        memcpy(arguments.range.cpu + 192 + mode * sizeof(root), &root, sizeof(root));
        switch (mode)
        {
        case 0: gpu::draw(commands, arguments.range.gpu + 192 + mode * sizeof(root), 3); break;
        case 1: gpu::draw_indexed(commands, arguments.range.gpu + 192 + mode * sizeof(root), index_range, gpu::IndexType::uint32, 3); break;
        case 2: gpu::draw_indirect(commands, arguments.range.gpu + 192 + mode * sizeof(root),
                                   {.gpu = arguments.range.gpu + 64, .size = sizeof(indirect)}); break;
        case 3: gpu::draw_indexed_indirect(commands, arguments.range.gpu + 192 + mode * sizeof(root), index_range, gpu::IndexType::uint32,
                                         {.gpu = arguments.range.gpu + 128, .size = sizeof(indexed_indirect)}); break;
        }
        memset(&root, 0, sizeof(root));
        gpu::end_render_pass(commands);
        gpu::barrier(commands, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::transfer, gpu::Access::transfer_read);
        gpu::copy_texture_to_memory(commands, color, {.gpu = readback.range.gpu + mode * 256, .size = 256});
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_read, gpu::Stage::color_output, gpu::Access::color_write);
    }
    gpu::end_commands(commands);
    gpu::submit(device, {.commands = {commands}, .completion = {.semaphore = timeline, .value = 2}});
    gpu::wait_timeline({.semaphore = timeline, .value = 2});
    bool valid = true;
    for (uint32 pixel = 0; pixel != 256; ++pixel)
        valid &= readback.range.cpu[pixel * 4] == 255 && readback.range.cpu[pixel * 4 + 1] == 0 && readback.range.cpu[pixel * 4 + 3] == 255;
    if (!valid) fprintf(stderr, "Metal direct/indexed/indirect raster readback failed.\n");
    gpu::destroy_command_pool(pool);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(arguments);
    gpu::destroy_render_view(view);
    gpu::destroy_texture(color);
    gpu::destroy_texture_heap(heap);
    gpu::destroy_pso(pso);
    return valid;
}

bool test_viewports_and_culling(gpu::Device* device, gpu::TimelineSemaphore* timeline)
{
    const gpu::Span<byte> code = load_test_shader(NOGRAPHICSAPI_METAL_DRAW_SHADER);
    if (!code.data) return false;
    const gpu::CullMode culls[]{gpu::CullMode::none, gpu::CullMode::clockwise, gpu::CullMode::counter_clockwise};
    gpu::PSO* pipelines[3]{};
    for (uint32 i = 0; i < 3; ++i)
    {
        pipelines[i] = gpu::create_graphics_pso(device, {
            .vertex = {.code = {code.data, code.size}, .entry_point = "vertexTriangle"},
            .fragment = {.code = {code.data, code.size}, .entry_point = "fragmentMain"},
            .color_targets = {{.format = gpu::Format::rgba8_unorm}},
            .rasterization = {.cull = culls[i]},
        });
        if (!pipelines[i])
        {
            for (gpu::PSO* pipeline : pipelines) gpu::destroy_pso(pipeline);
            free(code.data);
            return false;
        }
    }
    free(code.data);
    struct Case
    {
        const char* name;
        uint32 pipeline = 0;
        bool reversed = false;
        bool offset = false;
        bool negative = false;
        bool scissor = false;
        bool visible = true;
    };
    const Case cases[]{
        {.name = "default viewport"},
        {.name = "offset viewport", .offset = true},
        {.name = "offset viewport and scissor", .offset = true, .scissor = true},
        {.name = "negative viewport and clockwise culling", .pipeline = 1, .offset = true, .negative = true},
        {.name = "clockwise culling", .pipeline = 1, .visible = false},
        {.name = "counter-clockwise culling", .pipeline = 2},
        {.name = "reversed clockwise culling", .pipeline = 1, .reversed = true},
        {.name = "reversed counter-clockwise culling", .pipeline = 2, .reversed = true, .visible = false},
    };
    const gpu::TextureDesc desc{
        .extent = {.x = 8, .y = 8, .z = 1},
        .usage = gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
    };
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    gpu::CommandBuffer* commands = gpu::begin_commands(pool);
    const gpu::TextureHeap heap = gpu::create_texture_heap(device, gpu::get_texture_size_align(device, desc).size);
    gpu::Texture* color = gpu::create_texture(commands, desc, heap, 0);
    gpu::RenderView* view = gpu::create_render_view(color);
    const gpu::GpuHeap indices = gpu::create_gpu_heap(device, 32 + sizeof(gpu::ClearColor));
    *reinterpret_cast<gpu::ClearColor*>(indices.range.cpu + 32) = {.x = 1, .w = 1};
    const uint32 index_data[]{0, 1, 2, 0, 2, 1};
    memcpy(indices.range.cpu, index_data, sizeof(index_data));
    const gpu::GpuHeap readback = gpu::create_gpu_heap(device, 256 * (sizeof(cases) / sizeof(cases[0])), gpu::MemoryType::readback);
    for (uint32 i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        gpu::begin_render_pass(commands, {.colors = {{.render_view = view, .load = gpu::LoadOp::clear}}});
        if (cases[i].offset)
            gpu::set_viewport(commands, {.x = 2, .y = cases[i].negative ? 7.0f : 1.0f, .width = 4, .height = cases[i].negative ? -6.0f : 6.0f});
        if (cases[i].scissor) gpu::set_scissor(commands, {.x = 3, .y = 2, .width = 2, .height = 3});
        gpu::bind_pso(commands, pipelines[cases[i].pipeline]);
        gpu::draw_indexed(commands, indices.range.gpu + 32,
            {.gpu = indices.range.gpu + (cases[i].reversed ? 3 : 0) * sizeof(uint32), .size = 3 * sizeof(uint32)}, gpu::IndexType::uint32, 3);
        gpu::end_render_pass(commands);
        gpu::barrier(commands, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::transfer, gpu::Access::transfer_read);
        gpu::copy_texture_to_memory(commands, color, {.gpu = readback.range.gpu + i * 256, .size = 256});
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_read, gpu::Stage::color_output, gpu::Access::color_write);
    }
    gpu::end_commands(commands);
    gpu::submit(device, {.commands = {commands}, .completion = {.semaphore = timeline, .value = 3}});
    gpu::wait_timeline({.semaphore = timeline, .value = 3});
    bool valid = true;
    for (uint32 i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        for (uint32 y = 0; y < 8; ++y)
            for (uint32 x = 0; x < 8; ++x)
            {
                if (!cases[i].offset && x + y == 7) continue;
                const bool inside = cases[i].offset ? x >= 2 && x < 6 && y >= 1 && y < 7 &&
                    (cases[i].negative ? 6 * x < 4 * y + 7 : 6 * x + 4 * y < 35) : x + y < 7;
                const bool covered = cases[i].visible && inside && (!cases[i].scissor || (x >= 3 && x < 5 && y >= 2 && y < 5));
                const byte* pixel = readback.range.cpu + i * 256 + (y * 8 + x) * 4;
                if (pixel[0] != (covered ? 255 : 0) || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 255)
                {
                    fprintf(stderr, "Metal %s failed at %u,%u: %u %u %u %u.\n", cases[i].name, x, y,
                            pixel[0], pixel[1], pixel[2], pixel[3]);
                    valid = false;
                }
            }
    gpu::destroy_command_pool(pool);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(indices);
    gpu::destroy_render_view(view);
    gpu::destroy_texture(color);
    gpu::destroy_texture_heap(heap);
    for (gpu::PSO* pipeline : pipelines) gpu::destroy_pso(pipeline);
    return valid;
}

bool test_independent_pool_timestamps(gpu::Device* device)
{
    constexpr uint32 pool_count = 40;
    gpu::CommandPool* pools[pool_count]{};
    gpu::CommandBuffer* commands[pool_count]{};
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    uint64 timestamps[pool_count];
    memset(timestamps, 0xff, sizeof(timestamps));
    bool valid = timeline != nullptr;
    for (uint32 i = 0; valid && i < pool_count; ++i)
    {
        pools[i] = gpu::create_command_pool(device);
        commands[i] = gpu::begin_commands(pools[i]);
        valid = commands[i] != nullptr;
        if (!valid) break;
        gpu::write_timestamp(commands[i], timestamps + i);
        gpu::end_commands(commands[i]);
    }
    if (valid)
    {
        gpu::submit(device, {.commands = {commands, pool_count}, .completion = {.semaphore = timeline, .value = 1}});
        gpu::wait_timeline({.semaphore = timeline, .value = 1});
        for (gpu::CommandPool* pool : pools) gpu::read_timestamps(pool);
        for (uint32 i = 0; i < pool_count; ++i)
            valid &= gpu::get_device_caps(device).timestamp_period_ns != 0
                ? timestamps[i] != 0 && timestamps[i] != ~uint64{0} && (!i || timestamps[i] >= timestamps[i - 1]) : timestamps[i] == ~uint64{0};
    }
    gpu::destroy_timeline_semaphore(timeline);
    for (gpu::CommandPool* pool : pools) gpu::destroy_command_pool(pool);
    if (!valid) fprintf(stderr, "Metal independent command-pool timestamp storage failed.\n");
    return valid;
}

}

int main()
{
    const gpu::DeviceInit initialized = gpu::create_device();
    if (initialized.error == gpu::Error::unsupported) return 77;
    if (initialized.error != gpu::Error::none) return 1;
    printf("Metal backend on %s\n", gpu::get_device_caps(initialized.device).device_name);
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(initialized.device);
    const bool valid = test_heaps_and_roots(initialized.device, timeline) && test_draws(initialized.device, timeline) &&
                       test_viewports_and_culling(initialized.device, timeline) &&
                       test_independent_pool_timestamps(initialized.device);
    gpu::wait_idle(initialized.device);
    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_device(initialized.device);
    return valid ? 0 : 1;
}
