#include "example_support.hpp"

#include <cassert>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace gpu;
using namespace std;

vector<uint32_t> read_spirv(const char* path)
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
    if (byte_count < static_cast<long>(5 * sizeof(uint32_t)) ||
        byte_count % static_cast<long>(sizeof(uint32_t)) != 0)
    {
        fprintf(stderr, "Invalid SPIR-V file size: %s\n", path);
        fclose(file);
        return {};
    }
    rewind(file);

    vector<uint32_t> code(
        static_cast<size_t>(byte_count) / sizeof(uint32_t));
    const bool read_succeeded =
        fread(code.data(), sizeof(uint32_t), code.size(), file) ==
        code.size();
    fclose(file);
    if (!read_succeeded || code[0] != 0x07230203u)
    {
        fprintf(stderr, "Invalid SPIR-V file: %s\n", path);
        return {};
    }
    return code;
}

bool read_binary_file(const char* path,
                      Span<byte> data) noexcept
{
    assert(path && data.data && data.size);
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        fprintf(stderr, "Failed to open resource file: %s\n", path);
        return false;
    }
    const bool size_succeeded =
        fseek(file, 0, SEEK_END) == 0 &&
        ftell(file) == static_cast<long>(data.size);
    rewind(file);
    const bool read_succeeded =
        size_succeeded &&
        fread(data.data, 1, data.size, file) == data.size;
    fclose(file);
    if (!read_succeeded)
        fprintf(stderr, "Invalid resource file: %s\n", path);
    return read_succeeded;
}

namespace
{

constexpr const char* window_class_name = "NoGraphicsAPI_example_window";
constexpr DWORD window_style = WS_OVERLAPPEDWINDOW;

LRESULT CALLBACK example_window_proc(HWND hwnd,
                                     UINT message,
                                     WPARAM wparam,
                                     LPARAM lparam) noexcept
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

void* open_example_window(const char* title,
                          uint32_t width,
                          uint32_t height) noexcept
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
            {
                return false;
            }
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
