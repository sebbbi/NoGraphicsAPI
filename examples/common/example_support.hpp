#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#if !defined(_WIN32)
#error NoGraphicsAPI examples currently require Windows
#endif

std::vector<std::uint32_t> read_spirv(const char* path);
bool read_binary_file(const char* path,
                      gpu::Span<std::byte> data) noexcept;

void* open_example_window(const char* title,
                          std::uint32_t width,
                          std::uint32_t height) noexcept;
bool pump_example_window(void* window) noexcept;
void close_example_window(void*& window) noexcept;
