#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include "task_shader_shared.h"
#include "task_mesh_shared.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static_assert(sizeof(TaskShaderRoot) == 32 && sizeof(TaskMeshRoot) == 32);
static_assert(offsetof(TaskShaderRoot, data) == 0 && offsetof(TaskMeshRoot, data) == 0);
static_assert(offsetof(TaskShaderRoot, count) == 8 && offsetof(TaskMeshRoot, count) == 8);
static_assert(offsetof(TaskShaderRoot, visible_mask) == 12 && offsetof(TaskMeshRoot, visible_mask) == 12);
static_assert(offsetof(TaskShaderRoot, color) == 16 && offsetof(TaskMeshRoot, color) == 16);
static_assert(sizeof(TaskTestData) == 784 && sizeof(TaskTestPayload) == 128);

static const uint64 pixel_offset = 1024;
static const uint64 pixel_bytes = task_test_width * task_test_height * 4;
static const uint64 data_bytes = sizeof(TaskTestData) + sizeof(gpu::uint32x3);

struct ShaderCode
{
    uint32 words[8192];
    uint32 count = 0;
};

struct Fixture
{
    gpu::Device* device = nullptr;
    gpu::PSO* pso = nullptr;
    gpu::TextureHeap texture_heap{};
    gpu::Texture* target = nullptr;
    gpu::RenderView* view = nullptr;
    gpu::GpuHeap seed{};
    gpu::GpuHeap data{};
    gpu::GpuHeap readback{};
    gpu::TimelinePoint completion{};
};

static bool load_shader(const char* path, ShaderCode& code) noexcept
{
    FILE* file = fopen(path, "rb");
    if (!file) { fprintf(stderr, "Cannot open task test shader: %s\n", path); return false; }
    fseek(file, 0, SEEK_END);
    long bytes = ftell(file);
    rewind(file);
    if (bytes < 20 || uint64(bytes) > sizeof(code.words) || (bytes & 3)) { fclose(file); return false; }
    bool read = fread(code.words, 1, size_t(bytes), file) == size_t(bytes);
    fclose(file);
    if (!read || code.words[0] != 0x07230203u) return false;
    code.count = uint32(bytes) / 4;
    return true;
}

static bool initialize(Fixture& fixture) noexcept
{
    ShaderCode task{};
    ShaderCode mesh{};
    ShaderCode fragment{};
    if (!load_shader(NOGRAPHICSAPI_TASK_TEST_TASK_SPV, task) || !load_shader(NOGRAPHICSAPI_TASK_TEST_MESH_SPV, mesh)
        || !load_shader(NOGRAPHICSAPI_TASK_TEST_FRAGMENT_SPV, fragment)) return false;
    fixture.pso = gpu::create_mesh_pso(fixture.device, {
        .task_spirv = {task.words, task.count},
        .mesh_spirv = {mesh.words, mesh.count},
        .fragment_spirv = {fragment.words, fragment.count},
        .color_targets = {{.format = gpu::Format::rgba8_unorm}},
    });
    if (!fixture.pso) return false;
    const gpu::TextureDesc target_desc{
        .extent = {.x = task_test_width, .y = task_test_height, .z = 1},
        .usage = gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
    };
    fixture.texture_heap = gpu::create_texture_heap(fixture.device, gpu::get_texture_size_align(fixture.device, target_desc).size);
    fixture.target = gpu::create_texture(fixture.device, target_desc, fixture.texture_heap, 0);
    if (!fixture.target) return false;
    fixture.view = gpu::create_render_view(fixture.target);
    if (!fixture.view) return false;
    fixture.completion.semaphore = gpu::create_timeline_semaphore(fixture.device);
    if (!fixture.completion.semaphore) return false;
    fixture.seed = gpu::create_gpu_heap(fixture.device, data_bytes);
    fixture.data = gpu::create_gpu_heap(fixture.device, data_bytes, gpu::MemoryType::gpu_only);
    fixture.readback = gpu::create_gpu_heap(fixture.device, pixel_offset + pixel_bytes, gpu::MemoryType::readback);
    return true;
}

static bool render_case(Fixture& fixture, uint32 count, uint32 visible_mask, uint32 scenario, bool indirect) noexcept
{
    TaskTestData& seed = *reinterpret_cast<TaskTestData*>(fixture.seed.range.cpu);
    memset(&seed, 0, sizeof(seed));
    for (uint32 id = 0; id < task_test_capacity; ++id) seed.visibility[id] = 1u << (id % 3);
    for (uint32 batch = 0; batch < task_test_batches; ++batch) seed.batch_counts[batch] = ~0u;
    uint32 batches = count ? (count + task_test_batch_size - 1) / task_test_batch_size : 1;
    *reinterpret_cast<gpu::uint32x3*>(fixture.seed.range.cpu + sizeof(TaskTestData)) = {.x = batches, .y = 1, .z = 1};
    memset(fixture.readback.range.cpu, 0xa5, size_t(fixture.readback.range.size));
    const TaskShaderRoot root{
        .data = reinterpret_cast<TaskTestData*>(fixture.data.range.gpu),
        .count = count,
        .visible_mask = visible_mask,
        .color = {.x = 1.0f, .y = float(scenario & 1u), .z = float(indirect), .w = 1.0f},
    };
    gpu::CommandBuffer* commands = gpu::begin_commands(fixture.device);
    gpu::barrier(commands, gpu::Stage::task | gpu::Stage::mesh | gpu::Stage::transfer, gpu::Access::shader_write | gpu::Access::transfer_read,
        gpu::Stage::transfer, gpu::Access::transfer_write);
    gpu::copy_memory(commands, gpu::gpu_range(fixture.seed), gpu::gpu_range(fixture.data));
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::task | gpu::Stage::mesh | gpu::Stage::indirect,
        gpu::Access::shader_read | gpu::Access::shader_write | gpu::Access::indirect_read);
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_read, gpu::Stage::color_output, gpu::Access::color_write);
    gpu::begin_render_pass(commands, {.colors = {{.render_view = fixture.view, .load = gpu::LoadOp::clear}}});
    gpu::set_viewport(commands, {.width = float(task_test_width), .height = float(task_test_height)});
    gpu::set_scissor(commands, {.width = task_test_width, .height = task_test_height});
    gpu::bind_pso(commands, fixture.pso);
    if (indirect)
        gpu::draw_meshlets_indirect(commands, root, {.gpu = fixture.data.range.gpu + sizeof(TaskTestData), .size = sizeof(gpu::uint32x3)});
    else
        gpu::draw_meshlets(commands, root, {.x = batches, .y = 1, .z = 1});
    gpu::end_render_pass(commands);
    gpu::barrier(commands, gpu::Stage::task | gpu::Stage::mesh, gpu::Access::shader_write, gpu::Stage::transfer, gpu::Access::transfer_read);
    gpu::barrier(commands, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::transfer, gpu::Access::transfer_read);
    gpu::copy_memory(commands, {.gpu = fixture.data.range.gpu, .size = sizeof(TaskTestData)},
        {.gpu = fixture.readback.range.gpu, .size = sizeof(TaskTestData)});
    gpu::copy_texture_to_memory(commands, fixture.target, {.gpu = fixture.readback.range.gpu + pixel_offset, .size = pixel_bytes});
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
    ++fixture.completion.value;
    gpu::submit({commands}, fixture.completion);
    gpu::wait_timeline(fixture.completion);

    const TaskTestData& actual = *reinterpret_cast<const TaskTestData*>(fixture.readback.range.cpu);
    bool valid = memcmp(actual.visibility, seed.visibility, sizeof(seed.visibility)) == 0 && actual.visits[task_test_capacity] == 0;
    for (uint32 batch = 0; batch < task_test_batches; ++batch)
    {
        uint32 expected = 0;
        for (uint32 lane = 0; lane < task_test_batch_size; ++lane)
        {
            uint32 id = batch * task_test_batch_size + lane;
            uint32 visible = uint32(id < count && (seed.visibility[id] & visible_mask) != 0);
            expected += visible;
            valid &= actual.visits[id] == visible;
        }
        valid &= actual.batch_counts[batch] == (batch < batches ? expected : ~0u);
    }
    for (uint64 offset = sizeof(TaskTestData); offset < pixel_offset; ++offset) valid &= fixture.readback.range.cpu[offset] == 0xa5;
    const uint8* pixels = reinterpret_cast<const uint8*>(fixture.readback.range.cpu + pixel_offset);
    for (uint32 y = 0; y < task_test_height; ++y)
        for (uint32 x = 0; x < task_test_width; ++x)
        {
            bool covered = x / 4 < count && (seed.visibility[x / 4] & visible_mask) != 0 && (x % 4 == 1 || x % 4 == 2) && y >= 2 && y < 6;
            const uint8* pixel = pixels + (y * task_test_width + x) * 4;
            valid &= pixel[0] == (covered ? 255 : 0) && pixel[1] == (covered && (scenario & 1u) ? 255 : 0)
                && pixel[2] == (covered && indirect ? 255 : 0) && pixel[3] == 255;
        }
    if (!valid) fprintf(stderr, "Task shader %s case %u failed: count=%u mask=%u.\n", indirect ? "indirect" : "direct", scenario, count, visible_mask);
    return valid;
}

int main()
{
    const gpu::DeviceInit initialized = gpu::create_device({.timestamp_query_count = 0});
    if (initialized.error == gpu::Error::unsupported) return 77;
    if (initialized.error != gpu::Error::none) return 1;
    Fixture fixture{.device = initialized.device};
    bool valid = initialize(fixture);
    if (valid)
    {
        const uint32 counts[] = {0, 1, 32, 33, 70, 70, 70};
        const uint32 masks[] = {7, 7, 7, 5, 7, 2, 0};
        for (uint32 indirect = 0; indirect < 2; ++indirect)
            for (uint32 scenario = 0; scenario < sizeof(counts) / sizeof(counts[0]); ++scenario)
                valid &= render_case(fixture, counts[scenario], masks[scenario], scenario, indirect != 0);
    }
    gpu::wait_idle(fixture.device);
    gpu::destroy_gpu_heap(fixture.readback);
    gpu::destroy_gpu_heap(fixture.data);
    gpu::destroy_gpu_heap(fixture.seed);
    gpu::destroy_timeline_semaphore(fixture.completion.semaphore);
    gpu::destroy_render_view(fixture.view);
    gpu::destroy_texture(fixture.target);
    gpu::destroy_texture_heap(fixture.texture_heap);
    gpu::destroy_pso(fixture.pso);
    gpu::destroy_device(fixture.device);
    printf("Task shader compaction, culling and root ABI: %s\n", valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
