# Vulkan support

`NoGraphicsAPI` uses Vulkan 1.4 plus a small set of recent extensions to implement the model from
[*No Graphics API*](https://www.sebastianaaltonen.com/blog/no-graphics-api):

- buffers are GPU pointers rather than public buffer objects;
- textures and samplers are 32-bit indices into application-owned descriptor heaps;
- a compact root ABI carries pointers, descriptor indices, and constants to each dispatch or draw;
- barriers describe memory hazards without naming resources or image layouts.

The result has no public descriptor sets, descriptor pools, descriptor-set layouts, pipeline
layouts, render passes, or framebuffers. Vulkan objects still exist where the driver requires them,
but they do not shape the application-facing model.

The current backend targets desktop Vulkan 1.4. MoltenVK is intentionally unsupported, and Win32 is
the only presentation backend. Headless library builds are supported on the other configured
desktop platforms.

## Vulkan feature surface

Support is determined by extension enumeration and feature queries at startup. A Vulkan version
alone is not enough; this table highlights the dependencies that shape the API rather than every
conventional feature checked by device creation.

| Requirement | Contribution to the model |
|---|---|
| Vulkan 1.4 and `maintenance5` | Modern pipeline creation, including inline shader code without temporary shader modules. |
| [`VK_EXT_descriptor_heap`][descriptor-heap] | Application-owned heaps, host descriptor writes, layout-free pipelines, and `vkCmdPushDataEXT`. |
| [`VK_KHR_shader_untyped_pointers`][untyped-pointers] | Supplies the shader-untyped-pointer capability required by the descriptor-heap SPIR-V path. |
| [`VK_KHR_device_address_commands`][address-commands] | Index, indirect, and copy commands consume GPU addresses rather than buffer handles. |
| [`VK_KHR_unified_image_layouts`][unified-layouts] (optional) | Optimizes ordinary `GENERAL` image use; the public model is unchanged without it. |
| [`VK_EXT_mesh_shader`][mesh-shader] | Direct and indirect mesh-workgroup draws. Task shaders are unsupported. |
| Buffer device address | Gives GPU heaps 64-bit shader addresses for their lifetime and enables typed pointer fields in shared structures. |
| Vulkan 1.3 `synchronization2` and `dynamicRendering` | Resource-free barriers and rendering without render-pass or framebuffer objects. |
| Core Vulkan dynamic state | Command-set viewport, scissor, and exposed depth/stencil state. |
| Timeline semaphores | Application-visible completion points plus private command-context retirement. |
| Shader and layout features | Scalar layout, float16, 16-bit push/storage access, draw parameters, independent blending, and formatless storage-image access. |
| Texture features | At least BC or ASTC LDR compression; exact format and usage support remains queryable. |
| Win32 WSI | `VK_KHR_surface`, `VK_KHR_win32_surface`, `VK_KHR_swapchain`, and the maintenance extensions listed below. |

Windowed devices also require `VK_KHR_get_surface_capabilities2`,
`VK_KHR_surface_maintenance1`, and [`VK_KHR_swapchain_maintenance1`][swapchain-maintenance]. The
original EXT variants are accepted as a paired fallback. Swapchain maintenance supplies a completion
fence for each presentation.
Debug builds enable `VK_EXT_debug_utils` and the Khronos validation layer when available.

## GPU heaps and application-side allocation

The backend requires device-local host-visible coherent memory for mapped heaps and descriptor
heaps, plus compatible device-local memory for GPU-only heaps and textures. GPU-only selections
prefer a non-host-visible type, but can use a host-visible UMA type without mapping or exposing it.
There is no host-only or non-coherent fallback path.

`create_gpu_heap()` returns one application-sized block. `cpu_visible`, `gpu_only`, and `readback`
provide addressable data storage; the two descriptor memory types provide mapped descriptor storage.
`GpuCpuRange<T>::size` is always bytes, regardless of `T`, and a GPU-only heap has a null CPU pointer.

The backend has no data suballocator. The optional utility library provides fixed-16-byte
`BumpAllocator` and reusable `HeapAllocator` policies over `GpuHeap::range`; the graphics API does not
depend on either policy. `BumpAllocator::allocate_atomic()` provides relaxed-atomic concurrent worker
reservations that return disjoint mapped ranges. All other allocator operations require exclusive access
and must not overlap `allocate()` or `allocate_atomic()`.

## GPU pointers

GPU pointers are the central departure from conventional graphics APIs. The application derives
typed pointers and command `GpuRange` values from its heap suballocations. Every non-null returned
pointer is 16-byte aligned.
Types or interior ranges requiring stronger alignment must be placed accordingly by the application.
The same pointer type can appear in a C++ root structure and its Slang counterpart:

```cpp
struct Root
{
    Vertex* vertices;
    ObjectData* objects;
    uint32 texture_index;
    uint32 sampler_index;
};
```

The shared ABI requires a 64-bit pointer target. The GPU dereferences pointers through the
`SPV_KHR_physical_storage_buffer` model, while the CPU treats GPU addresses as opaque values.
Pointer targets need valid, stable storage through GPU completion. Suballocation reuse and owning
heap lifetime are the application's timeline responsibility.

Commands that need an address and byte count accept `GpuRange`. The backend passes that numeric
address directly to Vulkan; command recording does not recover a public buffer object or retain its
owning heap. Buffers therefore never need descriptor slots.

| Public operation | Vulkan command path |
|---|---|
| `draw_indexed()` | `vkCmdBindIndexBuffer3KHR` with an address range, then `vkCmdDrawIndexed`. |
| `draw_indirect()` / `draw_indexed_indirect()` | `vkCmdDrawIndirect2KHR` / `vkCmdDrawIndexedIndirect2KHR`. |
| `draw_meshlets_indirect()` | `vkCmdDrawMeshTasksIndirect2EXT`. |
| `dispatch_indirect()` | `vkCmdDispatchIndirect2KHR`. |
| Buffer and texture copies | `vkCmdCopyMemoryKHR`, `vkCmdCopyMemoryToImageKHR`, and `vkCmdCopyImageToMemoryKHR`. |

Address commands use fully bound storage-buffer ranges. Interior ranges are valid, so applications
can suballocate and pass only the relevant byte interval. Interior-pointer alignment, bounds, and
lifetime remain the caller's contract.

Conventional vertex-input bindings are absent. Vertex shaders normally load through a typed pointer
in root data; indexed draws bind only the index address range required by Vulkan.

## Application-owned descriptor heaps

Texture and sampler descriptors live in separate mapped `GpuHeap` values created with
`MemoryType::texture_descriptor_heap` and `MemoryType::sampler_descriptor_heap`. The application
decides their sizes and addresses slots with `DeviceCaps::texture_descriptor_size` and
`DeviceCaps::sampler_descriptor_size`.

`write_texture_descriptor()` and `write_sampler_descriptor()` ask Vulkan to encode one descriptor
at the selected CPU address. `set_texture_descriptor_heap()` and `set_sampler_descriptor_heap()` bind
the corresponding GPU ranges. Shaders index Slang's `ResourceDescriptorHeap` and
`SamplerDescriptorHeap`; heap-using shaders compile for [`SPV_EXT_descriptor_heap`][spirv-heap].

Texture views can select a compatible format only when `TextureDesc::mutable_format` is enabled;
they can select a mip/layer range and color, depth, or stencil aspect. The default
`TextureAspect::automatic` preserves format-derived selection. Sampler descriptors are encoded
directly, so there is no public sampler object. Buffers use GPU pointers instead of resource-heap
entries.

The application owns descriptor-slot reuse. A slot cannot be overwritten while in-flight work may
read it. This is intentionally the same explicit lifetime model used for mapped heap suballocations.

## Placed texture storage

`TextureHeap` is image memory, not a texture descriptor heap. Each value owns one allocation of the
device's single selected GPU-only texture memory type; it has no CPU mapping or GPU pointer.
There are no CPU-visible or readback texture-heap variants.

Device initialization selects one device-local type compatible with the backend's supported
optimal-tiled texture profile. A host-visible UMA type remains opaque and unmapped. This fixed choice
allows eager heap creation, so `SizeAlign` deliberately exposes no memory-type bits.

`get_texture_size_align()` reports a concrete texture's placement requirement.
`DeviceCaps::texture_heap_alignment` is the common allocator granularity selected for the supported
profile; it is not a Vulkan guarantee for arbitrary image create descriptions. Utility
`TextureAllocator` uses that granularity and returns a POD `PlacedTexture`. Pass the aggregate back to
the same allocator after GPU use has completed. The API does not track texture-to-heap or
texture-to-descriptor dependencies.

Texture upload and readback go through buffer-backed `GpuRange` values with
`copy_memory_to_texture()` and `copy_texture_to_memory()`. The application retires texture-heap ranges
with its submission timeline.

## Root ABI

Each draw, mesh draw, and dispatch accepts one `ByteSpan`; the typed convenience path accepts a
trivially copyable root structure. Immediately before the native command, the backend copies those
bytes with `vkCmdPushDataEXT`. The structure may combine:

- typed GPU pointers;
- 32-bit texture and sampler indices;
- ordinary scalar, vector, and matrix values.

The CPU root value only needs to survive the API call because the command buffer receives a copy.
One root is shared by the active graphics stages. Referenced heap ranges and descriptor entries follow the
normal submission and timeline lifetime rules. Root-data size must be a multiple of four and fit
`DeviceCaps::max_push_data_size`; `{}` is the rootless ABI.

Pipelines are created with `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` and a null pipeline layout.
The backend never records push constants, descriptor sets, descriptor buffers, or push descriptors,
so command buffers remain entirely in descriptor-heap mode.

This is the one deliberate adaptation of the blog's proposed ABI. The post passes independent
GPU-resident vertex and pixel roots, while Vulkan push data has a CPU source. `NoGraphicsAPI`
therefore places one shared root in push-data storage. Shader-visible pointer fields and descriptor
indices retain the proposed model, but roots cannot be generated or selected by the GPU.

Shared C++/Slang structures use C layout, with row-major matrix layout where needed. Slang
2026.14.1 or newer is required for the native descriptor-heap path.

## Resource-free barriers and unified image layout

`barrier()` maps stage and access masks to one global `VkMemoryBarrier2` in
`vkCmdPipelineBarrier2`. It has no texture or buffer parameter. The application describes the real
producer/consumer hazard, while resource identity and image-layout bookkeeping disappear from the
public API.

Every ordinary texture use—sampling, storage access, attachment access, and copies—uses
`VK_IMAGE_LAYOUT_GENERAL`. `VK_KHR_unified_image_layouts`, when available, makes that choice
efficient rather than merely legal.

The backend privately handles the two unavoidable cases:

- a newly created image receives its initial `UNDEFINED` to `GENERAL` transition before first use;
- swapchain images transition between `GENERAL` and `PRESENT_SRC_KHR` around presentation.

Resource-free barriers do not imply automatic synchronization. Applications still issue a barrier
for actual read/write hazards and use timeline points for reuse of mutable CPU/GPU data.

## Pipelines, rendering, and mesh work

Graphics, mesh, and compute PSOs consume SPIR-V directly. Vulkan 1.4 maintenance5 lets pipeline
stages take inline `VkShaderModuleCreateInfo`, and descriptor-heap pipelines need no pipeline
layout. Graphics and mesh PSOs retain attachment compatibility, rasterization, and blend state;
viewport, scissor, and the public depth/stencil state are set independently on the command buffer.

Rendering uses `vkCmdBeginRendering` and `vkCmdEndRendering`. `RenderView` represents the persistent
image view needed for attachment use, but there is no render-pass or framebuffer object. Attachment
load/store state and clears are specified at the rendering call. Barriers remain explicit and are
not implied by rendering boundaries. Each `begin_render_pass()` sets a full render-area viewport and
scissor and disables depth/stencil, preventing state from leaking between passes. Applications call
`set_viewport()`, `set_scissor()`, or `set_depth_stencil()` after beginning a pass to override those defaults.

The raster path has empty fixed vertex input because shaders fetch through GPU pointers. Mesh PSOs
use `VK_EXT_mesh_shader` and support direct and indirect meshlet draws. Both paths share the same
root ABI and descriptor heaps.

## Submission, presentation, and lifetime

Command buffers are one-shot recording handles. Every buffer begun since the previous submission
must appear exactly once in the next submit or present span. The span defines execution order, all
handles are consumed, and the caller-provided timeline point is signaled without waiting for
execution.

Public timeline semaphores are the application's reuse mechanism. Poll or wait for the point that
last used mutable upload data, readback storage, a texture placement, indirect argument memory, or a
descriptor slot before modifying or recycling it.

The optional utility `DeleteQueue` runs deferred callbacks once their nondecreasing application
timeline values complete. Applications normally tick it once per frame. At shutdown, call
`wait_idle()` and then drain the queue before destroying the device.

The backend does not recycle application allocator entries or descriptor slots; `wait_idle()` remains
an intentional whole-device drain.

For presentation, `acquire()` returns a swapchain-owned `RenderView` and extent, or an empty frame
while the drawable extent is zero. Binary WSI semaphores remain private, while
`VK_KHR_swapchain_maintenance1` present fences support safe reuse and swapchain replacement without
draining unrelated queue work.

## Validation

The tests cover the public CPU-facing contracts, while the examples exercise representative GPU
paths. Debug builds enable Vulkan validation when it is installed. Runtime extension and feature
queries remain authoritative.

See [No Graphics API comparison](no-graphics-api-comparison.md) for the feature-by-feature assessment
of direct matches, Vulkan adaptations, and intentionally unsupported areas.

[descriptor-heap]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_heap.html
[untyped-pointers]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_untyped_pointers.html
[address-commands]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_device_address_commands.html
[unified-layouts]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_unified_image_layouts.html
[mesh-shader]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_mesh_shader.html
[swapchain-maintenance]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_swapchain_maintenance1.html
[spirv-heap]: https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_descriptor_heap.html
