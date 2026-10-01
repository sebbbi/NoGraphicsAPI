#include "shader_code.h"
#include "shaders/metal_barrier_shared.h"
#include <stdio.h>
#include <string.h>

static_assert(sizeof(MetalBarrierRoot) == 48 && offsetof(MetalBarrierRoot, value) == 32);

int main(int argc, const char* const* argv)
{
    const bool timestamps = argc == 2 && strcmp(argv[1], "--timestamps") == 0;
    if (argc != 1 && !timestamps) return 1;
    const gpu::DeviceInit initialized = gpu::create_device({.timestamp_query_count = timestamps ? 2u : 0u});
    if (initialized.error == gpu::Error::unsupported) return 77;
    if (initialized.error != gpu::Error::none) return 1;
    gpu::Device* device = initialized.device;
    const gpu::Span<byte> compute_code = load_test_shader(NOGRAPHICSAPI_METAL_BARRIER_COMPUTE);
    const gpu::Span<byte> graphics_code = load_test_shader(NOGRAPHICSAPI_METAL_BARRIER_GRAPHICS);
    if (!compute_code.data || !graphics_code.data)
    {
        free(compute_code.data);
        free(graphics_code.data);
        gpu::destroy_device(device);
        return 1;
    }
    gpu::PSO* compute = gpu::create_compute_pso(device, {
        .code = {compute_code.data, compute_code.size}, .entry_point = "computeMain",
    });
    gpu::PSO* graphics = gpu::create_graphics_pso(device, {
        .vertex = {.code = {graphics_code.data, graphics_code.size}, .entry_point = "vertexMain"},
        .fragment = {.code = {graphics_code.data, graphics_code.size}, .entry_point = "fragmentMain"},
        .color_targets = {{.format = gpu::Format::rgba8_unorm}},
    });
    gpu::PSO* mesh = gpu::create_mesh_pso(device, {
        .mesh = {.code = {graphics_code.data, graphics_code.size}, .entry_point = "meshMain", .threadgroup_size = {.x = 32, .y = 1, .z = 1}},
        .fragment = {.code = {graphics_code.data, graphics_code.size}, .entry_point = "fragmentMain"},
        .color_targets = {{.format = gpu::Format::rgba8_unorm}},
    });
    free(compute_code.data);
    free(graphics_code.data);
    if (!compute || !graphics || !mesh)
    {
        gpu::destroy_pso(mesh);
        gpu::destroy_pso(graphics);
        gpu::destroy_pso(compute);
        gpu::destroy_device(device);
        return 1;
    }
    const gpu::TextureDesc desc{
        .extent = {.x = 8, .y = 8, .z = 1},
        .usage = gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
    };
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    gpu::CommandBuffer* commands = gpu::begin_commands(pool);
    const gpu::TextureHeap heap = gpu::create_texture_heap(device, gpu::get_texture_size_align(device, desc).size);
    gpu::Texture* color = gpu::create_texture(commands, desc, heap, 0);
    gpu::RenderView* view = gpu::create_render_view(color);
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    const gpu::GpuHeap upload = gpu::create_gpu_heap(device, 64 + 2 * sizeof(MetalBarrierRoot));
    const gpu::GpuHeap data = gpu::create_gpu_heap(device, 64, gpu::MemoryType::gpu_only);
    const gpu::GpuHeap readback = gpu::create_gpu_heap(device, 512, gpu::MemoryType::readback);
    bool valid = true;
    for (uint32 mode = 0; mode < 8; ++mode)
    {
        if (mode)
        {
            gpu::reset_command_pool(pool);
            commands = gpu::begin_commands(pool);
        }
        memset(upload.range.cpu, 0, 64);
        reinterpret_cast<uint32*>(upload.range.cpu)[0] = 71 + mode;
        memset(readback.range.cpu, 0xa5, 512);
        gpu::copy_memory(commands, {.gpu = upload.range.gpu, .size = 64}, gpu::gpu_range(data));
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write,
            gpu::Stage::compute | gpu::Stage::vertex | gpu::Stage::mesh, gpu::Access::shader_read | gpu::Access::shader_write);
        MetalBarrierRoot root{
            .input = reinterpret_cast<uint32*>(data.range.gpu),
            .output = reinterpret_cast<uint32*>(data.range.gpu + 8),
            .indices = reinterpret_cast<uint32*>(data.range.gpu + 20),
            .arguments = reinterpret_cast<uint32*>(data.range.gpu + 32),
            .value = 123 + mode,
            .clipped = mode >> 2,
        };
        memcpy(upload.range.cpu + (root.overwrite ? 112 : 64), &root, sizeof(root));
        gpu::bind_pso(commands, compute);
        gpu::dispatch(commands, upload.range.gpu + (root.overwrite ? 112 : 64), {.x = 1, .y = 1, .z = 1});
        if (timestamps) gpu::write_timestamp(commands, reinterpret_cast<uint64*>(readback.range.cpu + 384));
        gpu::Stage consumers = gpu::Stage::fragment;
        gpu::Access reads = gpu::Access::shader_read;
        if ((mode & 3) == 1) { consumers = consumers | gpu::Stage::index_input; reads = reads | gpu::Access::index_read; }
        if ((mode & 3) == 2) { consumers = consumers | gpu::Stage::indirect; reads = reads | gpu::Access::indirect_read; }
        gpu::barrier(commands, gpu::Stage::compute, gpu::Access::shader_write, consumers, reads);
        gpu::begin_render_pass(commands, {.colors = {{.render_view = view, .load = gpu::LoadOp::clear}}});
        gpu::bind_pso(commands, (mode & 3) == 3 ? mesh : graphics);
        switch (mode & 3)
        {
        case 0: gpu::draw(commands, upload.range.gpu + 64, 3); break;
        case 1: gpu::draw_indexed(commands, upload.range.gpu + 64, {.gpu = root.indices, .size = 12}, gpu::IndexType::uint32, 3); break;
        case 2: gpu::draw_indirect(commands, upload.range.gpu + 64, {.gpu = root.arguments, .size = 16}); break;
        case 3: gpu::draw_meshlets(commands, upload.range.gpu + 64, {.x = 1, .y = 1, .z = 1}); break;
        }
        gpu::end_render_pass(commands);
        // Fragment's execution scope includes earlier geometry reads, including draws that produce no fragments.
        gpu::barrier(commands, gpu::Stage::fragment, gpu::Access::none, gpu::Stage::compute, gpu::Access::shader_write);
        root.value = 211 + mode;
        root.overwrite = 1;
        memcpy(upload.range.cpu + (root.overwrite ? 112 : 64), &root, sizeof(root));
        gpu::bind_pso(commands, compute);
        gpu::dispatch(commands, upload.range.gpu + (root.overwrite ? 112 : 64), {.x = 1, .y = 1, .z = 1});
        gpu::barrier(commands, gpu::Stage::vertex | gpu::Stage::mesh | gpu::Stage::compute | gpu::Stage::color_output,
            gpu::Access::shader_write | gpu::Access::color_write, gpu::Stage::transfer, gpu::Access::transfer_read);
        gpu::copy_texture_to_memory(commands, color, {.gpu = readback.range.gpu, .size = 256});
        gpu::copy_memory(commands, gpu::gpu_range(data), {.gpu = readback.range.gpu + 256, .size = 64});
        if (timestamps) gpu::write_timestamp(commands, reinterpret_cast<uint64*>(readback.range.cpu + 392));
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
        gpu::end_commands(commands);
        gpu::submit(device, {.commands = {commands}, .completion = {.semaphore = timeline, .value = mode + 1}});
        gpu::wait_timeline({.semaphore = timeline, .value = mode + 1});
        gpu::read_timestamps(pool);
        bool passed = true;
        for (uint32 pixel = 0; pixel < 64; ++pixel)
        {
            const byte* rgba = readback.range.cpu + pixel * 4;
            passed &= rgba[0] == (root.clipped ? 0 : 123 + mode) && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255;
        }
        const uint32* result = reinterpret_cast<const uint32*>(readback.range.cpu + 256);
        passed &= result[0] == 211 + mode && result[1] == 123 + mode;
        for (uint32 vertex = 0; vertex < 3; ++vertex) passed &= result[2 + vertex] == 71 + mode;
        for (uint32 guard = 320; guard < 384; ++guard) passed &= readback.range.cpu[guard] == 0xa5;
        if (timestamps && gpu::get_device_caps(device).timestamp_period_ns != 0)
        {
            const uint64* ticks = reinterpret_cast<const uint64*>(readback.range.cpu + 384);
            passed &= ticks[0] != 0 && ticks[1] >= ticks[0] && ticks[1] != 0xa5a5a5a5a5a5a5a5ull;
        }
        else
            for (uint32 guard = 384; guard < 400; ++guard) passed &= readback.range.cpu[guard] == 0xa5;
        if (!passed) fprintf(stderr, "Metal stage barrier case %u failed (timestamps %u): input %u, fragment %u, vertices %u %u %u.\n",
            mode, uint32(timestamps), result[0], result[1], result[2], result[3], result[4]);
        valid &= passed;
    }
    gpu::wait_idle(device);
    gpu::destroy_command_pool(pool);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(data);
    gpu::destroy_gpu_heap(upload);
    gpu::destroy_render_view(view);
    gpu::destroy_texture(color);
    gpu::destroy_texture_heap(heap);
    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_pso(mesh);
    gpu::destroy_pso(graphics);
    gpu::destroy_pso(compute);
    gpu::destroy_device(device);
    printf("Metal stage barriers %s timestamps: %s\n", timestamps ? "with" : "without", valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
