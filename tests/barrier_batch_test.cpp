#include "shader_code.h"
#include "barrier_batch_shared.h"
#include <string.h>

using namespace gpu;

int main()
{
    const Span<byte> code = read_shader(NOGRAPHICSAPI_BARRIER_BATCH_PATH);
    if (!code.data) return 1;
    const DeviceInit initialized = create_device({.timestamp_query_count = 8});
    if (initialized.error != Error::none) { free(code.data); return initialized.error == Error::unsupported ? 77 : 1; }
    Device* device = initialized.device;
    PSO* pso = create_compute_pso(device, {.code = {code.data, code.size}, .entry_point = "computeMain", .threadgroup_size = {.x = 64, .y = 1, .z = 1}});
    free(code.data);
    if (!pso) { destroy_device(device); return 1; }
    const GpuHeap upload = create_gpu_heap(device, 1024);
    const GpuHeap data = create_gpu_heap(device, 512, MemoryType::gpu_only);
    const GpuHeap readback = create_gpu_heap(device, 9 * 256, MemoryType::readback);
    CommandPool* pool = create_command_pool(device);
    TimelinePoint completion{.semaphore = create_timeline_semaphore(device)};
    uint32 initial[64];
    for (uint32 i = 0; i < 64; ++i) initial[i] = i * 17;
    memcpy(upload.range.cpu, initial, sizeof(initial));
    for (uint32 i = 0; i < 8; ++i)
    {
        const BarrierBatchRoot root{.input = reinterpret_cast<uint32*>(data.range.gpu + (i & 1u) * 256),
            .output = reinterpret_cast<uint32*>(data.range.gpu + ((i + 1u) & 1u) * 256), .value = i + 1};
        memcpy(upload.range.cpu + 256 + i * sizeof(root), &root, sizeof(root));
    }
    bool valid = true;
    for (uint32 mode = 0; mode < 4; ++mode)
    {
        reset_command_pool(pool);
        uint64 ticks[8]{};
        CommandBuffer* commands = begin_commands(pool);
        copy_memory(commands, {.gpu = upload.range.gpu, .size = 256}, {.gpu = data.range.gpu, .size = 256});
        if (mode == 3)
        {
            barrier(commands, Stage::transfer, Access::transfer_write, Stage::compute, Access::none);
            barrier(commands, Stage::compute, Access::none, Stage::transfer, Access::transfer_read);
            copy_memory(commands, {.gpu = data.range.gpu, .size = 256}, {.gpu = readback.range.gpu + 8 * 256, .size = 256});
            barrier(commands, Stage::transfer, Access::transfer_read, Stage::compute, Access::shader_write);
        }
        barrier(commands, Stage::transfer, Access::transfer_write, Stage::compute, Access::shader_read);
        for (uint32 i = 0; i < 8; ++i)
        {
            bind_pso(commands, pso);
            dispatch(commands, upload.range.gpu + 256 + i * sizeof(BarrierBatchRoot), {.x = 1, .y = 1, .z = 1});
            barrier(commands, Stage::compute, Access::shader_write, Stage::compute, Access::shader_read);
            if (mode == 1) write_timestamp(commands, ticks + i);
            barrier(commands, Stage::compute, Access::shader_write, Stage::transfer, Access::transfer_read);
            barrier(commands, Stage::compute, Access::shader_write, Stage::compute, Access::shader_read);
            copy_memory(commands, {.gpu = data.range.gpu + ((i + 1u) & 1u) * 256, .size = 256},
                {.gpu = readback.range.gpu + i * 256, .size = 256});
            barrier(commands, Stage::transfer, Access::transfer_read, Stage::compute, Access::shader_write);
            if (mode == 2 && i == 3)
            {
                end_commands(commands);
                submit(device, {.commands = {commands}, .completion = {.semaphore = completion.semaphore, .value = ++completion.value}});
                commands = begin_commands(pool);
            }
        }
        barrier(commands, Stage::transfer, Access::transfer_write, Stage::host, Access::host_read);
        end_commands(commands);
        submit(device, {.commands = {commands}, .completion = {.semaphore = completion.semaphore, .value = ++completion.value}});
        wait_timeline(completion);
        read_timestamps(pool);
        uint32 values[9][64];
        memcpy(values, readback.range.cpu, (mode == 3 ? 9u : 8u) * sizeof(values[0]));
        bool passed = true;
        for (uint32 i = 0; i < 8; ++i)
        {
            for (uint32 lane = 0; lane < 64; ++lane) passed &= values[i][lane] == initial[lane] + (i + 1u) * (i + 2u) / 2u;
            if (mode == 1) passed &= ticks[i] != 0 && (!i || ticks[i] >= ticks[i - 1]);
        }
        if (mode == 3)
            for (uint32 lane = 0; lane < 64; ++lane) passed &= values[8][lane] == initial[lane];
        printf("Batched compute/transfer dependencies, mode %u: %s\n", mode, passed ? "passed" : "failed");
        valid &= passed;
    }
    wait_idle(device);
    destroy_command_pool(pool);
    destroy_timeline_semaphore(completion.semaphore);
    destroy_gpu_heap(readback);
    destroy_gpu_heap(data);
    destroy_gpu_heap(upload);
    destroy_pso(pso);
    destroy_device(device);
    return valid ? 0 : 1;
}
