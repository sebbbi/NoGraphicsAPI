# NoGraphicsAPI

`NoGraphicsAPI` is an experimental Vulkan 1.4 implementation of the ideas in Sebastian Aaltonen's
[*No Graphics API*](https://www.sebastianaaltonen.com/blog/no-graphics-api). It explores how much of
a conventional graphics API disappears when shaders use 64-bit GPU pointers, texture and sampler
descriptors live in application-owned GPU memory, and synchronization describes hazards instead of
resource state.

The Vulkan backend is implemented and exercised by three example applications. Metal support is not
implemented; [the Metal 4 design](docs/metal-porting.md) records the proposed mapping and open issues.

## How it maps to *No Graphics API*

- **GPU pointers replace buffer objects and bindings.** `create_gpu_heap()` returns a raw allocation
  with a GPU address and, for mapped memory, a CPU address. Address-based commands consume
  `GpuRange {gpu, size}` directly, and shaders follow typed 64-bit pointers for vertex fetch and
  arbitrary data structures.
- **Applications own descriptor heaps.** Texture and sampler descriptor heaps are mapped GPU heaps.
  The application chooses slots, writes descriptors through the CPU address, binds the GPU range,
  and passes 32-bit indices to shaders.
- **Root data is one small payload.** A shared C++/Slang structure is copied with
  `vkCmdPushDataEXT` for each draw or dispatch. Its pointer fields are GPU addresses. Unlike the
  blog's GPU-resident, stage-specific roots, graphics stages share one CPU-supplied root.
- **Barriers describe execution and memory hazards.** The public API exposes global stage/access
  barriers, not per-resource transition lists. Normal textures remain in one unified layout.
- **Pipeline binding state stays small.** There are no public buffer objects, descriptor sets,
  descriptor layouts, pipeline layouts, or sampler objects. PSOs contain shader and fixed-function
  state, while data arrives through the root and descriptor heaps.
- **Submission is explicit and asynchronous.** Applications provide timeline points for reuse and
  deferred destruction. Submitted command buffers are one-shot.

The [design comparison](docs/no-graphics-api-comparison.md) separates faithful mappings, Vulkan-driven
differences, and features that remain outside the prototype.

## Vulkan realization

The backend intentionally creates no `VkDescriptorSetLayout`, `VkDescriptorPool`, `VkDescriptorSet`,
or `VkPipelineLayout`. Its central extensions are:

- `VK_EXT_descriptor_heap` for application-owned resource/sampler heaps and `vkCmdPushDataEXT`;
- `VK_KHR_device_address_commands` for address-based index, indirect, and copy commands;
- `VK_KHR_shader_untyped_pointers` as the descriptor-heap SPIR-V prerequisite;
- `VK_KHR_unified_image_layouts`, when available, to optimize ordinary texture access in
  `VK_IMAGE_LAYOUT_GENERAL`;
- `VK_EXT_mesh_shader` for mesh pipelines and dispatch.

Win32 presentation uses `VK_KHR_surface`, `VK_KHR_win32_surface`,
`VK_KHR_get_surface_capabilities2`, `VK_KHR_surface_maintenance1`, `VK_KHR_swapchain`, and
`VK_KHR_swapchain_maintenance1`, with the original EXT maintenance extensions accepted as a fallback.
Debug builds also enable `VK_EXT_debug_utils` and the Khronos validation layer when available.

Vulkan 1.4 supplies buffer device addresses, timeline semaphores, dynamic rendering,
synchronization2, scalar block layout, and the remaining core features. See
[Vulkan support](docs/vulkan-support.md) for the concise feature and command mapping.

## GPU memory and descriptor heaps

There are no public buffer objects or internal suballocators. Applications own data and descriptor
heaps; optimal-tiled textures use separate GPU-only heaps. The optional utility library provides
application-side data and texture allocation policies.

`GpuRange` is the non-owning address/size view used by commands. Texture and sampler descriptors are
addressed in Slang through the standard heap syntax:

```slang
Texture2D<float4> texture = ResourceDescriptorHeap[texture_index];
SamplerState sampler = SamplerDescriptorHeap[sampler_index];
float4 texel = texture.Sample(sampler, uv);
```

## Shared root ABI

Shared scalar, vector, and matrix types come from `<NoGraphicsAPIUtility/shader_types.h>`. Root
structures are declared once and included by C++ and Slang:

```cpp
struct RootArguments
{
    Vertex* vertices;
    float4x4 mvp;
};
```

On the CPU, pointer fields receive GPU virtual addresses while ordinary values are copied directly:

```cpp
GpuCpuRange<Vertex> vertex_memory = bump_allocator.allocate<Vertex>(vertex_count);
RootArguments root{
    .vertices = vertex_memory.gpu,
    .mvp = mvp,
};
gpu::draw(commands, root, vertex_count);
```

Slang declares the same root as push-constant data and reads both pointer and value fields directly:

```slang
[[vk::push_constant]] ConstantBuffer<RootArguments> root;

Vertex vertex = root.vertices[vertex_id];
float4x4 mvp = root.mvp;
```

The draw or dispatch copies the root bytes immediately through `vkCmdPushDataEXT`; the root does not
need to outlive the call. Shared structures use C layout, and matrix-bearing roots use row-major matrix
layout. Root values must be trivially copyable, have a size divisible by four, and be no larger than
`DeviceCaps::max_push_data_size`.

Public descriptor structures have useful defaults. Call sites use C++20 designated initializers to
name only fields that differ from those defaults. `Span` and `ByteSpan` are non-owning pointer/count
views used for function inputs.

The full shader-side contract is in the [Slang shader contract](docs/slang.md).

## Build and run

Requirements:

- CMake 3.24+, a C++20 compiler, and Vulkan SDK headers and development libraries version 1.4.357
  or newer;
- a little-endian x86-64 target; the optional utility math target additionally requires AVX2 and FMA;
- a Vulkan 1.4 loader and device exposing the required descriptor-heap, device-address-command,
  untyped-pointer, and mesh extensions above, plus their required BDA, synchronization, and
  16-bit/scalar-layout features;
- coherent CPU-visible GPU memory (for example through PCIe Resizable BAR on a discrete GPU or UMA on
  an integrated GPU), plus device-local memory compatible with the supported buffers and textures;
- Slang 2026.14.1+ and SPIRV-Tools 2026.3+ when building the examples.

MSVC and clang-cl are supported on Windows. GNU and Clang can build the headless library on other
platforms. MinGW, 32-bit x86, and ARM targets are not supported.

The utility implementation is shipped in-tree, so configuration has no Git or network dependency.
The default configuration builds NoGraphicsAPI and its companion utility library. Examples and tests
are opt-in so embedding NoGraphicsAPI with `add_subdirectory()` does not add development targets:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix path/to/install
```

The same install provides independent `NoGraphicsAPI` and `NoGraphicsAPIUtility` packages:

```cmake
find_package(NoGraphicsAPI CONFIG REQUIRED)
find_package(NoGraphicsAPIUtility CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE
    NoGraphicsAPI::NoGraphicsAPI
    NoGraphicsAPIUtility::math
    NoGraphicsAPIUtility::textures)
```

NoGraphicsAPIUtility provides shared C++/Slang types, math, data and texture suballocation, and a
timeline-driven `DeleteQueue`. These are optional application-side policies; NoGraphicsAPI does not
depend on them. The queue delays allocator reuse and resource destruction until the application
timeline completes.

For repository development on Windows, enable the examples and tests explicitly. Building examples
requires the Slang and SPIR-V Tools versions listed above.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNOGRAPHICSAPI_BUILD_EXAMPLES=ON -DNOGRAPHICSAPI_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The Windows examples use the normal swapchain path:

- [`triangle`](examples/triangle/triangle.cpp): the minimum vertex/fragment draw and presentation path;
- [`cube`](examples/cube/cube.cpp): typed GPU-pointer vertex fetch plus application-owned texture and
  sampler heaps;
- [`deferred_renderer`](examples/deferred_renderer/deferred_renderer.cpp): GPU-compute simulation and
  mesh-shader rendering in a multi-pass workload with pointer-based scene data, barriers between
  passes, and timeline-managed CPU/GPU overlap.

They can be run from their corresponding directories under `build/examples` when
`NOGRAPHICSAPI_BUILD_EXAMPLES` is enabled.

## Hardware support

Driver support was checked on 5 September 2026 against the latest available packages and recent
capability reports. These are API compatibility floors, not performance recommendations. `create_device()`
remains authoritative because it also requires suitable queues and a coherent, host-visible device-local
memory type, typically backed by a PCIe BAR aperture. Vulkan 1.4 alone is insufficient, and
`VK_KHR_unified_image_layouts` remains optional.

The latest available Windows drivers are [AMD Adrenalin 26.9.1](https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-WIN-26-9-1.html)
and [NVIDIA 616.64 WHQL](https://us.download.nvidia.com/Windows/616.64/616.64-win11-win10-release-notes.pdf).

The entries below cover each checked driver path; the linked capability reports are representative
architecture checks.

| Architecture | Driver path | Product family | Current result |
| --- | --- | --- | --- |
| AMD RDNA 2 | Windows / Adrenalin 26.9.1 | Radeon RX 6000 | Unsupported |
| AMD RDNA 2 | Linux / Mesa RADV 26.2+ | Steam Deck (Van Gogh) | Supported |
| AMD RDNA 3 | Windows / Adrenalin 26.9.1 | Radeon RX 7000 | Supported |
| AMD RDNA 4 | Windows / Adrenalin 26.9.1 | Radeon RX 9000 | Supported |
| NVIDIA Turing | Windows / NVIDIA 616.64 | GeForce RTX 20 | Supported |
| NVIDIA Ampere | Windows / NVIDIA 616.64 | GeForce RTX 30 | Supported |
| NVIDIA Ada Lovelace | Windows / NVIDIA 616.64 | GeForce RTX 40 | Supported |
| NVIDIA Blackwell | Windows / NVIDIA 616.64 | GeForce RTX 50 | Supported |

AMD limits `VK_EXT_descriptor_heap` to RDNA 3 and RDNA 4 on Windows, so no Radeon RX 6000 GPU qualifies
there. With Mesa [RADV 26.2 or newer](https://docs.mesa3d.org/relnotes/26.2.0.html), Steam Deck has all
required device extensions. Linux/SteamOS window presentation is not implemented yet and is planned
for a near-term update.

Reports for [RDNA 3 on Adrenalin 26.8.1](https://vulkan.gpuinfo.org/displayreport.php?id=51443)
and [RDNA 4 on Adrenalin 26.7.1](https://vulkan.gpuinfo.org/displayreport.php?id=51293) advertise all
required device functionality. AMD exposes the promoted `VK_KHR_surface_maintenance1` and
`VK_KHR_swapchain_maintenance1` names; the backend accepts these as well as the original EXT names.
Both KHR extensions are no-functional-change promotions:
[surface](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_surface_maintenance1.html) and
[swapchain](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_swapchain_maintenance1.html).
AMD's [Vulkan extension history](https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-WIN-VULKAN.html)
and [RDNA 2 descriptor-heap status](https://github.com/GPUOpen-Drivers/AMD-Gfx-Drivers/issues/90#issuecomment-3889407307)
provide current driver details.

Current reports advertise every Win32 backend requirement on
[Turing](https://vulkan.gpuinfo.org/displayreport.php?id=51475),
[Ampere](https://vulkan.gpuinfo.org/displayreport.php?id=51549),
[Ada](https://vulkan.gpuinfo.org/displayreport.php?id=51469), and
[Blackwell](https://vulkan.gpuinfo.org/displayreport.php?id=51573) NVIDIA GPUs.

Intel's [current Windows package](https://www.intel.com/content/www/us/en/download/785597/intel-arc-graphics-windows.html)
is 32.0.101.8992, but support is not verified. The latest public
[Arc Windows report](https://vulkan.gpuinfo.org/displayreport.php?id=51355), for 32.0.101.8991, lacks the
descriptor-heap, device-address-command, and shader-untyped-pointer extensions. Mesa ANV
[26.2 or newer](https://docs.mesa3d.org/relnotes/26.2.0.html)
exposes the required device functionality on verified Arc A-series and B-series hardware, but this
repository currently supports only headless use outside Windows.

## Current scope

Implemented today: graphics, mesh, and compute PSOs; direct and indirect work; GPU-address copies;
application-owned descriptor heaps; common texture types and views; dynamic rendering; global
barriers; timeline submission; deferred destruction; and Win32 presentation.

This is a deliberately single-threaded, single-queue prototype and a low-level thin wrapper, not a
validation layer. API preconditions are generally enforced by debug assertions; use the Vulkan
validation layer during development. Ray tracing, task shaders, sparse memory, device-generated
command graphs beyond the existing indirect operations, pipeline caching, MSAA, non-Win32
presentation, and a Metal backend are outside the current implementation. The public header remains
the source of truth for the exact API surface.

Further reading:

- [Comparison with *No Graphics API*](docs/no-graphics-api-comparison.md)
- [Vulkan extension and command mapping](docs/vulkan-support.md)
- [Slang shader contract and root ABI](docs/slang.md)
- [Proposed Metal 4 port](docs/metal-porting.md)

## License

NoGraphicsAPI and NoGraphicsAPIUtility are distributed under the [MIT License](LICENSE). The cube
example's texture is derived from Vulkan-Tools and is redistributed under Apache-2.0. See
[third-party notices](THIRD_PARTY_NOTICES.md) for complete attribution and license details.
