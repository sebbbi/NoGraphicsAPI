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

constexpr uint32 segment_count = 3;
constexpr uint32 width = segment_count * 8;
constexpr uint32 height = 8;
constexpr uint64 pixel_bytes = width * height * 4;
constexpr uint64 timestamp_bytes = segment_count * 2 * sizeof(uint64);

struct ShaderCode
{
    uint32 words[8192];
    uint32 count = 0;
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

struct Worker
{
    gpu::CommandPool* pool = nullptr;
    gpu::CommandBuffer* commands = nullptr;
    gpu::PSO* pso = nullptr;
    const gpu::RenderingDesc* rendering = nullptr;
    uint64* timestamps = nullptr;
    uint32 index = 0;
    uint32 iteration = 0;
    StartGate* start = nullptr;
#if defined(_WIN32)
    HANDLE thread = nullptr;
#else
    pthread_t thread{};
#endif
};

struct Fixture
{
    gpu::Device* device = nullptr;
    gpu::CommandPool* readback_pool = nullptr;
    gpu::PSO* pso = nullptr;
    gpu::TextureHeap texture_heap{};
    gpu::Texture* target = nullptr;
    gpu::RenderView* view = nullptr;
    gpu::GpuHeap readback{};
    gpu::TimelinePoint completion{};
    uint64 previous_timestamp = ~uint64{0};
    Worker workers[segment_count]{};
};

bool load_shader(const char* path, ShaderCode& code) noexcept
{
    FILE* file = fopen(path, "rb");
    if (!file) { fprintf(stderr, "Cannot open render continuation shader: %s\n", path); return false; }
    fseek(file, 0, SEEK_END);
    const long bytes = ftell(file);
    rewind(file);
    if (bytes < 20 || uint64(bytes) > sizeof(code.words) || (bytes & 3)) { fclose(file); return false; }
    const bool read = fread(code.words, 1, size_t(bytes), file) == size_t(bytes);
    fclose(file);
    if (!read || code.words[0] != 0x07230203u) return false;
    code.count = uint32(bytes) / 4;
    return true;
}

void draw_segment(gpu::CommandBuffer* commands, const Worker& worker) noexcept
{
    gpu::RenderingFlags flags = gpu::RenderingFlags::none;
    if (worker.index != 0) flags = flags | gpu::RenderingFlags::resuming;
    if (worker.index != segment_count - 1) flags = flags | gpu::RenderingFlags::suspending;
    gpu::begin_render_pass(commands, *worker.rendering, flags);
    if ((worker.iteration & 1u) == 0) gpu::write_timestamp(commands, worker.timestamps);
    gpu::bind_pso(commands, worker.pso);
    gpu::set_scissor(commands, {.x = int32(worker.index * 8 + (worker.iteration & 1u) * 4), .width = 4, .height = height});
    gpu::draw(commands, {}, 3);
    if ((worker.iteration & 1u) == 0) gpu::write_timestamp(commands, worker.timestamps + 1);
    gpu::end_render_pass(commands);
}

#if defined(_WIN32)
DWORD WINAPI record_segment(void* argument) noexcept
#else
void* record_segment(void* argument) noexcept
#endif
{
    Worker* worker = static_cast<Worker*>(argument);
#if defined(_WIN32)
    WaitForSingleObject(worker->start->event, INFINITE);
#else
    pthread_mutex_lock(&worker->start->mutex);
    while (!worker->start->signaled)
        pthread_cond_wait(&worker->start->condition, &worker->start->mutex);
    pthread_mutex_unlock(&worker->start->mutex);
#endif
    gpu::reset_command_pool(worker->pool);
    worker->commands = gpu::begin_commands(worker->pool);
    if (worker->index == 0)
        gpu::barrier(worker->commands, gpu::Stage::transfer, gpu::Access::transfer_read, gpu::Stage::color_output, gpu::Access::color_write);
    draw_segment(worker->commands, *worker);
    gpu::end_commands(worker->commands);
    return 0;
}

bool record_workers(gpu::Span<Worker> workers) noexcept
{
    StartGate start{};
#if defined(_WIN32)
    start.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!start.event) return false;
#else
    if (pthread_mutex_init(&start.mutex, nullptr) != 0) return false;
    if (pthread_cond_init(&start.condition, nullptr) != 0)
    {
        pthread_mutex_destroy(&start.mutex);
        return false;
    }
#endif
    uint32 started = 0;
    for (; started < workers.size; ++started)
    {
        workers.data[started].start = &start;
#if defined(_WIN32)
        workers.data[started].thread = CreateThread(nullptr, 0, record_segment, workers.data + started, 0, nullptr);
        if (!workers.data[started].thread) break;
#else
        if (pthread_create(&workers.data[started].thread, nullptr, record_segment, workers.data + started) != 0) break;
#endif
    }
#if defined(_WIN32)
    SetEvent(start.event);
#else
    pthread_mutex_lock(&start.mutex);
    start.signaled = true;
    pthread_cond_broadcast(&start.condition);
    pthread_mutex_unlock(&start.mutex);
#endif
    bool valid = started == workers.size;
    for (uint32 index = 0; index < started; ++index)
    {
#if defined(_WIN32)
        valid = (WaitForSingleObject(workers.data[index].thread, INFINITE) == WAIT_OBJECT_0) && valid;
        CloseHandle(workers.data[index].thread);
#else
        valid = (pthread_join(workers.data[index].thread, nullptr) == 0) && valid;
#endif
    }
#if defined(_WIN32)
    CloseHandle(start.event);
#else
    pthread_cond_destroy(&start.condition);
    pthread_mutex_destroy(&start.mutex);
#endif
    if (!valid) fprintf(stderr, "Render continuation worker creation or join failed.\n");
    return valid;
}

bool initialize(Fixture& fixture) noexcept
{
    ShaderCode vertex{};
    ShaderCode fragment{};
    if (!load_shader(NOGRAPHICSAPI_CONTINUATION_VERTEX_SPV, vertex) || !load_shader(NOGRAPHICSAPI_CONTINUATION_FRAGMENT_SPV, fragment)) return false;
    fixture.pso = gpu::create_graphics_pso(fixture.device, {
        .vertex_spirv = {vertex.words, vertex.count},
        .fragment_spirv = {fragment.words, fragment.count},
        .color_targets = {{.format = gpu::Format::rgba8_unorm}},
    });
    if (!fixture.pso) return false;
    fixture.readback_pool = gpu::create_command_pool(fixture.device);
    if (!fixture.readback_pool) return false;
    for (uint32 index = 0; index < segment_count; ++index)
    {
        fixture.workers[index].pool = gpu::create_command_pool(fixture.device);
        if (!fixture.workers[index].pool) return false;
    }
    fixture.completion.semaphore = gpu::create_timeline_semaphore(fixture.device);
    if (!fixture.completion.semaphore) return false;
    fixture.readback = gpu::create_gpu_heap(fixture.device, pixel_bytes + timestamp_bytes, gpu::MemoryType::readback);
    const gpu::TextureDesc target_desc{
        .extent = {.x = width, .y = height, .z = 1},
        .usage = gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
    };
    fixture.texture_heap = gpu::create_texture_heap(fixture.device, gpu::get_texture_size_align(fixture.device, target_desc).size);
    gpu::CommandBuffer* commands = gpu::begin_commands(fixture.readback_pool);
    fixture.target = gpu::create_texture(commands, target_desc, fixture.texture_heap, 0);
    if (!fixture.target) return false;
    fixture.view = gpu::create_render_view(fixture.target);
    if (!fixture.view) return false;
    gpu::end_commands(commands);
    ++fixture.completion.value;
    gpu::submit(fixture.device, {.commands = {commands}, .completion = fixture.completion});
    gpu::wait_timeline(fixture.completion);
    return true;
}

bool render_iteration(Fixture& fixture, uint32 iteration) noexcept
{
    memset(fixture.readback.range.cpu, 0xff, pixel_bytes + timestamp_bytes);
    const gpu::ColorAttachment color{.render_view = fixture.view, .load = gpu::LoadOp::clear};
    const gpu::RenderingDesc rendering{.colors = {&color, 1}};
    for (uint32 index = 0; index < segment_count; ++index)
    {
        fixture.workers[index].pso = fixture.pso;
        fixture.workers[index].rendering = &rendering;
        fixture.workers[index].timestamps = reinterpret_cast<uint64*>(fixture.readback.range.gpu + pixel_bytes) + index * 2;
        fixture.workers[index].index = index;
        fixture.workers[index].iteration = iteration;
    }
    if (iteration < 4)
    {
        if (!record_workers(fixture.workers)) return false;
    }
    else
    {
        gpu::reset_command_pool(fixture.workers[0].pool);
        fixture.workers[0].commands = gpu::begin_commands(fixture.workers[0].pool);
        gpu::barrier(fixture.workers[0].commands, gpu::Stage::transfer, gpu::Access::transfer_read, gpu::Stage::color_output, gpu::Access::color_write);
        for (uint32 index = 0; index < segment_count; ++index)
            draw_segment(fixture.workers[0].commands, fixture.workers[index]);
        gpu::end_commands(fixture.workers[0].commands);
    }
    gpu::reset_command_pool(fixture.readback_pool);
    gpu::CommandBuffer* readback = gpu::begin_commands(fixture.readback_pool);
    gpu::barrier(readback, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::transfer, gpu::Access::transfer_read);
    gpu::copy_texture_to_memory(readback, fixture.target, {.gpu = fixture.readback.range.gpu, .size = pixel_bytes});
    gpu::barrier(readback, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
    gpu::end_commands(readback);
    ++fixture.completion.value;
    gpu::CommandBuffer* commands[segment_count + 1]{fixture.workers[0].commands, fixture.workers[1].commands, fixture.workers[2].commands, readback};
    if (iteration >= 4) commands[1] = readback;
    gpu::submit(fixture.device, {.commands = {commands, iteration < 4 ? segment_count + 1 : 2}, .completion = fixture.completion});
    gpu::wait_timeline(fixture.completion);
    bool valid = true;
    for (uint32 y = 0; y < height; ++y)
        for (uint32 x = 0; x < width; ++x)
        {
            const uint8* pixel = fixture.readback.range.cpu + (y * width + x) * 4;
            const bool covered = x % 8 / 4 == (iteration & 1u);
            valid &= pixel[0] == (covered ? 255 : 0) && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255;
        }
    const uint64* timestamps = reinterpret_cast<const uint64*>(fixture.readback.range.cpu + pixel_bytes);
    for (uint32 index = 0; index < segment_count * 2; ++index)
    {
        if (iteration & 1u) valid &= timestamps[index] == ~uint64{0};
        else valid &= timestamps[index] != ~uint64{0} && (index == 0 || timestamps[index] >= timestamps[index - 1]);
    }
    if ((iteration & 1u) == 0)
    {
        valid &= timestamps[0] != fixture.previous_timestamp;
        fixture.previous_timestamp = timestamps[0];
    }
    if (!valid) fprintf(stderr, "Render continuation pixel or timestamp readback failed in iteration %u.\n", iteration);
    return valid;
}

} // namespace

int main()
{
    const gpu::DeviceInit initialized = gpu::create_device({.timestamp_query_count = segment_count * 2});
    if (initialized.error == gpu::Error::unsupported) return 77;
    if (initialized.error != gpu::Error::none) return 1;
    Fixture fixture{.device = initialized.device};
    bool valid = initialize(fixture);
    for (uint32 iteration = 0; valid && iteration < 6; ++iteration)
        valid = render_iteration(fixture, iteration);
    gpu::wait_idle(fixture.device);
    for (uint32 index = 0; index < segment_count; ++index)
        gpu::destroy_command_pool(fixture.workers[index].pool);
    gpu::destroy_command_pool(fixture.readback_pool);
    gpu::destroy_render_view(fixture.view);
    gpu::destroy_texture(fixture.target);
    gpu::destroy_texture_heap(fixture.texture_heap);
    gpu::destroy_gpu_heap(fixture.readback);
    gpu::destroy_timeline_semaphore(fixture.completion.semaphore);
    gpu::destroy_pso(fixture.pso);
    gpu::destroy_device(fixture.device);
    printf("Parallel render continuation, timestamp readback and pool reuse: %s\n", valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
