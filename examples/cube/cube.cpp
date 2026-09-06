// The texture map is adapted from Vulkan-Tools' vkcube sample. Copyright
// notices and the Apache-2.0 license are in NOTICE.md and LICENSE-Apache-2.0.txt.

#include "cube_shared.h"

#include "example_support.hpp"

#include <NoGraphicsAPIUtility/bump_allocator.hpp>
#include <NoGraphicsAPIUtility/math.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>

#include <cassert>
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
	constexpr uint64_t data_heap_size = 1024 * 1024;
	constexpr uint64_t texture_heap_size = 256 * 1024 * 1024;
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
    });

    // GPU resources
    GpuHeap data_heap = create_gpu_heap(device, data_heap_size);
    BumpAllocator data_allocator(data_heap.range);
    const GpuCpuRange<CubeVertex> vertex_allocation = data_allocator.allocate<CubeVertex>(cube_vertex_count);
    const GpuCpuRange<uint16_t> index_allocation = data_allocator.allocate<uint16_t>(cube_index_count);
    const GpuCpuRange<byte> upload_allocation = data_allocator.allocate(texture_byte_count);
    memcpy(vertex_allocation.cpu, cube_vertices, sizeof(cube_vertices));
	memcpy(index_allocation.cpu, cube_indices, sizeof(cube_indices));
    read_binary_file(NOGRAPHICSAPI_CUBE_TEXTURE_PATH, Span<byte>(upload_allocation.cpu, texture_byte_count));

	GpuHeap texture_descriptor_heap = create_gpu_heap(device, caps.texture_descriptor_size, MemoryType::texture_descriptor_heap);
	GpuHeap sampler_descriptor_heap = create_gpu_heap(device, caps.sampler_descriptor_size, MemoryType::sampler_descriptor_heap);
	TextureHeap texture_heap = create_texture_heap(device, texture_heap_size);
    TextureAllocator texture_allocator(device, texture_heap, 16);

	TimelinePoint latest_completion{.semaphore = create_timeline_semaphore(device)};

	// Textures
	PlacedTexture texture = texture_allocator.allocate({
		.extent = {.x = texture_width, .y = texture_height, .z = 1},
		.format = Format::rgba8_srgb,
		.usage = TextureUsage::sampled | TextureUsage::transfer_destination,
	});

	write_texture_descriptor(device, texture_descriptor_heap.range.cpu, texture.texture, TextureDescriptorType::sampled);
	write_sampler_descriptor(device, sampler_descriptor_heap.range.cpu, {
		.min_filter = Filter::nearest,
		.mag_filter = Filter::nearest,
		.address_u = AddressMode::clamp_to_edge,
		.address_v = AddressMode::clamp_to_edge,
	});

	CommandBuffer* upload_commands = begin_commands(device);
	copy_memory_to_texture(upload_commands, gpu_range(upload_allocation), texture.texture);

	barrier(upload_commands,
		Stage::transfer, Access::transfer_write,
		Stage::fragment, Access::shader_read);

	latest_completion.value++;
	submit({ upload_commands }, latest_completion);

	PlacedTexture depth{};
	RenderView* depth_render_view = nullptr;
	uint32x2 depth_extent{};
	uint64_t frame_index = 0;

	while (pump_example_window(window))
	{
        const SwapchainFrame frame = acquire(device);
        if (!frame.render_view)
            continue;

        // Window resize?
        if (frame.extent.x != depth_extent.x || frame.extent.y != depth_extent.y)
		{
			if (depth.texture)
				wait_timeline(latest_completion);
			destroy_render_view(depth_render_view);
			texture_allocator.free(depth);
			depth = texture_allocator.allocate({
				.extent = {.x = frame.extent.x, .y = frame.extent.y, .z = 1},
				.format = Format::d32_float,
				.usage = TextureUsage::depth_stencil_attachment,
			});
			depth_render_view = create_render_view(depth.texture);
			depth_extent = frame.extent;
		}

		// Render
		CommandBuffer* commands = begin_commands(device);
        set_texture_descriptor_heap(commands, gpu_range(texture_descriptor_heap));
        set_sampler_descriptor_heap(commands, gpu_range(sampler_descriptor_heap));

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

		set_depth_stencil(commands, {.depth_test = true, .depth_write = true});
		bind_pso(commands, cube_pso);

        float4x4 projection = math::perspective_rh_zo(45.0f * numbers::pi_v<float> / 180.0f,
                                                     float(frame.extent.x) / float(frame.extent.y), 0.1f, 100.0f);
        projection.rows[1].y = -projection.rows[1].y;
		const float4x4 view = math::look_at_rh({.x = 0.0f, .y = 3.0f, .z = 5.0f}, {.x = 0.0f, .y = 0.0f, .z = 0.0f}, {.x = 0.0f, .y = 1.0f, .z = 0.0f});
        const float4x4 model = math::rotation_y(radians_per_frame * float(frame_index++));
        const float4x4 mvp = projection * view * model;

		const CubeRootArguments root {
            .vertices = vertex_allocation.gpu,
            .transform = mvp,
        };

		draw_indexed(commands, root, gpu_range(index_allocation), IndexType::uint16, cube_index_count);

		end_render_pass(commands);

        // Submit
        latest_completion.value++;
		submit_and_present(device, {commands}, latest_completion);
	}

	wait_timeline(latest_completion);

	// Cleanup
	destroy_timeline_semaphore(latest_completion.semaphore);
    destroy_pso(cube_pso);
	destroy_render_view(depth_render_view);
	texture_allocator.free(depth);
	texture_allocator.free(texture);
    destroy_texture_heap(texture_heap);
	destroy_gpu_heap(sampler_descriptor_heap);
	destroy_gpu_heap(texture_descriptor_heap);
    destroy_gpu_heap(data_heap);

	destroy_device(device);
	close_example_window(window);
	return 0;
}
