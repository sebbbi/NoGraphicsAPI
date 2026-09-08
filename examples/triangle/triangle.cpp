#include "example_support.hpp"

#include <stdio.h>
#include <stdlib.h>

using namespace gpu;

int main()
{
    constexpr uint32 width = 512;
    constexpr uint32 height = 512;

    void* window = open_example_window("NoGraphicsAPI triangle", width, height);
    Device* device = create_device({.window = window, .swapchain_format = Format::bgra8_srgb}).device;

    if (!window || !device)
    {
        destroy_device(device);
        close_example_window(window);
        return 1;
    }

    printf("Using %s\n", get_device_caps(device).device_name);

    const Span<uint32> vertex_spirv = read_spirv(NOGRAPHICSAPI_VERTEX_SPV_PATH);
    const Span<uint32> fragment_spirv = read_spirv(NOGRAPHICSAPI_FRAGMENT_SPV_PATH);
    PSO* triangle_pso = create_graphics_pso(device, {
        .vertex_spirv = vertex_spirv,
        .fragment_spirv = fragment_spirv,
        .color_targets = { { .format = Format::bgra8_srgb } }
    });
    free(fragment_spirv.data);
    free(vertex_spirv.data);

    TimelinePoint latest_completion{ .semaphore = create_timeline_semaphore(device) };

    while (pump_example_window(window))
    {
        const SwapchainFrame frame = acquire(device);
        if (!frame.render_view)
            continue;
        CommandBuffer* commands = begin_commands(device);
        begin_render_pass(commands, {
            .colors = { { .render_view = frame.render_view, .load = LoadOp::clear } },
        });
        bind_pso(commands, triangle_pso);
        draw(commands, {}, 3);
        end_render_pass(commands);
        latest_completion.value++;
        submit_and_present(device, { commands }, latest_completion);
    }

    wait_idle(device);

    destroy_timeline_semaphore(latest_completion.semaphore);
    destroy_pso(triangle_pso);

    destroy_device(device);
    close_example_window(window);
    return 0;
}
