#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include "queue_family_shared.h"
#include "shader_code.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER) && !defined(NDEBUG)
#include <crtdbg.h>
#endif

using namespace gpu;

static const uint64 slot_bytes = 2048;
static const uint64 data_bytes = queue_test_width * queue_test_height * 4;
static const uint64 timestamp_offset = 3 * slot_bytes;

struct Fixture
{
    Device* device = nullptr;
    uint32 queues[3]{};
    CommandPool* pools[3]{};
    TimelineSemaphore* signals[4]{};
    uint64 value = 1;
    PSO* compute = nullptr;
    GpuHeap upload{};
    GpuHeap data{};
    GpuHeap readback{};
    TextureDescriptorHeap* descriptors = nullptr;
    TextureHeap texture_heap{};
    Texture* textures[2]{};
    RenderView* target = nullptr;
};

static bool initialize(Fixture& fixture) noexcept
{
    Span<byte> code = load_test_shader(NOGRAPHICSAPI_QUEUE_FAMILY_SPV);
    if (!code.data) return false;
    fixture.compute = create_compute_pso(fixture.device, {.code = {code.data, code.size},
        .entry_point = "computeMain", .threadgroup_size = {.x = 8, .y = 8, .z = 1}});
    free(code.data);
    if (!fixture.compute) return false;
    const DeviceCaps& caps = get_device_caps(fixture.device);
    fixture.queues[0] = caps.copy_queue_count ? caps.general_queue_count + caps.compute_queue_count : 0;
    fixture.queues[1] = caps.compute_queue_count ? caps.general_queue_count : 0;
    for (uint32 index = 0; index < 3; ++index) fixture.pools[index] = create_command_pool(fixture.device, fixture.queues[index]);
    for (TimelineSemaphore*& signal : fixture.signals) signal = create_timeline_semaphore(fixture.device);
    fixture.upload = create_gpu_heap(fixture.device, 2 * slot_bytes + sizeof(QueueFamilyRoot));
    fixture.data = create_gpu_heap(fixture.device, 2 * slot_bytes, MemoryType::gpu_only);
    fixture.readback = create_gpu_heap(fixture.device, 4 * slot_bytes, MemoryType::readback);
    fixture.descriptors = create_texture_descriptor_heap(fixture.device, 2);
    const TextureDesc description{
        .extent = {.x = queue_test_width, .y = queue_test_height, .z = 1},
        .usage = TextureUsage::sampled | TextureUsage::storage | TextureUsage::color_attachment |
                 TextureUsage::transfer_source | TextureUsage::transfer_destination,
    };
    const SizeAlign placement = get_texture_size_align(fixture.device, description);
    const uint64 second_offset = (placement.size + placement.align - 1) / placement.align * placement.align;
    fixture.texture_heap = create_texture_heap(fixture.device, second_offset + placement.size);
    CommandBuffer* commands = begin_commands(fixture.pools[0]);
    fixture.textures[0] = create_texture(commands, description, fixture.texture_heap, 0);
    fixture.textures[1] = create_texture(commands, description, fixture.texture_heap, second_offset);
    fixture.target = create_render_view(fixture.textures[1]);
    write_texture_descriptor(fixture.descriptors, 0, fixture.textures[0], TextureDescriptorType::sampled);
    write_texture_descriptor(fixture.descriptors, 1,
                             fixture.textures[1], TextureDescriptorType::storage);
    end_commands(commands);
    submit(fixture.device, {.commands = {commands}, .completion = {.semaphore = fixture.signals[0], .value = fixture.value}}, fixture.queues[0]);
    wait_timeline({.semaphore = fixture.signals[0], .value = fixture.value});
    return true;
}

static bool run_case(Fixture& fixture, uint32 iteration) noexcept
{
    ++fixture.value;
    for (CommandPool* pool : fixture.pools) reset_command_pool(pool);
    for (uint32 index = 0; index < data_bytes / 4; ++index)
    {
        reinterpret_cast<uint32*>(fixture.upload.range.cpu)[index] = iteration * 1000 + index;
        uint8* pixel = fixture.upload.range.cpu + slot_bytes + index * 4;
        pixel[0] = uint8(index + iteration);
        pixel[1] = uint8(index * 3 + iteration);
        pixel[2] = uint8(index * 7 + iteration);
        pixel[3] = 255;
    }
    memset(fixture.readback.range.cpu, 0xa5, size_t(fixture.readback.range.size));
    uint64* timestamp_cpu = reinterpret_cast<uint64*>(fixture.readback.range.cpu + timestamp_offset);
    CommandBuffer* producer = begin_commands(fixture.pools[0]);
    write_timestamp(producer, timestamp_cpu);
    copy_memory(producer, {.gpu = fixture.upload.range.gpu, .size = data_bytes}, {.gpu = fixture.data.range.gpu, .size = data_bytes});
    copy_memory_to_texture(producer, {.gpu = fixture.upload.range.gpu + slot_bytes, .size = data_bytes}, fixture.textures[0]);
    write_timestamp(producer, timestamp_cpu + 1);
    end_commands(producer);

    CommandBuffer* compute = begin_commands(fixture.pools[1]);
    write_timestamp(compute, timestamp_cpu + 2);
    bind_pso(compute, fixture.compute);
    set_texture_descriptor_heap(compute, fixture.descriptors);
    *reinterpret_cast<QueueFamilyRoot*>(fixture.upload.range.cpu + 2 * slot_bytes) = {
        .source = reinterpret_cast<uint32*>(fixture.data.range.gpu),
        .destination = reinterpret_cast<uint32*>(fixture.data.range.gpu + slot_bytes),
        .source_texture = 0,
        .destination_texture = 1,
    };
    dispatch(compute, fixture.upload.range.gpu + 2 * slot_bytes, {.x = (queue_test_width + 7) / 8, .y = (queue_test_height + 7) / 8, .z = 1});
    write_timestamp(compute, timestamp_cpu + 3);
    end_commands(compute);

    CommandBuffer* graphics = begin_commands(fixture.pools[2]);
    write_timestamp(graphics, timestamp_cpu + 4);
    copy_memory(graphics, {.gpu = fixture.data.range.gpu + slot_bytes, .size = data_bytes}, {.gpu = fixture.readback.range.gpu, .size = data_bytes});
    copy_texture_to_memory(graphics, fixture.textures[1], {.gpu = fixture.readback.range.gpu + slot_bytes, .size = data_bytes});
    barrier(graphics, Stage::transfer, Access::transfer_read, Stage::color_output, Access::color_write);
    begin_render_pass(graphics, {.colors = {{.render_view = fixture.target, .load = LoadOp::clear, .clear = {.x = 1.0f, .w = 1.0f}}}});
    end_render_pass(graphics);
    write_timestamp(graphics, timestamp_cpu + 5);
    end_commands(graphics);

    CommandBuffer* readback = begin_commands(fixture.pools[0]);
    write_timestamp(readback, timestamp_cpu + 6);
    copy_texture_to_memory(readback, fixture.textures[1], {.gpu = fixture.readback.range.gpu + 2 * slot_bytes, .size = data_bytes});
    barrier(readback, Stage::transfer, Access::transfer_write, Stage::host, Access::host_read);
    write_timestamp(readback, timestamp_cpu + 7);
    end_commands(readback);

    CommandBuffer* commands[]{producer, compute, graphics};
    const TimelinePoint waits[]{
        {.semaphore = fixture.signals[iteration == 1 ? 0 : 3], .value = fixture.value - 1},
        {.semaphore = fixture.signals[0], .value = fixture.value},
        {.semaphore = fixture.signals[1], .value = fixture.value},
    };
    const SubmitDesc produced{.commands = {commands, 1}, .waits = {waits, 1},
                              .completion = {.semaphore = fixture.signals[0], .value = fixture.value}};
    const SubmitDesc computed{.commands = {commands + 1, 1}, .waits = {waits + 1, 1},
                              .completion = {.semaphore = fixture.signals[1], .value = fixture.value}};
    const SubmitDesc rendered{.commands = {commands + 2, 1}, .waits = {waits + 2, 1},
                              .completion = {.semaphore = fixture.signals[2], .value = fixture.value}};
    // All three families can submit consumers before their producer without blocking the producer's queue.
    if (fixture.queues[0] != 0 && fixture.queues[1] != 0)
    {
        submit(fixture.device, rendered);
        submit(fixture.device, computed, fixture.queues[1]);
        submit(fixture.device, produced, fixture.queues[0]);
    }
    else
    {
        submit(fixture.device, produced, fixture.queues[0]);
        submit(fixture.device, computed, fixture.queues[1]);
        submit(fixture.device, rendered);
    }
    submit(fixture.device, {.commands = {readback}, .waits = {{.semaphore = fixture.signals[2], .value = fixture.value}},
                            .completion = {.semaphore = fixture.signals[3], .value = fixture.value}}, fixture.queues[0]);
    wait_timeline({.semaphore = fixture.signals[3], .value = fixture.value});
    for (CommandPool* pool : fixture.pools) read_timestamps(pool);

    bool valid = true;
    for (uint32 index = 0; index < data_bytes / 4; ++index)
    {
        valid = reinterpret_cast<const uint32*>(fixture.readback.range.cpu)[index] == (iteration * 1000 + index) * 3 + 7 && valid;
        const uint8* input = fixture.upload.range.cpu + slot_bytes + index * 4;
        const uint8* pixel = fixture.readback.range.cpu + slot_bytes + index * 4;
        valid = pixel[0] == input[2] && pixel[1] == input[1] && pixel[2] == input[0] && pixel[3] == input[3] && valid;
        const uint8* cleared = fixture.readback.range.cpu + 2 * slot_bytes + index * 4;
        valid = cleared[0] == 255 && cleared[1] == 0 && cleared[2] == 0 && cleared[3] == 255 && valid;
    }
    const uint64* timestamps = reinterpret_cast<const uint64*>(fixture.readback.range.cpu + timestamp_offset);
    for (uint32 index = 0; index < 8; index += 2)
    {
        valid = valid && (get_device_caps(fixture.device).timestamp_period_ns != 0
            ? timestamps[index] != 0xa5a5a5a5a5a5a5a5ull && timestamps[index + 1] != 0xa5a5a5a5a5a5a5a5ull && timestamps[index + 1] >= timestamps[index]
            : timestamps[index] == 0xa5a5a5a5a5a5a5a5ull && timestamps[index + 1] == 0xa5a5a5a5a5a5a5a5ull);
    }
    for (uint64 index = 0; index < fixture.readback.range.size; ++index)
    {
        if (index < timestamp_offset ? index % slot_bytes < data_bytes : index < timestamp_offset + 8 * sizeof(uint64)) continue;
        valid = fixture.readback.range.cpu[index] == 0xa5 && valid;
    }
    if (!valid) fprintf(stderr, "Queue-family data, texture, timestamp, or guard check failed at iteration %u.\n", iteration);
    return valid;
}

static void shutdown(Fixture& fixture) noexcept
{
    wait_idle(fixture.device);
    for (CommandPool* pool : fixture.pools) destroy_command_pool(pool);
    for (TimelineSemaphore* signal : fixture.signals) destroy_timeline_semaphore(signal);
    destroy_render_view(fixture.target);
    for (Texture* texture : fixture.textures) destroy_texture(texture);
    destroy_texture_heap(fixture.texture_heap);
    destroy_texture_descriptor_heap(fixture.descriptors);
    destroy_gpu_heap(fixture.readback);
    destroy_gpu_heap(fixture.data);
    destroy_gpu_heap(fixture.upload);
    destroy_pso(fixture.compute);
    destroy_device(fixture.device);
}

int main(int argc, char** argv)
{
#if defined(_MSC_VER) && !defined(NDEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    const bool compute = argc == 1 || strcmp(argv[1], "--compute") == 0;
    const bool copy = argc == 1 || strcmp(argv[1], "--copy") == 0;
    const DeviceInit initialized = create_device({.desired_compute_queue_count = compute ? 32u : 0u,
                                                 .desired_copy_queue_count = copy ? 32u : 0u, .timestamp_query_count = 2});
    if (initialized.error != Error::none)
    {
        fprintf(stderr, "Dedicated queue device creation: %u.\n", uint32(initialized.error));
        return initialized.error == Error::unsupported ? 77 : 1;
    }
    Fixture fixture{.device = initialized.device};
    const DeviceCaps& caps = get_device_caps(fixture.device);
    printf("%s: %u general, %u compute, %u copy queues; copy granularity %u x %u x %u.\n", caps.device_name,
           caps.general_queue_count, caps.compute_queue_count, caps.copy_queue_count,
           caps.copy_texture_granularity.x, caps.copy_texture_granularity.y, caps.copy_texture_granularity.z);
    bool valid = caps.general_queue_count == 1 && caps.queue_count == 1 + caps.compute_queue_count + caps.copy_queue_count &&
                 (compute ? caps.compute_queue_count >= 1 && caps.compute_queue_count <= 32 : caps.compute_queue_count == 0) &&
                 (copy ? caps.copy_queue_count >= 1 && caps.copy_queue_count <= 32 : caps.copy_queue_count == 0);
    valid = initialize(fixture) && valid;
    if (valid)
        for (uint32 iteration = 1; iteration <= 8; ++iteration) valid = run_case(fixture, iteration) && valid;
    shutdown(fixture);
    return valid ? 0 : 1;
}
