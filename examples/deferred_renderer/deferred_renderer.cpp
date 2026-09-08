#include "deferred_lighting_shared.h"
#include "example_support.hpp"
#include "gbuffer_shared.h"
#include "simulation_shared.h"

#include <NoGraphicsAPIUtility/bump_allocator.hpp>
#include <NoGraphicsAPIUtility/delete_queue.hpp>
#include <NoGraphicsAPIUtility/math.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

using namespace gpu;

namespace
{

constexpr uint32 initial_width = 1280;
constexpr uint32 initial_height = 720;
constexpr uint32 object_count = object_grid_width * object_grid_width;
constexpr uint32 frames_in_flight = 2;
constexpr uint32 gbuffer_texture_count = uint32(GBufferTexture::count);
constexpr uint64 data_heap_size = 16 * 1024 * 1024;
constexpr uint64 texture_heap_size = 256 * 1024 * 1024;
constexpr float cube_scale = 0.42f;
constexpr float cube_rotation_speed = 0.5f;
constexpr float minimum_orbit_radius = 54.0f;
constexpr float maximum_orbit_radius = 240.0f;
constexpr float3 camera_position{
    .x = 260.0f,
    .y = 210.0f,
    .z = 300.0f,
};

struct GBuffer
{
    PlacedTexture albedo{};
    PlacedTexture normal_roughness{};
    PlacedTexture depth{};
    RenderView* albedo_render_view = nullptr;
    RenderView* normal_roughness_render_view = nullptr;
    RenderView* depth_render_view = nullptr;
    uint32 width = 0;
    uint32 height = 0;
};

void destroy_gbuffer(TextureAllocator& texture_allocator, GBuffer& gbuffer) noexcept
{
    destroy_render_view(gbuffer.depth_render_view);
    destroy_render_view(gbuffer.normal_roughness_render_view);
    destroy_render_view(gbuffer.albedo_render_view);
    texture_allocator.free(gbuffer.depth);
    texture_allocator.free(gbuffer.normal_roughness);
    texture_allocator.free(gbuffer.albedo);
    gbuffer = {};
}

void recreate_gbuffer(Device* device, TextureAllocator& texture_allocator, GBuffer& gbuffer, byte* descriptors, uint64 descriptor_size,
                     uint32 width, uint32 height) noexcept
{
    gbuffer = {
        .albedo = texture_allocator.allocate({
            .extent = {.x = width, .y = height, .z = 1},
            .usage = TextureUsage::sampled | TextureUsage::color_attachment,
        }),
        .normal_roughness = texture_allocator.allocate({
            .extent = {.x = width, .y = height, .z = 1},
            .format = Format::rgba16_float,
            .usage = TextureUsage::sampled | TextureUsage::color_attachment,
        }),
        .depth = texture_allocator.allocate({
            .extent = {.x = width, .y = height, .z = 1},
            .format = Format::d32_float,
            .usage = TextureUsage::sampled | TextureUsage::depth_stencil_attachment,
        }),
        .width = width,
        .height = height,
    };

    gbuffer.albedo_render_view = create_render_view(gbuffer.albedo.texture);
    gbuffer.normal_roughness_render_view = create_render_view(gbuffer.normal_roughness.texture);
    gbuffer.depth_render_view = create_render_view(gbuffer.depth.texture);

    write_texture_descriptor(device, descriptors + size_t(GBufferTexture::albedo) * descriptor_size, gbuffer.albedo.texture, TextureDescriptorType::sampled);
    write_texture_descriptor(device, descriptors + size_t(GBufferTexture::normal_roughness) * descriptor_size, gbuffer.normal_roughness.texture,
                             TextureDescriptorType::sampled);
    write_texture_descriptor(device, descriptors + size_t(GBufferTexture::depth) * descriptor_size, gbuffer.depth.texture, TextureDescriptorType::sampled);
}

float random_signed(uint32& state) noexcept
{
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return float(state >> 8u) * (2.0f / 16777216.0f) - 1.0f;
}

void initialize_object_data(ObjectData* objects) noexcept
{
    constexpr float minimum_radius_squared = minimum_orbit_radius * minimum_orbit_radius;
    constexpr float maximum_radius_squared = maximum_orbit_radius * maximum_orbit_radius;
    const float softening_squared = gravity_softening * gravity_softening;
    uint32 random_state = 0x12345678u;

    for (uint32 i = 0; i != object_count; ++i)
    {
        float3 position{};
        float radius_squared = 0.0f;
        do
        {
            position = {
                .x = random_signed(random_state) * maximum_orbit_radius,
                .y = random_signed(random_state) * maximum_orbit_radius,
                .z = random_signed(random_state) * maximum_orbit_radius,
            };
            radius_squared = math::dot(position, position);
        } while (radius_squared < minimum_radius_squared || radius_squared > maximum_radius_squared);

        float3 tangent{};
        float axis_length_squared = 0.0f;
        do
        {
            const float3 random_axis{
                .x = random_signed(random_state),
                .y = random_signed(random_state),
                .z = random_signed(random_state),
            };
            axis_length_squared = math::dot(random_axis, random_axis);
            tangent = math::cross(position, random_axis);
        } while (axis_length_squared < 0.01f || axis_length_squared > 1.0f || math::dot(tangent, tangent) < 0.01f);
        tangent = math::normalize(tangent);

        const float inverse_softened_distance = math::rsqrt(radius_squared + softening_squared);
        const float circular_speed = sqrtf(
            central_gravity * radius_squared *
            inverse_softened_distance * inverse_softened_distance *
            inverse_softened_distance);
        const float speed_scale = 0.985f + random_signed(random_state) * 0.045f;
        objects[i] = {
            .position = position,
            .velocity = tangent * (circular_speed * speed_scale),
        };
    }
}

} // namespace

int main()
{
    // Init
    void* window = open_example_window("NoGraphicsAPI deferred renderer", initial_width, initial_height);
    Device* device = create_device({.window = window, .swapchain_format = Format::bgra8_srgb}).device;

    if (!window || !device)
    {
        destroy_device(device);
        close_example_window(window);
        return 1;
    }

    const DeviceCaps& caps = get_device_caps(device);
    printf("Using %s\n", caps.device_name);

    // Shaders
    const Span<uint32> simulation_spirv = read_spirv(NOGRAPHICSAPI_SIMULATION_COMPUTE_SPV_PATH);
    const Span<uint32> gbuffer_mesh_spirv = read_spirv(NOGRAPHICSAPI_GBUFFER_MESH_SPV_PATH);
    const Span<uint32> gbuffer_fragment_spirv = read_spirv(NOGRAPHICSAPI_GBUFFER_FRAGMENT_SPV_PATH);
    const Span<uint32> deferred_vertex_spirv = read_spirv(NOGRAPHICSAPI_DEFERRED_VERTEX_SPV_PATH);
    const Span<uint32> deferred_fragment_spirv = read_spirv(NOGRAPHICSAPI_DEFERRED_FRAGMENT_SPV_PATH);
    if (!simulation_spirv.data || !gbuffer_mesh_spirv.data || !gbuffer_fragment_spirv.data || !deferred_vertex_spirv.data || !deferred_fragment_spirv.data)
    {
        free(deferred_fragment_spirv.data);
        free(deferred_vertex_spirv.data);
        free(gbuffer_fragment_spirv.data);
        free(gbuffer_mesh_spirv.data);
        free(simulation_spirv.data);
        destroy_device(device);
        close_example_window(window);
        return 1;
    }
    PSO* simulation_pso = create_compute_pso(device, simulation_spirv);
    free(simulation_spirv.data);

    PSO* gbuffer_pso = create_mesh_pso(device, {
        .mesh_spirv = gbuffer_mesh_spirv,
        .fragment_spirv = gbuffer_fragment_spirv,
        .color_targets = {
            {.format = Format::rgba8_unorm},
            {.format = Format::rgba16_float},
        },
        .depth_format = Format::d32_float,
        .rasterization = { .cull = CullMode::clockwise },
    });
    free(gbuffer_fragment_spirv.data);
    free(gbuffer_mesh_spirv.data);

    PSO* deferred_lighting_pso = create_graphics_pso(device, {
        .vertex_spirv = deferred_vertex_spirv,
        .fragment_spirv = deferred_fragment_spirv,
        .color_targets = {{.format = Format::bgra8_srgb}},
    });
    free(deferred_fragment_spirv.data);
    free(deferred_vertex_spirv.data);

    // GPU resources
    GpuHeap texture_descriptor_heap =
        create_gpu_heap(device, caps.texture_descriptor_size * gbuffer_texture_count * frames_in_flight, MemoryType::texture_descriptor_heap);
    GpuHeap data_heap = create_gpu_heap(device, data_heap_size);
    BumpAllocator data_allocator(data_heap.range);
    const GpuCpuRange<ObjectData> object_allocation = data_allocator.allocate<ObjectData>(object_count);
    initialize_object_data(object_allocation.cpu);
    TextureHeap texture_heap = create_texture_heap(device, texture_heap_size);
    TextureAllocator texture_allocator(device, texture_heap, 64);

    // Init camera and simulation
    const float4x4 view = math::look_at_rh(camera_position,
        {.x = 0.0f, .y = 0.0f, .z = 0.0f},
        {.x = 0.0f, .y = 1.0f, .z = 0.0f});
    const float3 view_right = math::to_float3(view.rows[0]);
    const float3 view_up = math::to_float3(view.rows[1]);
    const float3 view_forward = -math::to_float3(view.rows[2]);
    GBuffer gbuffer{};
    uint32 descriptor_row = 0;
    double previous_time = example_time_seconds();
    float rotation_angle = 0.0f;

    TimelinePoint latest_completion{.semaphore = create_timeline_semaphore(device)};
    DeleteQueue delete_queue(latest_completion.semaphore, frames_in_flight);

    while (pump_example_window(window))
    {
        // Limit the application to two frames in flight so double-buffered descriptors are safe to reuse
        if (latest_completion.value >= frames_in_flight)
        {
            wait_timeline({.semaphore = latest_completion.semaphore, .value = latest_completion.value - frames_in_flight + 1});
        }
        delete_queue.tick();

        const SwapchainFrame frame = acquire(device);
        if (!frame.render_view)
            continue;
        const uint32x2 extent = frame.extent;

        // Window resize?
        if (extent.x != gbuffer.width || extent.y != gbuffer.height)
        {
            if (gbuffer.albedo.texture)
            {
                delete_queue.defer(latest_completion.value, [&texture_allocator, gbuffer]() mutable noexcept { destroy_gbuffer(texture_allocator, gbuffer); });
                descriptor_row = (descriptor_row + 1) % frames_in_flight;
            }
            recreate_gbuffer(device, texture_allocator, gbuffer,
                             texture_descriptor_heap.range.cpu + size_t(descriptor_row * gbuffer_texture_count) * caps.texture_descriptor_size,
                             caps.texture_descriptor_size, extent.x, extent.y);
        }

        CommandBuffer* commands = begin_commands(device);
        set_texture_descriptor_heap(commands, gpu_range(texture_descriptor_heap));

        // Simulation
        const double current_time = example_time_seconds();
        const float delta_seconds = float(current_time - previous_time);
        previous_time = current_time;

        barrier(commands,
            Stage::compute | Stage::mesh, Access::shader_write | Access::shader_read,
            Stage::compute,  Access::shader_read | Access::shader_write);

        bind_pso(commands, simulation_pso);
        dispatch(commands, SimulationRoot{
            .objects = object_allocation.gpu,
            .delta_seconds = delta_seconds,
        }, {.x = object_count / simulation_thread_count, .y = 1, .z = 1});

        // G-buffer
        barrier(commands,
            Stage::compute | Stage::fragment | Stage::depth_stencil_tests, Access::shader_write | Access::shader_read | Access::depth_stencil_write,
            Stage::mesh | Stage::color_output | Stage::depth_stencil_tests, Access::shader_read | Access::color_write | Access::depth_stencil_write);

        begin_render_pass(commands, {
            .colors = { {
                    .render_view = gbuffer.albedo_render_view,
                    .load = LoadOp::clear,
                }, {
                    .render_view = gbuffer.normal_roughness_render_view,
                    .load = LoadOp::clear,
                },
            },
            .depth = {
                .render_view = gbuffer.depth_render_view,
                .load = LoadOp::clear,
            },
        });

        set_depth_stencil(commands, {.depth_test = true, .depth_write = true});
        bind_pso(commands, gbuffer_pso);

        float4x4 projection = math::perspective_rh_zo(math::pi / 3.0f, float(extent.x) / float(extent.y), 0.3f, 1500.0f);
        projection.rows[1].y = -projection.rows[1].y;
        rotation_angle += cube_rotation_speed * delta_seconds;

        const GBufferRoot gbuffer_root{
            .objects = object_allocation.gpu,
            .view_projection = projection * view,
            .orientation = math::to_float3x4(
                math::rotation_y(rotation_angle) *
                math::rotation_x(rotation_angle * 0.5f) *
                math::scale({.x = cube_scale, .y = cube_scale, .z = cube_scale})),
        };

        draw_meshlets(commands, gbuffer_root, {.x = object_grid_width, .y = object_grid_width, .z = 1});

        end_render_pass(commands);

        // Lighting
        barrier(commands,
            Stage::color_output | Stage::depth_stencil_tests, Access::color_write | Access::depth_stencil_write,
            Stage::fragment, Access::shader_read);

        begin_render_pass(commands, {
            .colors = {{
                .render_view = frame.render_view,
                .load = LoadOp::discard,
            }},
        });

        bind_pso(commands, deferred_lighting_pso);

        const DeferredLightingRoot deferred_lighting_root = {
            .camera_position = math::to_float4(camera_position, 1.0f),
            .ray_center = math::to_float4(view_forward),
            .ray_horizontal = math::to_float4(view_right / projection.rows[0].x),
            .ray_vertical = math::to_float4(view_up / projection.rows[1].y),
            .depth_linearize =
                {
                    .x = -projection.rows[2].w,
                    .y = -projection.rows[2].z,
                },
            .gbuffer_pixel_scale =
                {
                    .x = float(gbuffer.width) / float(frame.extent.x),
                    .y = float(gbuffer.height) / float(frame.extent.y),
                },
            .gbuffer_texture_base = descriptor_row * gbuffer_texture_count,
        };

        draw(commands, deferred_lighting_root, 3);

        end_render_pass(commands);

        // Submit
        latest_completion.value++;
        submit_and_present(device, {commands}, latest_completion);
    }

    wait_idle(device);
    delete_queue.drain();

    // Cleanup
    destroy_timeline_semaphore(latest_completion.semaphore);
    destroy_pso(deferred_lighting_pso);
    destroy_pso(gbuffer_pso);
    destroy_pso(simulation_pso);
    destroy_gbuffer(texture_allocator, gbuffer);
    destroy_texture_heap(texture_heap);
    destroy_gpu_heap(data_heap);
    destroy_gpu_heap(texture_descriptor_heap);

    destroy_device(device);
    close_example_window(window);
    return 0;
}
