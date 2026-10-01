# Building and integration

See the README for [Windows](../README.md#windows-installation-and-quick-start),
[macOS](../README.md#macos-installation-and-quick-start), and [hardware requirements](../README.md#hardware-requirements).
CMake selects native Metal on Apple platforms and Vulkan elsewhere. Metal 4 is preferred at runtime; Metal 3 uses the same build.

| Target | Toolchain |
| --- | --- |
| Windows x86-64 | MSVC or clang-cl. |
| Linux x86-64 | GCC or Clang; headless library and tests. |
| macOS 15+ ARM64 | Xcode 26+ SDK and Metal compiler. |
| iOS/iPadOS 18+ ARM64 | Xcode 26+ with the device SDK. |

MinGW, 32-bit targets, and non-Apple ARM targets are unsupported. Apple hardware requirements are listed separately from CPU build architectures.

## Library build

Examples and tests are off by default. All library sources are included in the repository;
configuration needs no Git or network access.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build --config Release --prefix path/to/install
```

For examples and tests, configure with `-DNOGRAPHICSAPI_BUILD_EXAMPLES=ON` and
`-DNOGRAPHICSAPI_BUILD_TESTS=ON`, then run `ctest --test-dir build -C Release --output-on-failure`.
Debug Vulkan builds enable validation when installed. Metal validation is opt-in through Xcode or
`MTL_DEBUG_LAYER=1`; see [Metal validation](metal-validation.md) for supported instrumentation and known driver issues.
Apple example/test shader builds require stock Slang 2026.18.2+ and Xcode's Metal compiler. Vulkan shader builds require
Slang 2026.14.1+ and SPIRV-Tools 2026.3+. Shader tools are not required for a library-only build.

For iOS, disable desktop examples and tests and select the device SDK:

```sh
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0
cmake --build build-ios --config Release
```

Compile metallibs for the target SDK; macOS libraries cannot be used on iOS. See [Slang compilation](slang.md).
For presentation, supply an application-owned `CAMetalLayer*` as `DeviceDesc::window`.

Tests also build a private compatibility library: `_buffer_commands` tests force Vulkan buffer commands or Metal 3
even on devices supporting the preferred path. Normal library builds always select the preferred available backend.

## Using the library

Add the source tree directly:

```cmake
add_subdirectory(path/to/NoGraphicsAPI)
target_link_libraries(my_application PRIVATE NoGraphicsAPI::NoGraphicsAPI)
```

Or use the installed packages:

```cmake
find_package(NoGraphicsAPI CONFIG REQUIRED)
find_package(NoGraphicsAPIUtility CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE
    NoGraphicsAPI::NoGraphicsAPI
    NoGraphicsAPIUtility::math
    NoGraphicsAPIUtility::textures)
```

The utility package is optional. Component targets include `types`, `math`, `allocators`, `textures`, and
`uploads`, under the `NoGraphicsAPIUtility::` namespace. The
[upload queue header](../utility/include/NoGraphicsAPIUtility/upload_queue.hpp) describes upload
batching and synchronization; [cube](../examples/cube/cube.cpp) shows its use.
