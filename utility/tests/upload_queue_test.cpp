#include <NoGraphicsAPIUtility/texture_upload.hpp>

#if defined(NOGRAPHICSAPI_UPLOAD_QUEUE_SPV_PATH)
#include "../../tests/shaders/upload_queue_shared.h"
#endif

#include <stdio.h>
#include <string.h>

using namespace gpu;

static_assert(!__is_constructible(UploadQueue, const UploadQueue&));
static_assert(!__is_assignable(UploadQueue&, const UploadQueue&));
static_assert(!__is_constructible(UploadQueue));
static_assert(__is_nothrow_constructible(UploadQueue, Device*));
static_assert(__is_nothrow_constructible(UploadQueue, Device*, uint64, uint32));
static_assert(__is_nothrow_constructible(UploadQueue, UploadQueue&&));
static_assert(!detail::is_convertible_v<Device*, UploadQueue>);

static const uint32 buffer_bytes = 20 * 1024;
static const uint32 readback_bytes = 128 * 1024;
static const uint32 guard_bytes = 32;
static const uint32 texture_count = 6;
static const uint32 timestamp_batches = UploadQueue::retirement_capacity + 2;
static uint32 failures = 0;

struct TextureCase
{
    uint32 texture = 0;
    uint32 mip = 0;
    uint32 face = 0;
    uint32 bytes = 0;
    uint64 offset = 0;
};

struct Fixture
{
    Device* device = nullptr;
    CommandPool* pool = nullptr;
    UploadQueue ring;
    TimelinePoint completion{};
    GpuHeap buffer{};
    GpuHeap readback{};
    GpuHeap timestamps{};
    TextureHeap texture_heap{};
    Texture* textures[texture_count]{};
    RenderView* attachments[2]{};
    TextureDesc descriptions[texture_count]{
        {.type = TextureType::three_d, .extent = {.x = 37, .y = 23, .z = 11}, .format = Format::r8_unorm,
            .usage = TextureUsage::transfer_source | TextureUsage::transfer_destination},
        {.type = TextureType::two_d_array, .extent = {.x = 519, .y = 7, .z = 1}, .layer_count = 5, .format = Format::r8_unorm,
            .usage = TextureUsage::transfer_source | TextureUsage::transfer_destination},
        {.extent = {.x = 137, .y = 9, .z = 1}, .mip_levels = 8, .format = Format::bc6h_ufloat,
            .usage = TextureUsage::transfer_source | TextureUsage::transfer_destination},
        {.type = TextureType::cube, .extent = {.x = 9, .y = 9, .z = 1}, .mip_levels = 4, .layer_count = 6, .format = Format::bc6h_ufloat,
            .usage = TextureUsage::transfer_source | TextureUsage::transfer_destination},
        {.extent = {.x = 7, .y = 5, .z = 1}, .format = Format::rgba8_unorm,
            .usage = TextureUsage::color_attachment | TextureUsage::transfer_source | TextureUsage::transfer_destination},
        {.extent = {.x = 7, .y = 5, .z = 1}, .format = Format::d32_float,
            .usage = TextureUsage::depth_stencil_attachment | TextureUsage::transfer_source | TextureUsage::transfer_destination},
    };
    TextureCase cases[64]{};
    uint32 case_count = 0;
    uint8 expected[readback_bytes]{};
    bool unsupported = false;
#if defined(NOGRAPHICSAPI_UPLOAD_QUEUE_SPV_PATH)
    PSO* compute = nullptr;
#endif
};

static void check(bool condition, const char* message)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

static void shutdown(Fixture& fixture)
{
    if (!fixture.device) return;
    fixture.ring.wait();
    wait_idle(fixture.device);
    fixture.ring.destroy();
    destroy_command_pool(fixture.pool);
#if defined(NOGRAPHICSAPI_UPLOAD_QUEUE_SPV_PATH)
    destroy_pso(fixture.compute);
#endif
    for (uint32 index = 0; index < 2; ++index) destroy_render_view(fixture.attachments[index]);
    for (uint32 index = 0; index < texture_count; ++index) destroy_texture(fixture.textures[index]);
    destroy_texture_heap(fixture.texture_heap);
    destroy_gpu_heap(fixture.buffer);
    destroy_gpu_heap(fixture.readback);
    destroy_gpu_heap(fixture.timestamps);
    destroy_timeline_semaphore(fixture.completion.semaphore);
    destroy_device(fixture.device);
}

static bool initialize(Fixture& fixture)
{
    fixture.pool = create_command_pool(fixture.device);
    if (!fixture.pool) return false;
    fixture.completion.semaphore = create_timeline_semaphore(fixture.device);
    if (!fixture.completion.semaphore) return false;
    fixture.buffer = create_gpu_heap(fixture.device, buffer_bytes, MemoryType::gpu_only);
    if (!fixture.buffer.owner) return false;
    fixture.readback = create_gpu_heap(fixture.device, readback_bytes, MemoryType::readback);
    if (!fixture.readback.owner) return false;
    fixture.timestamps = create_gpu_heap(fixture.device, timestamp_batches * 2 * sizeof(uint64) + guard_bytes * 2, MemoryType::readback);
    if (!fixture.timestamps.owner) return false;
#if defined(NOGRAPHICSAPI_UPLOAD_QUEUE_SPV_PATH)
    FILE* shader = fopen(NOGRAPHICSAPI_UPLOAD_QUEUE_SPV_PATH, "rb");
    if (!shader) { fprintf(stderr, "Cannot open upload queue compute shader.\n"); return false; }
    uint32 code[4096]{};
    const size_t shader_size = fread(code, 1, sizeof(code), shader);
    const bool valid_shader = shader_size != 0 && shader_size % sizeof(uint32) == 0 && !ferror(shader) && feof(shader);
    fclose(shader);
    if (!valid_shader) { fprintf(stderr, "Invalid upload queue compute shader.\n"); return false; }
    fixture.compute = create_compute_pso(fixture.device, {code, shader_size / sizeof(uint32)});
    if (!fixture.compute) return false;
#endif
    uint64 heap_bytes = 0;
    uint64 offsets[texture_count]{};
    for (uint32 index = 0; index < texture_count; ++index)
    {
        if (index == 2 || index == 3)
            if (!get_device_caps(fixture.device).texture_compression_bc) fixture.descriptions[index].format = Format::rgba8_unorm;
        if (!supports_texture_format(fixture.device, fixture.descriptions[index].format, fixture.descriptions[index].usage))
        {
            fixture.unsupported = true;
            return false;
        }
        const SizeAlign size = get_texture_size_align(fixture.device, fixture.descriptions[index]);
        if (!size.size) return false;
        offsets[index] = (heap_bytes + size.align - 1) & ~(size.align - 1);
        heap_bytes = offsets[index] + size.size;
    }
    fixture.texture_heap = create_texture_heap(fixture.device, heap_bytes);
    if (!fixture.texture_heap.owner) return false;
    CommandBuffer* initialization = begin_commands(fixture.pool);
    for (uint32 index = 0; index < texture_count; ++index)
    {
        fixture.textures[index] = create_texture(initialization, fixture.descriptions[index], fixture.texture_heap, offsets[index]);
        if (!fixture.textures[index]) return false;
    }
    end_commands(initialization);
    submit(fixture.device, {.commands = {initialization},
        .completion = {.semaphore = fixture.completion.semaphore, .value = ++fixture.completion.value}});
    wait_timeline(fixture.completion);
    reset_command_pool(fixture.pool);
    for (uint32 index = 0; index < 2; ++index)
    {
        fixture.attachments[index] = create_render_view(fixture.textures[index + 4]);
        if (!fixture.attachments[index]) return false;
    }
    memset(fixture.expected, 0xa5, sizeof(fixture.expected));
    uint64 output = buffer_bytes + guard_bytes * 2;
    for (uint32 index = 0; index < texture_count; ++index)
    {
        const TextureDesc& description = fixture.descriptions[index];
        const TextureFormatInfo format = get_texture_format_info(description.format);
        for (uint32 face = 0; face < (description.type == TextureType::cube ? 6u : 1u); ++face)
            for (uint32 mip = 0; mip < description.mip_levels; ++mip)
            {
                uint32 width = description.extent.x >> mip;
                uint32 height = description.extent.y >> mip;
                uint32 depth = description.extent.z >> mip;
                if (!width) width = 1;
                if (!height) height = 1;
                if (!depth) depth = 1;
                uint32 bytes = ((width + format.block_extent.x - 1) / format.block_extent.x)
                    * ((height + format.block_extent.y - 1) / format.block_extent.y) * format.bytes_per_block * depth;
                if (description.type == TextureType::two_d_array) bytes *= description.layer_count;
                output = (output + 15) & ~uint64(15);
                fixture.cases[fixture.case_count++] = {.texture = index, .mip = mip, .face = face, .bytes = bytes, .offset = output};
                for (uint32 byte = 0; byte < bytes; ++byte)
                    fixture.expected[output + byte] = uint8(byte * 37 + (byte >> 7) * 19 + index * 23 + face * 53 + mip * 11);
                if (index == 5)
                    for (uint32 pixel = 0; pixel < bytes / sizeof(float); ++pixel)
                    {
                        const float value = float(pixel + 1) / 64.0f;
                        memcpy(fixture.expected + output + pixel * sizeof(float), &value, sizeof(value));
                    }
                output += bytes + guard_bytes;
            }
    }
    check(output < readback_bytes, "fixture fits its guarded readback allocation");
    return true;
}

static void read_results(Fixture& fixture)
{
    fixture.ring.wait();
    memset(fixture.readback.range.cpu, 0xa5, readback_bytes);
    CommandBuffer* commands = begin_commands(fixture.pool);
    copy_memory(commands, gpu_range(fixture.buffer), {.gpu = fixture.readback.range.gpu + guard_bytes, .size = buffer_bytes});
    for (uint32 index = 0; index < fixture.case_count; ++index)
    {
        const TextureCase& item = fixture.cases[index];
        copy_texture_to_memory(commands, fixture.textures[item.texture], {.gpu = fixture.readback.range.gpu + item.offset, .size = item.bytes},
            {.mip_level = item.mip, .base_slice = item.face, .slice_count = item.texture == 3 ? 1u : 0u});
    }
    barrier(commands, Stage::transfer, Access::transfer_write, Stage::host, Access::host_read);
    end_commands(commands);
    submit(fixture.device, {.commands = {commands}, .completion = {.semaphore = fixture.completion.semaphore, .value = ++fixture.completion.value}});
    wait_timeline(fixture.completion);
    reset_command_pool(fixture.pool);
    check(memcmp(fixture.expected, fixture.readback.range.cpu, readback_bytes) == 0, "all buffer/texture bytes and readback sentinels match");
}

static void run_small_ring(Fixture& fixture)
{
    fixture.ring.upload_buffer(gpu_range(fixture.buffer), {fixture.expected + guard_bytes, buffer_bytes});
    fixture.ring.flush();
    uint8 bytes[4096]{};
    for (uint32 iteration = 0; iteration < 64; ++iteration)
    {
        for (uint32 index = 0; index < 128; ++index) bytes[index] = uint8(iteration * 31 + index * 7);
        memcpy(fixture.expected + guard_bytes + 4096 + iteration * 128, bytes, 128);
        fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu + 4096 + iteration * 128, .size = 128}, {bytes, 128});
    }
    for (uint32 index = 0; index < sizeof(bytes); ++index) bytes[index] = uint8(index * 43 + (index >> 8) * 17);
    memcpy(fixture.expected + guard_bytes + 512, bytes, 256);
    fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu + 512, .size = 256}, {bytes, 256});
    memcpy(fixture.expected + guard_bytes + 12288, bytes, sizeof(bytes));
    fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu + 12288, .size = sizeof(bytes)}, {bytes, sizeof(bytes)});
    fixture.ring.flush();
    for (uint32 iteration = 0; iteration < 64; ++iteration)
    {
        const uint32 value = 0x91a60f23u * (iteration + 1);
        memcpy(fixture.expected + guard_bytes + 256 + iteration * 4, &value, sizeof(value));
        fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu + 256 + iteration * 4, .size = sizeof(value)}, {&value, sizeof(value)});
        fixture.ring.flush();
        check(fixture.ring.stats().pending_batches <= UploadQueue::retirement_capacity, "retirement metadata remains bounded across repeated submissions");
    }
    for (uint32 index = 0; index < fixture.case_count; ++index)
    {
        const TextureCase& item = fixture.cases[index];
        upload_texture(fixture.ring, fixture.textures[item.texture], fixture.descriptions[item.texture],
            {fixture.expected + item.offset, item.bytes}, {.mip_level = item.mip, .base_slice = item.face, .slice_count = item.texture == 3 ? 1u : 0u});
    }
    fixture.ring.flush();
    for (uint32 index = 0; index < 252; ++index) bytes[index] = uint8(173 + index * 13);
    upload_texture(fixture.ring, fixture.textures[0], fixture.descriptions[0], {bytes, 252},
        {.offset = {.x = 3, .y = 5, .z = 2}, .extent = {.x = 7, .y = 9, .z = 4}});
    for (uint32 z = 0; z < 4; ++z)
        for (uint32 y = 0; y < 9; ++y)
            memcpy(fixture.expected + fixture.cases[0].offset + ((z + 2) * 23 + y + 5) * 37 + 3, bytes + (z * 9 + y) * 7, 7);
    upload_texture(fixture.ring, fixture.textures[1], fixture.descriptions[1], {bytes, 24},
        {.base_slice = 2, .slice_count = 2, .offset = {.x = 5, .y = 1}, .extent = {.x = 3, .y = 4, .z = 1}});
    for (uint32 z = 0; z < 2; ++z)
        for (uint32 y = 0; y < 4; ++y)
            memcpy(fixture.expected + fixture.cases[1].offset + ((z + 2) * 7 + y + 1) * 519 + 5, bytes + (z * 4 + y) * 3, 3);
    read_results(fixture);
    const UploadQueueStats stats = fixture.ring.stats();
    check(stats.capacity == 256 && stats.peak_bytes <= 256, "oversized uploads and repeated wraps retain the fixed capacity");
    check(!stats.pending_operations && !stats.pending_batches && !stats.bytes_in_use, "wait drains commands and retires every staged byte");
    printf("Upload queue: %llu submissions, %llu waits, peak %llu bytes; buffer and texture regions verified.\n",
        static_cast<unsigned long long>(stats.submissions), static_cast<unsigned long long>(stats.waits), static_cast<unsigned long long>(stats.peak_bytes));
}

static void run_operation_limit(Fixture& fixture)
{
    fixture.ring = UploadQueue(fixture.device, 128 * 1024);
    const uint64 submissions = fixture.ring.stats().submissions;
    for (uint32 index = 0; index < UploadQueue::operation_limit + 1; ++index)
    {
        const uint32 value = 0xde817f03u * (index + 1);
        memcpy(fixture.expected + guard_bytes + 32 + index * 4, &value, sizeof(value));
        fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu + 32 + index * 4, .size = sizeof(value)}, {&value, sizeof(value)});
    }
    check(fixture.ring.stats().submissions == submissions + 1 && fixture.ring.stats().pending_operations == 1,
        "copy count flushes before staging exceeds the operation limit");
    read_results(fixture);
    check(fixture.ring.stats().capacity == 128 * 1024, "operation pressure does not grow the ring");
}

static void run_reclaim_before_flush(Fixture& fixture)
{
    fixture.ring = UploadQueue(fixture.device, 256);
    uint8 bytes[320]{};
    for (uint32 index = 0; index < sizeof(bytes); ++index) bytes[index] = uint8(index * 53 + 17);
    memcpy(fixture.expected + guard_bytes, bytes, sizeof(bytes));
    fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu, .size = 128}, {bytes, 128});
    fixture.ring.flush();
    wait_idle(fixture.device);
    fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu + 128, .size = 64}, {bytes + 128, 64});
    check(fixture.ring.stats().bytes_in_use == 192 && fixture.ring.stats().pending_batches == 1,
        "completed retirement remains uncollected before pressure");
    const uint64 submissions = fixture.ring.stats().submissions;
    fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu + 192, .size = 128}, {bytes + 192, 128});
    check(fixture.ring.stats().submissions == submissions && fixture.ring.stats().pending_operations == 2,
        "completed staging space is reclaimed without flushing pending copies");
    read_results(fixture);
}

static void run_non_power_of_two(Fixture& fixture)
{
    fixture.ring = UploadQueue(fixture.device, 272);
    uint8 bytes[272]{};
    uint32 offset = 1024;
    const uint32 sizes[]{272, 4, 20};
    for (uint32 iteration = 0; iteration < 8; ++iteration)
        for (uint32 size : sizes)
        {
            for (uint32 index = 0; index < size; ++index) bytes[index] = uint8(iteration * 71 + size * 3 + index * 19);
            memcpy(fixture.expected + guard_bytes + offset, bytes, size);
            fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu + offset, .size = size}, {bytes, size});
            offset += size;
        }
    read_results(fixture);
    const UploadQueueStats stats = fixture.ring.stats();
    check(stats.capacity == 272 && stats.peak_bytes == 272 && !stats.bytes_in_use,
        "unaligned tails and exact-capacity uploads wrap within a fixed non-power-of-two ring");
}

static void run_empty_batches_and_destroy(Fixture& fixture)
{
    fixture.ring = UploadQueue(fixture.device, 256);
    memset(fixture.timestamps.range.cpu, 0xa5, size_t(fixture.timestamps.range.size));
    for (uint32 index = 0; index < timestamp_batches; ++index)
    {
        uint64* destination = reinterpret_cast<uint64*>(fixture.timestamps.range.gpu + guard_bytes) + index * 2;
        fixture.ring.write_timestamp(destination);
        fixture.ring.write_timestamp(destination + 1);
        fixture.ring.flush();
    }
    fixture.ring.wait();
    const UploadQueueStats stats = fixture.ring.stats();
    check(stats.submissions == timestamp_batches && !stats.bytes_in_use && !stats.pending_batches,
        "timestamp-only batches submit and retire without reserving staging bytes");
    const uint64* timestamps = reinterpret_cast<const uint64*>(fixture.timestamps.range.cpu + guard_bytes);
    for (uint32 index = 0; index < timestamp_batches; ++index)
        check(timestamps[index * 2] != 0xa5a5a5a5a5a5a5a5ull && timestamps[index * 2 + 1] != 0xa5a5a5a5a5a5a5a5ull
            && timestamps[index * 2] <= timestamps[index * 2 + 1], "timestamp-only batches resolve both ordered timestamps");
    for (uint32 index = 0; index < guard_bytes; ++index)
        check(fixture.timestamps.range.cpu[index] == 0xa5
            && fixture.timestamps.range.cpu[fixture.timestamps.range.size - guard_bytes + index] == 0xa5, "timestamp guards remain unchanged");
    uint8 bytes[64]{};
    for (uint32 index = 0; index < sizeof(bytes); ++index) bytes[index] = uint8(index * 29 + 67);
    memcpy(fixture.expected + guard_bytes, bytes, sizeof(bytes));
    fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu, .size = sizeof(bytes)}, {bytes, sizeof(bytes)});
    check(fixture.ring.stats().pending_operations == 1, "destruction starts with an unsubmitted upload");
    fixture.ring.destroy();
    check(!fixture.ring.stats().capacity && !fixture.ring.stats().pending_operations && !fixture.ring.stats().pending_batches,
        "destruction resets the ring after flushing pending uploads");
    read_results(fixture);
}

static void run_move_ownership(Fixture& fixture)
{
    fixture.ring.wait();
    uint8 bytes[64]{};
    for (uint32 index = 0; index < sizeof(bytes); ++index) bytes[index] = uint8(index * 41 + 93);
    memcpy(fixture.expected + guard_bytes + 512, bytes, sizeof(bytes));
    {
        UploadQueue original(fixture.device, 256);
        original.upload_buffer({.gpu = fixture.buffer.range.gpu + 512, .size = sizeof(bytes)}, {bytes, sizeof(bytes)});
        UploadQueue moved(static_cast<UploadQueue&&>(original));
        check(!original.stats().capacity && moved.stats().pending_operations == 1, "move construction transfers unsubmitted copies");
        UploadQueue replacement(fixture.device, 128);
        replacement = static_cast<UploadQueue&&>(moved);
        check(!moved.stats().capacity && replacement.stats().capacity == 256 && replacement.stats().pending_operations == 1,
            "move assignment replaces an initialized destination and preserves pending copies");
        UploadQueue& alias = replacement;
        replacement = static_cast<UploadQueue&&>(alias);
        check(replacement.stats().pending_operations == 1, "self-move preserves pending work");
    }
    read_results(fixture);
}

static void run_pitched_texture(Fixture& fixture)
{
    uint8 bytes[160]{};
    memset(bytes, 0xd7, sizeof(bytes));
    for (uint32 row = 0; row < 5; ++row)
        for (uint32 column = 0; column < 28; ++column) bytes[row * 32 + column] = uint8(row * 53 + column * 19);
    for (uint32 index = 0; index < fixture.case_count; ++index)
        if (fixture.cases[index].texture == 4)
            for (uint32 row = 0; row < 5; ++row)
                memcpy(fixture.expected + fixture.cases[index].offset + row * 28, bytes + row * 32, 28);
    fixture.ring.upload_texture(fixture.textures[4], {bytes, sizeof(bytes)}, {.row_pitch_bytes = 32, .slice_pitch_bytes = 160});
    read_results(fixture);
}

static void run_attachment_consumers(Fixture& fixture)
{
    for (uint32 index = 0; index < fixture.case_count; ++index)
    {
        const TextureCase& item = fixture.cases[index];
        if (item.texture < 4) continue;
        if (item.texture == 4)
            for (uint32 byte = 0; byte < item.bytes; ++byte) fixture.expected[item.offset + byte] ^= 0x5a;
        else
            for (uint32 pixel = 0; pixel < item.bytes / sizeof(float); ++pixel)
            {
                const float depth = float(64 - pixel) / 128.0f;
                memcpy(fixture.expected + item.offset + pixel * sizeof(float), &depth, sizeof(depth));
            }
        upload_texture(fixture.ring, fixture.textures[item.texture], fixture.descriptions[item.texture],
            {fixture.expected + item.offset, item.bytes});
    }
    fixture.ring.flush();
    CommandBuffer* commands = begin_commands(fixture.pool);
    const ColorAttachment color{.render_view = fixture.attachments[0]};
    begin_render_pass(commands, {.colors = {&color, 1}, .depth = {.render_view = fixture.attachments[1]}});
    end_render_pass(commands);
    barrier(commands, Stage::color_output | Stage::depth_stencil_tests, Access::color_write | Access::depth_stencil_write,
        Stage::transfer, Access::transfer_read);
    end_commands(commands);
    submit(fixture.device, {.commands = {commands}, .completion = {.semaphore = fixture.completion.semaphore, .value = ++fixture.completion.value}});
    wait_timeline(fixture.completion);
    reset_command_pool(fixture.pool);
    read_results(fixture);
}

#if defined(NOGRAPHICSAPI_UPLOAD_QUEUE_SPV_PATH)
static void run_compute_callbacks(Fixture& fixture)
{
    fixture.ring = UploadQueue(fixture.device, 272);
    uint32 initial[16]{};
    for (uint32 index = 0; index < 16; ++index) initial[index] = index * 23 + 7;
    memcpy(fixture.expected + guard_bytes, initial, sizeof(initial));
    fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu, .size = sizeof(initial)}, {initial, sizeof(initial)});
    uint32 callbacks = 0;
    for (uint32 iteration = 0; iteration < 2; ++iteration)
    {
        fixture.ring.upload_with_compute(sizeof(initial), [&](CommandBuffer* commands, GpuCpuRange<byte> staging) noexcept
        {
            ++callbacks;
            check(staging.size == sizeof(initial) && !(reinterpret_cast<uintptr>(staging.cpu) & 15)
                && !(reinterpret_cast<uintptr>(staging.gpu) & 15), "callback receives the requested aligned staging range");
            for (uint32 index = 0; index < 16; ++index) reinterpret_cast<uint32*>(staging.cpu)[index] = iteration * 31 + index + 1;
            bind_pso(commands, fixture.compute);
            dispatch(commands, UploadQueueRoot{
                .source = reinterpret_cast<uint32*>(staging.gpu), .destination = reinterpret_cast<uint32*>(fixture.buffer.range.gpu), .count = 16,
            }, {.x = 1, .y = 1, .z = 1});
        });
        check(callbacks == iteration + 1, "compute callback executes synchronously exactly once");
        for (uint32 index = 0; index < 16; ++index) initial[index] += iteration * 31 + index + 1;
    }
    memcpy(fixture.expected + guard_bytes, initial, sizeof(initial));
    const uint32 replacement[4]{0x12345678u, 0x9abcdef0u, 0x31415926u, 0x27182818u};
    fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu, .size = sizeof(replacement)}, {replacement, sizeof(replacement)});
    memcpy(fixture.expected + guard_bytes, replacement, sizeof(replacement));
    check(fixture.ring.stats().submissions == 0 && fixture.ring.stats().pending_operations == 4,
        "transfer-compute-compute-transfer ordering remains within one batch");
    read_results(fixture);

    uint32 values[64]{};
    for (uint32 index = 0; index < 64; ++index) values[index] = index * 19 + 3;
    fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu + 1024, .size = sizeof(values)}, {values, sizeof(values)});
    const uint64 submissions = fixture.ring.stats().submissions;
    for (uint32 iteration = 0; iteration < 5; ++iteration)
    {
        fixture.ring.upload_with_compute(sizeof(values), [&](CommandBuffer* commands, GpuCpuRange<byte> staging) noexcept
        {
            ++callbacks;
            for (uint32 index = 0; index < 64; ++index) reinterpret_cast<uint32*>(staging.cpu)[index] = iteration * 37 + index * 5 + 11;
            bind_pso(commands, fixture.compute);
            dispatch(commands, UploadQueueRoot{
                .source = reinterpret_cast<uint32*>(staging.gpu), .destination = reinterpret_cast<uint32*>(fixture.buffer.range.gpu + 1024), .count = 64,
            }, {.x = 1, .y = 1, .z = 1});
        });
        for (uint32 index = 0; index < 64; ++index) values[index] += iteration * 37 + index * 5 + 11;
    }
    memcpy(fixture.expected + guard_bytes + 1024, values, sizeof(values));
    check(callbacks == 7 && fixture.ring.stats().submissions == submissions + 5,
        "full staging reservations flush before exposing each callback's command buffer");
    read_results(fixture);
    check(fixture.ring.stats().peak_bytes <= 272, "compute callbacks preserve bounded ring storage across wraps");
}
#endif

static void run_multiple_queues(Fixture& fixture)
{
    if (get_device_caps(fixture.device).queue_count < 2)
    {
        printf("Cross-queue upload checks skipped: one compatible queue.\n");
        return;
    }
    fixture.ring.destroy();
    UploadQueue original(fixture.device, 256, 1);
    TimelinePoint pending = original.flush();
    ++pending.value;
    uint32 values[64]{};
    for (uint32 index = 0; index < 64; ++index) values[index] = index * 0x91a60723u + 11;
    original.upload_buffer({.gpu = fixture.buffer.range.gpu, .size = sizeof(values)}, {values, sizeof(values)});
    UploadQueue moved(static_cast<UploadQueue&&>(original));
    fixture.ring = static_cast<UploadQueue&&>(moved);
    check(!original.stats().capacity && !moved.stats().capacity && fixture.ring.stats().pending_operations == 1,
        "moves transfer a nonzero-queue uploader with pending copies");
    memcpy(fixture.expected + guard_bytes, values, sizeof(values));
    memset(fixture.readback.range.cpu, 0xa5, sizeof(values) + guard_bytes * 2);
    CommandBuffer* consumer = begin_commands(fixture.pool);
    copy_memory(consumer, {.gpu = fixture.buffer.range.gpu, .size = sizeof(values)},
        {.gpu = fixture.readback.range.gpu + guard_bytes, .size = sizeof(values)});
    barrier(consumer, Stage::transfer, Access::transfer_write, Stage::host, Access::host_read);
    end_commands(consumer);
    // Queue zero waits first, so the moved uploader must retain queue one to make progress.
    submit(fixture.device, {.commands = {consumer}, .waits = {pending},
        .completion = {.semaphore = fixture.completion.semaphore, .value = ++fixture.completion.value}});
    const TimelinePoint uploaded = fixture.ring.flush();
    const TimelinePoint empty_flush = fixture.ring.flush();
    check(uploaded.semaphore == pending.semaphore && uploaded.value == pending.value
        && empty_flush.semaphore == uploaded.semaphore && empty_flush.value == uploaded.value,
        "flush returns the latest completion even without a pending batch");
    wait_timeline(fixture.completion);
    reset_command_pool(fixture.pool);
    check(memcmp(values, fixture.readback.range.cpu + guard_bytes, sizeof(values)) == 0,
        "upload completion supplies the cross-queue memory dependency");
    for (uint32 index = 0; index < guard_bytes; ++index)
        check(fixture.readback.range.cpu[index] == 0xa5 && fixture.readback.range.cpu[guard_bytes + sizeof(values) + index] == 0xa5,
            "cross-queue readback guards remain unchanged");
    fixture.ring.wait();
}

static void run_retirement_capacity(Fixture& fixture)
{
    if (get_device_caps(fixture.device).queue_count < 2) return;
    fixture.ring = UploadQueue(fixture.device, 1024);
    TimelineSemaphore* gate = create_timeline_semaphore(fixture.device);
    if (!gate) { check(false, "retirement gate initializes"); return; }
    CommandPool* release_pool = create_command_pool(fixture.device);
    if (!release_pool) { check(false, "retirement release pool initializes"); destroy_timeline_semaphore(gate); return; }
    CommandBuffer* blocked = begin_commands(fixture.pool);
    end_commands(blocked);
    submit(fixture.device, {.commands = {blocked}, .waits = {{.semaphore = gate, .value = 1}},
        .completion = {.semaphore = fixture.completion.semaphore, .value = ++fixture.completion.value}});
    for (uint32 index = 0; index < UploadQueue::retirement_capacity; ++index)
    {
        const uint32 value = 0xcafe0000u + index;
        fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu + index * sizeof(value), .size = sizeof(value)}, {&value, sizeof(value)});
        fixture.ring.flush();
        memcpy(fixture.expected + guard_bytes + index * sizeof(value), &value, sizeof(value));
    }
    check(fixture.ring.stats().pending_batches == UploadQueue::retirement_capacity,
        "all retirement slots remain occupied while the queue is gated");
    CommandBuffer* release = begin_commands(release_pool);
    end_commands(release);
    submit(fixture.device, {.commands = {release}, .completion = {.semaphore = gate, .value = 1}}, 1);
    const uint32 final_value = 0x1973abcd;
    fixture.ring.upload_buffer({.gpu = fixture.buffer.range.gpu + 256, .size = sizeof(final_value)}, {&final_value, sizeof(final_value)});
    memcpy(fixture.expected + guard_bytes + 256, &final_value, sizeof(final_value));
    fixture.ring.wait();
    wait_timeline(fixture.completion);
    wait_timeline({.semaphore = gate, .value = 1});
    reset_command_pool(fixture.pool);
    destroy_command_pool(release_pool);
    destroy_timeline_semaphore(gate);
    read_results(fixture);
}

int main()
{
    const DeviceInit initialized = create_device({.desired_queue_count = 2, .timestamp_query_count = 2});
    if (initialized.error != Error::none)
    {
        fprintf(stderr, "No compatible Vulkan device: %u\n", uint32(initialized.error));
        return initialized.error == Error::unsupported ? 77 : 1;
    }
    Fixture fixture{.device = initialized.device, .ring = UploadQueue(initialized.device, 256)};
    if (!initialize(fixture)) { shutdown(fixture); return fixture.unsupported ? 77 : 1; }
    run_small_ring(fixture);
    run_pitched_texture(fixture);
    run_attachment_consumers(fixture);
    run_reclaim_before_flush(fixture);
    run_non_power_of_two(fixture);
    run_empty_batches_and_destroy(fixture);
    run_move_ownership(fixture);
    run_operation_limit(fixture);
#if defined(NOGRAPHICSAPI_UPLOAD_QUEUE_SPV_PATH)
    run_compute_callbacks(fixture);
#endif
    run_retirement_capacity(fixture);
    run_multiple_queues(fixture);
    shutdown(fixture);
    printf("Upload queue: %u failures.\n", failures);
    return failures ? 1 : 0;
}
