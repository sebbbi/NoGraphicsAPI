# NoGraphicsAPI

`NoGraphicsAPI` is an experimental Vulkan 1.4 implementation of the ideas in Sebastian Aaltonen's
[*No Graphics API*](https://www.sebastianaaltonen.com/blog/no-graphics-api). It explores how much of
a conventional graphics API disappears when shaders use 64-bit GPU pointers, texture and sampler
descriptors live in application-owned GPU memory, and synchronization describes hazards instead of
resource state.

The Vulkan backend is implemented and exercised by three example applications. Metal support is not
implemented; [the Metal 4 design](docs/metal-porting.md) records the proposed mapping and open issues.

## How it maps to *No Graphics API*

- **GPU pointers replace buffer objects and bindings.** `gpu_malloc()` returns a GPU address and a
  persistently mapped CPU address. Address-based commands consume `GpuRange {gpu, size}` directly,
  and shaders follow typed 64-bit pointers for vertex fetch and arbitrary data structures.
- **Applications own descriptor heaps.** Texture and sampler heaps are ordinary mapped allocations.
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
- **Submission is explicit and asynchronous.** Applications provide timeline points for their own
  data reuse. Submitted command buffers are one-shot, and destruction after submission is retired
  without a device-wide idle wait.

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
`VK_KHR_get_surface_capabilities2`, `VK_EXT_surface_maintenance1`, `VK_KHR_swapchain`, and
`VK_EXT_swapchain_maintenance1`.
Debug builds also enable `VK_EXT_debug_utils` and the Khronos validation layer when available.

Vulkan 1.4 supplies buffer device addresses, timeline semaphores, dynamic rendering,
synchronization2, scalar block layout, and the remaining core features. See
[Vulkan support](docs/vulkan-support.md) for the concise feature and command mapping.

## GPU memory and descriptor heaps

There is no public buffer handle. `GpuAllocation<T>` exposes a CPU pointer, a GPU pointer, and a byte
size. The ordinary memory classes are:

- `cpu_visible`: coherent device-local memory for data written by the CPU;
- `gpu_only`: device-local memory with no CPU mapping (`cpu` is null);
- `readback`: coherent mapped memory with host-cached memory preferred;
- `texture_heap` and `sampler_heap`: mapped application-owned descriptor storage.

`GpuRange` is the non-owning GPU address/size view used by commands. The application owns allocation,
descriptor-slot, and pointed-to data lifetimes; timeline points mark when mutable storage can be
reused. Texture and sampler descriptors are addressed in Slang through the standard heap syntax:

```slang
Texture2D<float4> texture = ResourceDescriptorHeap[texture_index];
SamplerState sampler = SamplerDescriptorHeap[sampler_index];
float4 texel = texture.Sample(sampler, uv);
```

## Shared root ABI

Root structures are declared once and included by C++ and Slang:

```cpp
struct RootArguments
{
    Vertex* vertices;
    float4x4 mvp;
};
```

On the CPU, pointer fields receive GPU virtual addresses while ordinary values are copied directly:

```cpp
RootArguments root{
    .vertices = vertices.gpu,
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
- a little-endian x86-64 target with AVX2 and FMA;
- a Vulkan 1.4 loader and device exposing the required descriptor-heap, device-address-command,
  untyped-pointer, and mesh extensions above, plus their required BDA, synchronization, and
  16-bit/scalar-layout features;
- coherent CPU-visible GPU memory (for example through PCIe Resizable BAR on a discrete GPU or UMA on
  an integrated GPU), plus a separate non-host-visible device-local memory type;
- Slang 2026.14.1+ and SPIRV-Tools 2026.3+ when building the examples.

MSVC and clang-cl are supported on Windows. GNU and Clang can build the headless library on other
platforms; configure with `-DNOGRAPHICSAPI_BUILD_EXAMPLES=OFF`. MinGW, 32-bit x86, and ARM targets are
not supported.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The Windows examples use the normal swapchain path:

- [`triangle`](examples/triangle/triangle.cpp): the minimum vertex/fragment draw and presentation path;
- [`cube`](examples/cube/cube.cpp): typed GPU-pointer vertex fetch plus application-owned texture and
  sampler heaps;
- [`deferred_renderer`](examples/deferred_renderer/deferred_renderer.cpp): a large instanced,
  multi-pass workload with pointer-based scene data, barriers between passes, and timeline-managed
  CPU/GPU overlap.

They can be run from their corresponding directories under `build/examples`.

## Current scope

Implemented today: graphics, mesh, and compute PSOs; direct and indirect work; GPU-address copies;
application-owned descriptor heaps; common texture types and views; dynamic rendering; global
barriers; timeline submission; deferred destruction; and Win32 presentation.

This is a deliberately single-threaded, single-queue prototype. Ray tracing, task shaders, sparse
memory, device-generated command graphs beyond the existing indirect operations, pipeline caching,
MSAA, non-Win32 presentation, and a Metal backend are outside the current implementation. The
public header remains the source of truth for the exact API surface.

Further reading:

- [Comparison with *No Graphics API*](docs/no-graphics-api-comparison.md)
- [Vulkan extension and command mapping](docs/vulkan-support.md)
- [Slang shader contract and root ABI](docs/slang.md)
- [Proposed Metal 4 port](docs/metal-porting.md)
