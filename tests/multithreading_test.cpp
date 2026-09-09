#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace
{

constexpr uint32 thread_count = 4;
constexpr uint32 textures_per_thread = 8;
constexpr uint64 texture_bytes = 8 * 8 * 4;
constexpr gpu::TextureDesc texture_desc{
    .extent = {.x = 8, .y = 8, .z = 1},
    .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment |
             gpu::TextureUsage::transfer_source | gpu::TextureUsage::transfer_destination,
};

struct StartGate
{
#if defined(_WIN32)
    HANDLE event = nullptr;
#else
    pthread_mutex_t mutex{};
    pthread_cond_t condition{};
    bool signaled = false;
#endif
};

struct Thread
{
    void (*run)(void*) noexcept = nullptr;
    void* argument = nullptr;
    StartGate* start = nullptr;
#if defined(_WIN32)
    HANDLE handle = nullptr;
#else
    pthread_t handle{};
#endif
};

#if defined(_WIN32)
DWORD WINAPI run_thread(void* argument) noexcept
#else
void* run_thread(void* argument) noexcept
#endif
{
    Thread* thread = static_cast<Thread*>(argument);
    if (thread->start)
    {
#if defined(_WIN32)
        WaitForSingleObject(thread->start->event, INFINITE);
#else
        pthread_mutex_lock(&thread->start->mutex);
        while (!thread->start->signaled)
            pthread_cond_wait(&thread->start->condition, &thread->start->mutex);
        pthread_mutex_unlock(&thread->start->mutex);
#endif
    }
    thread->run(thread->argument);
    return 0;
}

bool start_thread(Thread& thread) noexcept
{
#if defined(_WIN32)
    thread.handle = CreateThread(nullptr, 0, run_thread, &thread, 0, nullptr);
    if (!thread.handle)
        fprintf(stderr, "CreateThread failed: %lu.\n", GetLastError());
    return thread.handle != nullptr;
#else
    return pthread_create(&thread.handle, nullptr, run_thread, &thread) == 0;
#endif
}

bool join_thread(Thread& thread) noexcept
{
#if defined(_WIN32)
    const bool joined = WaitForSingleObject(thread.handle, INFINITE) == WAIT_OBJECT_0;
    if (!joined)
        fprintf(stderr, "Thread join failed: %lu.\n", GetLastError());
    CloseHandle(thread.handle);
    return joined;
#else
    return pthread_join(thread.handle, nullptr) == 0;
#endif
}

bool run_threads(gpu::Span<Thread> threads) noexcept
{
    StartGate start{};
#if defined(_WIN32)
    start.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!start.event)
        return false;
#else
    if (pthread_mutex_init(&start.mutex, nullptr) != 0)
        return false;
    if (pthread_cond_init(&start.condition, nullptr) != 0)
    {
        pthread_mutex_destroy(&start.mutex);
        return false;
    }
#endif
    uint32 started_count = 0;
    for (; started_count != threads.size; ++started_count)
    {
        threads.data[started_count].start = &start;
        if (!start_thread(threads.data[started_count]))
            break;
    }
#if defined(_WIN32)
    SetEvent(start.event);
#else
    pthread_mutex_lock(&start.mutex);
    start.signaled = true;
    pthread_cond_broadcast(&start.condition);
    pthread_mutex_unlock(&start.mutex);
#endif
    bool valid = started_count == threads.size;
    if (!valid)
        fprintf(stderr, "Started %u of %zu worker threads.\n", started_count, threads.size);
    for (uint32 index = 0; index != started_count; ++index)
        valid = join_thread(threads.data[index]) && valid;
#if defined(_WIN32)
    CloseHandle(start.event);
#else
    pthread_cond_destroy(&start.condition);
    pthread_mutex_destroy(&start.mutex);
#endif
    return valid;
}

struct RecordingContext
{
    gpu::Device* device = nullptr;
    const gpu::TextureHeap* texture_heap = nullptr;
    uint64 texture_stride = 0;
    uint32 index = 0;
    gpu::CommandPool* pool = nullptr;
    gpu::CommandBuffer* upload_commands = nullptr;
    gpu::CommandBuffer* readback_commands = nullptr;
    gpu::TimelineSemaphore* timeline = nullptr;
    gpu::GpuHeap upload{};
    gpu::GpuHeap readback{};
    gpu::GpuHeap texture_descriptors{};
    gpu::GpuHeap sampler_descriptors{};
    gpu::Texture* textures[textures_per_thread]{};
    gpu::RenderView* views[textures_per_thread]{};
};

void record_textures(void* argument) noexcept
{
    RecordingContext* context = static_cast<RecordingContext*>(argument);
    const gpu::DeviceCaps& caps = gpu::get_device_caps(context->device);
    context->pool = gpu::create_command_pool(context->device);
    context->upload_commands = gpu::begin_commands(context->pool);
    context->timeline = gpu::create_timeline_semaphore(context->device);
    context->upload = gpu::create_gpu_heap(context->device, textures_per_thread * texture_bytes);
    context->readback = gpu::create_gpu_heap(context->device, textures_per_thread * texture_bytes + sizeof(uint64) * 2, gpu::MemoryType::readback);
    context->texture_descriptors = gpu::create_gpu_heap(context->device, textures_per_thread * caps.texture_descriptor_size,
                                                     gpu::MemoryType::texture_descriptor_heap);
    context->sampler_descriptors = gpu::create_gpu_heap(context->device, textures_per_thread * caps.sampler_descriptor_size,
                                                     gpu::MemoryType::sampler_descriptor_heap);
    for (uint32 index = 0; index != textures_per_thread * texture_bytes / sizeof(uint32); ++index)
        reinterpret_cast<uint32*>(context->upload.range.cpu)[index] = 0x12340000u + context->index * 0x1000u + index;
    memset(context->readback.range.cpu, 0xcd, textures_per_thread * texture_bytes);
    memset(context->readback.range.cpu + textures_per_thread * texture_bytes, 0xff, sizeof(uint64) * 2);

    for (uint32 index = 0; index != textures_per_thread; ++index)
    {
        context->textures[index] = gpu::create_texture(context->upload_commands, texture_desc, *context->texture_heap,
                                                     (context->index * textures_per_thread + index) * context->texture_stride);
        context->views[index] = gpu::create_render_view(context->textures[index]);
        gpu::write_texture_descriptor(context->device, context->texture_descriptors.range.cpu + index * caps.texture_descriptor_size,
                                      context->textures[index], gpu::TextureDescriptorType::sampled);
        gpu::write_sampler_descriptor(context->device, context->sampler_descriptors.range.cpu + index * caps.sampler_descriptor_size);
        gpu::copy_memory_to_texture(context->upload_commands, {.gpu = context->upload.range.gpu + index * texture_bytes, .size = texture_bytes},
                                    context->textures[index]);
    }
    gpu::write_timestamp(context->upload_commands, reinterpret_cast<uint64*>(context->readback.range.gpu + textures_per_thread * texture_bytes));
    gpu::end_commands(context->upload_commands);
    context->readback_commands = gpu::begin_commands(context->pool);
    for (uint32 index = 0; index != textures_per_thread; ++index)
    {
        gpu::copy_texture_to_memory(context->readback_commands, context->textures[index],
                                    {.gpu = context->readback.range.gpu + index * texture_bytes, .size = texture_bytes});
    }
    gpu::barrier(context->readback_commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
    gpu::write_timestamp(context->readback_commands, reinterpret_cast<uint64*>(context->readback.range.gpu + textures_per_thread * texture_bytes) + 1);
    gpu::end_commands(context->readback_commands);
}

void destroy_recording_resources(RecordingContext& context) noexcept
{
    gpu::destroy_command_pool(context.pool);
    for (uint32 index = 0; index != textures_per_thread; ++index)
    {
        gpu::destroy_render_view(context.views[index]);
        gpu::destroy_texture(context.textures[index]);
    }
    gpu::destroy_gpu_heap(context.sampler_descriptors);
    gpu::destroy_gpu_heap(context.texture_descriptors);
    gpu::destroy_gpu_heap(context.readback);
    gpu::destroy_gpu_heap(context.upload);
    gpu::destroy_timeline_semaphore(context.timeline);
}

void wait_for_submission(void* argument) noexcept
{
    gpu::wait_timeline(*static_cast<gpu::TimelinePoint*>(argument));
}

bool test_parallel_recording(gpu::Device* device) noexcept
{
    const gpu::SizeAlign requirements = gpu::get_texture_size_align(device, texture_desc);
    const uint64 texture_stride = (requirements.size + requirements.align - 1) / requirements.align * requirements.align;
    const gpu::TextureHeap texture_heap = gpu::create_texture_heap(device, thread_count * textures_per_thread * texture_stride);
    RecordingContext contexts[thread_count]{};
    Thread threads[thread_count]{};
    for (uint32 index = 0; index != thread_count; ++index)
    {
        contexts[index] = {.device = device, .texture_heap = &texture_heap, .texture_stride = texture_stride, .index = index};
        threads[index] = {.run = record_textures, .argument = contexts + index};
    }
    bool valid = run_threads(threads);
    if (valid)
    {
        gpu::CommandPool* pool = gpu::create_command_pool(device);
        gpu::CommandBuffer* held_commands = gpu::begin_commands(pool);
        gpu::TimelinePoint completion{.semaphore = contexts[thread_count - 1].timeline, .value = 3};
        gpu::TimelinePoint waiter_completion{.semaphore = contexts[0].timeline, .value = 2};
        Thread waiter{.run = wait_for_submission, .argument = &waiter_completion};
        const bool waiter_started = start_thread(waiter);
        valid = waiter_started;
        for (uint32 index = 0; index != thread_count; ++index)
        {
            // Submit independent ended buffers while held_commands is still recording.
            gpu::submit(gpu::get_queue(device), {.commands = {contexts[index].upload_commands},
                                               .completion = {.semaphore = contexts[index].timeline, .value = 1}});
            gpu::submit(gpu::get_queue(device), {.commands = {contexts[index].readback_commands},
                                               .waits = {{.semaphore = contexts[index].timeline, .value = 1}},
                                               .completion = {.semaphore = contexts[index].timeline, .value = 2}});
        }
        gpu::end_commands(held_commands);
        gpu::submit(gpu::get_queue(device), {.commands = {held_commands}, .completion = completion});
        if (waiter_started)
            valid = join_thread(waiter) && valid;
        gpu::wait_timeline(completion);
        gpu::destroy_command_pool(pool);
        for (uint32 index = 0; index != thread_count; ++index)
        {
            gpu::wait_timeline({.semaphore = contexts[index].timeline, .value = 2});
            const uint64* timestamps = reinterpret_cast<const uint64*>(contexts[index].readback.range.cpu + textures_per_thread * texture_bytes);
            if (timestamps[0] == ~uint64{0} || timestamps[1] == ~uint64{0} || timestamps[0] > timestamps[1])
            {
                fprintf(stderr, "Worker %u timestamp readback failed.\n", index);
                valid = false;
            }
            for (uint32 word = 0; word != textures_per_thread * texture_bytes / sizeof(uint32); ++word)
            {
                if (reinterpret_cast<uint32*>(contexts[index].upload.range.cpu)[word] == reinterpret_cast<uint32*>(contexts[index].readback.range.cpu)[word])
                    continue;
                fprintf(stderr, "Worker %u texture %llu word %u: expected %08x, received %08x.\n", index, word * sizeof(uint32) / texture_bytes,
                        word, reinterpret_cast<uint32*>(contexts[index].upload.range.cpu)[word],
                        reinterpret_cast<uint32*>(contexts[index].readback.range.cpu)[word]);
                fprintf(stderr, "Readback words 0=%08x 64=%08x last=%08x.\n", reinterpret_cast<uint32*>(contexts[index].readback.range.cpu)[0],
                        reinterpret_cast<uint32*>(contexts[index].readback.range.cpu)[64],
                        reinterpret_cast<uint32*>(contexts[index].readback.range.cpu)[textures_per_thread * texture_bytes / sizeof(uint32) - 1]);
                valid = false;
                break;
            }
        }
    }
    for (uint32 index = 0; index != thread_count; ++index)
        destroy_recording_resources(contexts[index]);
    gpu::destroy_texture_heap(texture_heap);
    return valid;
}

struct QueueContext
{
    gpu::Device* device = nullptr;
    gpu::Queue* queue = nullptr;
    uint32 index = 0;
    bool valid = true;
};

void submit_copies(void* argument) noexcept
{
    QueueContext* context = static_cast<QueueContext*>(argument);
    gpu::CommandPool* pool = gpu::create_command_pool(context->device);
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(context->device);
    const gpu::GpuHeap upload = gpu::create_gpu_heap(context->device, texture_bytes);
    const gpu::GpuHeap scratch = gpu::create_gpu_heap(context->device, texture_bytes, gpu::MemoryType::gpu_only);
    const gpu::GpuHeap readback = gpu::create_gpu_heap(context->device, texture_bytes + sizeof(uint64), gpu::MemoryType::readback);
    for (uint32 iteration = 1; iteration <= 32; ++iteration)
    {
        for (uint32 index = 0; index != texture_bytes / sizeof(uint32); ++index)
            reinterpret_cast<uint32*>(upload.range.cpu)[index] = context->index * 0x10000u + iteration * 0x100u + index;
        *reinterpret_cast<uint64*>(readback.range.cpu + texture_bytes) = ~uint64{0};
        gpu::CommandBuffer* commands = gpu::begin_commands(pool);
        gpu::copy_memory(commands, gpu::gpu_range(upload), gpu::gpu_range(scratch));
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::transfer, gpu::Access::transfer_read);
        gpu::copy_memory(commands, gpu::gpu_range(scratch), {.gpu = readback.range.gpu, .size = texture_bytes});
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
        gpu::write_timestamp(commands, reinterpret_cast<uint64*>(readback.range.gpu + texture_bytes));
        gpu::end_commands(commands);
        gpu::submit(context->queue, {.commands = {commands}, .completion = {.semaphore = timeline, .value = iteration}});
        gpu::wait_timeline({.semaphore = timeline, .value = iteration});
        context->valid = memcmp(upload.range.cpu, readback.range.cpu, texture_bytes) == 0 && context->valid;
        context->valid = *reinterpret_cast<const uint64*>(readback.range.cpu + texture_bytes) != ~uint64{0} && context->valid;
        gpu::reset_command_pool(pool);
    }
    gpu::destroy_command_pool(pool);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(scratch);
    gpu::destroy_gpu_heap(upload);
    gpu::destroy_timeline_semaphore(timeline);
}

bool test_multiple_queues(gpu::Device* device) noexcept
{
    QueueContext contexts[2]{
        {.device = device, .queue = gpu::get_queue(device, 0), .index = 0},
        {.device = device, .queue = gpu::get_queue(device, 1), .index = 1},
    };
    if (contexts[0].queue == contexts[1].queue)
    {
        fprintf(stderr, "Different queue indices returned the same queue handle.\n");
        return false;
    }
    Thread threads[2]{{.run = submit_copies, .argument = contexts}, {.run = submit_copies, .argument = contexts + 1}};
    bool valid = run_threads(threads);
    valid = valid && contexts[0].valid && contexts[1].valid;

    gpu::CommandPool* producer_pool = gpu::create_command_pool(device);
    gpu::CommandPool* consumer_pool = gpu::create_command_pool(device);
    gpu::TimelineSemaphore* produced = gpu::create_timeline_semaphore(device);
    gpu::TimelineSemaphore* consumed = gpu::create_timeline_semaphore(device);
    const gpu::GpuHeap upload = gpu::create_gpu_heap(device, texture_bytes);
    const gpu::GpuHeap scratch = gpu::create_gpu_heap(device, texture_bytes, gpu::MemoryType::gpu_only);
    const gpu::GpuHeap readback = gpu::create_gpu_heap(device, texture_bytes, gpu::MemoryType::readback);
    for (uint32 index = 0; index != texture_bytes / sizeof(uint32); ++index)
        reinterpret_cast<uint32*>(upload.range.cpu)[index] = 0xcafe0000u + index;
    memset(readback.range.cpu, 0, texture_bytes);
    gpu::CommandBuffer* producer = gpu::begin_commands(producer_pool);
    gpu::copy_memory(producer, gpu::gpu_range(upload), gpu::gpu_range(scratch));
    gpu::end_commands(producer);
    gpu::CommandBuffer* consumer = gpu::begin_commands(consumer_pool);
    gpu::copy_memory(consumer, gpu::gpu_range(scratch), gpu::gpu_range(readback));
    gpu::barrier(consumer, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
    gpu::end_commands(consumer);
    // Submit the consumer first: its cross-queue wait supplies the memory dependency.
    gpu::submit(contexts[1].queue, {.commands = {consumer}, .waits = {{.semaphore = produced, .value = 1}},
                                  .completion = {.semaphore = consumed, .value = 1}});
    gpu::submit(contexts[0].queue, {.commands = {producer}, .completion = {.semaphore = produced, .value = 1}});
    gpu::wait_timeline({.semaphore = consumed, .value = 1});
    gpu::wait_timeline({.semaphore = produced, .value = 1});
    valid = memcmp(upload.range.cpu, readback.range.cpu, texture_bytes) == 0 && valid;
    gpu::destroy_command_pool(consumer_pool);
    gpu::destroy_command_pool(producer_pool);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(scratch);
    gpu::destroy_gpu_heap(upload);
    gpu::destroy_timeline_semaphore(consumed);
    gpu::destroy_timeline_semaphore(produced);
    return valid;
}

} // namespace

int main(int argc, char** argv)
{
    const bool multiple_queues = argc == 2 && strcmp(argv[1], "--multiple-queues") == 0;
    const gpu::DeviceInit device_init = gpu::create_device({.desired_queue_count = 32});
    if (device_init.error == gpu::Error::unsupported)
        return 77;
    if (device_init.error != gpu::Error::none)
        return 1;
    const uint32 queue_count = gpu::get_device_caps(device_init.device).queue_count;
    printf("Requested 32 queues; device exposes %u compatible queue(s).\n", queue_count);
    if (multiple_queues && queue_count < 2)
    {
        fprintf(stderr, "Multiple-queue test skipped: device exposes %u compatible queue(s); parallel recording is tested separately.\n", queue_count);
        gpu::wait_idle(device_init.device);
        gpu::destroy_device(device_init.device);
        return 77;
    }
    const bool valid = queue_count >= 1 && queue_count <= 32 &&
                       (multiple_queues ? test_multiple_queues(device_init.device) : test_parallel_recording(device_init.device));
    gpu::wait_idle(device_init.device);
    gpu::destroy_device(device_init.device);
    if (!valid)
        fprintf(stderr, "%s test failed.\n", multiple_queues ? "Multiple-queue" : "Parallel recording");
    return valid ? 0 : 1;
}
