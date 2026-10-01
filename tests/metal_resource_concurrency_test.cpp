#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include "queue_family_shared.h"
#include "shader_code.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

namespace
{
constexpr uint32 worker_count = 4;

struct Gate
{
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    uint32 arrived = 0;
    uint32 expected = 0;
    bool open = false;
    bool cancelled = false;
};

void wait_gate(Gate& gate) noexcept
{
    pthread_mutex_lock(&gate.mutex);
    if (++gate.arrived == gate.expected)
    {
        gate.open = true;
        pthread_cond_broadcast(&gate.condition);
    }
    while (!gate.open) pthread_cond_wait(&gate.condition, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex);
}

void destroy_gate(Gate& gate) noexcept
{
    pthread_cond_destroy(&gate.condition);
    pthread_mutex_destroy(&gate.mutex);
}

struct Worker
{
    void (*run)(void*) noexcept = nullptr;
    void* context = nullptr;
    Gate* start = nullptr;
    pthread_t thread{};
};

void* run_worker(void* argument) noexcept
{
    Worker& worker = *static_cast<Worker*>(argument);
    wait_gate(*worker.start);
    if (!worker.start->cancelled) worker.run(worker.context);
    return nullptr;
}

bool run_workers(Worker (&workers)[worker_count]) noexcept
{
    Gate start{.expected = worker_count + 1};
    uint32 started = 0;
    for (; started < worker_count; ++started)
    {
        workers[started].start = &start;
        if (pthread_create(&workers[started].thread, nullptr, run_worker, workers + started) != 0) break;
    }
    if (started != worker_count)
    {
        pthread_mutex_lock(&start.mutex);
        start.cancelled = true;
        start.open = true;
        pthread_cond_broadcast(&start.condition);
        pthread_mutex_unlock(&start.mutex);
        for (uint32 i = 0; i < started; ++i) pthread_join(workers[i].thread, nullptr);
        destroy_gate(start);
        fprintf(stderr, "Could not start all resource workers.\n");
        return false;
    }
    wait_gate(start);
    bool valid = true;
    for (Worker& worker : workers) valid = pthread_join(worker.thread, nullptr) == 0 && valid;
    destroy_gate(start);
    return valid;
}

constexpr uint64 copy_bytes = 4096;
constexpr uint64 copy_readback_bytes = copy_bytes + 128;
constexpr uint32 churn_count = 2048;
constexpr uint32 churn_batch_size = 4;
constexpr uint64 churn_heap_bytes = 4096 + 3 * 256;

struct CopyWorker
{
    gpu::Device* device = nullptr;
    uint32 queue = 0;
    uint32* writers_done = nullptr;
    gpu::GpuHeap upload{};
    gpu::GpuHeap scratch{};
    gpu::GpuHeap readback{};
    uint32 iterations = 0;
    bool valid = true;
};

struct ChurnWorker
{
    gpu::Device* device = nullptr;
    Gate* peak = nullptr;
    uint32* writers_done = nullptr;
    uint32 queue = 0;
    gpu::GpuHeap upload{};
    gpu::GpuHeap readback{};
    bool valid = true;
};

void churn_heaps(void* argument) noexcept
{
    ChurnWorker& worker = *static_cast<ChurnWorker*>(argument);
    gpu::CommandPool* pool = gpu::create_command_pool(worker.device, worker.queue);
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(worker.device);
    for (uint32 iteration = 0; iteration < churn_count / churn_batch_size; ++iteration)
    {
        gpu::GpuHeap heaps[churn_batch_size]{};
        for (uint32 i = 0; i < churn_batch_size; ++i)
            heaps[i] = gpu::create_gpu_heap(worker.device, 4096 + ((iteration + i + worker.queue) & 3u) * 256,
                (iteration + i) & 1u ? gpu::MemoryType::gpu_only : gpu::MemoryType::cpu_visible);
        if (iteration == 0) wait_gate(*worker.peak);
        for (uint32 i = 0; i < worker.upload.range.size / sizeof(uint32); ++i)
            reinterpret_cast<uint32*>(worker.upload.range.cpu)[i] = (worker.queue << 28) | (iteration << 13) | i;
        memset(worker.readback.range.cpu, 0xcd, worker.readback.range.size);
        gpu::CommandBuffer* commands = gpu::begin_commands(pool);
        for (uint32 i = 0; i < churn_batch_size; ++i)
            gpu::copy_memory(commands, {.gpu = worker.upload.range.gpu + i * churn_heap_bytes, .size = heaps[i].range.size}, gpu::gpu_range(heaps[i]));
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::transfer, gpu::Access::transfer_read);
        for (uint32 i = 0; i < churn_batch_size; ++i)
            gpu::copy_memory(commands, gpu::gpu_range(heaps[i]),
                {.gpu = worker.readback.range.gpu + 64 + i * churn_heap_bytes, .size = heaps[i].range.size});
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
        gpu::end_commands(commands);
        const gpu::TimelinePoint completion{.semaphore = timeline, .value = iteration + 1};
        gpu::submit(worker.device, {.commands = {commands}, .completion = completion}, worker.queue);
        gpu::wait_timeline(completion);
        for (uint32 i = 0; i < churn_batch_size; ++i)
        {
            worker.valid = memcmp(worker.upload.range.cpu + i * churn_heap_bytes,
                worker.readback.range.cpu + 64 + i * churn_heap_bytes, heaps[i].range.size) == 0 && worker.valid;
            for (uint64 guard = heaps[i].range.size; guard < churn_heap_bytes; ++guard)
                worker.valid = worker.readback.range.cpu[64 + i * churn_heap_bytes + guard] == 0xcd && worker.valid;
        }
        for (uint32 guard = 0; guard < 64; ++guard)
            worker.valid = worker.readback.range.cpu[guard] == 0xcd &&
                worker.readback.range.cpu[64 + churn_batch_size * churn_heap_bytes + guard] == 0xcd && worker.valid;
        gpu::reset_command_pool(pool);
        for (uint32 i = 0; i < churn_batch_size; ++i)
            gpu::destroy_gpu_heap(heaps[(iteration + (worker.queue & 1u ? churn_batch_size - 1 - i : i)) % churn_batch_size]);
    }
    gpu::destroy_command_pool(pool);
    gpu::destroy_timeline_semaphore(timeline);
    __atomic_fetch_add(worker.writers_done, 1u, __ATOMIC_RELEASE);
}

void copy_stable_heaps(void* argument) noexcept
{
    CopyWorker& worker = *static_cast<CopyWorker*>(argument);
    gpu::CommandPool* pool = gpu::create_command_pool(worker.device, worker.queue);
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(worker.device);
    const uint64 offsets[]{0, 4, copy_bytes / 2, copy_bytes - 4};
    while (worker.iterations < 256 || __atomic_load_n(worker.writers_done, __ATOMIC_ACQUIRE) != 2)
    {
        memset(worker.readback.range.cpu, 0xcd, copy_readback_bytes);
        gpu::CommandBuffer* commands = gpu::begin_commands(pool);
        gpu::copy_memory(commands, gpu::gpu_range(worker.upload), gpu::gpu_range(worker.scratch));
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::transfer, gpu::Access::transfer_read);
        gpu::copy_memory(commands, gpu::gpu_range(worker.scratch), {.gpu = worker.readback.range.gpu, .size = copy_bytes});
        for (uint32 i = 0; i < 4; ++i)
            gpu::copy_memory(commands, {.gpu = worker.scratch.range.gpu + offsets[i], .size = 4},
                             {.gpu = worker.readback.range.gpu + copy_bytes + i * 16 + 4, .size = 4});
        gpu::copy_memory(commands, {.gpu = worker.upload.range.gpu + copy_bytes - 4, .size = 4},
                         {.gpu = worker.readback.range.gpu + copy_readback_bytes - 4, .size = 4});
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
        gpu::end_commands(commands);
        const gpu::TimelinePoint completion{.semaphore = timeline, .value = ++worker.iterations};
        gpu::submit(worker.device, {.commands = {commands}, .completion = completion}, worker.queue);
        gpu::wait_timeline(completion);
        worker.valid = memcmp(worker.upload.range.cpu, worker.readback.range.cpu, copy_bytes) == 0 && worker.valid;
        for (uint32 i = 0; i < (copy_readback_bytes - copy_bytes) / 4; ++i)
        {
            uint32 expected = 0xcdcdcdcdu;
            if (i < 16 && i % 4 == 1) expected = *reinterpret_cast<const uint32*>(worker.upload.range.cpu + offsets[i / 4]);
            if (i == (copy_readback_bytes - copy_bytes) / 4 - 1)
                expected = *reinterpret_cast<const uint32*>(worker.upload.range.cpu + copy_bytes - 4);
            worker.valid = reinterpret_cast<const uint32*>(worker.readback.range.cpu + copy_bytes)[i] == expected && worker.valid;
        }
        gpu::reset_command_pool(pool);
    }
    gpu::destroy_command_pool(pool);
    gpu::destroy_timeline_semaphore(timeline);
}

bool test_registry(gpu::Device* device) noexcept
{
    uint32 writers_done = 0;
    Gate peak{.expected = 2};
    CopyWorker copies[2]{{.device = device, .writers_done = &writers_done}, {.device = device, .queue = 1, .writers_done = &writers_done}};
    for (CopyWorker& copy : copies)
    {
        copy.upload = gpu::create_gpu_heap(device, copy_bytes);
        copy.scratch = gpu::create_gpu_heap(device, copy_bytes, gpu::MemoryType::gpu_only);
        copy.readback = gpu::create_gpu_heap(device, copy_readback_bytes, gpu::MemoryType::readback);
        for (uint32 i = 0; i < copy_bytes / 4; ++i)
            reinterpret_cast<uint32*>(copy.upload.range.cpu)[i] = 0x98760000u + copy.queue * 0x1000u + i;
    }
    ChurnWorker churn[2]{{.device = device, .peak = &peak, .writers_done = &writers_done, .queue = 2},
                         {.device = device, .peak = &peak, .writers_done = &writers_done, .queue = 3}};
    for (ChurnWorker& writer : churn)
    {
        writer.upload = gpu::create_gpu_heap(device, churn_batch_size * churn_heap_bytes);
        writer.readback = gpu::create_gpu_heap(device, churn_batch_size * churn_heap_bytes + 128, gpu::MemoryType::readback);
    }
    // Six stable copy heaps + four writer staging heaps + 46 retained heaps + two four-heap batches reach 64.
    gpu::GpuHeap retained[46]{};
    for (gpu::GpuHeap& heap : retained) heap = gpu::create_gpu_heap(device, 256);
    Worker workers[worker_count]{{.run = copy_stable_heaps, .context = copies}, {.run = copy_stable_heaps, .context = copies + 1},
                                 {.run = churn_heaps, .context = churn}, {.run = churn_heaps, .context = churn + 1}};
    const bool valid = run_workers(workers) && copies[0].valid && copies[1].valid && churn[0].valid && churn[1].valid;
    printf("Registry: 64 live heaps, two writers verified %u heap lifetimes, stable copy iterations %u/%u: %s\n",
           churn_count * 2, copies[0].iterations, copies[1].iterations, valid ? "passed" : "failed");
    for (const gpu::GpuHeap& heap : retained) gpu::destroy_gpu_heap(heap);
    for (const ChurnWorker& writer : churn)
    {
        gpu::destroy_gpu_heap(writer.readback);
        gpu::destroy_gpu_heap(writer.upload);
    }
    for (const CopyWorker& copy : copies)
    {
        gpu::destroy_gpu_heap(copy.readback);
        gpu::destroy_gpu_heap(copy.scratch);
        gpu::destroy_gpu_heap(copy.upload);
    }
    destroy_gate(peak);
    return valid;
}

constexpr uint32 timestamp_contexts = 9;
constexpr uint32 timestamp_queries = 513;
constexpr uint64 timestamp_stride = (timestamp_queries + 3) * sizeof(uint64);

struct TimestampWorker
{
    gpu::Device* device = nullptr;
    Gate* peak = nullptr;
    uint32 queue = 0;
    bool valid = true;
};

void churn_timestamp_contexts(void* argument) noexcept
{
    TimestampWorker& worker = *static_cast<TimestampWorker*>(argument);
    const gpu::GpuHeap upload = gpu::create_gpu_heap(worker.device, timestamp_contexts * 4);
    const gpu::GpuHeap readback = gpu::create_gpu_heap(worker.device, timestamp_contexts * timestamp_stride, gpu::MemoryType::readback);
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(worker.device);
    uint64 previous[timestamp_contexts][timestamp_queries]{};
    for (uint32 iteration = 0; iteration < 8; ++iteration)
    {
        gpu::CommandPool* pool = gpu::create_command_pool(worker.device, worker.queue);
        gpu::CommandBuffer* batch[timestamp_contexts]{};
        memset(readback.range.cpu, 0xff, readback.range.size);
        for (uint32 context = 0; context < timestamp_contexts; ++context)
        {
            reinterpret_cast<uint32*>(upload.range.cpu)[context] = (worker.queue + 1) * 10000 + iteration * 100 + context;
            gpu::CommandBuffer* commands = gpu::begin_commands(pool);
            batch[iteration & 1u ? timestamp_contexts - context - 1 : context] = commands;
            for (uint32 index = 0; index < timestamp_queries; ++index)
            {
                if (index == timestamp_queries / 2)
                    gpu::copy_memory(commands, {.gpu = upload.range.gpu + context * 4, .size = 4},
                        {.gpu = readback.range.gpu + context * timestamp_stride + (timestamp_queries + 1) * sizeof(uint64), .size = 4});
                gpu::write_timestamp(commands, reinterpret_cast<uint64*>(readback.range.cpu + context * timestamp_stride) + index + 1);
            }
            gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
            gpu::end_commands(commands);
        }
        // Every reserved slot is written while 36 contexts span multiple native counter pages.
        if (iteration == 0) wait_gate(*worker.peak);
        const gpu::TimelinePoint completion{.semaphore = timeline, .value = iteration + 1};
        gpu::submit(worker.device, {.commands = batch, .completion = completion}, worker.queue);
        gpu::wait_timeline(completion);
        gpu::read_timestamps(pool);
        for (uint32 context = 0; context < timestamp_contexts; ++context)
        {
            const uint64* values = reinterpret_cast<const uint64*>(readback.range.cpu + context * timestamp_stride);
            bool valid = values[0] == ~uint64{0} && values[timestamp_queries + 2] == ~uint64{0} &&
                uint32(values[timestamp_queries + 1] >> 32) == ~uint32{0} &&
                uint32(values[timestamp_queries + 1]) == reinterpret_cast<const uint32*>(upload.range.cpu)[context];
            for (uint32 index = 0; index < timestamp_queries; ++index)
            {
                valid = (gpu::get_device_caps(worker.device).timestamp_period_ns != 0
                    ? values[index + 1] > previous[context][index] && values[index + 1] != ~uint64{0} &&
                      (!index || values[index + 1] >= values[index]) : values[index + 1] == ~uint64{0}) && valid;
                previous[context][index] = values[index + 1];
            }
            if (!valid) fprintf(stderr, "Timestamp slot collision or reuse failed on queue %u, iteration %u, context %u.\n",
                worker.queue, iteration, context);
            worker.valid = valid && worker.valid;
        }
        gpu::destroy_command_pool(pool);
    }
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(upload);
    gpu::destroy_timeline_semaphore(timeline);
}

bool test_timestamps(gpu::Device* device) noexcept
{
    Gate peak{.expected = worker_count};
    TimestampWorker contexts[worker_count]{};
    Worker workers[worker_count]{};
    for (uint32 i = 0; i < worker_count; ++i)
    {
        contexts[i] = {.device = device, .peak = &peak, .queue = i};
        workers[i] = {.run = churn_timestamp_contexts, .context = contexts + i};
    }
    bool valid = run_workers(workers);
    for (const TimestampWorker& context : contexts) valid = context.valid && valid;
    destroy_gate(peak);
    printf("Concurrent timestamps: %u slots across %u contexts, eight allocation/retirement cycles: %s\n",
        worker_count * timestamp_contexts * timestamp_queries, worker_count * timestamp_contexts, valid ? "passed" : "failed");
    return valid;
}

constexpr uint64 texture_bytes = queue_test_width * queue_test_height * 4;
constexpr gpu::TextureDesc texture_desc{
    .extent = {.x = queue_test_width, .y = queue_test_height, .z = 1},
    .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source | gpu::TextureUsage::transfer_destination,
};

struct DescriptorWorker
{
    gpu::Device* device = nullptr;
    gpu::PSO* pso = nullptr;
    const gpu::TextureHeap* heap = nullptr;
    gpu::TextureDescriptorHeap* descriptors = nullptr;
    uint64 texture_stride = 0;
    uint32 queue = 0;
    bool valid = true;
};

void update_descriptors(void* argument) noexcept
{
    DescriptorWorker& worker = *static_cast<DescriptorWorker*>(argument);
    gpu::CommandPool* pool = gpu::create_command_pool(worker.device, worker.queue);
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(worker.device);
    const gpu::GpuHeap upload = gpu::create_gpu_heap(worker.device, texture_bytes * 2 + sizeof(QueueFamilyRoot));
    const gpu::GpuHeap readback = gpu::create_gpu_heap(worker.device, texture_bytes * 2 + 32, gpu::MemoryType::readback);
    for (uint32 i = 0; i < texture_bytes * 2 / 4; ++i)
        reinterpret_cast<uint32*>(upload.range.cpu)[i] = 0xff000000u | ((worker.queue + 1) << 16) | (i * 31u & 0xffffu);
    gpu::Texture* textures[4]{};
    for (uint32 iteration = 0; iteration < 16; ++iteration)
    {
        gpu::CommandBuffer* commands = gpu::begin_commands(pool);
        if (iteration == 0)
        {
            for (uint32 i = 0; i < 4; ++i)
                textures[i] = gpu::create_texture(commands, texture_desc, *worker.heap, (worker.queue * 4 + i) * worker.texture_stride);
            for (uint32 i = 0; i < 2; ++i)
                gpu::copy_memory_to_texture(commands, {.gpu = upload.range.gpu + i * texture_bytes, .size = texture_bytes}, textures[i]);
        }
        const uint32 source = (iteration + worker.queue) & 1u;
        const uint32 slot = worker.queue * 4;
        gpu::write_texture_descriptor(worker.descriptors, slot, textures[source], gpu::TextureDescriptorType::sampled);
        gpu::write_texture_descriptor(worker.descriptors, slot + 1, textures[source + 2], gpu::TextureDescriptorType::storage);
        gpu::copy_texture_descriptors(worker.descriptors, slot, worker.descriptors, slot + 2, 2);
        memset(readback.range.cpu, 0xcd, readback.range.size);
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::compute, gpu::Access::shader_read);
        gpu::set_texture_descriptor_heap(commands, worker.descriptors);
        gpu::bind_pso(commands, worker.pso);
        *reinterpret_cast<QueueFamilyRoot*>(upload.range.cpu + texture_bytes * 2) = {
            .source = reinterpret_cast<uint32*>(upload.range.gpu + source * texture_bytes),
            .destination = reinterpret_cast<uint32*>(readback.range.gpu), .source_texture = slot + 2, .destination_texture = slot + 3,
        };
        gpu::dispatch(commands, upload.range.gpu + texture_bytes * 2,
            {.x = (queue_test_width + 7) / 8, .y = (queue_test_height + 7) / 8, .z = 1});
        gpu::barrier(commands, gpu::Stage::compute, gpu::Access::shader_write, gpu::Stage::transfer, gpu::Access::transfer_read);
        gpu::copy_texture_to_memory(commands, textures[source + 2], {.gpu = readback.range.gpu + texture_bytes + 16, .size = texture_bytes});
        gpu::barrier(commands, gpu::Stage::compute | gpu::Stage::transfer, gpu::Access::shader_write | gpu::Access::transfer_write,
                     gpu::Stage::host, gpu::Access::host_read);
        gpu::end_commands(commands);
        const gpu::TimelinePoint completion{.semaphore = timeline, .value = iteration + 1};
        gpu::submit(worker.device, {.commands = {commands}, .completion = completion}, worker.queue);
        gpu::wait_timeline(completion);
        const uint32* input = reinterpret_cast<const uint32*>(upload.range.cpu + source * texture_bytes);
        const uint32* output = reinterpret_cast<const uint32*>(readback.range.cpu);
        const uint32* pixels = reinterpret_cast<const uint32*>(readback.range.cpu + texture_bytes + 16);
        for (uint32 i = 0; i < texture_bytes / 4; ++i)
            worker.valid = output[i] == input[i] * 3u + 7u &&
                           pixels[i] == ((input[i] & 0xff00ff00u) | (input[i] >> 16 & 0xffu) | (input[i] & 0xffu) << 16) && worker.valid;
        for (uint32 i = 0; i < 16; ++i)
            worker.valid = readback.range.cpu[texture_bytes + i] == 0xcd && readback.range.cpu[texture_bytes * 2 + 16 + i] == 0xcd && worker.valid;
        gpu::reset_command_pool(pool);
    }
    gpu::destroy_command_pool(pool);
    for (gpu::Texture* texture : textures) gpu::destroy_texture(texture);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(upload);
    gpu::destroy_timeline_semaphore(timeline);
}

bool test_descriptors(gpu::Device* device) noexcept
{
    gpu::Span<byte> code = load_test_shader(NOGRAPHICSAPI_CONCURRENCY_SHADER);
    if (!code.data) return false;
    gpu::PSO* pso = gpu::create_compute_pso(device, {.code = {code.data, code.size}, .entry_point = "computeMain",
                                                   .threadgroup_size = {.x = 8, .y = 8, .z = 1}});
    free(code.data);
    if (!pso) return false;
    const gpu::SizeAlign requirements = gpu::get_texture_size_align(device, texture_desc);
    const uint64 stride = (requirements.size + requirements.align - 1) / requirements.align * requirements.align;
    const gpu::TextureHeap heap = gpu::create_texture_heap(device, worker_count * 4 * stride);
    gpu::TextureDescriptorHeap* descriptors = gpu::create_texture_descriptor_heap(device, worker_count * 4);
    DescriptorWorker contexts[worker_count]{};
    Worker workers[worker_count]{};
    for (uint32 i = 0; i < worker_count; ++i)
    {
        contexts[i] = {.device = device, .pso = pso, .heap = &heap, .descriptors = descriptors, .texture_stride = stride, .queue = i};
        workers[i] = {.run = update_descriptors, .context = contexts + i};
    }
    bool valid = run_workers(workers);
    for (const DescriptorWorker& context : contexts) valid = context.valid && valid;
    gpu::destroy_texture_descriptor_heap(descriptors);
    gpu::destroy_texture_heap(heap);
    gpu::destroy_pso(pso);
    printf("Shared placement heap and disjoint descriptor updates with GPU readback: %s\n", valid ? "passed" : "failed");
    return valid;
}
}

int main(int argc, char** argv)
{
    const char* mode = argc == 2 ? argv[1] : "all";
    if (strcmp(mode, "all") && strcmp(mode, "registry") && strcmp(mode, "timestamps") && strcmp(mode, "descriptors")) return 1;
    const gpu::DeviceInit initialized = gpu::create_device({.desired_queue_count = worker_count, .timestamp_query_count = timestamp_queries});
    if (initialized.error == gpu::Error::unsupported) return 77;
    if (initialized.error != gpu::Error::none) return 1;
    gpu::Device* device = initialized.device;
    if (gpu::get_device_caps(device).general_queue_count < worker_count)
    {
        gpu::wait_idle(device);
        gpu::destroy_device(device);
        return 77;
    }
    bool valid = true;
    if (!strcmp(mode, "all") || !strcmp(mode, "registry")) valid = test_registry(device) && valid;
    if (!strcmp(mode, "all") || !strcmp(mode, "timestamps")) valid = test_timestamps(device) && valid;
    if (!strcmp(mode, "all") || !strcmp(mode, "descriptors")) valid = test_descriptors(device) && valid;
    gpu::wait_idle(device);
    gpu::destroy_device(device);
    return valid ? 0 : 1;
}
