#include <NoGraphicsAPIUtility/upload_queue.hpp>

#include <assert.h>
#include <string.h>

namespace gpu
{

namespace
{

constexpr Access gpu_access = Access::shader_read | Access::shader_write | Access::transfer_read | Access::transfer_write
    | Access::color_read | Access::color_write | Access::depth_stencil_read | Access::depth_stencil_write
    | Access::indirect_read | Access::index_read | Access::descriptor_read;

} // namespace

UploadQueue::UploadQueue(Device* device, uint64 capacity, uint32 queue_index, uint32 max_pending_batches) noexcept
{
    assert(device && capacity >= alignment && capacity % alignment == 0 && max_pending_batches);
    const DeviceCaps& caps = get_device_caps(device);
    assert(queue_index < caps.queue_count);
    state_.device = device;
    state_.queue_index = queue_index;
    if (queue_index < caps.general_queue_count + caps.compute_queue_count)
    {
        state_.upload_stages = state_.upload_stages | Stage::compute;
        state_.upload_access = state_.upload_access | Access::shader_read | Access::shader_write | Access::descriptor_read;
        state_.queue_access = state_.upload_access | Access::indirect_read;
        if (queue_index < caps.general_queue_count) state_.queue_access = gpu_access;
    }
    else
    {
        state_.texture_granularity = caps.copy_texture_granularity;
    }
    state_.heap = create_gpu_heap(device, capacity, MemoryType::cpu_visible);
    state_.completion.semaphore = create_timeline_semaphore(device);
    state_.batches = new Batch[max_pending_batches]{};
    state_.max_pending_batches = max_pending_batches;
    for (uint32 index = 0; index < max_pending_batches; ++index)
    {
        Batch& batch = state_.batches[index];
        batch.pool = create_command_pool(device, queue_index);
        end_commands(begin_commands(batch.pool));
        reset_command_pool(batch.pool);
    }
}

UploadQueue::~UploadQueue() noexcept
{
    destroy();
}

UploadQueue::UploadQueue(UploadQueue&& other) noexcept : state_(other.state_)
{
    assert(!other.state_.in_callback);
    other.state_ = {};
}

UploadQueue& UploadQueue::operator=(UploadQueue&& other) noexcept
{
    assert(!state_.in_callback && !other.state_.in_callback);
    if (this == &other) return *this;
    destroy();
    state_ = other.state_;
    other.state_ = {};
    return *this;
}

void UploadQueue::destroy() noexcept
{
    assert(!state_.in_callback);
    if (state_.completion.semaphore) wait();
    for (uint32 index = 0; index < state_.max_pending_batches; ++index) destroy_command_pool(state_.batches[index].pool);
    delete[] state_.batches;
    destroy_gpu_heap(state_.heap);
    destroy_timeline_semaphore(state_.completion.semaphore);
    state_ = {};
}

void UploadQueue::reclaim() noexcept
{
    State& state = state_;
    if (!state.retirement_count) return;
    const uint64 completed = timeline_completed_value(state.completion.semaphore);
    while (state.retirement_count && state.batches[state.retirement_first].value <= completed)
    {
        Batch& batch = state.batches[state.retirement_first];
        state.tail = batch.end;
        reset_command_pool(batch.pool);
        state.retirement_first = (state.retirement_first + 1) % state.max_pending_batches;
        --state.retirement_count;
    }
}

void UploadQueue::wait_oldest() noexcept
{
    assert(state_.retirement_count);
    wait_timeline({.semaphore = state_.completion.semaphore, .value = state_.batches[state_.retirement_first].value});
    ++state_.waits;
    reclaim();
}

GpuCpuRange<byte> UploadQueue::reserve(uint64 byte_size) noexcept
{
    State& state = state_;
    assert(state.completion.semaphore && !state.in_callback && byte_size && byte_size <= state.heap.range.size);
    if (state.operation_count == operation_limit) flush();
    for (;;)
    {
        uint64 offset = (state.head % state.heap.range.size + alignment - 1) & ~(alignment - 1);
        if (byte_size > state.heap.range.size - offset) offset = state.heap.range.size;
        const uint64 start = state.head - state.head % state.heap.range.size + offset;
        if (state.head == state.tail && !state.retirement_count) state.head = state.tail = start;
        if (start + byte_size - state.tail <= state.heap.range.size)
        {
            state.head = start + byte_size;
            if (state.head - state.tail > state.peak_bytes) state.peak_bytes = state.head - state.tail;
            return {.cpu = state.heap.range.cpu + start % state.heap.range.size,
                    .gpu = state.heap.range.gpu + start % state.heap.range.size, .size = byte_size};
        }
        const uint32 previous_count = state.retirement_count;
        reclaim();
        if (state.retirement_count != previous_count) continue;
        if (state.commands)
        {
            flush();
            continue;
        }
        wait_oldest();
    }
}

CommandBuffer* UploadQueue::begin() noexcept
{
    State& state = state_;
    assert(state.completion.semaphore && !state.in_callback);
    if (!state.commands)
    {
        if (state.retirement_count == state.max_pending_batches)
        {
            reclaim();
            if (state.retirement_count == state.max_pending_batches) wait_oldest();
        }
        state.commands = begin_commands(state.batches[(state.retirement_first + state.retirement_count) % state.max_pending_batches].pool);
        barrier(state.commands, Stage::all_commands, state.queue_access, state.upload_stages, state.upload_access);
    }
    return state.commands;
}

void UploadQueue::upload_buffer(GpuRange destination, ByteSpan source) noexcept
{
    assert(state_.completion.semaphore && !state_.in_callback && (source.data || !source.size) && source.size <= destination.size);
    for (uint64 offset = 0; offset < source.size;)
    {
        const uint64 size = source.size - offset < state_.heap.range.size ? source.size - offset : state_.heap.range.size;
        const GpuCpuRange<byte> staging = reserve(size);
        memcpy(staging.cpu, source.data + offset, size_t(size));
        copy_memory(begin(), gpu_range(staging), {.gpu = static_cast<byte*>(destination.gpu) + offset, .size = size});
        ++state_.operation_count;
        offset += size;
    }
}

void UploadQueue::upload_texture(Texture* destination, ByteSpan source, const TextureCopyDesc& copy) noexcept
{
    assert(source.data);
    const GpuCpuRange<byte> staging = reserve(source.size);
    memcpy(staging.cpu, source.data, source.size);
    copy_memory_to_texture(begin(), gpu_range(staging), destination, copy);
    ++state_.operation_count;
}

GpuCpuRange<byte> UploadQueue::begin_compute(uint64 byte_size) noexcept
{
    assert(state_.upload_stages != Stage::transfer && "compute uploads require a general or compute queue");
    const GpuCpuRange<byte> staging = reserve(byte_size);
    barrier(begin(), Stage::transfer | Stage::compute, Access::transfer_write | Access::shader_write,
            Stage::compute, Access::shader_read | Access::shader_write | Access::descriptor_read);
    state_.in_callback = true;
    return staging;
}

void UploadQueue::end_compute() noexcept
{
    assert(state_.in_callback);
    barrier(state_.commands, Stage::compute, Access::shader_write, Stage::transfer | Stage::compute,
            Access::transfer_read | Access::transfer_write | Access::shader_read | Access::shader_write | Access::descriptor_read);
    state_.in_callback = false;
    ++state_.operation_count;
}

void UploadQueue::write_timestamp(uint64* gpu_destination) noexcept
{
    gpu::write_timestamp(begin(), gpu_destination);
}

TimelinePoint UploadQueue::flush() noexcept
{
    State& state = state_;
    assert(!state.in_callback);
    if (!state.commands) return state.completion;
    reclaim();
    barrier(state.commands, state.upload_stages, state.upload_access, Stage::all_commands, state.queue_access);
    end_commands(state.commands);
    ++state.completion.value;
    submit(state.device, {.commands = {state.commands}, .completion = state.completion}, state.queue_index);
    Batch& batch = state.batches[(state.retirement_first + state.retirement_count) % state.max_pending_batches];
    batch.end = state.head;
    batch.value = state.completion.value;
    ++state.retirement_count;
    ++state.submissions;
    state.commands = nullptr;
    state.operation_count = 0;
    return state.completion;
}

void UploadQueue::wait() noexcept
{
    flush();
    reclaim();
    if (state_.retirement_count)
    {
        wait_timeline(state_.completion);
        ++state_.waits;
        reclaim();
    }
}

UploadQueueStats UploadQueue::stats() const noexcept
{
    return {
        .capacity = state_.heap.range.size,
        .bytes_in_use = state_.head - state_.tail,
        .peak_bytes = state_.peak_bytes,
        .submissions = state_.submissions,
        .waits = state_.waits,
        .pending_batches = state_.retirement_count,
        .pending_operations = state_.operation_count,
    };
}

} // namespace gpu
