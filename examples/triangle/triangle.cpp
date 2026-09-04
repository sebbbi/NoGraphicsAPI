#include "example_support.hpp"

#include <cstdint>
#include <cstdio>

using namespace gpu;
using namespace std;

int main()
{
    constexpr uint32_t width = 512;
    constexpr uint32_t height = 512;

    void* window = open_example_window("NoGraphicsAPI triangle", width, height);
    const DeviceInit device_init = create_device({
        .window = window,
        .swapchain_format = Format::bgra8_srgb
    });

    Device* device = device_init.device;

    if (device_init.error != Error::none)
    {
        destroy_device(device);
        close_example_window(window);
        return 1;
    }

    printf("Using %s\n", get_device_caps(device).device_name);

    PSO* triangle_pso = create_graphics_pso(device, {
        .vertex_spirv = read_spirv(NOGRAPHICSAPI_VERTEX_SPV_PATH),
        .fragment_spirv = read_spirv(NOGRAPHICSAPI_FRAGMENT_SPV_PATH),
        .color_targets = { { .format = Format::bgra8_srgb } }
    });

    TimelinePoint latest_completion{ .semaphore = create_timeline_semaphore(device) };

    while (pump_example_window(window))
    {
        const SwapchainFrame frame = acquire(device);
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

	wait_timeline(latest_completion);

    destroy_timeline_semaphore(latest_completion.semaphore);
    destroy_pso(triangle_pso);

    destroy_device(device);
    close_example_window(window);
    return 0;
}
