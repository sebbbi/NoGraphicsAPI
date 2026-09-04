// The texture map is adapted from Vulkan-Tools' vkcube sample. Copyright
// notices and the Apache-2.0 license are in NOTICE.md and LICENSE-Apache-2.0.txt.

#include "cube_shared.h"

#include "example_support.hpp"

#include <NoGraphicsAPI/math.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numbers>

using namespace gpu;
using namespace std;

namespace {

	constexpr uint32_t width = 500;
	constexpr uint32_t height = 500;
	constexpr uint32_t texture_width = 256;
	constexpr uint32_t texture_height = 256;
	constexpr size_t texture_byte_count = size_t(texture_width) * texture_height * 4;
	constexpr float radians_per_frame = 4.0f * numbers::pi_v<float> / 180.0f;

	constexpr CubeVertex cube_vertices[] = {
		{ .position = { .x = -1.0f, .y = -1.0f, .z = -1.0f, .w = 1.0f }, .uv = { .x = 0.0f, .y = 1.0f } },
		{ .position = { .x = -1.0f, .y = -1.0f, .z = 1.0f, .w = 1.0f }, .uv = { .x = 1.0f, .y = 1.0f } },
		{ .position = { .x = -1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f }, .uv = { .x = 1.0f, .y = 0.0f } },
		{ .position = { .x = -1.0f, .y = 1.0f, .z = -1.0f, .w = 1.0f }, .uv = { .x = 0.0f, .y = 0.0f } },

		{ .position = { .x = -1.0f, .y = -1.0f, .z = -1.0f, .w = 1.0f }, .uv = { .x = 1.0f, .y = 1.0f } },
		{ .position = { .x = -1.0f, .y = 1.0f, .z = -1.0f, .w = 1.0f }, .uv = { .x = 1.0f, .y = 0.0f } },
		{ .position = { .x = 1.0f, .y = 1.0f, .z = -1.0f, .w = 1.0f }, .uv = { .x = 0.0f, .y = 0.0f } },
		{ .position = { .x = 1.0f, .y = -1.0f, .z = -1.0f, .w = 1.0f }, .uv = { .x = 0.0f, .y = 1.0f } },

		{ .position = { .x = -1.0f, .y = -1.0f, .z = -1.0f, .w = 1.0f }, .uv = { .x = 1.0f, .y = 0.0f } },
		{ .position = { .x = 1.0f, .y = -1.0f, .z = -1.0f, .w = 1.0f }, .uv = { .x = 1.0f, .y = 1.0f } },
		{ .position = { .x = 1.0f, .y = -1.0f, .z = 1.0f, .w = 1.0f }, .uv = { .x = 0.0f, .y = 1.0f } },
		{ .position = { .x = -1.0f, .y = -1.0f, .z = 1.0f, .w = 1.0f }, .uv = { .x = 0.0f, .y = 0.0f } },

		{ .position = { .x = -1.0f, .y = 1.0f, .z = -1.0f, .w = 1.0f }, .uv = { .x = 1.0f, .y = 0.0f } },
		{ .position = { .x = -1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f }, .uv = { .x = 0.0f, .y = 0.0f } },
		{ .position = { .x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f }, .uv = { .x = 0.0f, .y = 1.0f } },
		{ .position = { .x = 1.0f, .y = 1.0f, .z = -1.0f, .w = 1.0f }, .uv = { .x = 1.0f, .y = 1.0f } },

		{ .position = { .x = 1.0f, .y = 1.0f, .z = -1.0f, .w = 1.0f }, .uv = { .x = 1.0f, .y = 0.0f } },
		{ .position = { .x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f }, .uv = { .x = 0.0f, .y = 0.0f } },
		{ .position = { .x = 1.0f, .y = -1.0f, .z = 1.0f, .w = 1.0f }, .uv = { .x = 0.0f, .y = 1.0f } },
		{ .position = { .x = 1.0f, .y = -1.0f, .z = -1.0f, .w = 1.0f }, .uv = { .x = 1.0f, .y = 1.0f } },

		{ .position = { .x = -1.0f, .y = -1.0f, .z = 1.0f, .w = 1.0f }, .uv = { .x = 0.0f, .y = 1.0f } },
		{ .position = { .x = 1.0f, .y = -1.0f, .z = 1.0f, .w = 1.0f }, .uv = { .x = 1.0f, .y = 1.0f } },
		{ .position = { .x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f }, .uv = { .x = 1.0f, .y = 0.0f } },
		{ .position = { .x = -1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f }, .uv = { .x = 0.0f, .y = 0.0f } },
	};

	constexpr uint16_t cube_indices[] = {
		0, 1, 2, 2, 3, 0,
		4, 5, 6, 6, 7, 4,
		8, 9, 10, 10, 11, 8,
		12, 13, 14, 14, 15, 12,
		16, 17, 18, 18, 19, 16,
		20, 21, 22, 22, 23, 20,
	};

	constexpr size_t cube_vertex_count = sizeof(cube_vertices) / sizeof(cube_vertices[0]);
	constexpr uint32_t cube_index_count = uint32_t(sizeof(cube_indices) / sizeof(cube_indices[0]));

} // namespace

int main() {
	// Init
	void* window = open_example_window("NoGraphicsAPI spinning textured cube", width, height);
    const DeviceInit device_init = create_device({
        .window = window,
        .swapchain_format = Format::bgra8_srgb,
    });

    Device* device = device_init.device;

    if (device_init.error != Error::none)
    {
        destroy_device(device);
        close_example_window(window);
        return 1;
    }

	const DeviceCaps& caps = get_device_caps(device);
    printf("Using %s\n", caps.device_name);

	// Shaders
	PSO* cube_pso = create_graphics_pso(device, {
        .vertex_spirv = read_spirv(NOGRAPHICSAPI_CUBE_VERTEX_SPV_PATH),
        .fragment_spirv = read_spirv(NOGRAPHICSAPI_CUBE_FRAGMENT_SPV_PATH),
        .color_targets = {{.format = Format::bgra8_srgb}},
        .depth_format = Format::d32_float,
        .rasterization = { .cull = CullMode::clockwise },
        .depth_stencil = { .depth_test = true, .depth_write = true },
    });

	// GPU resources
	GpuAllocation<CubeVertex> vertex_memory = gpu_malloc<CubeVertex>(device, cube_vertex_count);
    memcpy(vertex_memory.cpu, cube_vertices, sizeof(cube_vertices));
	GpuAllocation<uint16_t> index_memory = gpu_malloc<uint16_t>(device, cube_index_count);
	memcpy(index_memory.cpu, cube_indices, sizeof(cube_indices));

	GpuAllocation<byte> texture_upload = gpu_malloc<byte>(device, texture_byte_count);
    read_binary_file(NOGRAPHICSAPI_CUBE_TEXTURE_PATH, Span<byte>(texture_upload.cpu, texture_byte_count));

	GpuAllocation<byte> texture_heap = gpu_malloc(device, caps.texture_descriptor_stride, MemoryType::texture_heap);
	GpuAllocation<byte> sampler_heap = gpu_malloc(device, caps.sampler_descriptor_stride, MemoryType::sampler_heap);

	TimelinePoint latest_completion{.semaphore = create_timeline_semaphore(device)};

	// Textures
	Texture* texture = create_texture(device, {
		.width = texture_width,
		.height = texture_height,
		.format = Format::rgba8_srgb,
		.usage = TextureUsage::sampled | TextureUsage::transfer_destination,
	});

	write_texture_descriptor(device, texture_heap.cpu + caps.texture_descriptor_stride * 0, texture, TextureDescriptorType::sampled);
	write_sampler_descriptor(device, sampler_heap.cpu + caps.sampler_descriptor_stride * 0, {
		.min_filter = Filter::nearest,
		.mag_filter = Filter::nearest,
		.address_u = AddressMode::clamp_to_edge,
		.address_v = AddressMode::clamp_to_edge,
	});

	CommandBuffer* upload_commands = begin_commands(device);
	copy_memory_to_texture(upload_commands, gpu_range(texture_upload), texture);

	barrier(upload_commands,
		Stage::transfer, Access::transfer_write,
		Stage::fragment, Access::shader_read);

	latest_completion.value++;
	submit({ upload_commands }, latest_completion);

	Texture* depth = nullptr;
	RenderView* depth_render_view = nullptr;
	uint32_t depth_width = 0;
	uint32_t depth_height = 0;
	uint64_t frame_index = 0;

	while (pump_example_window(window))
	{
		const SwapchainFrame frame = acquire(device);

        // Window resize?
        if (frame.width != depth_width || frame.height != depth_height)
		{
			destroy_render_view(depth_render_view);
			destroy_texture(depth);
			depth = create_texture(device, {
				.width = frame.width,
				.height = frame.height,
				.format = Format::d32_float,
				.usage = TextureUsage::depth_stencil_attachment,
			});
			depth_render_view = create_render_view(depth);
			depth_width = frame.width;
			depth_height = frame.height;
		}

		// Render
		CommandBuffer* commands = begin_commands(device);
        set_texture_heap(commands, gpu_range(texture_heap));
        set_sampler_heap(commands, gpu_range(sampler_heap));

		barrier(commands, 
			Stage::depth_stencil_tests, Access::depth_stencil_write, 
			Stage::depth_stencil_tests, Access::depth_stencil_write);

		begin_render_pass(commands, {
			.colors = { {
				.render_view = frame.render_view,
				.load = LoadOp::clear,
				.clear = { .x = 0.2f, .y = 0.2f, .z = 0.2f, .w = 0.2f },
			}},
			.depth = {
				.render_view = depth_render_view,
				.load = LoadOp::clear,
				.store = StoreOp::discard,
			},
		});

		bind_pso(commands, cube_pso);

        float4x4 projection = math::perspective_rh_zo(45.0f * numbers::pi_v<float> / 180.0f, float(frame.width) / float(frame.height), 0.1f, 100.0f);
        projection.rows[1].y = -projection.rows[1].y;
		const float4x4 view = math::look_at_rh({.x = 0.0f, .y = 3.0f, .z = 5.0f}, {.x = 0.0f, .y = 0.0f, .z = 0.0f}, {.x = 0.0f, .y = 1.0f, .z = 0.0f});
        const float4x4 model = math::rotation_y(radians_per_frame * float(frame_index++));
        const float4x4 mvp = projection * view * model;

		const CubeRootArguments root {
            .vertices = vertex_memory.gpu,
            .transform = mvp,
        };

		draw_indexed(commands, root, gpu_range(index_memory), IndexType::uint16, cube_index_count);

		end_render_pass(commands);

        // Submit
        latest_completion.value++;
		submit_and_present(device, {commands}, latest_completion);
	}

	wait_timeline(latest_completion);

	// Cleanup
	destroy_timeline_semaphore(latest_completion.semaphore);
    destroy_pso(cube_pso);
	gpu_free(texture_upload);
	destroy_render_view(depth_render_view);
	destroy_texture(depth);
	destroy_texture(texture);
	gpu_free(index_memory);
	gpu_free(vertex_memory);
	gpu_free(sampler_heap);
	gpu_free(texture_heap);

	destroy_device(device);
	close_example_window(window);
	return 0;
}
