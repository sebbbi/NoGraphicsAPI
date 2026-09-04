#include "deferred_lighting_shared.h"
#include "example_support.hpp"
#include "gbuffer_shared.h"
#include "simulation_shared.h"

#include <NoGraphicsAPI/math.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

using namespace gpu;
using namespace std;

namespace
{

constexpr uint32_t initial_width = 1280;
constexpr uint32_t initial_height = 720;
constexpr uint32_t object_count = object_grid_width * object_grid_width;
constexpr uint32_t frames_in_flight = 2;
constexpr uint32_t gbuffer_texture_count = uint32_t(GBufferTexture::count);
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
    Texture* albedo = nullptr;
    Texture* normal_roughness = nullptr;
    Texture* depth = nullptr;
    RenderView* albedo_render_view = nullptr;
    RenderView* normal_roughness_render_view = nullptr;
    RenderView* depth_render_view = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

void recreate_gbuffer(Device* device,
                      GBuffer& gbuffer,
                      byte* descriptors,
                      uint64_t descriptor_stride,
                      uint32_t width,
                      uint32_t height) noexcept
{
    destroy_render_view(gbuffer.depth_render_view);
    destroy_render_view(gbuffer.normal_roughness_render_view);
    destroy_render_view(gbuffer.albedo_render_view);
    destroy_texture(gbuffer.depth);
    destroy_texture(gbuffer.normal_roughness);
    destroy_texture(gbuffer.albedo);

    gbuffer = {
        .albedo = create_texture(device, {
            .width = width,
            .height = height,
            .usage = TextureUsage::sampled |
                        TextureUsage::color_attachment,
        }),
        .normal_roughness = create_texture(device, {
            .width = width,
            .height = height,
            .format = Format::rgba16_float,
            .usage = TextureUsage::sampled |
                        TextureUsage::color_attachment,
        }),
        .depth = create_texture(device, {
            .width = width,
            .height = height,
            .format = Format::d32_float,
            .usage = TextureUsage::sampled |
                        TextureUsage::depth_stencil_attachment,
        }),
        .width = width,
        .height = height,
    };

    gbuffer.albedo_render_view = create_render_view(gbuffer.albedo);
    gbuffer.normal_roughness_render_view = create_render_view(gbuffer.normal_roughness);
    gbuffer.depth_render_view = create_render_view(gbuffer.depth);

    const size_t stride = size_t(descriptor_stride);
    write_texture_descriptor(device, descriptors + size_t(GBufferTexture::albedo) * stride, gbuffer.albedo, TextureDescriptorType::sampled);
    write_texture_descriptor(device, descriptors + size_t(GBufferTexture::normal_roughness) * stride, gbuffer.normal_roughness, TextureDescriptorType::sampled);
    write_texture_descriptor(device, descriptors + size_t(GBufferTexture::depth) * stride, gbuffer.depth, TextureDescriptorType::sampled);
}

float random_signed(uint32_t& state) noexcept
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
    constexpr float softening_squared = gravity_softening * gravity_softening;
    uint32_t random_state = 0x12345678u;

    for (uint32_t i = 0; i != object_count; ++i)
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
        const float circular_speed = sqrt(
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
    const DeviceInit device_init = create_device({.window = window, .swapchain_format = Format::bgra8_srgb});

    Device* device = device_init.device;

    if (device_init.error != Error::none)
    {
        destroy_device(device);
        close_example_window(window);
        return 1;
    }

    const DeviceCaps& caps = get_device_caps(device);
    printf("Using %s\n", caps.device_name);

    // GPU resources
    GpuAllocation<byte> texture_heap = gpu_malloc(device, caps.texture_descriptor_stride * gbuffer_texture_count * frames_in_flight, MemoryType::texture_heap);
    GpuAllocation<ObjectData> object_data = gpu_malloc<ObjectData>(device, object_count);
    initialize_object_data(object_data.cpu);

    // Shaders
    PSO* simulation_pso = create_compute_pso(device, read_spirv(NOGRAPHICSAPI_SIMULATION_COMPUTE_SPV_PATH));

    PSO* gbuffer_pso = create_mesh_pso(device, {
        .mesh_spirv = read_spirv(NOGRAPHICSAPI_GBUFFER_MESH_SPV_PATH),
        .fragment_spirv = read_spirv(NOGRAPHICSAPI_GBUFFER_FRAGMENT_SPV_PATH),
        .color_targets = {
            {.format = Format::rgba8_unorm},
            {.format = Format::rgba16_float},
        },
        .depth_format = Format::d32_float,
        .rasterization = { .cull = CullMode::clockwise },
        .depth_stencil = { .depth_test = true, .depth_write = true }
    });

    PSO* deferred_lighting_pso = create_graphics_pso(device, {
        .vertex_spirv = read_spirv(NOGRAPHICSAPI_DEFERRED_VERTEX_SPV_PATH),
        .fragment_spirv = read_spirv(NOGRAPHICSAPI_DEFERRED_FRAGMENT_SPV_PATH),
        .color_targets = {{.format = Format::bgra8_srgb}},
    });

    // Init camera and simulation
    const float4x4 view = math::look_at_rh(camera_position,
        {.x = 0.0f, .y = 0.0f, .z = 0.0f},
        {.x = 0.0f, .y = 1.0f, .z = 0.0f});
    const float3 view_right = math::to_float3(view.rows[0]);
    const float3 view_up = math::to_float3(view.rows[1]);
    const float3 view_forward = -math::to_float3(view.rows[2]);
    GBuffer gbuffer{};
    uint32_t gbuffer_texture_base = 0;
    auto previous_time = chrono::steady_clock::now();
    float rotation_angle = 0.0f;

    TimelinePoint latest_completion{.semaphore = create_timeline_semaphore(device)};

    while (pump_example_window(window))
    {
        const DrawableExtent extent = get_drawable_extent(device);

        // Limit the application to two frames in flight so double-buffered descriptors are safe to reuse
        if (latest_completion.value >= frames_in_flight)
        {
            wait_timeline({.semaphore = latest_completion.semaphore, .value = latest_completion.value - frames_in_flight + 1});
        }

        // Window resize?
        if (extent.width != gbuffer.width || extent.height != gbuffer.height)
        {
            if (gbuffer.albedo)
                gbuffer_texture_base = gbuffer_texture_count - gbuffer_texture_base;
            recreate_gbuffer(device, gbuffer, texture_heap.cpu + size_t(gbuffer_texture_base) * caps.texture_descriptor_stride, caps.texture_descriptor_stride,
                             extent.width, extent.height);
        }

        const SwapchainFrame frame = acquire(device);

        CommandBuffer* commands = begin_commands(device);
        set_texture_heap(commands, gpu_range(texture_heap));

        // Simulation
        const auto current_time = chrono::steady_clock::now();
        const float delta_seconds = chrono::duration<float>(current_time - previous_time).count();
        previous_time = current_time;

        barrier(commands,
            Stage::compute | Stage::mesh, Access::shader_write | Access::shader_read,
            Stage::compute,  Access::shader_read | Access::shader_write);

        bind_pso(commands, simulation_pso);
        dispatch(commands, SimulationRoot{
            .objects = object_data.gpu,
            .delta_seconds = delta_seconds,
        }, object_count / simulation_thread_count);

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

        bind_pso(commands, gbuffer_pso);

        float4x4 projection = math::perspective_rh_zo(math::pi / 3.0f, float(extent.width) / float(extent.height), 0.3f, 1500.0f);
        projection.rows[1].y = -projection.rows[1].y;
        float4x4 view_projection = projection * view;
        rotation_angle += cube_rotation_speed * delta_seconds;

        const GBufferRoot gbuffer_root{
            .objects = object_data.gpu,
            .view_projection = view_projection,
            .orientation = math::to_float3x4(
                math::rotation_y(rotation_angle) *
                math::rotation_x(rotation_angle * 0.5f) *
                math::scale({.x = cube_scale, .y = cube_scale, .z = cube_scale})),
        };

        draw_meshlets(commands, gbuffer_root, object_grid_width, object_grid_width);

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
                    .x = float(gbuffer.width) / float(frame.width),
                    .y = float(gbuffer.height) / float(frame.height),
                },
            .gbuffer_texture_base = gbuffer_texture_base,
        };

        draw(commands, deferred_lighting_root, 3);

        end_render_pass(commands);

        // Submit
        latest_completion.value++;
        submit_and_present(device, {commands}, latest_completion);
    }

    wait_timeline(latest_completion);

    // Cleanup
    destroy_timeline_semaphore(latest_completion.semaphore);
    destroy_pso(deferred_lighting_pso);
    destroy_pso(gbuffer_pso);
    destroy_pso(simulation_pso);
    destroy_render_view(gbuffer.depth_render_view);
    destroy_render_view(gbuffer.normal_roughness_render_view);
    destroy_render_view(gbuffer.albedo_render_view);
    destroy_texture(gbuffer.depth);
    destroy_texture(gbuffer.normal_roughness);
    destroy_texture(gbuffer.albedo);
    gpu_free(object_data);
    gpu_free(texture_heap);

    destroy_device(device);
    close_example_window(window);
    return 0;
}
