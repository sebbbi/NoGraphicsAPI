#include "deferred_lighting_shared.h"
#include "example_support.hpp"
#include "gbuffer_shared.h"

#include <NoGraphicsAPI/math.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace gpu;
using namespace std;

namespace
{

constexpr uint32_t initial_width = 1280;
constexpr uint32_t initial_height = 720;
constexpr uint32_t object_count = 256 * 1024;
constexpr uint32_t frames_in_flight = 2;
constexpr uint32_t gbuffer_texture_count = uint32_t(GBufferTexture::count);
constexpr float cube_scale = 0.42f;
constexpr float cube_rotation_speed = 0.5f;
constexpr float central_gravity = 81000.0f;
constexpr float gravity_softening = 12.0f;
constexpr float minimum_orbit_radius = 54.0f;
constexpr float maximum_orbit_radius = 240.0f;
constexpr float3 camera_position{
    .x = 260.0f,
    .y = 210.0f,
    .z = 300.0f,
};

constexpr GBufferVertex cube_vertices[] = {
    {.position = {.x = -1.0f, .y = -1.0f, .z = -1.0f}, .normal = {.x = -1.0f}},
    {.position = {.x = -1.0f, .y = -1.0f, .z = 1.0f}, .normal = {.x = -1.0f}},
    {.position = {.x = -1.0f, .y = 1.0f, .z = 1.0f}, .normal = {.x = -1.0f}},
    {.position = {.x = -1.0f, .y = 1.0f, .z = -1.0f}, .normal = {.x = -1.0f}},

    {.position = {.x = -1.0f, .y = -1.0f, .z = -1.0f}, .normal = {.z = -1.0f}},
    {.position = {.x = -1.0f, .y = 1.0f, .z = -1.0f}, .normal = {.z = -1.0f}},
    {.position = {.x = 1.0f, .y = 1.0f, .z = -1.0f}, .normal = {.z = -1.0f}},
    {.position = {.x = 1.0f, .y = -1.0f, .z = -1.0f}, .normal = {.z = -1.0f}},

    {.position = {.x = -1.0f, .y = -1.0f, .z = -1.0f}, .normal = {.y = -1.0f}},
    {.position = {.x = 1.0f, .y = -1.0f, .z = -1.0f}, .normal = {.y = -1.0f}},
    {.position = {.x = 1.0f, .y = -1.0f, .z = 1.0f}, .normal = {.y = -1.0f}},
    {.position = {.x = -1.0f, .y = -1.0f, .z = 1.0f}, .normal = {.y = -1.0f}},

    {.position = {.x = -1.0f, .y = 1.0f, .z = -1.0f}, .normal = {.y = 1.0f}},
    {.position = {.x = -1.0f, .y = 1.0f, .z = 1.0f}, .normal = {.y = 1.0f}},
    {.position = {.x = 1.0f, .y = 1.0f, .z = 1.0f}, .normal = {.y = 1.0f}},
    {.position = {.x = 1.0f, .y = 1.0f, .z = -1.0f}, .normal = {.y = 1.0f}},

    {.position = {.x = 1.0f, .y = 1.0f, .z = -1.0f}, .normal = {.x = 1.0f}},
    {.position = {.x = 1.0f, .y = 1.0f, .z = 1.0f}, .normal = {.x = 1.0f}},
    {.position = {.x = 1.0f, .y = -1.0f, .z = 1.0f}, .normal = {.x = 1.0f}},
    {.position = {.x = 1.0f, .y = -1.0f, .z = -1.0f}, .normal = {.x = 1.0f}},

    {.position = {.x = -1.0f, .y = -1.0f, .z = 1.0f}, .normal = {.z = 1.0f}},
    {.position = {.x = 1.0f, .y = -1.0f, .z = 1.0f}, .normal = {.z = 1.0f}},
    {.position = {.x = 1.0f, .y = 1.0f, .z = 1.0f}, .normal = {.z = 1.0f}},
    {.position = {.x = -1.0f, .y = 1.0f, .z = 1.0f}, .normal = {.z = 1.0f}},
};

constexpr uint16_t cube_indices[] = {
    0,  1,  2,  2,  3,  0,
    4,  5,  6,  6,  7,  4,
    8,  9,  10, 10, 11, 8,
    12, 13, 14, 14, 15, 12,
    16, 17, 18, 18, 19, 16,
    20, 21, 22, 22, 23, 20,
};

constexpr size_t cube_vertex_count = sizeof(cube_vertices) / sizeof(cube_vertices[0]);
constexpr uint32_t cube_index_count = uint32_t(sizeof(cube_indices) / sizeof(cube_indices[0]));

struct ObjectState
{
    float3 position;
    float3 velocity;
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

void initialize_object_states(ObjectState* objects) noexcept
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

void update_object_data(ObjectData* object_data,
                        ObjectState* objects,
                        float angle,
                        float delta_seconds) noexcept
{
    constexpr float softening_squared = gravity_softening * gravity_softening;
    const float3x4 orientation = math::to_float3x4(
        math::rotation_y(angle) *
        math::rotation_x(angle * 0.5f) *
        math::scale({
            .x = cube_scale,
            .y = cube_scale,
            .z = cube_scale,
        }));

    for (uint32_t i = 0; i != object_count; ++i)
    {
        ObjectState& object = objects[i];
        const float inverse_distance = math::rsqrt(math::dot(object.position, object.position) + softening_squared);
        const float gravity_scale = -central_gravity * inverse_distance * inverse_distance * inverse_distance * delta_seconds;
        object.velocity += object.position * gravity_scale;
        object.position += object.velocity * delta_seconds;
        object_data[i].transform = math::set_translation(orientation, object.position);
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
    GpuAllocation<GBufferVertex> vertex_memory = gpu_malloc<GBufferVertex>(device, cube_vertex_count);
    memcpy(vertex_memory.cpu, cube_vertices, sizeof(cube_vertices));
    GpuAllocation<uint16_t> index_memory = gpu_malloc<uint16_t>(device, cube_index_count);
    memcpy(index_memory.cpu, cube_indices, sizeof(cube_indices));
    GpuAllocation<ObjectData> object_data = gpu_malloc<ObjectData>(device, size_t(object_count) * frames_in_flight);

    // Shaders
    PSO* gbuffer_pso = create_graphics_pso(device, {
        .vertex_spirv = read_spirv(NOGRAPHICSAPI_GBUFFER_VERTEX_SPV_PATH),
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

    static ObjectState object_states[object_count];
    initialize_object_states(object_states);

    TimelinePoint latest_completion{.semaphore = create_timeline_semaphore(device)};

    while (pump_example_window(window))
    {
        const DrawableExtent extent = get_drawable_extent(device);

        // Limit the application to two frames in flight (double buffered data)
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

        // Simulation
        const size_t frame_slot = size_t(latest_completion.value % frames_in_flight);
        const auto current_time = chrono::steady_clock::now();
        float delta_seconds = chrono::duration<float>(current_time - previous_time).count();
        previous_time = current_time;
        rotation_angle += cube_rotation_speed * delta_seconds;
        update_object_data(object_data.cpu + frame_slot * object_count, object_states, rotation_angle, delta_seconds);

        // Render
        const SwapchainFrame frame = acquire(device);

        CommandBuffer* commands = begin_commands(device);
        set_texture_heap(commands, gpu_range(texture_heap));

        float4x4 projection = math::perspective_rh_zo(math::pi / 3.0f, float(extent.width) / float(extent.height), 0.3f, 1500.0f);
        projection.rows[1].y = -projection.rows[1].y;
        float4x4 view_projection = projection * view;

        // G-buffer
        barrier(commands,
            Stage::fragment | Stage::depth_stencil_tests,
            Access::shader_read | Access::depth_stencil_write,
            Stage::color_output | Stage::depth_stencil_tests,
            Access::color_write | Access::depth_stencil_write);

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

        const GBufferRoot gbuffer_root{
            .vertices = vertex_memory.gpu,
            .objects = object_data.gpu + frame_slot * object_count,
            .view_projection = view_projection,
        };

        draw_indexed(commands, gbuffer_root, gpu_range(index_memory), IndexType::uint16, cube_index_count, object_count);

        end_render_pass(commands);

        // Lighting
        barrier(commands,
            Stage::color_output | Stage::depth_stencil_tests,
            Access::color_write | Access::depth_stencil_write,
            Stage::fragment,
            Access::shader_read);

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
    destroy_render_view(gbuffer.depth_render_view);
    destroy_render_view(gbuffer.normal_roughness_render_view);
    destroy_render_view(gbuffer.albedo_render_view);
    destroy_texture(gbuffer.depth);
    destroy_texture(gbuffer.normal_roughness);
    destroy_texture(gbuffer.albedo);
    gpu_free(object_data);
    gpu_free(index_memory);
    gpu_free(vertex_memory);
    gpu_free(texture_heap);

    destroy_device(device);
    close_example_window(window);
    return 0;
}
