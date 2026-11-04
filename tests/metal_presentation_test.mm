#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreGraphics/CoreGraphics.h>
#include <TargetConditionals.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char** argv)
{
    @autoreleasepool
    {
        gpu::DeviceDesc desc = {};
        for (gpu::int32 i = 1; i < argc; ++i)
        {
            if (strcmp(argv[i], "--triple") == 0) desc.desired_swapchain_image_count = 3;
            else if (strcmp(argv[i], "--steam-overlay") == 0) desc.steam_overlay_compatibility = true;
            else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
        }
        CAMetalLayer* layer = [CAMetalLayer new];
        layer.drawableSize = CGSizeMake(16, 16);
        // Initialization must override a conflicting caller-provided layer setting.
        layer.framebufferOnly = desc.steam_overlay_compatibility ? YES : NO;
        desc.window = layer;
        desc.swapchain_format = gpu::Format::rgba16_float;
        desc.swapchain_color_space = gpu::ColorSpace::extended_srgb_linear;
        const gpu::DeviceInit edr = gpu::create_device(desc);
        if (edr.error == gpu::Error::unsupported) { [layer release]; return 77; }
        if (!edr.device || layer.pixelFormat != MTLPixelFormatRGBA16Float || !layer.wantsExtendedDynamicRangeContent
            || !CFEqual(CGColorSpaceGetName(layer.colorspace), kCGColorSpaceExtendedLinearSRGB) || layer.EDRMetadata) return 1;
        gpu::wait_idle(edr.device);
        gpu::destroy_device(edr.device);
        desc.swapchain_format = gpu::Format::bgra8_unorm;
        if (gpu::create_device(desc).error != gpu::Error::unsupported || layer.pixelFormat != MTLPixelFormatRGBA16Float) return 1;
        desc.swapchain_color_space = gpu::ColorSpace::srgb;
        const gpu::DeviceInit init = gpu::create_device(desc);
        if (init.error == gpu::Error::unsupported) { [layer release]; return 77; }
        if (!init.device) { [layer release]; return 1; }
#if TARGET_OS_OSX
        if (layer.framebufferOnly != (desc.steam_overlay_compatibility ? NO : YES))
#else
        if (!layer.framebufferOnly)
#endif
        {
            fprintf(stderr, "Drawable framebuffer-only setting does not match the requested presentation mode.\n");
            gpu::wait_idle(init.device);
            gpu::destroy_device(init.device);
            [layer release];
            return 1;
        }
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
            gpu::wait_idle(init.device);
            const bool hdr = (frame & 2u) != 0;
            const gpu::Format format = hdr ? gpu::Format::rgba16_float : gpu::Format::bgra8_unorm;
            const gpu::ColorSpace space = hdr ? gpu::ColorSpace::extended_srgb_linear : gpu::ColorSpace::srgb;
            if (gpu::set_swapchain_format(init.device, format, space) != gpu::Error::none
                || bool(layer.wantsExtendedDynamicRangeContent) != hdr
                || layer.pixelFormat != (hdr ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm)
                || !CFEqual(CGColorSpaceGetName(layer.colorspace), hdr ? kCGColorSpaceExtendedLinearSRGB : kCGColorSpaceSRGB)) return 2;
            if (gpu::set_swapchain_format(init.device, format, hdr ? gpu::ColorSpace::srgb : gpu::ColorSpace::extended_srgb_linear)
                != gpu::Error::unsupported || bool(layer.wantsExtendedDynamicRangeContent) != hdr) return 2;
            if (!gpu::set_present_interval(init.device, frame & 1u ? 16666667 : 0)) return 2;
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
                                                         .clear = {.x = hdr ? 3.0f : 0.2f, .y = 0.4f, .z = 0.6f, .w = 1}}}});
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
