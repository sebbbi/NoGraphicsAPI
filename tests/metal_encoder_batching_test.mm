#include "shader_code.h"
#include "shaders/metal_barrier_shared.h"
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <string.h>

namespace
{
IMP original_compute = nullptr;
uint32 compute_encoders = 0;

id compute_hook(id commands, SEL selector)
{
    ++compute_encoders;
    return reinterpret_cast<id (*)(id, SEL)>(original_compute)(commands, selector);
}
}

int main()
{
    @autoreleasepool
    {
        const gpu::DeviceInit initialized = gpu::create_device({.timestamp_query_count = 8});
        if (initialized.error == gpu::Error::unsupported) return 77;
        if (initialized.error != gpu::Error::none) return 1;
        gpu::Device* device = initialized.device;
        const gpu::Span<byte> code = read_shader(NOGRAPHICSAPI_METAL_BARRIER_COMPUTE);
        if (!code.data) { gpu::destroy_device(device); return 1; }
        gpu::PSO* compute = gpu::create_compute_pso(device, {.code = {code.data, code.size}, .entry_point = "chainMain"});
        free(code.data);
        if (!compute) { gpu::destroy_device(device); return 1; }

        id<MTLDevice> native_device = MTLCreateSystemDefaultDevice();
        id<MTL4CommandBuffer> native_commands = [native_device newCommandBuffer];
        Method method = class_getInstanceMethod([native_commands class], @selector(computeCommandEncoder));
        original_compute = method_setImplementation(method, reinterpret_cast<IMP>(compute_hook));

        gpu::CommandPool* pool = gpu::create_command_pool(device);
        gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
        const gpu::GpuHeap upload = gpu::create_gpu_heap(device, 4096);
        const gpu::GpuHeap data = gpu::create_gpu_heap(device, 64, gpu::MemoryType::gpu_only);
        const gpu::GpuHeap readback = gpu::create_gpu_heap(device, 512, gpu::MemoryType::readback);
        const gpu::TextureDesc texture_desc{.extent = {.x = 8, .y = 8, .z = 1},
            .usage = gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source};
        const gpu::TextureHeap texture_heap = gpu::create_texture_heap(device, gpu::get_texture_size_align(device, texture_desc).size);
        gpu::Texture* texture = gpu::create_texture(device, texture_desc, texture_heap, 0);
        gpu::RenderView* view = gpu::create_render_view(texture);
        uint64 completed = 0;
        bool valid = true;
        for (uint32 mode = 0; mode < 5; ++mode)
        {
            gpu::reset_command_pool(pool);
            compute_encoders = 0;
            uint64 ticks[8]{};
            const uint32 initial[4]{7, 0, 0, 0};
            memcpy(upload.range.cpu, initial, sizeof(initial));
            for (uint32 i = 0; i < 8; ++i)
            {
                const MetalBarrierRoot root{.input = reinterpret_cast<uint32*>(data.range.gpu + (i & 1u) * 16),
                    .output = reinterpret_cast<uint32*>(data.range.gpu + ((i + 1) & 1u) * 16), .value = i + 1};
                memcpy(upload.range.cpu + 64 + i * sizeof(root), &root, sizeof(root));
            }
            gpu::CommandBuffer* commands = gpu::begin_commands(pool);
            gpu::initialize_texture(commands, texture);
            // Leading and consecutive barriers must not create separate encoders.
            gpu::barrier(commands, gpu::Stage::compute, gpu::Access::shader_read, gpu::Stage::transfer, gpu::Access::transfer_write);
            gpu::copy_memory(commands, {.gpu = upload.range.gpu, .size = sizeof(initial)}, {.gpu = data.range.gpu, .size = sizeof(initial)});
            gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::compute, gpu::Access::shader_read);
            for (uint32 i = 0; i < 8; ++i)
            {
                gpu::bind_pso(commands, compute);
                gpu::dispatch(commands, upload.range.gpu + 64 + i * sizeof(MetalBarrierRoot), {.x = 1, .y = 1, .z = 1});
                if (mode == 1) gpu::write_timestamp(commands, ticks + i);
                if (i == 3 && mode == 2)
                {
                    // The following compute barrier must reach past this unrelated render encoder.
                    gpu::begin_render_pass(commands, {.colors = {{.render_view = view, .load = gpu::LoadOp::clear}}});
                    gpu::end_render_pass(commands);
                }
                gpu::barrier(commands, gpu::Stage::compute, gpu::Access::shader_write, gpu::Stage::compute, gpu::Access::shader_read);
                gpu::barrier(commands, gpu::Stage::compute, gpu::Access::shader_write, gpu::Stage::transfer, gpu::Access::transfer_read);
                gpu::copy_memory(commands, {.gpu = data.range.gpu + ((i + 1) & 1u) * 16, .size = 4},
                    {.gpu = readback.range.gpu + i * 4, .size = 4});
                gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_read, gpu::Stage::compute, gpu::Access::shader_write);
                if (i == 3 && mode == 3)
                {
                    gpu::end_commands(commands);
                    gpu::submit(device, {.commands = {commands}, .completion = {.semaphore = timeline, .value = ++completed}});
                    commands = gpu::begin_commands(pool);
                }
            }
            if (mode == 4)
            {
                for (uint32 i = 0; i < 3; ++i)
                {
                    gpu::barrier(commands, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::color_output, gpu::Access::color_write);
                    gpu::begin_render_pass(commands, {.colors = {{.render_view = view, .load = gpu::LoadOp::clear,
                        .clear = {.x = 1.0f, .w = 1.0f}}}});
                    gpu::end_render_pass(commands);
                }
                gpu::barrier(commands, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::transfer, gpu::Access::transfer_read);
                gpu::copy_texture_to_memory(commands, texture, {.gpu = readback.range.gpu + 256, .size = 256});
            }
            gpu::end_commands(commands);
            gpu::submit(device, {.commands = {commands}, .completion = {.semaphore = timeline, .value = ++completed}});
            gpu::wait_timeline({.semaphore = timeline, .value = completed});
            gpu::read_timestamps(pool);
            bool passed = compute_encoders == (mode == 1 ? 9u : mode >= 2 ? 2u : 1u);
            const uint32* values = reinterpret_cast<const uint32*>(readback.range.cpu);
            for (uint32 i = 0; i < 8; ++i)
            {
                passed &= values[i] == 7 + (i + 1) * (i + 2) / 2;
                if (mode == 1) passed &= ticks[i] != 0 && (!i || ticks[i] >= ticks[i - 1]);
            }
            if (mode == 4)
                for (uint32 i = 0; i < 64; ++i) passed &= values[64 + i] == 0xff0000ffu;
            printf("Metal encoder batching mode %u: %u compute encoders, final value %u: %s\n",
                mode, compute_encoders, values[7], passed ? "passed" : "failed");
            valid &= passed;
        }
        gpu::wait_idle(device);
        method_setImplementation(method, original_compute);
        [native_commands release];
        [native_device release];
        gpu::destroy_render_view(view);
        gpu::destroy_texture(texture);
        gpu::destroy_texture_heap(texture_heap);
        gpu::destroy_gpu_heap(readback);
        gpu::destroy_gpu_heap(data);
        gpu::destroy_gpu_heap(upload);
        gpu::destroy_command_pool(pool);
        gpu::destroy_timeline_semaphore(timeline);
        gpu::destroy_pso(compute);
        gpu::destroy_device(device);
        return valid ? 0 : 1;
    }
}
