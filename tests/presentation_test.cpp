#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <stdio.h>

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

int main()
{
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
        .swapchain_format = gpu::Format::bgra8_srgb,
        .desired_queue_count = 2,
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
    gpu::CommandPool* independent_pool = gpu::create_command_pool(device);
    gpu::TimelinePoint completion{.semaphore = gpu::create_timeline_semaphore(device)};
    uint32 width = 256;
    uint32 height = 192;

    for (uint32 frame_index = 0; frame_index != 8; ++frame_index)
    {
        gpu::wait_timeline(completion);
        gpu::reset_command_pool(present_pool);
        gpu::reset_command_pool(independent_pool);

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
        gpu::begin_render_pass(commands, {
            .colors = {{
                .render_view = frame.render_view,
                .load = gpu::LoadOp::clear,
                .clear = {.x = float(frame_index) / 8.0f, .y = 0.25f, .z = 0.5f, .w = 1.0f},
            }},
        });
        gpu::end_render_pass(commands);
        gpu::end_commands(commands);
        gpu::end_commands(second);
        ++completion.value;
        gpu::submit_and_present(device, {.commands = {second, commands}, .completion = completion});
    }

    gpu::wait_idle(device);
    gpu::destroy_command_pool(independent_pool);
    gpu::destroy_command_pool(present_pool);
    gpu::destroy_timeline_semaphore(completion.semaphore);
    gpu::destroy_device(device);
    CHECK(DestroyWindow(window));
    CHECK(UnregisterClassA(window_class.lpszClassName, window_class.hInstance));
    if (!failures)
        puts("Presentation, resize, zero drawable, and independent submission checks passed.");
    return failures ? 1 : 0;
}
