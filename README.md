# NoGraphicsAPI

`NoGraphicsAPI` is a small Vulkan-only rendering layer inspired by
[No Graphics API](https://www.sebastianaaltonen.com/blog/no-graphics-api). It follows the proposal's
64-bit GPU-pointer and application-owned descriptor-heap model, while using Vulkan's CPU push-data
path for small CPU root structures. The exact similarities, deliberate differences, and current
scope limits are documented in the
[design comparison](docs/no-graphics-api-comparison.md). Its C++ API lives in the short `gpu`
namespace.

Metal support is not implemented. A proposed
[Metal 4 porting design](docs/metal-porting.md) records the API changes and backend contracts
currently under investigation.

The implementation deliberately creates no `VkDescriptorSetLayout`, `VkDescriptorPool`,
`VkDescriptorSet`, or `VkPipelineLayout`. Internally it uses:

- `VK_EXT_descriptor_heap` for the texture/sampler descriptor heaps and `vkCmdPushDataEXT`;
- `VK_EXT_mesh_shader` for mesh PSOs and direct mesh-workgroup draws;
- `VK_KHR_device_address_commands` for index binding, indirect draws/dispatch, and buffer/image copies;
- `VK_KHR_unified_image_layouts` to keep every normal image access in `VK_IMAGE_LAYOUT_GENERAL`;
- `VK_KHR_swapchain` plus `VK_EXT_swapchain_maintenance1` for the Win32 presentation path;
- Slang pointers for custom vertex fetch and arbitrary BDA-backed structures;
- Vulkan 1.4 dynamic rendering and synchronization2 for the remaining fixed-function work.

There is no public buffer object or owning memory class. `gpu_malloc<T>()` returns the plain
`GpuAllocation<T>` aggregate with CPU/GPU/size plus opaque O(1) release bookkeeping. Its default `MemoryType::cpu_visible` is strictly
device-local, host-visible, and host-coherent, so ordinary GPU allocations expose both a
persistently mapped CPU pointer and their GPU pointer. `MemoryType::readback` uses the same required
memory properties and additionally prefers host-cached memory. `MemoryType::gpu_only` is
non-host-visible, so its `cpu` pointer is null. `gpu_free(allocation)` takes the unchanged allocation
by reference exactly once; the allocation carries the device bookkeeping needed to free itself.
Address-based command APIs take the separate `GpuRange {gpu, size}` aggregate by
value, and `gpu_range()` converts
an allocation to its full range. This lets a caller bind all or part of an allocation
without exposing a Vulkan buffer handle. `gpu_malloc()` with
`MemoryType::texture_heap` or `MemoryType::sampler_heap` creates a coherent, directly writable
descriptor heap. The texture mode is backed by Vulkan's natively named resource heap. Both the
byte-count and typed `gpu_malloc<T>()` overloads support these modes, so descriptor storage may be
represented by a caller-defined structure.
The application chooses 32-bit texture/sampler slot indices, writes descriptors through the
returned CPU pointer, and explicitly binds the corresponding GPU range on command buffers that use
them.

The public API is a set of free functions in namespace `gpu`. Devices, swapchains, textures,
render views, PSOs, command buffers, and timeline semaphores are opaque raw pointer
handles rather than C++ ownership wrappers. Device and swapchain creation return handle/error aggregates; other creation
functions return a handle directly. Matching `destroy_*()` functions release them, and applications
destroy owned handles explicitly in reverse creation order. A submitted command-buffer batch is
consumed by `submit()` or `submit_and_present()`. Every command buffer returned by
`begin_commands()` must appear exactly once in the next submission or presentation span. `submit()`
derives its device from the batch, while `submit_and_present()` takes the device first for its
device-owned swapchain. Both take one explicit caller-owned `TimelinePoint` to signal.
`write_texture_descriptor()` and `write_sampler_descriptor()` take their device first;
texture descriptor writes additionally validate that the texture belongs to that device.
`get_device_caps()` returns a reference into the device and does not copy the capability record;
that reference remains valid until `destroy_device()`.

CPU arrays in public descriptors use `Span<T> {data, size}`, a non-owning pointer/count view with no
iterator or accessor layer. It constructs from a pointer and count, a C array, or a contiguous
container exposing compatible `data()` and `size()` members, including `std::vector`, without
including its header. `Span<const T>` also accepts an initializer list for concise call arguments.
A temporary container or initializer list remains alive through its containing full expression, so
it may back a span consumed directly by that function call. Such a span must never be retained.

Every public descriptor field has a useful default. Call sites use C++20 designated initializers,
name each explicitly supplied field, and omit fields whose defaults are already correct. `Span` and
`ByteSpan` are constructor-bearing exceptions because their concise input forms are part of the API.

The entire project is built without C++ exceptions or RTTI. Programming errors use assertions.
Initialization and resource-creation failures are checked immediately; normal recording and
submission code assumes success. `create_device()` returns a small handle/error aggregate because
device support and window-system compatibility are recoverable startup results. Supplying a window
and swapchain format in `DeviceDesc` creates the device-owned swapchain; omitting them creates a
headless device. `desired_swapchain_image_count` defaults to two and accepts one through eight. The
request is clamped to the surface's FIFO-specific limits, and Vulkan may create more images than
requested; an actual count above the backend capacity of eight is unsupported. The same value
selects how many of the eight statically stored presentation contexts are active, bounding
outstanding presentations independently of the actual image count. The native window must remain
valid until `destroy_device()` returns. A windowed device and all calls using it stay on the native
window's message-pump thread; Win32 WSI entry points may otherwise synchronously wait for that
thread to process a window message.
Memory allocation failure is fatal and deliberately has no recovery path;
`gpu_malloc()` therefore does not return a nullable out-of-memory result. Applications are
responsible for keeping their memory use within the available budget.

## Requirements

- A little-endian x86-64 target with a 64-bit pointer ABI and IEEE-754 binary32, an AVX2-and-FMA
  runtime CPU, a Vulkan 1.4 loader/device, and the loader's development library. The build rejects
  32-bit x86, ARM64, and ARM64EC targets.
- MSVC or clang-cl on Windows; GNU, Clang, or AppleClang for headless non-Windows builds. MinGW is
  not supported and is rejected during configuration and compilation.
- `VK_EXT_descriptor_heap`, `VK_EXT_mesh_shader`, `VK_KHR_device_address_commands`,
  `VK_KHR_shader_untyped_pointers`, and `VK_KHR_unified_image_layouts`.
- On Windows, `VK_KHR_surface`, `VK_KHR_win32_surface`,
  `VK_KHR_get_surface_capabilities2`, `VK_EXT_surface_maintenance1`, `VK_KHR_swapchain`, and
  `VK_EXT_swapchain_maintenance1`, including its `swapchainMaintenance1` feature.
- The BDA, descriptor-heap, device-address-command, untyped-pointer, unified-image-layout,
  mesh-shader, depth-bias-clamp, sampler-anisotropy, dynamic-rendering, synchronization2,
  scalar-layout, and 16-bit storage features checked at startup.
- A device-local memory type on an at least 256 MiB heap that is both host-visible and host-coherent.
  This is the default CPU-visible allocation class and is also used for readback and
  application-owned descriptor heaps. Host-only and non-coherent fallback memory are unsupported.
- A separate device-local, non-host-visible memory type on an at least 256 MiB heap for GPU-only
  allocations and textures.
- CMake 3.24+, a C++20 compiler, Slang 2026.14.1 or newer, SPIRV-Tools 2026.3 or
  newer, and a system Vulkan SDK whose headers report version 1.4.357 or newer. The cube requires
  Slang's native `SPV_EXT_descriptor_heap` capability. Shaders sharing C++ POD structures use
  `-fvk-use-c-layout`; matrix-bearing structures additionally use `-matrix-layout-row-major`.

The build uses the Vulkan headers and loader development library supplied by that system SDK and
rejects versions below 1.4.357 during configuration.
Visual Studio debugger settings derive `VK_LAYER_PATH` from the same CMake-selected SDK so the
validation layer uses the header version the examples were built against.
Non-Windows builds must configure with `-DNOGRAPHICSAPI_BUILD_EXAMPLES=OFF`; the examples and Win32 WSI
are Windows-only.

## Build and run

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/examples/triangle/example_triangle
./build/examples/cube/example_cube
./build/examples/deferred_renderer/example_deferred_renderer
```

On Windows, each example opens a Win32 window and runs a normal acquire/render/present loop against
a Vulkan swapchain. Presentation stays on the GPU; there is no CPU readback, GDI blit, image dump,
per-frame idle wait, or artificial sleep. FIFO presentation supplies pacing.

The 512x512 triangle follows the first triangle in the
[Khronos Vulkan Tutorial](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/02_Graphics_pipeline_basics/01_Shader_modules.html):
a traditional vertex shader produces three constant positions/colors from `SV_VertexID`, and the
CPU records one three-vertex draw into the swapchain. It has no vertex allocation, mesh
shader, sampled texture, descriptor heap, compute stage, or depth/stencil attachment.

The 500x500 cube follows the official
[Khronos/LunarG `vkcube` sample](https://github.com/KhronosGroup/Vulkan-Tools/tree/vulkan-sdk-1.4.357.0/cube).
It spins four degrees per frame and reproduces the sample's cube geometry, camera, nearest-filtered
sRGB texture, face lighting, clear color, and depth test. Vertex data is fetched through BDA, while
the application allocates, writes, and binds one texture heap and one sampler heap. Its raw RGBA8
texture preserves the exact RGB content of the upstream
[`lunarg.ppm.h` asset](https://github.com/KhronosGroup/Vulkan-Tools/blob/vulkan-sdk-1.4.357.0/cube/lunarg.ppm.h),
distributed with the sample under the upstream
[Apache-2.0 license](https://github.com/KhronosGroup/Vulkan-Tools/blob/vulkan-sdk-1.4.357.0/LICENSE.txt).
The fixed 256x256 RGBA8 image is read directly into mapped upload memory without an image decoder.

`deferred_renderer` draws 1,048,576 orbiting cubes in one indexed instanced draw. A typed
GPU vertex pointer addresses 24 face-split cube vertices, while a GPU `uint16_t` index buffer
supplies the 36 draw indices. Two mapped 48 MiB
`ObjectData` slots hold the per-object matrices so the CPU can update one slot while the GPU uses
the other. The first pass fills `rgba8_unorm` albedo and `rgba16_float` normal/roughness targets
plus sampled `d32_float` depth. The fullscreen deferred pass reconstructs world position by
scaling an interpolated camera ray with linearized hardware depth, then evaluates point,
directional, and specular lighting.

The examples require Windows and fail at compile time on other platforms. Configure with
`-DNOGRAPHICSAPI_BUILD_EXAMPLES=OFF` for a headless non-Windows library build.

## Shared root ABI

The root structure header is included by both C++ and Slang:

```cpp
struct RootArguments
{
    Vertex* vertices;
};
```

The root is an ordinary CPU POD. Its pointer fields carry GPU addresses and must not be dereferenced
by the CPU. `shader_types.h` supplies only the C-layout-compatible POD equivalents for Slang vector
and matrix types. CPU code opts into operators and math functions with `<NoGraphicsAPI/math.hpp>`;
that header is never included by shared shader structures. Shared structures use
`-fvk-use-c-layout` and `-matrix-layout-row-major` when they contain a matrix. Every draw and
dispatch has a single root overload taking `ByteSpan` by value. A valid root structure converts implicitly;
the span stores its address and `sizeof(T)`. Type-erased callers explicitly construct
`ByteSpan{byte_ptr, byte_size}`, while a rootless call passes `{}`. The command copies the complete
root through `vkCmdPushDataEXT` immediately before recording the native command.
Shaders read root fields directly from push data
instead of first reading and following a GPU address. Vulkan cannot load push-data state directly
from GPU memory. The CPU root need not outlive the draw or dispatch call. Its type must be trivially
copyable, its size must be a multiple of four bytes, and it must fit
`DeviceCaps::max_push_data_size`.

```cpp
RootArguments root{.vertices = vertices.gpu};
gpu::draw(commands, root, vertex_count);
```

The cube uses a root containing a vertex pointer and a full shared `float4x4` model-view-projection
transform. C++ builds the matrix with `math.hpp`, and the vertex shader multiplies each
position by it directly. Its single texture and sampler occupy slot zero in their respective heaps:

```slang
Texture2D<float4> texture = ResourceDescriptorHeap[0];
SamplerState sampler = SamplerDescriptorHeap[0];
```

## Scope

Implemented now: capability-driven device creation, plain GPU allocation and range aggregates,
coherent device-local mapped CPU-visible and readback memory, GPU-only memory, application-owned
texture/sampler heap allocation modes, sampled and storage texture descriptor writers, six texture types,
alternate-format mip/layer descriptors, pitched subresource copies, sampler descriptor writers, explicit
heap binding, vertex/fragment, mesh/fragment, and compute PSOs with null layouts, typed
CPU-root draw/dispatch calls, multiple color attachments, independent color/depth/stencil
load-store operations, persistent mip/slice render views, minimal immutable triangle raster state,
dynamic rendering, direct/indexed/indirect
draws, direct/indirect meshlet draws and compute dispatches, device-address copies, global memory
barriers, unified image layouts, caller-owned timeline-backed asynchronous submission, and Win32
swapchain presentation.

`GraphicsPSODesc` and `MeshPSODesc` use a caller-owned
`Span<const ColorTargetDesc>`. Each entry supplies one color format, per-target ordinary
non-constant color/alpha blend state, and an RGBA write mask; depth and stencil formats remain
separate optional fields. Both PSO descriptors carry cull winding, depth bias, and
depth/stencil state.
A mesh PSO replaces the vertex stage and input assembly with mesh SPIR-V that must output
triangles while retaining the remaining state. `draw_meshlets()` supplies mesh workgroup counts directly,
while `draw_meshlets_indirect()` reads one or more workgroup-count records from a `GpuRange`.
`Stage::mesh` exposes mesh-shader memory dependencies without adding a task-shader stage.

`RenderingDesc` supplies a span of `ColorAttachment` values plus optional `DepthAttachment`
and `StencilAttachment` values. Every attachment references a persistent `RenderView` and
selects `LoadOp::load`, `clear`, or `discard` and `StoreOp::store` or `discard`; clear data is stored
next to the render target it affects. `create_render_view()` selects exactly one mip and
physical array slice, with cube faces addressed as six consecutive physical slices. Applications
create these views once and may reuse them across command buffers. Each texture maintains a live-view
counter: view creation increments it, view destruction decrements it, and `destroy_texture()`
terminates in every build when the count is nonzero. This enforces object destruction order only;
once every referencing command buffer has been submitted, the caller may destroy the view and then
the texture without waiting. Their Vulkan objects remain in the deferred-deletion queue until the
private retirement timeline completes. Simultaneous depth and stencil use the same render view. `acquire()` instead
returns a swapchain-owned render view that remains valid until swapchain recreation or
destruction and must not be destroyed by the application.

`SamplerDesc` independently selects minification, magnification, and mip filtering; three address
modes; optional comparison; and fixed 4x anisotropy. LOD bias is fixed at zero and
the LOD range is unclamped.

Depth testing/writes/comparison, shared stencil read/write masks, per-face stencil
compare/operations/reference, depth bias, culling (none or clockwise/counter-clockwise winding),
per-target write masks, and ordinary non-constant blending are immutable PSO
state. Nonzero depth-bias values enable bias. Graphics input assembly is always triangle-list with
primitive restart disabled; rasterization is always fill mode with line width 1. Mesh shaders must
output triangles. Only viewport and scissor remain dynamic. `independentBlend` is required at
device creation and is therefore an invariant rather than a `DeviceCaps` field.
`Format::s8_uint` and
`Format::d32_float_s8_uint` provide stencil-only and combined depth/stencil attachment formats,
and `TextureUsage::depth_stencil_attachment` covers both aspects.

Ordinary value memory is suballocated from 256 MiB internal pages, with one fully bound universal
`VkBuffer` per page and separate pools for CPU-visible, GPU-only, and readback memory. Suballocation uses
a pinned revision of [OffsetAllocator](https://github.com/sebbbi/OffsetAllocator). Allocation tries
the pool's active page first, scans older pages only when that page cannot satisfy the request, and
creates a new page only when none fit. A freed page becomes active. `GpuAllocation` carries opaque
owner/token bookkeeping, so `gpu_free()` releases ordinary allocations directly in O(1) without a
page or allocation-record search.

Every texture or sampler descriptor heap instead owns one descriptor-capable `VkBuffer` and
one dedicated allocation from the same coherent mapped device-local memory used by CPU-visible
allocations. The buffer covers any hidden prefix alignment slack, the user bytes, suffix padding,
and Vulkan-required implementation reservation; no 256 MiB descriptor page is created. The public
`GpuAllocation` CPU/GPU pointers and size expose only the requested user range. Requested allocation alignment applies to
the GPU address; the CPU pointer is byte-addressable mapped storage. Slot zero is therefore exactly
the returned CPU/GPU byte in each address domain; slot `i` is
`i * DeviceCaps::texture_descriptor_stride` bytes into a texture heap
or `i * DeviceCaps::sampler_descriptor_stride` bytes into a sampler heap. The corresponding
`*_descriptor_size` fields report the raw descriptor byte counts used by the driver writer.

Textures are normally suballocated from separate 256 MiB GPU-only texture-memory heaps; only textures
that Vulkan requires to be dedicated or that cannot fit a page use the dedicated-allocation path.
`create_texture()` does not submit work: the next `begin_commands()` batches the one required
`VK_IMAGE_LAYOUT_UNDEFINED` to `VK_IMAGE_LAYOUT_GENERAL` metadata initialization for every newly
created texture. `TextureType` exposes 1D, 2D, 3D, cube, 2D-array, and cube-array textures.
`TextureDesc::layer_count` and `TextureDescriptorDesc` layer ranges count logical cubes for cube types;
each logical cube occupies six physical array slices. The backend declares every compatible public
descriptor format while creating a texture, so alternate-format support adds no creation-time API field.
`TextureDescriptorDesc::format == Format::undefined` inherits the texture format, while zero mip or
layer count selects the remaining range. Sampled/storage descriptors are generated directly from
texture creation metadata. `RenderViewDesc` instead selects one physical mip/slice attachment
subresource whose persistent handle is created and destroyed explicitly.

`Format` covers the complete texture-format set required by the applications, plus the
stencil-only format. `get_texture_format_info()`
reports each format's texel-block width, height, byte size, and depth/stencil aspects.
`supports_texture_format()` tests a concrete format/usage combination against the selected
device's enabled compression feature and optimal-tiling format features. Device creation requires
at least one of BC or ASTC LDR and exposes both independent feature bits through `DeviceCaps`, so
applications can select ASTC or BC assets once at initialization and assert the guaranteed
fallback exists. EAC remains optional and is reported through the generic query.

`TextureCopyDesc` selects one mip, an offset and extent, and a range of physical slices. Cube faces
therefore occupy six consecutive slice indices even though creation and descriptor ranges count
logical cubes. Zero width, height, depth, or slice count selects the remaining subresource range;
zero row and slice pitches mean tightly packed memory. Nonzero `row_pitch_bytes` and
`slice_pitch_bytes` describe padded data inside the supplied `GpuRange`. To start at an interior
byte address, pass an interior `GpuRange` with its `gpu` pointer and `size` adjusted accordingly.
Mip-chain construction is intentionally not a core API operation; applications can upload authored
levels with these direct copy commands or provide their own higher-level utility.
For block-compressed formats, offsets are aligned to the 4x4 block extent and extents are block
aligned unless they reach a mip edge. Tight and explicit pitches are measured in bytes; explicit
pitches must contain whole compressed blocks and are converted to Vulkan's texel-count fields.

`NoGraphicsAPI` deliberately supports only single-sample textures and rendering.
`VK_EXT_descriptor_heap` cannot encode sampled multisample descriptors, which would make MSAA a
special-case attachment and resolve path outside the otherwise uniform descriptor-heap model.
MSAA is not commonly used in modern rendering pipelines, so the API omits the associated
multisample texture, rasterization, and resolve surface.

The caller creates a `TimelineSemaphore` and owns its monotonically increasing signal-value policy.
Each `begin_commands()` selects a context containing one transient command pool and one primary
command buffer. A completed context is reset and reused. If the next context is still recording or
executing, the ring grows instead of waiting; that capacity is retained for later batches. The
device starts with two contexts and therefore converges naturally on the application's peak number
of simultaneously live command buffers.

`submit({commands...}, point)` or `submit_and_present(device, {commands...}, point)` must receive
every command buffer begun since the preceding batch exactly once. The span order defines execution
order.
The call ends the buffers, queues the whole batch in one `vkQueueSubmit2`, signals exactly
`point.value`, consumes every submitted handle, and returns without waiting for execution. One
private device timeline also advances once per batch; the resulting value retires every command
context in that batch and permits its pool to be reset when execution completes. Submission metadata
and command contexts retain their prior high-water capacity, so ordinary recording and submission
perform no host or Vulkan allocation after the workload reaches a stable peak.
`timeline_completed_value()` polls GPU progress, while `wait_timeline(point)` blocks for one explicit
point. NoGraphicsAPI neither retains nor waits on the caller's timeline for internal command-context
reuse.

WSI synchronization is separate. The device statically stores eight present contexts, while
`DeviceDesc::desired_swapchain_image_count` selects the active count and defaults to two. Each active
context owns private acquire/rendered binary semaphores and a present fence.
`get_drawable_extent()` reports the current resolution without acquiring an image, so
resolution-dependent offscreen work can be submitted before `acquire()`. The returned frame extent
is authoritative if the surface changes after that earlier snapshot. Acquisition polls completed
present fences and waits before reusing the next context in the active ring. A stale swapchain is
replaced immediately with the current
handle as `oldSwapchain`, and acquire-side `VK_ERROR_OUT_OF_DATE_KHR` recreates and retries within
the same call. Old image views and handles remain alive until all associated present fences signal.
`vkAcquireNextImageKHR` uses an infinite timeout and waits for an image. `acquire()` requires a
non-zero drawable extent; callers pause presentation while the window is minimized. Given that
precondition, it always returns a populated frame: ordinary backpressure waits and out-of-date
recovery remains internal. Present fences make context reuse, swapchain recreation, and deferred
destruction precise without `vkDeviceWaitIdle`.
`submit_and_present()` inserts no explicit completion wait, although Vulkan itself permits
`vkQueuePresentKHR` to block for a finite implementation-dependent interval.

`destroy_timeline_semaphore()` and the other resource destruction functions are nonblocking after
submission. They remove the public object immediately and enqueue its Vulkan handles in a fixed
capacity, allocation-free FIFO keyed by the private command-retirement timeline. Queue order
preserves dependent destruction order; each enqueue polls progress and collection examines only the
oldest prefix. If that poll cannot free an entry at capacity, the library terminates rather than
waiting. The examples use one timeline and increment a `uint64_t` for every `submit()` and
`submit_and_present()` call. They retain points only where application-owned data is reused.
`deferred_renderer` alternates two application data ranges, polls for any completed range, and skips
that loop iteration if both remain in flight.

This remains a focused, deliberately single-threaded prototype. Device and command-buffer operations
are not thread-safe; the implementation contains no mutex or atomic synchronization. Ray tracing,
task shaders, sparse heaps, capture/replay, device-generated commands, non-Win32 WSI,
multi-queue scheduling, and pipeline caching are out of scope. Normal texture accesses remain in
`VK_IMAGE_LAYOUT_GENERAL`; the backend alone performs the required swapchain transitions to and
from `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`.

The unified-layout extension deliberately does not remove the one-time initialization out of
`VK_IMAGE_LAYOUT_UNDEFINED`; NoGraphicsAPI records it lazily in the next recording's batched
texture-initialization dependency.

Address-based command functions consume `GpuRange` aggregates directly. Command recording performs no
hidden allocation lookup or command-buffer lifetime retention. GPU pointers copied from root data and
descriptor-heap contents are opaque to the backend. The caller must keep referenced public objects
alive until every command buffer using them has been submitted, and must not overwrite mutable
allocations or descriptor slots until the corresponding application timeline point completes.
After submission, destroy/free calls invalidate public values immediately while the backend defers
their physical Vulkan lifetime. Callers synchronize genuine read/write hazards with
`barrier()`; no image-layout state is
exposed by the public API.

See the [comparison with *No Graphics API*](docs/no-graphics-api-comparison.md),
[Vulkan support and cross-reference](docs/vulkan-support.md),
[Metal 4 porting design](docs/metal-porting.md), and
[Slang integration and ABI](docs/slang.md) for the design rationale, exact contracts, and current
implementation notes.
