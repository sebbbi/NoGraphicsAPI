#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace
{

int failures = 0;

#define CHECK(expression) do { if (!(expression)) { fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expression); ++failures; } } while (0)

void pump_messages() noexcept
{
    MSG message{};
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
}

} // namespace

int main(int argc, char** argv)
{
    const bool queue_families = argc == 2 && strcmp(argv[1], "--queue-families") == 0;
    const bool scrgb = argc == 2 && strcmp(argv[1], "--scrgb") == 0;
    const bool switch_color_space = argc == 2 && strcmp(argv[1], "--switch-color-space") == 0;
    const WNDCLASSEXA window_class{
        .cbSize = sizeof(WNDCLASSEXA),
        .lpfnWndProc = DefWindowProcA,
        .hInstance = GetModuleHandleA(nullptr),
        .lpszClassName = "NoGraphicsAPI_presentation_test",
    };
    if (!RegisterClassExA(&window_class))
    {
        fprintf(stderr, "RegisterClassExA failed: %lu\n", GetLastError());
        return 1;
    }
    HWND window = CreateWindowExA(0, window_class.lpszClassName, "NoGraphicsAPI presentation test", WS_POPUP,
                                 0, 0, 256, 192, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window)
    {
        fprintf(stderr, "CreateWindowExA failed: %lu\n", GetLastError());
        UnregisterClassA(window_class.lpszClassName, window_class.hInstance);
        return 1;
    }

    const gpu::DeviceInit device_init = gpu::create_device({
        .window = window,
        .swapchain_format = scrgb ? gpu::Format::rgba16_float : gpu::Format::bgra8_srgb,
        .swapchain_color_space = scrgb ? gpu::ColorSpace::extended_srgb_linear : gpu::ColorSpace::srgb,
        .desired_queue_count = 2,
        .desired_compute_queue_count = queue_families ? 1u : 0u,
        .desired_copy_queue_count = queue_families ? 1u : 0u,
    });
    if (device_init.error != gpu::Error::none)
    {
        gpu::destroy_device(device_init.device);
        DestroyWindow(window);
        UnregisterClassA(window_class.lpszClassName, window_class.hInstance);
        if (device_init.error == gpu::Error::unsupported)
            return 77;
        fprintf(stderr, "create_device failed: %u\n", unsigned(device_init.error));
        return 1;
    }

    gpu::Device* device = device_init.device;
    gpu::CommandPool* present_pool = gpu::create_command_pool(device);
    gpu::CommandPool* middle_pool = gpu::create_command_pool(device);
    gpu::CommandPool* last_pool = gpu::create_command_pool(device);
    gpu::CommandPool* independent_pool = gpu::create_command_pool(device);
    uint64 timestamp_cpu[3]{};
    gpu::TimelinePoint completion{.semaphore = gpu::create_timeline_semaphore(device)};
    uint32 width = 256;
    uint32 height = 192;
    bool hdr_output = scrgb;
    bool skipped = false;

    for (uint32 frame_index = 0; frame_index != 8; ++frame_index)
    {
        gpu::reset_command_pool(present_pool);
        gpu::reset_command_pool(middle_pool);
        gpu::reset_command_pool(last_pool);
        gpu::reset_command_pool(independent_pool);
        for (uint32 index = 0; index != 3; ++index) timestamp_cpu[index] = ~uint64{0};

        if (switch_color_space)
        {
            gpu::wait_idle(device);
            if (frame_index == 1 || frame_index == 5 || frame_index == 6)
            {
                const bool requested_hdr = frame_index != 6;
                const gpu::Error error = gpu::set_swapchain_format(device,
                    requested_hdr ? gpu::Format::rgba16_float : gpu::Format::bgra8_srgb,
                    requested_hdr ? gpu::ColorSpace::extended_srgb_linear : gpu::ColorSpace::srgb);
                if (frame_index == 1 && error == gpu::Error::unsupported)
                {
                    skipped = true;
                    break;
                }
                CHECK(error == gpu::Error::none);
                if (error != gpu::Error::none) break;
                hdr_output = requested_hdr;
            }
            // Rejection must preserve the active mode and leave the next acquire usable.
            CHECK(gpu::set_swapchain_format(device, gpu::Format::d32_float, gpu::ColorSpace::srgb) == gpu::Error::unsupported);
            CHECK(gpu::set_swapchain_format(device, hdr_output ? gpu::Format::rgba16_float : gpu::Format::bgra8_srgb,
                                           hdr_output ? gpu::ColorSpace::extended_srgb_linear : gpu::ColorSpace::srgb) == gpu::Error::none);
        }

        if (frame_index == 2)
        {
            width = 384;
            height = 256;
            CHECK(SetWindowPos(window, nullptr, 0, 0, int(width), int(height), SWP_NOZORDER | SWP_NOACTIVATE));
        }
        if (frame_index == 4)
        {
            // A zero-size hidden window exercises minimized-surface behavior without showing a native window.
            CHECK(SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE));
            pump_messages();
            const gpu::uint32x2 empty_extent = gpu::get_drawable_extent(device);
            CHECK(empty_extent.x == 0 && empty_extent.y == 0);
            if (switch_color_space)
            {
                CHECK(gpu::set_swapchain_format(device, gpu::Format::bgra8_srgb, gpu::ColorSpace::srgb) == gpu::Error::none);
                hdr_output = false;
                CHECK(gpu::set_swapchain_format(device, gpu::Format::d32_float, gpu::ColorSpace::srgb) == gpu::Error::unsupported);
            }
            gpu::CommandBuffer* empty_commands = gpu::begin_commands(present_pool);
            const gpu::SwapchainFrame empty_frame = gpu::acquire(empty_commands);
            CHECK(!empty_frame.render_view && empty_frame.extent.x == 0 && empty_frame.extent.y == 0);
            if (empty_frame.render_view)
            {
                gpu::end_commands(empty_commands);
                ++completion.value;
                gpu::submit_and_present(device, {.commands = {empty_commands}, .completion = completion});
                gpu::wait_timeline(completion);
            }
            width = 320;
            height = 240;
            CHECK(SetWindowPos(window, nullptr, 0, 0, int(width), int(height), SWP_NOZORDER | SWP_NOACTIVATE));
        }
        pump_messages();
        const gpu::uint32x2 extent = gpu::get_drawable_extent(device);
        CHECK(extent.x == width && extent.y == height);
        gpu::CommandBuffer* commands = gpu::begin_commands(present_pool);
        const gpu::SwapchainFrame frame = gpu::acquire(commands);
        CHECK(frame.render_view && frame.extent.x == width && frame.extent.y == height);
        if (!frame.render_view) break;

        gpu::CommandBuffer* first = gpu::begin_commands(independent_pool);
        gpu::barrier(first, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::transfer, gpu::Access::transfer_read);
        gpu::end_commands(first);
        gpu::CommandBuffer* second = gpu::begin_commands(independent_pool);
        gpu::barrier(second, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::transfer, gpu::Access::transfer_read);

        // Submit unrelated work while the presentation buffer and another independent buffer are still recording.
        ++completion.value;
        gpu::submit(device, {.commands = {first}, .completion = completion});
        const gpu::ColorAttachment colors[]{{
            .render_view = frame.render_view,
            .load = gpu::LoadOp::clear,
            .clear = {.x = float(frame_index) / 8.0f, .y = hdr_output ? 2.5f : 0.25f, .z = hdr_output ? 12.5f : 0.5f, .w = 1.0f},
        }};
        const gpu::RenderingDesc rendering{.colors = colors};
        const bool split_pass = (frame_index & 1u) != 0;
        gpu::begin_render_pass(commands, rendering, split_pass ? gpu::RenderingFlags::suspending : gpu::RenderingFlags::none);
        gpu::write_timestamp(commands, timestamp_cpu);
        gpu::end_render_pass(commands);
        gpu::end_commands(commands);
        gpu::end_commands(second);
        ++completion.value;
        if (split_pass)
        {
            // Record continuations in reverse order, using the same clear description in every segment.
            gpu::CommandBuffer* last = gpu::begin_commands(last_pool);
            gpu::begin_render_pass(last, rendering, gpu::RenderingFlags::resuming);
            gpu::write_timestamp(last, timestamp_cpu + 2);
            gpu::end_render_pass(last);
            gpu::end_commands(last);

            gpu::CommandBuffer* middle = gpu::begin_commands(middle_pool);
            gpu::begin_render_pass(middle, rendering, gpu::RenderingFlags::resuming | gpu::RenderingFlags::suspending);
            gpu::write_timestamp(middle, timestamp_cpu + 1);
            gpu::end_render_pass(middle);
            gpu::end_commands(middle);
            gpu::submit_and_present(device, {.commands = {second, commands, middle, last}, .completion = completion});
        }
        else
        {
            gpu::submit_and_present(device, {.commands = {second, commands}, .completion = completion});
        }
        gpu::wait_timeline(completion);
        gpu::read_timestamps(present_pool);
        gpu::read_timestamps(middle_pool);
        gpu::read_timestamps(last_pool);
        CHECK(timestamp_cpu[0] != ~uint64{0});
        if (split_pass)
        {
            CHECK(timestamp_cpu[1] != ~uint64{0} && timestamp_cpu[2] != ~uint64{0});
            CHECK(timestamp_cpu[0] <= timestamp_cpu[1] && timestamp_cpu[1] <= timestamp_cpu[2]);
        }
        else
        {
            CHECK(timestamp_cpu[1] == ~uint64{0} && timestamp_cpu[2] == ~uint64{0});
        }
    }

    gpu::wait_idle(device);
    gpu::destroy_command_pool(independent_pool);
    gpu::destroy_command_pool(last_pool);
    gpu::destroy_command_pool(middle_pool);
    gpu::destroy_command_pool(present_pool);
    gpu::destroy_timeline_semaphore(completion.semaphore);
    gpu::destroy_device(device);
    CHECK(DestroyWindow(window));
    CHECK(UnregisterClassA(window_class.lpszClassName, window_class.hInstance));
    if (skipped && !failures)
    {
        puts("The surface does not support FP16 scRGB; presentation color-space switch test skipped.");
        return 77;
    }
    if (!failures)
        puts("Presentation, color-space selection, suspended rendering, timestamp readback, resize, zero drawable, and independent submission checks passed.");
    return failures ? 1 : 0;
}
