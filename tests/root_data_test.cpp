#include "root_data_generate_shared.h"
#include <NoGraphicsAPIUtility/bump_allocator.hpp>
#include "shader_code.h"
#include <stddef.h>

static_assert(sizeof(RootData) == 256 && offsetof(RootData, values) == 16);
static_assert(sizeof(RootDataGenerate) == 32);

static gpu::PSO* load_compute(gpu::Device* device, const char* path, uint32 threads)
{
    gpu::Span<byte> code = load_test_shader(path);
    if (!code.data) return nullptr;
    gpu::PSO* pso = gpu::create_compute_pso(device, {.code = {code.data, code.size}, .entry_point = "computeMain",
        .threadgroup_size = {.x = threads, .y = 1, .z = 1}});
    free(code.data);
    return pso;
}

int main(int argc, char** argv)
{
    gpu::DeviceInit init = gpu::create_device();
    if (init.error == gpu::Error::unsupported) return 77;
    if (init.error != gpu::Error::none) return 1;
    gpu::Device* device = init.device;
    gpu::PSO* consume = load_compute(device, argc > 1 ? argv[1] : NOGRAPHICSAPI_ROOT_DATA_PATH, 64);
    gpu::PSO* generate = load_compute(device, NOGRAPHICSAPI_ROOT_GENERATE_PATH, 1);
    if (!consume || !generate) return 1;
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    gpu::CommandPool* external_pool = gpu::create_command_pool(device);
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    const gpu::GpuHeap cpu_roots = gpu::create_gpu_heap(device, 3 * sizeof(RootData) + 2 * sizeof(RootDataGenerate));
    gpu::BumpAllocator arena(cpu_roots.range);
    const gpu::GpuHeap roots = gpu::create_gpu_heap(device, 2 * sizeof(RootData) + 16, gpu::MemoryType::gpu_only);
    const gpu::GpuHeap output = gpu::create_gpu_heap(device, 6 * 64 * sizeof(uint32), gpu::MemoryType::gpu_only);
    const gpu::GpuHeap readback = gpu::create_gpu_heap(device, output.range.size, gpu::MemoryType::readback);
    bool valid = true;
    for (uint32 round = 0; round < 4; ++round)
    {
        gpu::CommandBuffer* first = gpu::begin_commands(pool);
        gpu::CommandBuffer* second = gpu::begin_commands(pool);
        gpu::bind_pso(first, consume);
        gpu::bind_pso(second, consume);
        RootData root{.output = reinterpret_cast<uint32*>(output.range.gpu)};
        for (uint32 i = 0; i < 3; ++i)
        {
            root.output_index = i * 64;
            root.seed = round * 100 + i;
            for (uint32 j = 0; j < 60; ++j) root.values[j] = root.seed * 7 + j;
            const gpu::GpuCpuRange<RootData> copied = arena.allocate<RootData>();
            *copied.cpu = root;
            gpu::dispatch(i == 1 ? second : first, copied.gpu, {.x = 1, .y = 1, .z = 1});
        }
        memset(&root, 0, sizeof(root));
        gpu::bind_pso(second, generate);
        const gpu::GpuCpuRange<RootDataGenerate> generated = arena.allocate_atomic<RootDataGenerate>();
        *generated.cpu = {.destination = reinterpret_cast<RootData*>(roots.range.gpu),
            .output = reinterpret_cast<uint32*>(output.range.gpu),
            .dispatch_arguments = reinterpret_cast<uint32*>(roots.range.gpu + 2 * sizeof(RootData)), .first_index = 3, .seed = round * 100 + 3};
        gpu::dispatch(second, generated.gpu, {.x = 2, .y = 1, .z = 1});
        gpu::end_commands(first);
        gpu::end_commands(second);
        gpu::CommandBuffer* third = gpu::begin_commands(external_pool);
        gpu::barrier(third, gpu::Stage::compute, gpu::Access::shader_write,
            gpu::Stage::compute | gpu::Stage::indirect, gpu::Access::shader_read | gpu::Access::indirect_read);
        gpu::bind_pso(third, consume);
        gpu::dispatch(third, roots.range.gpu, {.x = 1, .y = 1, .z = 1});
        gpu::dispatch_indirect(third, roots.range.gpu + sizeof(RootData), {.gpu = roots.range.gpu + 2 * sizeof(RootData), .size = 12});
        gpu::barrier(third, gpu::Stage::compute | gpu::Stage::indirect, gpu::Access::shader_read | gpu::Access::indirect_read,
            gpu::Stage::compute, gpu::Access::shader_write);
        const gpu::GpuCpuRange<RootDataGenerate> rewritten = arena.allocate<RootDataGenerate>();
        *rewritten.cpu = *generated.cpu;
        rewritten.cpu->first_index = 5;
        rewritten.cpu->seed = round * 100 + 5;
        gpu::bind_pso(third, generate);
        gpu::dispatch(third, rewritten.gpu, {.x = 1, .y = 1, .z = 1});
        gpu::barrier(third, gpu::Stage::compute, gpu::Access::shader_write, gpu::Stage::compute, gpu::Access::shader_read);
        gpu::bind_pso(third, consume);
        gpu::dispatch(third, roots.range.gpu, {.x = 1, .y = 1, .z = 1});
        gpu::barrier(third, gpu::Stage::compute, gpu::Access::shader_write, gpu::Stage::transfer, gpu::Access::transfer_read);
        gpu::copy_memory(third, gpu::gpu_range(output), gpu::gpu_range(readback));
        gpu::barrier(third, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
        gpu::end_commands(third);
        gpu::submit(device, {.commands = {first, second, third}, .completion = {.semaphore = timeline, .value = round + 1}});
        gpu::wait_timeline({.semaphore = timeline, .value = round + 1});
        const uint32* actual = reinterpret_cast<const uint32*>(readback.range.cpu);
        for (uint32 i = 0; i < 6; ++i)
            for (uint32 lane = 0; lane < 64; ++lane)
            {
                const uint32 seed = round * 100 + i;
                uint32 expected = seed + lane;
                for (uint32 j = 0; j < 60; ++j) expected = (expected * 33u) ^ (seed * 7 + j);
                if (actual[i * 64 + lane] != expected) valid = false;
            }
        gpu::reset_command_pool(pool);
        gpu::reset_command_pool(external_pool);
        arena.reset();
    }
    gpu::wait_idle(device);
    gpu::destroy_command_pool(external_pool);
    gpu::destroy_command_pool(pool);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(output);
    gpu::destroy_gpu_heap(roots);
    gpu::destroy_gpu_heap(cpu_roots);
    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_pso(generate);
    gpu::destroy_pso(consume);
    gpu::destroy_device(device);
    printf("Application root allocation, interleaved recording, arena reset, GPU-written roots and indirect arguments: %s\n", valid ? "PASS" : "FAIL");
    return valid ? 0 : 1;
}
