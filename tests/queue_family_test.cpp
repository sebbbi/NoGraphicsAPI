#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include "queue_family_shared.h"

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
    GpuHeap descriptors{};
    TextureHeap texture_heap{};
    Texture* textures[2]{};
    RenderView* target = nullptr;
    bool copy_timestamps = false;
};

static bool initialize(Fixture& fixture) noexcept
{
    FILE* file = fopen(NOGRAPHICSAPI_QUEUE_FAMILY_SPV, "rb");
    if (!file) { fprintf(stderr, "Cannot open queue-family test shader.\n"); return false; }
    uint32 code[8192]{};
    const size_t size = fread(code, 1, sizeof(code), file);
    const bool valid = size >= 20 && size % sizeof(uint32) == 0 && !ferror(file) && feof(file) && code[0] == 0x07230203u;
    fclose(file);
    if (!valid) { fprintf(stderr, "Invalid queue-family test shader.\n"); return false; }
    fixture.compute = create_compute_pso(fixture.device, {code, size / sizeof(uint32)});
    const DeviceCaps& caps = get_device_caps(fixture.device);
    fixture.queues[0] = caps.copy_queue_count ? caps.general_queue_count + caps.compute_queue_count : 0;
    fixture.queues[1] = caps.compute_queue_count ? caps.general_queue_count : 0;
    for (uint32 index = 0; index < 3; ++index) fixture.pools[index] = create_command_pool(fixture.device, fixture.queues[index]);
    for (TimelineSemaphore*& signal : fixture.signals) signal = create_timeline_semaphore(fixture.device);
    fixture.upload = create_gpu_heap(fixture.device, 2 * slot_bytes);
    fixture.data = create_gpu_heap(fixture.device, 2 * slot_bytes, MemoryType::gpu_only);
    fixture.readback = create_gpu_heap(fixture.device, 4 * slot_bytes, MemoryType::readback);
    fixture.descriptors = create_gpu_heap(fixture.device, 2 * caps.texture_descriptor_size, MemoryType::texture_descriptor_heap);
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
    write_texture_descriptor(fixture.device, fixture.descriptors.range.cpu, fixture.textures[0], TextureDescriptorType::sampled);
    write_texture_descriptor(fixture.device, fixture.descriptors.range.cpu + caps.texture_descriptor_size,
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
    uint64* timestamp_gpu = reinterpret_cast<uint64*>(fixture.readback.range.gpu + timestamp_offset);
    // NVIDIA 596.99 loses the device when resolving copy-queue timestamps; --copy-timestamps is the explicit reproducer.
    const bool copy_timestamps = fixture.queues[0] == 0 || fixture.copy_timestamps;
    CommandBuffer* producer = begin_commands(fixture.pools[0]);
    if (copy_timestamps) write_timestamp(producer, timestamp_gpu);
    copy_memory(producer, {.gpu = fixture.upload.range.gpu, .size = data_bytes}, {.gpu = fixture.data.range.gpu, .size = data_bytes});
    copy_memory_to_texture(producer, {.gpu = fixture.upload.range.gpu + slot_bytes, .size = data_bytes}, fixture.textures[0]);
    if (copy_timestamps) write_timestamp(producer, timestamp_gpu + 1);
    end_commands(producer);

    CommandBuffer* compute = begin_commands(fixture.pools[1]);
    write_timestamp(compute, timestamp_gpu + 2);
    bind_pso(compute, fixture.compute);
    set_texture_descriptor_heap(compute, gpu_range(fixture.descriptors));
    dispatch(compute, QueueFamilyRoot{
        .source = reinterpret_cast<uint32*>(fixture.data.range.gpu),
        .destination = reinterpret_cast<uint32*>(fixture.data.range.gpu + slot_bytes),
        .source_texture = 0,
        .destination_texture = 1,
    }, {.x = (queue_test_width + 7) / 8, .y = (queue_test_height + 7) / 8, .z = 1});
    write_timestamp(compute, timestamp_gpu + 3);
    end_commands(compute);

    CommandBuffer* graphics = begin_commands(fixture.pools[2]);
    write_timestamp(graphics, timestamp_gpu + 4);
    copy_memory(graphics, {.gpu = fixture.data.range.gpu + slot_bytes, .size = data_bytes}, {.gpu = fixture.readback.range.gpu, .size = data_bytes});
    copy_texture_to_memory(graphics, fixture.textures[1], {.gpu = fixture.readback.range.gpu + slot_bytes, .size = data_bytes});
    barrier(graphics, Stage::transfer, Access::transfer_read, Stage::color_output, Access::color_write);
    begin_render_pass(graphics, {.colors = {{.render_view = fixture.target, .load = LoadOp::clear, .clear = {.x = 1.0f, .w = 1.0f}}}});
    end_render_pass(graphics);
    write_timestamp(graphics, timestamp_gpu + 5);
    end_commands(graphics);

    CommandBuffer* readback = begin_commands(fixture.pools[0]);
    if (copy_timestamps) write_timestamp(readback, timestamp_gpu + 6);
    copy_texture_to_memory(readback, fixture.textures[1], {.gpu = fixture.readback.range.gpu + 2 * slot_bytes, .size = data_bytes});
    barrier(readback, Stage::transfer, Access::transfer_write, Stage::host, Access::host_read);
    if (copy_timestamps) write_timestamp(readback, timestamp_gpu + 7);
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
        if (!copy_timestamps && (index == 0 || index == 6)) continue;
        valid = timestamps[index] != 0xa5a5a5a5a5a5a5a5ull && timestamps[index + 1] != 0xa5a5a5a5a5a5a5a5ull &&
                timestamps[index + 1] >= timestamps[index] && valid;
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
    destroy_gpu_heap(fixture.descriptors);
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
    const bool copy_timestamps = argc == 2 && strcmp(argv[1], "--copy-timestamps") == 0;
    const bool copy = argc == 1 || strcmp(argv[1], "--copy") == 0 || copy_timestamps;
    const DeviceInit initialized = create_device({.desired_compute_queue_count = compute ? 32u : 0u,
                                                 .desired_copy_queue_count = copy ? 32u : 0u, .timestamp_query_count = 2});
    if (initialized.error != Error::none)
    {
        fprintf(stderr, "Dedicated queue device creation: %u.\n", uint32(initialized.error));
        return initialized.error == Error::unsupported ? 77 : 1;
    }
    Fixture fixture{.device = initialized.device, .copy_timestamps = copy_timestamps};
    const DeviceCaps& caps = get_device_caps(fixture.device);
    printf("%s: %u general, %u compute, %u copy queues; copy granularity %u x %u x %u.\n", caps.device_name,
           caps.general_queue_count, caps.compute_queue_count, caps.copy_queue_count,
           caps.copy_texture_granularity.x, caps.copy_texture_granularity.y, caps.copy_texture_granularity.z);
    if (copy && !copy_timestamps) printf("Copy-queue timestamps omitted: see docs/known-driver-issues.md and --copy-timestamps.\n");
    bool valid = caps.general_queue_count == 1 && caps.queue_count == 1 + caps.compute_queue_count + caps.copy_queue_count &&
                 (compute ? caps.compute_queue_count >= 1 && caps.compute_queue_count <= 32 : caps.compute_queue_count == 0) &&
                 (copy ? caps.copy_queue_count >= 1 && caps.copy_queue_count <= 32 : caps.copy_queue_count == 0);
    valid = initialize(fixture) && valid;
    if (valid)
        for (uint32 iteration = 1; iteration <= 8; ++iteration) valid = run_case(fixture, iteration) && valid;
    shutdown(fixture);
    return valid ? 0 : 1;
}
