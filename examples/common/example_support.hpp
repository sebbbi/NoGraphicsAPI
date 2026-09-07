#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#if !defined(_WIN32) && !defined(__linux__)
#error NoGraphicsAPI examples currently require Windows or Linux
#endif

// Free the returned buffer after creating the PSO that uses it.
gpu::Span<uint32> read_spirv(const char* path) noexcept;
bool read_binary_file(const char* path, gpu::Span<byte> data) noexcept;

double example_time_seconds() noexcept;

void* open_example_window(const char* title, uint32 width, uint32 height) noexcept;
bool pump_example_window(void* window) noexcept;
void close_example_window(void*& window) noexcept;
void* example_window_display() noexcept; // xcb_connection_t* on Linux; nullptr on Windows.
