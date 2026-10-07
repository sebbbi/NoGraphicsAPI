#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include "shader_abi_shared.h"
#include "shader_code.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static_assert(sizeof(ShaderAbiData) == 80 && sizeof(ShaderAbiRoot) == 96);
static_assert(offsetof(ShaderAbiRoot, vector) == 4 && offsetof(ShaderAbiRoot, color) == 16);
static_assert(offsetof(ShaderAbiRoot, output) == 32 && offsetof(ShaderAbiRoot, matrix) == 40 && offsetof(ShaderAbiRoot, sampled) == 80);

// Vulkan guarantees 4,000 sampler descriptors plus its reserved heap range.
constexpr uint32 sampler_count = 4000;

static bool test_shader_abi(gpu::Device* device)
{
    gpu::Span<byte> code = load_test_shader(NOGRAPHICSAPI_SHADER_ABI_PATH);
    if (!code.data) return false;
    gpu::PSO* pso = gpu::create_compute_pso(device, {.code = {code.data, code.size}, .entry_point = "computeMain",
        .threadgroup_size = {.x = shader_abi_thread_count, .y = 1, .z = 1}});
    free(code.data);
    if (!pso) return false;
    const gpu::TextureDesc desc{.extent = {.x = 2, .y = 1, .z = 1}, .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination};
    const gpu::SizeAlign placement = gpu::get_texture_size_align(device, desc);
    const uint64 stride = (placement.size + placement.align - 1) / placement.align * placement.align;
    const gpu::TextureHeap texture_heap = gpu::create_texture_heap(device, stride * shader_abi_thread_count);
    gpu::Texture* textures[shader_abi_thread_count]{};
    gpu::TextureDescriptorHeap* views = gpu::create_texture_descriptor_heap(device, shader_abi_thread_count);
    gpu::SamplerDescriptorHeap* samplers = gpu::create_sampler_descriptor_heap(device, sampler_count);
    const gpu::GpuHeap upload = gpu::create_gpu_heap(device, 128);
    const gpu::GpuHeap output = gpu::create_gpu_heap(device, 1024, gpu::MemoryType::gpu_only);
    const gpu::GpuHeap readback = gpu::create_gpu_heap(device, 1024, gpu::MemoryType::readback);
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    gpu::CommandBuffer* commands = gpu::begin_commands(pool);
    for (uint32 i = 0; i != shader_abi_thread_count; ++i)
    {
        textures[i] = gpu::create_texture(commands, desc, texture_heap, stride * i);
        if (!textures[i]) return false;
        gpu::write_texture_descriptor(views, i, textures[i], gpu::TextureDescriptorType::sampled);
        gpu::write_sampler_descriptor(samplers, sampler_count - shader_abi_thread_count + i,
            {.min_filter = gpu::Filter::nearest, .mag_filter = gpu::Filter::nearest,
             .address_u = (i & 1) ? gpu::AddressMode::repeat : gpu::AddressMode::clamp_to_edge});
        for (uint32 component = 0; component != 8; ++component) upload.range.cpu[i * 8 + component] = byte(17 * i + component + 1);
    }
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    if (!timeline) return false;
    ShaderAbiRoot root{
        .scalar = 3.0f,
        .vector = {.x = 5.0f, .y = 6.0f, .z = 7.0f},
        .color = {.x = 8.0f, .y = 9.0f, .z = 10.0f, .w = 11.0f},
        .output = reinterpret_cast<ShaderAbiData*>(output.range.gpu),
        .matrix = {.rows = {{.x = 12.0f, .y = 13.0f, .z = 14.0f}, {.x = 15.0f, .y = 16.0f, .z = 17.0f}, {.x = 18.0f, .y = 19.0f, .z = 20.0f}}},
        .sampled = reinterpret_cast<float4*>(output.range.gpu + 512),
        .sampler_base = sampler_count - shader_abi_thread_count,
    };
    for (uint32 i = 0; i != shader_abi_thread_count; ++i)
        gpu::copy_memory_to_texture(commands, {.gpu = upload.range.gpu + i * 8, .size = 8}, textures[i]);
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::compute, gpu::Access::shader_read);
    gpu::set_texture_descriptor_heap(commands, views);
    gpu::set_sampler_descriptor_heap(commands, samplers);
    gpu::bind_pso(commands, pso);
    memcpy(upload.range.cpu + 32, &root, sizeof(root));
    gpu::dispatch(commands, upload.range.gpu + 32, {.x = 1, .y = 1, .z = 1});
    gpu::barrier(commands, gpu::Stage::compute, gpu::Access::shader_write, gpu::Stage::transfer, gpu::Access::transfer_read);
    gpu::copy_memory(commands, gpu::gpu_range(output), gpu::gpu_range(readback));
    gpu::end_commands(commands);
    gpu::submit(device, {.commands = {commands}, .completion = {.semaphore = timeline, .value = 1}});
    gpu::wait_timeline({.semaphore = timeline, .value = 1});
    bool valid = true;
    const ShaderAbiData* values = reinterpret_cast<const ShaderAbiData*>(readback.range.cpu);
    const float4* sampled = reinterpret_cast<const float4*>(readback.range.cpu + 512);
    for (uint32 i = 0; i != shader_abi_thread_count; ++i)
    {
        valid &= values[i].scalar == root.scalar && memcmp(&values[i].vector, &root.vector, sizeof(root.vector)) == 0;
        valid &= memcmp(&values[i].color, &root.color, sizeof(root.color)) == 0 && memcmp(&values[i].matrix, &root.matrix, sizeof(root.matrix)) == 0;
        for (uint32 row = 0; row != 3; ++row)
            valid &= values[i].transformed[row] == root.matrix[row][0] * 2 + root.matrix[row][1] * 3 + root.matrix[row][2] * 5;
        for (uint32 component = 0; component != 4; ++component)
            valid &= fabsf(sampled[i][component] - float(upload.range.cpu[i * 8 + ((i & 1) ? 0 : 4) + component]) / 255.0f) < 0.00001f;
    }
    gpu::destroy_command_pool(pool);
    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(output);
    gpu::destroy_gpu_heap(upload);
    gpu::destroy_sampler_descriptor_heap(samplers);
    gpu::destroy_texture_descriptor_heap(views);
    for (gpu::Texture* texture : textures) gpu::destroy_texture(texture);
    gpu::destroy_texture_heap(texture_heap);
    gpu::destroy_pso(pso);
    return valid;
}

int main()
{
    const gpu::DeviceInit initialized = gpu::create_device();
    if (initialized.error == gpu::Error::unsupported) return 77;
    if (initialized.error != gpu::Error::none) return 1;
    const bool valid = test_shader_abi(initialized.device);
    gpu::wait_idle(initialized.device);
    gpu::destroy_device(initialized.device);
    printf("%s: shared shader root layout, matrices, GPU pointers, divergent texture slots and sampler indices %u..%u\n",
        valid ? "PASS" : "FAIL", sampler_count - shader_abi_thread_count, sampler_count - 1);
    return valid ? 0 : 1;
}
