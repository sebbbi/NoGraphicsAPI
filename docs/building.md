# Building and integration

See the README for [Windows installation](../README.md#windows-installation-and-quick-start) and
[hardware requirements](../README.md#hardware-requirements). Windows supports MSVC and clang-cl;
GNU and Clang can build the headless library on other platforms. MinGW, 32-bit x86, and ARM are not supported.

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
Debug builds enable Vulkan validation when installed.

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
