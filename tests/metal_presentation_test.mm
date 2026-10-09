#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdio.h>
int main(int argc, char**)
{
    @autoreleasepool
    {
        CAMetalLayer* layer = [CAMetalLayer new];
        layer.drawableSize = CGSizeMake(16, 16);
        const gpu::DeviceInit init = gpu::create_device({.window = layer, .desired_swapchain_image_count = argc > 1 ? 3u : 2u});
        if (init.error == gpu::Error::unsupported) { [layer release]; return 77; }
        if (!init.device) { [layer release]; return 1; }
        if (gpu::get_device_caps(init.device).indirect_mesh_draw != bool([layer.device supportsFamily:MTLGPUFamilyApple9]))
        {
            fprintf(stderr, "Native indirect mesh capability does not match the Metal device.\n");
            gpu::destroy_device(init.device);
            [layer release];
            return 1;
        }
        gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(init.device);
        gpu::CommandPool* pools[2] = {gpu::create_command_pool(init.device), gpu::create_command_pool(init.device)};
        for (uint64 frame = 1; frame <= 8; ++frame)
        {
            if (frame > 2) gpu::wait_timeline({.semaphore = timeline, .value = frame - 2});
            if (frame == 5)
            {
                gpu::wait_idle(init.device);
                layer.drawableSize = CGSizeMake(32, 24);
            }
            gpu::CommandPool* pool = pools[(frame - 1) & 1u];
            gpu::reset_command_pool(pool);
            const gpu::uint32x2 extent = gpu::get_drawable_extent(init.device);
            gpu::CommandBuffer* commands = gpu::begin_commands(pool);
            gpu::SwapchainFrame target = gpu::acquire(commands);
            if (!target.render_view || target.extent.x != extent.x || target.extent.y != extent.y) return 2;
            gpu::begin_render_pass(commands, {.colors = {{.render_view = target.render_view, .load = gpu::LoadOp::clear,
                                                         .clear = {.x = 0.2f, .y = 0.4f, .z = 0.6f, .w = 1}}}});
            gpu::end_render_pass(commands);
            gpu::end_commands(commands);
            gpu::submit_and_present(init.device, {.commands = {commands}, .completion = {.semaphore = timeline, .value = frame}});
        }
        gpu::wait_idle(init.device);
        for (gpu::CommandPool* pool : pools) gpu::destroy_command_pool(pool);
        gpu::destroy_timeline_semaphore(timeline);
        gpu::destroy_device(init.device);
        [layer release];

    }
}
