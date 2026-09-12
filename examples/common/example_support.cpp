#include "example_support.hpp"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>
#endif

using namespace gpu;

Span<uint32> read_spirv(const char* path) noexcept
{
    assert(path);
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        fprintf(stderr, "Failed to open SPIR-V file: %s\n", path);
        return {};
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fprintf(stderr, "Failed to read SPIR-V file: %s\n", path);
        fclose(file);
        return {};
    }
    const long byte_count = ftell(file);
    if (byte_count < static_cast<long>(5 * sizeof(uint32)) || byte_count % static_cast<long>(sizeof(uint32)) != 0)
    {
        fprintf(stderr, "Invalid SPIR-V file size: %s\n", path);
        fclose(file);
        return {};
    }
    rewind(file);

    Span<uint32> code(static_cast<uint32*>(malloc(size_t(byte_count))), size_t(byte_count) / sizeof(uint32));
    const bool read_succeeded = fread(code.data, sizeof(uint32), code.size, file) == code.size;
    fclose(file);
    if (!read_succeeded || code.data[0] != 0x07230203u)
    {
        fprintf(stderr, "Invalid SPIR-V file: %s\n", path);
        free(code.data);
        return {};
    }
    return code;
}

bool read_binary_file(const char* path, Span<byte> data) noexcept
{
    assert(path && data.data && data.size);
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        fprintf(stderr, "Failed to open resource file: %s\n", path);
        return false;
    }
    const bool size_succeeded = fseek(file, 0, SEEK_END) == 0 && ftell(file) == static_cast<long>(data.size);
    rewind(file);
    const bool read_succeeded = size_succeeded && fread(data.data, 1, data.size, file) == data.size;
    fclose(file);
    if (!read_succeeded)
        fprintf(stderr, "Invalid resource file: %s\n", path);
    return read_succeeded;
}

#if defined(_WIN32)

double example_time_seconds() noexcept
{
    static double seconds_per_tick = 0.0;
    if (seconds_per_tick == 0.0)
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        seconds_per_tick = 1.0 / double(frequency.QuadPart);
    }
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return double(counter.QuadPart) * seconds_per_tick;
}

namespace
{

constexpr const char* window_class_name = "NoGraphicsAPI_example_window";
constexpr DWORD window_style = WS_OVERLAPPEDWINDOW;

LRESULT CALLBACK example_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept
{
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
        {
            ShowWindow(hwnd, SW_HIDE);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcA(hwnd, message, wparam, lparam);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, message, wparam, lparam);
    }
}

} // namespace

void* open_example_window(const char* title, uint32 width, uint32 height) noexcept
{
    assert(title && width && height);
    const HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSEXA window_class{
        .cbSize = sizeof(WNDCLASSEXA),
        .style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC,
        .lpfnWndProc = example_window_proc,
        .hInstance = instance,
        .hCursor = LoadCursorA(nullptr, IDC_ARROW),
        .lpszClassName = window_class_name,
    };
    if (!RegisterClassExA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return {};

    RECT rectangle{
        .right = static_cast<LONG>(width),
        .bottom = static_cast<LONG>(height),
    };
    if (!AdjustWindowRectEx(&rectangle, window_style, FALSE, 0))
        return {};

    const HWND hwnd = CreateWindowExA(
        0,
        window_class_name,
        title,
        window_style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!hwnd)
        return {};
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    return hwnd;
}

bool pump_example_window(void* window) noexcept
{
    for (;;)
    {
        MSG message{};
        while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
                return false;
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }

        if (!IsIconic(static_cast<HWND>(window)))
            return true;
        WaitMessage();
    }
}

void close_example_window(void*& window) noexcept
{
    if (window)
        DestroyWindow(static_cast<HWND>(window));
    window = nullptr;
}

void* example_window_display() noexcept
{
    return nullptr;
}

#elif defined(__linux__)

double example_time_seconds() noexcept
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

namespace
{

xcb_connection_t* g_connection = nullptr;
xcb_atom_t g_wm_delete_window = XCB_ATOM_NONE;

} // namespace

void* open_example_window(const char* title, uint32 width, uint32 height) noexcept
{
    assert(title && width && height);
    int screen_number = 0;
    g_connection = xcb_connect(nullptr, &screen_number);
    if (xcb_connection_has_error(g_connection))
        return {};

    const xcb_setup_t* setup = xcb_get_setup(g_connection);
    xcb_screen_iterator_t screen_iterator = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < screen_number; ++index)
        xcb_screen_next(&screen_iterator);
    xcb_screen_t* screen = screen_iterator.data;

    const xcb_window_t window = xcb_generate_id(g_connection);
    const uint32 value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    const uint32 value_list[]{
        screen->black_pixel,
        XCB_EVENT_MASK_KEY_PRESS,
    };
    xcb_create_window(
        g_connection,
        XCB_COPY_FROM_PARENT,
        window,
        screen->root,
        0, 0,
        static_cast<uint16>(width),
        static_cast<uint16>(height),
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual,
        value_mask,
        value_list);

    xcb_change_property(
        g_connection,
        XCB_PROP_MODE_REPLACE,
        window,
        XCB_ATOM_WM_NAME,
        XCB_ATOM_STRING,
        8,
        static_cast<uint32>(strlen(title)),
        title);

    const xcb_intern_atom_cookie_t protocols_cookie =
        xcb_intern_atom(g_connection, 1, static_cast<uint16>(strlen("WM_PROTOCOLS")), "WM_PROTOCOLS");
    const xcb_intern_atom_cookie_t delete_window_cookie =
        xcb_intern_atom(g_connection, 0, static_cast<uint16>(strlen("WM_DELETE_WINDOW")), "WM_DELETE_WINDOW");
    xcb_intern_atom_reply_t* protocols_reply = xcb_intern_atom_reply(g_connection, protocols_cookie, nullptr);
    xcb_intern_atom_reply_t* delete_window_reply = xcb_intern_atom_reply(g_connection, delete_window_cookie, nullptr);
    if (protocols_reply && delete_window_reply)
    {
        g_wm_delete_window = delete_window_reply->atom;
        xcb_change_property(
            g_connection,
            XCB_PROP_MODE_REPLACE,
            window,
            protocols_reply->atom,
            XCB_ATOM_ATOM,
            32,
            1,
            &g_wm_delete_window);
    }
    free(protocols_reply);
    free(delete_window_reply);

    xcb_map_window(g_connection, window);
    xcb_flush(g_connection);
    return reinterpret_cast<void*>(static_cast<uintptr>(window));
}

bool pump_example_window(void* window) noexcept
{
    (void)window;
    for (xcb_generic_event_t* event = xcb_poll_for_event(g_connection); event; event = xcb_poll_for_event(g_connection))
    {
        const uint8 response_type = event->response_type & 0x7f;
        if (response_type == XCB_CLIENT_MESSAGE)
        {
            const xcb_client_message_event_t* client_message = reinterpret_cast<xcb_client_message_event_t*>(event);
            if (client_message->data.data32[0] == g_wm_delete_window)
            {
                free(event);
                return false;
            }
        }
        else if (response_type == XCB_KEY_PRESS)
        {
            // Keycode 9 is Escape under the standard PC-105 layout used by virtually every X11 setup.
            const xcb_key_press_event_t* key_press = reinterpret_cast<xcb_key_press_event_t*>(event);
            if (key_press->detail == 9)
            {
                free(event);
                return false;
            }
        }
        free(event);
    }
    return true;
}

void close_example_window(void*& window) noexcept
{
    if (window && g_connection)
        xcb_destroy_window(g_connection, static_cast<xcb_window_t>(reinterpret_cast<uintptr>(window)));
    if (g_connection)
        xcb_disconnect(g_connection);
    g_connection = nullptr;
    g_wm_delete_window = XCB_ATOM_NONE;
    window = nullptr;
}

void* example_window_display() noexcept
{
    return g_connection;
}

#endif
