# Vulkan support and design mapping

Status date: 2026-09-02.

`NoGraphicsAPI` is a Vulkan-only experiment inspired by
[No Graphics API](https://www.sebastianaaltonen.com/blog/no-graphics-api): typed buffer
device-address pointers, 32-bit indices into application-owned descriptor heaps, and resource-free
barriers. Its CPU root-data block is a deliberate Vulkan push-data adaptation rather than
the GPU-resident root pointer proposed in the post. See the
[design comparison](no-graphics-api-comparison.md) for the complete rationale.
There are no descriptor sets, descriptor pools, descriptor-set layouts, or pipeline layouts in the
public API or Vulkan backend.

MoltenVK is intentionally unsupported. The current capability check targets desktop Vulkan 1.4
drivers exposing all requirements below; a MoltenVK device that does not expose them is rejected.

`VK_EXT_descriptor_heap` (device extension 136, revision 1),
`VK_EXT_mesh_shader` (device extension 329, revision 1),
`VK_KHR_device_address_commands` (device extension 319, revision 1), and
`VK_KHR_unified_image_layouts` (device extension 528, revision 1) are ratified. `vkCmdPushDataEXT`
belongs to `VK_EXT_descriptor_heap`; it is not a separate extension.
The Win32 presentation path additionally uses `VK_KHR_swapchain` and
`VK_EXT_swapchain_maintenance1`; the latter depends on the instance extensions
`VK_EXT_surface_maintenance1` and `VK_KHR_get_surface_capabilities2`.

## Runtime requirements

The extension specifications permit some Vulkan 1.2/1.3 configurations, but this repository
deliberately requires a Vulkan 1.4 loader and physical device. Configuration requires a system
Vulkan SDK whose headers report version 1.4.357 or newer. Shader-required features in the table
remain mandatory even when Vulkan 1.4 leaves them optional; startup rejects devices missing one.

| Requirement | Enabled feature/API | Why `NoGraphicsAPI` requires it |
|---|---|---|
| Vulkan 1.4 | Required `maintenance5` | Provides the 64-bit pipeline-create flags path and permits pipeline stages to consume inline `VkShaderModuleCreateInfo` without temporary shader-module objects. |
| Vulkan 1.4 core sampler support | `samplerAnisotropy`; `maxSamplerAnisotropy` is at least 16 | Enables the fixed 4x anisotropic sampler profile without an older-device fallback. |
| [`VK_EXT_descriptor_heap`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_heap.html) | `VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap` | The application-owned NoGraphicsAPI texture heap is backed by Vulkan's resource heap; samplers use Vulkan's separate sampler heap. Host descriptor generation, explicit heap binding, `vkCmdPushDataEXT`, and pipelines with no layout are enabled. Capture/replay is not enabled. |
| [`VK_EXT_mesh_shader`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_mesh_shader.html) | `VkPhysicalDeviceMeshShaderFeaturesEXT::meshShader` | Mesh/fragment pipelines and direct or indirect mesh-workgroup draws. The task-shader feature is not required. |
| [`VK_KHR_shader_untyped_pointers`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_untyped_pointers.html) | `shaderUntypedPointers` | Native `ResourceDescriptorHeap`/`SamplerDescriptorHeap` access. Slang BDA pointers use the separate physical-storage-buffer path. |
| [`VK_KHR_device_address_commands`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_device_address_commands.html) | `deviceAddressCommands` | Address-range index binding, indirect vertex, mesh, and compute work, and buffer/image copies. |
| [`VK_KHR_unified_image_layouts`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_unified_image_layouts.html) | `unifiedImageLayouts` | Guarantees that `VK_IMAGE_LAYOUT_GENERAL` is efficient for every normal image use, eliminating public layout-state tracking. Video layouts are out of scope, so `unifiedImageLayoutsVideo` is not enabled. |
| Win32 surface extensions | `VK_KHR_surface`, `VK_KHR_win32_surface`, `VK_KHR_get_surface_capabilities2`, `VK_EXT_surface_maintenance1` | Creates a surface from the caller's Win32 window. `VkSurfacePresentModeEXT` and `vkGetPhysicalDeviceSurfaceCapabilities2KHR` query FIFO-specific image-count limits rather than conservative limits shared by all present modes. |
| [`VK_KHR_swapchain`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_swapchain.html) | Swapchain creation, binary acquire/render semaphores, FIFO presentation | Maps acquired swapchain textures to swapchain-owned `RenderView*` values and keeps window-system synchronization out of the public API. |
| [`VK_EXT_swapchain_maintenance1`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_swapchain_maintenance1.html) | `swapchainMaintenance1`, `VkSwapchainPresentFenceInfoEXT` | Gives every queued present a completion fence, making semaphore reuse, recreation, and destruction safe without a queue/device idle wait. |
| [Buffer device address](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_buffer_device_address.html) | `bufferDeviceAddress` | `GpuAllocation`/`GpuRange` addresses, internal heap-page addresses, and typed pointers in shared root structures. |
| Vulkan 1.3 | `synchronization2`, `dynamicRendering` | Barriers and rendering without render-pass/framebuffer objects. Slang currently lowers `discard` to core `OpKill`, so shader demote is neither required nor enabled. Extended dynamic state is neither required nor enabled. |
| Vulkan 1.2 | `shaderFloat16`, `scalarBlockLayout` | Half-precision shared values and C-compatible shared layouts. |
| Vulkan 1.1/core | `storagePushConstant16`, `storageBuffer16BitAccess`, `shaderDrawParameters`, `shaderInt16`, `depthBiasClamp`, `independentBlend`, graphics-stage stores/atomics, and storage-image reads/writes without a shader format | 16-bit members in copied root data and pointed-to storage; Slang's `SV_VertexID` lowering; baked clamped depth bias; per-target blending; and general sampled/storage texture use from graphics and compute shaders. True 16-bit stage interfaces are deliberately not required because current NVIDIA Vulkan drivers, including the RTX 4090 driver used for validation, do not expose optional `storageInputOutput16`. |
| Texture-compression features | At least one of `textureCompressionBC` or `textureCompressionASTC_LDR`; optional `textureCompressionETC2` | Every reported family is enabled. Device creation requires BC or ASTC LDR, while EAC remains optional. `DeviceCaps` exposes BC/ASTC directly and `supports_texture_format()` checks the exact format/usage pair. |
| Optional core texture feature | `imageCubeArray` | Enabled when reported. Cube-array creation rejects the call when the feature is unavailable; other texture modes and device creation remain available. |
| Host/queue | Little-endian x86-64 target with a 64-bit pointer ABI, IEEE-754 binary32, AVX2-and-FMA runtime CPU, graphics-and-compute-capable queue; Win32 presentation support when WSI is used | Shared pointer representation, SIMD math, and the current single-queue command/presentation implementation. MinGW, 32-bit x86, ARM64, and ARM64EC are unsupported. |
| Mapped device memory | A memory type and at least 256 MiB device-local heap with `DEVICE_LOCAL`, `HOST_VISIBLE`, and `HOST_COHERENT` | This strict GPU-memory class is exposed as `MemoryType::cpu_visible`; the same required properties also back `MemoryType::readback` and descriptor heaps. These allocations are persistently mapped without flush/invalidate operations; host-only and non-coherent fallbacks are unsupported. `HOST_CACHED` is preferred for readback but is not required. |
| GPU-only device memory | A memory type on an at least 256 MiB heap with `DEVICE_LOCAL` and without `HOST_VISIBLE` | `MemoryType::gpu_only` allocations and textures cannot fall back to CPU-visible memory. |

## Error model

`NoGraphicsAPI` has no C++ exception or RTTI path; all repository targets are compiled with both
features disabled. Device and optional window-system compatibility are one recoverable startup
result; `create_device()` returns a handle plus `Error` in one aggregate:

```cpp
struct DeviceInit
{
    Device* device = nullptr;
    Error error = Error::none;
};
```

Textures, PSOs, and command buffers are returned directly as opaque pointer handles.
Initialization and resource-creation results are checked immediately. Normal recording,
submission, and wait operations assume Vulkan success and use debug assertions; incorrect handles,
ranges, alignment, state transitions, and other programming errors are assertions as well. CPU and
GPU allocation failures are fatal and have no recovery path because the library cannot recover from
exhausted memory. `gpu_malloc()` does not return a nullable out-of-memory result.

The API consists of free functions in namespace `gpu`. `Device*`, `Texture*`, `RenderView*`,
`PSO*`, `CommandBuffer*`, and `TimelineSemaphore*` are raw opaque handles, not RAII objects.
Creation returns a handle and matching `destroy_*()` functions release owned objects explicitly.
When `DeviceDesc::window` is non-null, the device also owns its surface, swapchain generations, and
presentation contexts; there is no separate public swapchain handle.
`DeviceDesc::desired_swapchain_image_count` defaults to two, accepts one through eight, and controls
both the best-effort Vulkan image-count request and the number of active presentation contexts.
The request is clamped to the surface's FIFO-specific limits, and Vulkan may return more images
than requested. An actual count above the backend's static capacity of eight is unsupported.
`submit()` and `submit_and_present()` consume every command-buffer handle in their submitted span.
`begin_commands()` has no matching destruction operation: every begun command buffer must appear
exactly once in the next submission or presentation span. Applications destroy their remaining
owned handles in reverse creation order. `get_device_caps()` returns a
`const DeviceCaps&` tied to the device lifetime rather than copying the capability structure.

The repository queries every required feature at runtime rather than inferring support from the
Vulkan version alone. The canonical dependency expressions are in Khronos's
[Vulkan registry XML](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/main/xml/vk.xml).

## Extension and command matrix

| Public operation | Vulkan implementation | Important contract |
|---|---|---|
| `create_device()` | Enumerates the five base device extensions and, when `DeviceDesc::window` is non-null, creates the Win32 surface before physical-device selection and enables the surface/swapchain dependencies; queries the device feature chain, memory types, Vulkan 1.2 timeline limits, `VkPhysicalDeviceDescriptorHeapPropertiesEXT`, and `VkPhysicalDeviceMeshShaderPropertiesEXT`; creates a private retirement timeline plus two initial command contexts; and creates the initial swapchain and requested number of presentation contexts for a windowed device | `DeviceDesc` carries the optional native window, requested swapchain format, and desired swapchain image count. The count defaults to two, must be in [1, 8], and bounds outstanding presentations. Vulkan's image-count request is clamped to the FIFO-specific surface minimum and finite maximum and remains best effort because the implementation may return more images; an actual count above eight is unsupported. The window must remain valid through `destroy_device()`. The windowed device and every call using it remain on the HWND-owning message-pump thread because Win32 WSI can synchronously send window messages. Each command context owns one transient pool and one primary command buffer. A device missing any required extension, feature (including `independentBlend`), combined graphics/compute queue, Vulkan 1.4, coherent device-local host-visible memory, or non-host-visible device-local memory is skipped. A windowed device additionally requires that queue to support its surface and requires swapchain maintenance. `independentBlend` is always enabled and is an invariant, not a reported capability. No public timeline object is created. |
| `get_device_caps()` / `supports_texture_format()` | Returns cached physical-device compression bits and compares the requested usage against cached `VkFormatProperties3::optimalTilingFeatures` | BC or ASTC is guaranteed after successful device creation; both can be reported. The format query is allocation-free and does not create or probe an image. Exact type, dimensions, and image flags remain `create_texture()` checks. |
| `create_timeline_semaphore()` / `destroy_timeline_semaphore()` / `timeline_completed_value()` / `wait_timeline()` | Creates a Vulkan timeline semaphore with the caller's initial value, queries its counter, or waits with `vkWaitSemaphores`; destruction queues the handle on the private command-retirement timeline | The caller owns the monotonically increasing value policy and uses points for application resource lifetimes. NoGraphicsAPI never waits caller-owned points internally. Destruction is nonblocking after all signal submissions have been queued. Every timeline must be destroyed before its device. |
| `get_drawable_extent()` | Queries the window-backed device's current drawable extent without acquiring an image | Lets applications size resolution-dependent offscreen targets and record independent work before acquisition. It is a snapshot; the later acquired frame extent is authoritative. A minimized window returns a zero extent. |
| `acquire()` | Polls completed present fences, waits before reusing the next context in the configured active ring, and calls `vkAcquireNextImageKHR` with an infinite timeout. A stale swapchain is recreated immediately with the preceding handle as `oldSwapchain`; acquire-side `VK_ERROR_OUT_OF_DATE_KHR` repeats recreation and acquisition in the same call until an image is obtained | Returns a populated `SwapchainFrame {render_view, width, height}`. It requires a non-zero drawable extent and terminates when that precondition is violated; callers pause presentation while minimized. Presentation-context availability, image availability, and out-of-date recovery do not produce failure results. Recreation does not drain unrelated device work: old image views and the old swapchain are retained until their command and presentation completion. Acquire requires no active command buffers. The next `begin_commands()` records the internal `UNDEFINED`/`PRESENT_SRC_KHR` to `GENERAL` transition, so independent offscreen work can be submitted before acquire and the drawable pass begun afterward. |
| `submit_and_present()` | Records `GENERAL` to `PRESENT_SRC_KHR` in the batch's last command buffer, then uses one `vkQueueSubmit2` call containing a work batch and an ordered retirement batch. The work batch waits the selected acquire semaphore and signals the caller's timeline plus the binary rendered semaphore; the following empty batch signals the private command-retirement timeline. The function passes that context's fence through `VkSwapchainPresentFenceInfoEXT` to `vkQueuePresentKHR`, then advances the present-context index | Consumes the entire command-buffer batch and signals exactly the caller's value. Keeping the private signal in the following batch orders it after every work-batch signal, so timeline-semaphore destruction can use the generic retirement queue safely. Binary WSI semaphores remain private. Present-side out-of-date marks the swapchain stale so the next `acquire()` recreates it before acquiring; suboptimal does so when the surface extent, transform, or selected composite-alpha mode changed, avoiding an unhelpful rebuild every frame on persistent status. The backend inserts no completion wait, but Vulkan permits `vkQueuePresentKHR` itself to block for a finite interval. |
| `gpu_malloc(..., MemoryType::texture_heap)` / `MemoryType::sampler_heap` | Creates one descriptor-capable `VkBuffer` and one dedicated coherent mapped device-memory allocation per call; the private buffer range includes any prefix alignment slack, suffix padding, and the Vulkan-required hidden reserved region | The returned CPU/GPU pointers both name user byte zero and `GpuAllocation::size` is exactly the requested user size. Both the byte-count and typed overloads support descriptor heaps; the typed overload sizes the range as `count * sizeof(T)`. Descriptor heaps do not consume 256 MiB pooled pages. `gpu_free()` logically releases the heap immediately after its referring commands have been submitted and defers buffer/memory destruction. |
| `create_texture()` / `destroy_texture()` | Creates a 1D, 2D, or 3D optimal-tiled `VkImage`; adds cube compatibility where required and automatically supplies an image-format list containing the compatible public view formats; normally suballocates from a 256 MiB GPU-only image heap; destruction queues the image and allocation release | `TextureDesc::layer_count` counts logical cubes for cube types and physical array layers otherwise. Creation writes no descriptor and submits no work; the next `begin_commands()` call batches the sole `UNDEFINED` to `GENERAL` metadata transition. No persistent view is created implicitly. Destruction may follow submission without waiting for completion. |
| `create_render_view()` / `destroy_render_view()` | Creates one persistent 2D `VkImageView` over one mip and physical array slice; destruction queues the image-view handle | `RenderViewDesc` defaults to mip zero and slice zero. Creation increments the parent texture's live-view counter and logical destruction decrements it immediately; `destroy_texture()` terminates in every build while that count is nonzero. FIFO retirement preserves view-before-image destruction without an application wait. Cube faces are addressed as individual physical slices. |
| `write_texture_descriptor()` | Validates texture/device ownership, then calls `vkWriteResourceDescriptorsEXT` for a sampled/storage `VkImageDescriptorInfoEXT`, with a `VkImageViewCreateInfo` describing the selected format, image type, mip range, and layer range | `TextureDescriptorDesc::format == Format::undefined` inherits the texture format and zero counts select the remaining range. Cube view layers count logical cubes. Both descriptor kinds use `GENERAL`. |
| `write_sampler_descriptor()` | Calls `vkWriteSamplerDescriptorsEXT` from `SamplerDesc` at the caller's CPU destination | Minification, magnification, and mip filters; U/V/W addressing; optional comparison; and fixed 4x anisotropy are explicit. Bias is fixed at zero and the LOD range is unclamped. No public sampler object or Vulkan sampler handle is created. |
| `begin_commands()` | Selects the next context in the growable ring, resets its pool when its private retirement value has completed, or creates a new one-pool/one-buffer context when the cursor is still live; then begins one-shot recording | It does not wait for ordinary ring reuse. The device retains peak context capacity, and no descriptor heap is bound automatically. |
| `set_texture_heap()` / `set_sampler_heap()` | Calls `vkCmdBindResourceHeapEXT` / `vkCmdBindSamplerHeapEXT` for the supplied `GpuRange`, computing the hidden reserved suffix from device properties | Heap binding is explicit command state. The range must be the unchanged user GPU pointer/size returned by the matching `texture_heap` / `sampler_heap` allocation so that the computed suffix names actual backing storage. |
| `draw*()` / `draw_meshlets*()` / `dispatch*()` | Each command has one overload taking `ByteSpan` by value and calls `vkCmdPushDataEXT` immediately before the corresponding Vulkan command when the span is nonempty. A valid root structure converts implicitly, storing its address and `sizeof(T)`; type-erased callers explicitly construct `ByteSpan{byte_ptr, byte_size}`. | Root bytes are copied while recording, so the CPU value need only remain valid for this call. Struct roots must be trivially copyable and a multiple of four bytes; every size must fit `DeviceCaps::max_push_data_size`. A rootless shader declares no `PushConstant` block and passes `{}`, which emits no push-data command. |
| `create_graphics_pso()` / `create_mesh_pso()` / `create_compute_pso()` / `destroy_pso()` | `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` in `VkPipelineCreateFlags2CreateInfo`; `layout = VK_NULL_HANDLE`; maintenance5 chains each `VkShaderModuleCreateInfo` directly into its stage with `module = VK_NULL_HANDLE`; raster PSOs populate triangle-list input assembly with primitive restart disabled (graphics only), fill rasterization with line width 1, depth/stencil, per-target ordinary non-constant blend/write masks, and dynamic-rendering formats; destruction queues `VkPipeline` | SPIR-V is consumed during creation without temporary shader-module objects. Graphics uses vertex/fragment stages and an empty BDA-oriented vertex-input state; mesh uses `VK_SHADER_STAGE_MESH_BIT_EXT` plus fragment, no input assembly, and must output triangles. Culling is disabled or removes clockwise/counter-clockwise winding. Nonzero depth-bias values enable immutable bias state. Stencil read/write masks are shared, while compare, operations, and reference are per face. Only viewport and scissor are dynamic. |
| `begin_render_pass()` / `end_render_pass()` | Maps the `RenderingDesc` color span and optional depth/stencil values to `VkRenderingInfo` and `VkRenderingAttachmentInfo`, then calls `vkCmdBeginRendering` / `vkCmdEndRendering` | Every attachment references a persistent `RenderView` and independently selects load/store state. Active attachments share the selected-mip extent. Simultaneous depth and stencil reference the same render view and therefore the same texture, subresource, and exact `VkImageView`. Resolve modes and resolve views remain absent. |
| `gpu_malloc(..., MemoryType::cpu_visible)`, `MemoryType::gpu_only`, or `MemoryType::readback`; `gpu_free(allocation)` | 256 MiB fully bound universal `VkBuffer` pages, persistent mapping where applicable, `vkGetBufferDeviceAddress`, and pinned [OffsetAllocator](https://github.com/sebbbi/OffsetAllocator) suballocation | Malloc tries one active page first, falls back to a page-list scan only when needed, and promotes a found or retired-free page. `GpuAllocation<T>` carries CPU/GPU/size plus opaque owner/token fields, supporting O(1) deferred free. CPU-visible/readback allocations have both pointers; GPU-only has a null `cpu`. Allocation failure is fatal. Free requires the unchanged aggregate exactly once; the allocator reuses its range only after a nonblocking poll observes private command retirement. |
| `draw_indexed()` | `vkCmdBindIndexBuffer3KHR`, then `vkCmdDrawIndexed` | The supplied raw `GpuRange` is passed as the address range; its 2/4-byte alignment and draw bounds are asserted, but no allocation is looked up or retained. |
| `draw_indirect()` / `draw_indexed_indirect()` | `vkCmdDrawIndirect2KHR` / `vkCmdDrawIndexedIndirect2KHR`; the indexed form first calls `vkCmdBindIndexBuffer3KHR` | Raw `GpuRange` operands are passed directly; argument address and stride are four-byte aligned, and the caller guarantees allocation bounds/lifetime. |
| `draw_meshlets()` / `draw_meshlets_indirect()` | `vkCmdDrawMeshTasksEXT` / `vkCmdDrawMeshTasksIndirect2EXT` | Direct X/Y/Z counts are checked against the device's per-axis and total mesh-workgroup limits. Indirect calls consume one or more `VkDrawMeshTasksIndirectCommandEXT` records from a four-byte-aligned raw address range with an explicit count and optional stride. |
| `dispatch_indirect()` | `vkCmdDispatchIndirect2KHR` | The raw range must cover one dispatch command and be four-byte aligned; it is not retained. |
| `copy_memory()`, `copy_memory_to_texture()`, `copy_texture_to_memory()` | `vkCmdCopyMemoryKHR`, `vkCmdCopyMemoryToImageKHR`, `vkCmdCopyImageToMemoryKHR`; texture copies map `TextureCopyDesc` to one `VkDeviceMemoryImageCopyKHR` | Each memory operand is a raw `GpuRange`, which may select an interior address. Texture copies select one mip, texel region, and physical slice range; byte pitches describe placement within the range. Zero extents/counts select the remaining subresource and zero pitches select tight packing. Every image operand uses `GENERAL`. |
| `barrier()` | `vkCmdPipelineBarrier2` with a global memory barrier | Callers synchronize actual hazards without naming an image layout. Incompatible stage/access pairs assert. `Stage::all_commands` maps literally to Vulkan's GPU-command scope and excludes the independently combinable host stage. Host writes to coherent mapped memory are made available at queue submission, so `Stage::host` is destination-only and requires `Access::host_read` for GPU-to-CPU readback. `Access::descriptor_read` maps to both heap-read access bits. |
| `submit()` | Ends the nonempty all-live span, fills retained `VkCommandBufferSubmitInfo` storage, and uses one `vkQueueSubmit2` call containing the work batch followed by an empty batch that signals the private retirement value | The work batch signals the supplied application point. The following batch orders private retirement after it. Every begun handle must occur exactly once in the next submission, the span defines execution order, every context receives the same retirement value, every handle is consumed, and the call returns after queueing without waiting. |
| `wait_idle()` | Waits the latest private command-retirement value and every outstanding present-context fence, then resets every command pool | This is the explicit whole-device completion point without `vkDeviceWaitIdle`. Synchronous callers may instead wait one application submission point with `wait_timeline()`; the core does not combine submission and waiting. |

All ordinary descriptors, dynamic-rendering attachments, and copy operands name
`VK_IMAGE_LAYOUT_GENERAL`. Swapchain textures use that same layout while rendering, but the backend
privately transitions them to/from `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` around presentation.
Vulkan still requires a newly created image to start in `VK_IMAGE_LAYOUT_UNDEFINED` and transition
away from it once to initialize image metadata. `create_texture()` leaves that work pending;
`begin_commands()` batches every pending image into one dependency before application commands.
The public API never exposes an image layout or transition, and texture creation never submits or
waits.

The public memory surface is exactly two standard-layout, trivially copyable aggregates:

```cpp
struct GpuRange
{
    void* gpu = nullptr;
    std::uint64_t size = 0;
};

template<typename T = std::byte>
struct GpuAllocation
{
    T* cpu = nullptr;
    T* gpu = nullptr;
    std::uint64_t size = 0;
    const GpuAllocationOwner* allocation_owner = nullptr;
    std::uint64_t allocation_token = 0;
};
```

Both types have useful null defaults and no ownership behavior. The allocation owner/token are
opaque `gpu_free()` bookkeeping and must be preserved unchanged until the allocation is passed to
`gpu_free(allocation)` exactly once; `gpu_range()` deliberately drops them because command recording consumes
only an address and size.
`gpu_malloc<T>()` returns `GpuAllocation<T>`. Every public operation that needs a Vulkan
address-plus-size pair takes `GpuRange` by value. `gpu_range()`
converts an allocation to its complete range. Allocation failure is fatal; a successful GPU-only
allocation has a null `cpu` but valid `gpu` and `size` fields.

Command recording performs no address-to-allocation lookup and command buffers retain no allocation
records. Address commands consume the numeric pointer and size directly. The caller guarantees that
every range is inside an allocation that has not yet been logically freed. Once every command buffer
that references it has been submitted, `gpu_free()` may be called without waiting: allocator-token
reuse and any backing Vulkan destruction are deferred to that device's private command-retirement
timeline. The unchanged allocation aggregate must still be passed exactly once.

The allocator maintains separate page pools for `MemoryType::cpu_visible`, `gpu_only`, and `readback`. Every
page is one 256 MiB fully bound, non-sparse Vulkan buffer with shader-address, storage, index,
indirect, transfer-source, and transfer-destination usage. A pinned revision of
[OffsetAllocator](https://github.com/sebbbi/OffsetAllocator) manages non-overlapping aligned
suballocations inside each page. Each pool tries its active page first. Only an active-page miss
scans older pages; a successful fallback or retired free promotes that page, and a new 256 MiB page
is created only when none fit. The opaque owner/token lets deferred deletion return the range to
OffsetAllocator without page or allocation-record searches. A single allocation, including
alignment padding, must fit one page. Device selection therefore also requires a 256 MiB mapped
coherent device-local heap, a 256 MiB non-host-visible device-local heap, and
`maxBufferSize >= 256 MiB`.

Descriptor heaps are intentionally outside those page pools. Each `gpu_malloc()` call with
`MemoryType::texture_heap` or `MemoryType::sampler_heap` creates one descriptor-capable
buffer sized for any prefix alignment slack, the application bytes, suffix padding, and the
Vulkan-required reserved region, then binds one dedicated allocation of coherent mapped
device-local memory. This avoids
adding `VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT` to every 256 MiB page: Vulkan permits creation of
such a buffer to fail when its size exceeds the reported descriptor-heap limits. Only the user
range is returned, so heap slot zero remains the returned CPU/GPU pointer and each heap can be
written, selected, and freed independently.

Both `gpu_malloc()` overloads support descriptor heaps. The typed overload uses `count * sizeof(T)`
bytes and returns the user range as `GpuAllocation<T>`, allowing applications to describe fixed
descriptor layouts with their own structures. The byte-count overload remains useful when the heap
size or alignment comes from runtime device properties. Its optional `alignment` argument applies
to the GPU address; CPU virtual addresses and Vulkan device addresses are independent address
domains.

Textures normally use another OffsetAllocator-backed pool of 256 MiB GPU-only image-memory heaps.
Image allocation uses the same active-page-first, fallback-scan, and free-time-promotion policy per
Vulkan memory type. A
texture still owns its `VkImage`, but ordinary image memory is a pooled suballocation rather than one
allocation per texture. Vulkan images that require dedicated allocation, or cannot fit one page,
retain the Vulkan-required dedicated-memory path. Sampled and storage descriptors use image create
information directly. `create_render_view()` explicitly creates the persistent fixed-function
`VkImageView` required by dynamic rendering; texture creation itself does not create one.

Because every backing buffer is fully bound and has storage-buffer usage, address commands
consistently use
`VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR | VK_ADDRESS_COMMAND_STORAGE_BUFFER_USAGE_BIT_KHR`.
The exact flag and aliasing rules are in the
[device-address command proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_device_address_commands.html).

Texture creation queries the exact
optimal-tiling format/type/usage combination and checks its returned extent, mip, array-layer, and
resource-size limits. Sampled formats must support sampled-image access; the API does not impose
linear-filter support when a texture may only be used with nearest filtering. Storage formats must
support formatless reads and writes used by general Slang `RWTexture` declarations. Graphics and
mesh PSO color/depth/stencil formats are separately checked for attachment support.

`supports_texture_format(device, format, usage)` applies the same format-feature and aspect/usage
rules without creating an image. BC formats additionally require the enabled
`textureCompressionBC` core feature, ASTC formats require `textureCompressionASTC_LDR`, and EAC
requires `textureCompressionETC2`. Candidate selection rejects a physical device unless BC or ASTC
LDR is available; every supported compression feature is enabled, and `DeviceCaps` reports BC and
ASTC independently because both may be true.

The public format enum maps directly to Vulkan and covers R/RG/RGBA 8/16/32-bit UNORM, UINT, and
floating-point formats; RGBA/BGRA 8-bit UNORM/SRGB/UINT; 16-bit packed color; RGB/RGBA 32-bit UINT
and float; RGB10A2 and RG11B10 packed color; D16, D24S8, D32, S8, and D32S8; and EAC RG, ASTC 4x4,
BC3, BC5, and BC7 compression. `get_texture_format_info()` exposes the corresponding block extent,
bytes per block, and aspect flags without leaking `VkFormat`. The shared
`depth_stencil_attachment` usage maps to `VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`.

## Dynamic rendering and PSO state

`GraphicsPSODesc::color_targets` and `MeshPSODesc::color_targets` are non-owning spans
of up to eight `ColorTargetDesc` values, covering Vulkan's guaranteed attachment count without
dynamic allocation. Their order is the fragment shader's output-location order. Each target
selects its format, four-bit RGBA write mask, and independent ordinary non-constant color/alpha
blend equations. The separate optional `depth_format`
and `stencil_format` fields map to
`VkPipelineRenderingCreateInfo::depthAttachmentFormat` and `stencilAttachmentFormat`. When both
aspects are present, Vulkan requires the two attachment formats to match.

`RenderingDesc::colors` is a matching span of `ColorAttachment` values. Each color entry and the
optional depth and stencil entries independently select `LoadOp::load`, `clear`, or `discard`
and `StoreOp::store` or `discard`. These map directly to Vulkan attachment load/store
operations. The clear value is ignored unless the corresponding load operation is `clear`.
Simultaneous depth and stencil attachments must reference the same combined-aspect
`RenderView`, as required by dynamic rendering. All active attachments use the same width and
height.
No render-pass boundary inserts an application data-hazard barrier.

Each graphics or mesh PSO stores culling (none or clockwise/counter-clockwise winding), three
depth-bias values, depth/stencil state, and each target's write mask plus
ordinary non-constant color/alpha blend equations. Any nonzero depth-bias value enables bias.
Depth state contains test, write, and comparison. Stencil has shared read/write masks with per-face
compare, operations, and reference. Graphics input assembly is fixed to triangle-list with
primitive restart disabled, and rasterization is fixed to fill mode with line width 1. Its vertex
input is intentionally empty because shaders fetch vertex data through BDA. Mesh shaders must
output triangles. Tessellation and task shaders are not supported. Only viewport and scissor are
dynamic command state.

`independentBlend` is required and enabled during device creation, so per-target state is an
invariant and no corresponding `DeviceCaps` field exists. The PSO surface omits optional
fixed-function modes outside the API's minimal triangle-rendering scope.

## Texture types, views, and copies

The six public texture types map to Vulkan image and descriptor-view types as follows:

| `TextureType` | Vulkan image/view | Shape and layer contract |
|---|---|---|
| `one_d` | 1D / 1D | Height and depth are one; `layer_count` is one. |
| `two_d` | 2D / 2D | Depth and `layer_count` are one. |
| `three_d` | 3D / 3D | `layer_count` is one; depth is the texel depth. |
| `cube` | 2D with `CUBE_COMPATIBLE` / cube | Width equals height and one logical cube expands to six physical array layers. |
| `two_d_array` | 2D / 2D array | Depth is one; `layer_count` is the physical array-layer count. |
| `cube_array` | 2D with `CUBE_COMPATIBLE` / cube array | Width equals height; every logical layer expands to six physical array layers. |

`RenderViewDesc` selects one `mip_level` and one physical `slice`; cube faces are addressed
individually. The render area and automatic viewport and scissor use the selected mip's dimensions.
The resulting `RenderView` is persistent and can be reused by any number of command buffers.
It must be destroyed before its texture. Depth and stencil entries supplied together must reference
the same combined-aspect view, so both Vulkan attachment records use the same handle as required by
dynamic rendering. `TextureDescriptorDesc` ranges remain exclusive to sampled/storage descriptors.

`TextureDesc::layer_count`, `TextureDescriptorDesc::base_layer`, and
`TextureDescriptorDesc::layer_count` use logical cubes for `cube` and `cube_array`. A zero descriptor count
selects every remaining logical layer; `TextureDescriptorDesc::mip_count` has the same remaining-range
rule. `TextureDescriptorDesc::format == Format::undefined` inherits the creation format. An explicit
view format must be compatible with the image's creation format. The backend automatically declares
the finite set of compatible public formats through `VkImageFormatListCreateInfo`; no corresponding
field appears in `TextureDesc`. Uncompressed color formats use Vulkan's 8-, 16-, 32-, 64-, 96-,
and 128-bit compatibility classes. BC3, BC5, BC7, EAC RG, and ASTC 4x4 remain separate compressed
classes, with linear/sRGB variants compatible inside the same class. Depth/stencil formats require
an exact match. A selected format must also support the sampled or storage descriptor usage being
written; compatible sRGB views, for example, are normally sampled rather than storage views.

`TextureCopyDesc` deliberately uses physical `base_slice` and `slice_count`: array layers are
addressed directly and each cube contributes six consecutive face slices. A 3D texture instead
uses `z` and `depth`, with its sole array slice selected. The descriptor chooses one mip and one
texel box. Zero width, height, depth, or slice count means the remainder of that subresource.
`row_pitch_bytes` and `slice_pitch_bytes` describe successive rows and depth/layer slices in bytes.
Zero pitches request tight packing. To start within an allocation, the caller supplies an interior
`GpuRange` with its address and size adjusted. The backend validates the complete byte footprint,
converts explicit byte pitches to Vulkan's texel row length and image height. Compressed offsets
must be 4x4 block aligned; compressed extents must be
block aligned unless they reach the mip edge. Row pitches contain whole blocks, slice pitches
contain whole block rows, compressed color addresses use the 16-byte block alignment, and all
depth/stencil addresses use Vulkan's required four-byte alignment.
The combined `d24_unorm_s8_uint` and `d32_float_s8_uint` formats cannot be copy operands because
`TextureCopyDesc` has no aspect selector; texture creation rejects transfer usage for them. The
separate depth-only and `s8_uint` formats copy their single aspect without adding an aspect field.

`NoGraphicsAPI` supports only single-sample textures and rendering. `VK_EXT_descriptor_heap` has no
encoding for sampled multisample image descriptors, so MSAA would require a special-case attachment
and resolve path outside the uniform descriptor-heap model. MSAA is not commonly used in modern
rendering pipelines, so the public API deliberately omits the multisample texture, rasterization,
and resolve surface.

`MemoryType::gpu_only` allocations and textures require a non-host-visible device-local memory type and
are never mapped. This is enforced independently from the coherent device-local type used by
CPU-visible allocations, readback, and descriptor-heap storage.

All device, allocation, descriptor, command-recording, and submission state is deliberately
single-threaded. The implementation uses no mutexes or atomic counters. Calling functions on one
device concurrently is outside the API contract.

## Descriptor-heap and root-data model

The heap allocator uses the portable class sizes from
`VkPhysicalDeviceDescriptorHeapPropertiesEXT`: `imageDescriptorSize` for the NoGraphicsAPI texture heap
(Vulkan's resource heap) and `samplerDescriptorSize` for the sampler heap. It intentionally does not tightly pack using
`vkGetPhysicalDeviceDescriptorSizeEXT`; Khronos describes that query as a specialized,
non-portable optimization. Heap base addresses, descriptors, and reserved regions honor all
reported alignments and limits. The normative rules are in the
[descriptor-heaps chapter](https://docs.vulkan.org/spec/latest/chapters/descriptorheaps.html) and
[`VkPhysicalDeviceDescriptorHeapPropertiesEXT`](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorHeapPropertiesEXT.html).

The application passes `N * DeviceCaps::texture_descriptor_stride` bytes to
`gpu_malloc(..., MemoryType::texture_heap)`, or `N * DeviceCaps::sampler_descriptor_stride` bytes
to `gpu_malloc(..., MemoryType::sampler_heap)`. The returned `cpu`, `gpu`, and `size` describe only those `N`
user slots. Each dedicated backing buffer appends the required reserved bytes as an
implementation-owned suffix. Consequently application slot zero is exactly the returned pointer
and shader index zero. Any backing-buffer prefix alignment slack is hidden before that pointer and
requires no shader index adjustment.

The stride fields round the corresponding raw `texture_descriptor_size` or
`sampler_descriptor_size` up to Vulkan's descriptor-address alignment. Descriptor writer calls
still pass the raw size to Vulkan; the aligned stride is solely for heap sizing and slot addresses.

`write_texture_descriptor()` writes one device-sized texture descriptor directly at a caller-selected
CPU destination. It supports `TextureDescriptorType::sampled` or `storage` and validates
texture/device ownership. The descriptor's type follows `TextureType`, and
its format and mip/layer ranges follow the contracts above. `write_sampler_descriptor()` does the
same for a `SamplerDesc`; there is no public `Sampler` handle. Its filter, address, comparison,
and fixed 4x anisotropy values map directly to `VkSamplerCreateInfo`; bias is zero and the LOD range is
`[0, VK_LOD_CLAMP_NONE]`. Applications can arrange slots, contiguous tables, and multiple texture views
freely.

`set_texture_heap()` and `set_sampler_heap()` explicitly bind the application allocations. The
supplied `GpuRange` uses the unchanged user `gpu` and `size`; the backend
computes the reserved suffix offset and size from device properties for `VkBindHeapInfoEXT`. Neither
heap is created or selected by `begin_commands()`.

Descriptors are written into coherent device-local mapped storage. The runtime rejects devices
without a `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT` memory type, so explicit mapped-range flushes
and invalidations do not exist. GPU writes or copies into a heap must be synchronized to the
appropriate Vulkan sampler/resource-heap read access; descriptor availability does not replace
synchronization for the described texture itself.

Heap contents, descriptor slots, and GPU pointers copied from root data are opaque to command
recording. The caller must keep descriptor bytes unchanged and must not logically destroy or free a
referenced value until every command buffer using it has been submitted. After submission,
`destroy_*()` and `gpu_free()` invalidate the public value immediately but queue the backing Vulkan
handle or allocator token at the latest private command-retirement value. The FIFO queue preserves
destruction order for entries sharing a value and only its front needs to be polled. Reusing an
application-managed descriptor slot still requires its own completion policy; `deferred_renderer`
instead allocates a fresh small texture heap when its G-buffer size changes and retires the complete
old heap. Every public destroy/free operation asserts if any command buffer is recording.
`wait_idle()` remains available for an intentional whole-device drain.

The deletion queue is a fixed 4096-entry ring. Each entry stores a monotonic retirement value, one
64-bit payload, and a trivially copyable callback with exactly eight bytes of inline capture storage;
it uses neither `std::function` nor per-entry allocation. Compound releases enqueue their Vulkan
handles and allocator tokens in destruction order. Each enqueue polls retirement and collects the
completed prefix; if the ring remains full, it terminates with a capacity error instead of inserting
a host wait. Ready entries are also collected while acquiring a command context, allocating memory
or a texture, querying the drawable extent, waiting idle, and tearing down the device.

Shared root structures are ordinary CPU PODs and may contain typed BDA pointers represented by the
same pointer type in C++ and Slang. Each draw or dispatch has a single overload taking `ByteSpan` by value.
A valid root structure converts implicitly, and the span carries its address and `sizeof(T)` so the
command can immediately copy those bytes into command-buffer push data before issuing the native
command. Type-erased callers explicitly construct `ByteSpan{byte_ptr, byte_size}`. A rootless
command passes `{}` and emits no push data. The structure itself is placed in
push-data storage, avoiding shader
indirection through a pushed GPU address. Vulkan cannot load push-data state directly from GPU
memory. As a secondary consequence, the CPU root requires no GPU allocation and no lifetime beyond
the draw or dispatch call. GPU pointer fields must
not be dereferenced by the CPU, and the allocations they name need physical backing through GPU
completion; `gpu_free()` after submission supplies that backing through deferred retirement. A root can mix typed BDA
pointers, `uint32` texture/sampler indices, and 32-bit scalar/vector/matrix values. Slang shaders
sharing C++ POD structures use `-fvk-use-c-layout` and `-matrix-layout-row-major` for matrices.
`shader_types.h` supplies only the matching CPU-side POD types. C++ code includes
`<NoGraphicsAPI/math.hpp>` when it needs operators and math functions. The cube
root contains a typed vertex pointer and a full `float4x4` model-view-projection transform; its
vertex shader multiplies each position by that matrix directly. Its one sampled texture and
nearest/clamp sampler both use heap slot zero.

Shaders that access the heaps compile with capability
`spvDescriptorHeapEXT` and use Slang's `ResourceDescriptorHeap[index]` and
`SamplerDescriptorHeap[index]`. The cube uses this path. The native heap path maps to
[`SPV_EXT_descriptor_heap`](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_descriptor_heap.html),
whose `DescriptorHeapEXT` capability implicitly declares `UntypedPointersKHR`. Slang 2026.14.1 or
newer and this capability are mandatory, so heap-using shaders contain direct heap instructions.

Heap commands and descriptor-set, descriptor-buffer, push-descriptor, or push-constant commands
mutually invalidate their state. The backend never records `vkCmdBindDescriptorSets`,
descriptor-buffer commands, push descriptors, or
`vkCmdPushConstants`, so a `NoGraphicsAPI` command buffer stays entirely in heap mode. See
[`vkCmdPushDataEXT`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPushDataEXT.html)
for the host-copy contract and the descriptor-heaps chapter for the full invalidation list.

## Command recording and submission

Each device starts with two command contexts in a ring. Every context owns one transient
`VkCommandPool`, one primary `VkCommandBuffer`, one embedded public handle, and one private
retirement value. `begin_commands()` examines the cursor context. If its retirement value has
completed, the function resets that pool and reuses its buffer. If the context is still recording or
executing, the function creates another one-pool/one-buffer context instead of waiting and retains it
in the ring. Submit-info storage grows alongside the context ring. A workload that has reached its
peak number of simultaneously live command buffers therefore performs no host or Vulkan allocation
during ordinary recording or submission.

`submit()` and `submit_and_present()` accept a nonempty span containing every command buffer begun
since the preceding submission exactly once. The span order defines queue execution order; there is no
abandonment API. The backend ends all buffers and passes two batches to one `vkQueueSubmit2`: the
first contains the work and public signal, while the following empty batch signals private
retirement. This explicit batch order is required because signal operations within one batch are
unordered. The backend then consumes every public command-buffer handle. Pending texture initialization is attached to
the next buffer begun after creation, and an acquired swapchain transition is attached to the next
buffer begun after acquire. Applications order each such buffer before commands that use the image.
For presentation, the final transition to `PRESENT_SRC_KHR` is recorded in the span's last buffer.
Because acquire requires no active command buffers, an application can record and submit an
offscreen batch first, acquire late, then begin a second command buffer containing only the drawable
pass.

The device owns one private timeline semaphore for command-context reuse and deferred Vulkan-object
destruction. Its value advances once per `submit()` or `submit_and_present()` call, not once per
command buffer.
Every context in the work batch receives that same retirement value. `begin_commands()` polls this
timeline once at the start of a recording batch, resets completed contexts, and grows the ring if
the cursor remains busy. The timeline is not exposed and does not replace caller points used for
application-owned data reuse or readback.

Presentation uses a static capacity of eight contexts.
`DeviceDesc::desired_swapchain_image_count` selects the active prefix, defaults to two, and therefore
bounds outstanding presentations even when the implementation creates extra swapchain images.
Each present context contains acquire/rendered binary semaphores and a present fence; only
`acquire()` and `submit_and_present()` select and advance this ring. Acquisition polls completed
fences, then waits before reusing the next context in the ring. `vkAcquireNextImageKHR` uses an
infinite timeout, so ordinary image backpressure waits. Acquire-side out-of-date
recreates the swapchain and retries acquisition within the same call. Swapchain replacement calls
`vkCreateSwapchainKHR` immediately with the current handle as `oldSwapchain`. The WSI cursor remains
independent from command-context selection. A retired swapchain generation is tracked until every
present fence that names it completes; its image views and then its `VkSwapchainKHR` are enqueued on
the same generic command-retirement queue in that order. Acquisition requires a non-zero drawable
extent and otherwise always returns a populated frame.

The caller creates a `TimelineSemaphore`, owns its next value, and passes a `TimelinePoint` to every
`submit()` and `submit_and_present()`. The semaphore must belong to the command-buffer device, its
value must strictly exceed the previous submitted value, and it must obey Vulkan's
`maxTimelineSemaphoreValueDifference`; programming mistakes are debug assertions. The call signals
exactly the supplied value and returns after queue submission without waiting for execution.
`timeline_completed_value()` provides a nonblocking poll and `wait_timeline()` waits one known
submitted point. These caller-owned points are application data-reuse and readback markers;
NoGraphicsAPI does not retain or wait them internally.
If a finite `maxTimelineSemaphoreValueDifference` would be exceeded, private retirement is
force-polled once and capacity exhaustion terminates instead of adding an implicit host wait.

`wait_idle()` is the explicit whole-device completion point. It waits the latest private
command-retirement value and all outstanding present fences before resetting every command pool.
The interactive examples do not call it; they use the explicit query, acquire, record, submit, and
present operations in the order appropriate to their workloads.
Some wait their latest application point before reclaiming application-owned data at shutdown;
device destruction itself drains the private retirement timeline and device-owned WSI state.

The examples retain their latest application point for explicit data reuse, readback, and shutdown.
`deferred_renderer` alternates two `ObjectData` matrix ranges and stores each slot's exact G-buffer
submission value. It polls the timeline, uses any completed slot, and skips a loop iteration when
both slots remain in flight rather than waiting.
Internal command buffers and submission metadata are retained, but recording and submission retain
no public object or allocation. Public objects and allocations must remain live until every
referencing command buffer has been submitted; they may then be destroyed or freed immediately
because the queue defers their physical backing. Mutable allocation and descriptor contents must not
be overwritten until their application point completes. The queue, contexts, timeline values, and
allocators are single-threaded and use no locks or atomics.

## Example coverage and presentation

The triangle is a 512x512 minimal RGB rendering following the first triangle in the official
[Khronos Vulkan Tutorial](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/02_Graphics_pipeline_basics/01_Shader_modules.html).
It issues one draw of three vertices; a traditional vertex shader emits three constant
positions/colors from `SV_VertexID`. It has no vertex allocation, mesh shader, sampled
texture, descriptor index, compute stage, or depth/stencil attachment.

The cube is a 500x500 rendering based on the official
[Khronos/LunarG `vkcube` source](https://github.com/KhronosGroup/Vulkan-Tools/tree/vulkan-sdk-1.4.357.0/cube).
It uses BDA vertex fetch, sampled-image and sampler heaps, an sRGB texture, a `d32_float` depth
attachment, baked depth testing/writes, and the sample's four-degree-per-frame Y rotation. It allocates and
binds both heaps explicitly and writes one texture plus one nearest/clamp sampler into slot zero.
Its raw RGBA8 texture preserves the exact RGB content of the
[`lunarg.ppm.h`](https://github.com/KhronosGroup/Vulkan-Tools/blob/vulkan-sdk-1.4.357.0/cube/lunarg.ppm.h)
asset distributed under Vulkan-Tools' [Apache-2.0 license](https://github.com/KhronosGroup/Vulkan-Tools/blob/vulkan-sdk-1.4.357.0/LICENSE.txt),
and is read directly into mapped upload memory without an image decoder.

All three examples require Windows and render directly to device-owned Vulkan swapchains. They fail
at compile time on other platforms. The
triangle and cube stay intentionally sample-sized. `deferred_renderer` alternates between two mapped
48 MiB arrays containing 1,048,576 object matrices. It polls for a completed range and skips the
loop iteration instead of waiting when both remain in flight, then renders all cubes to the G-buffer
with one indexed instanced draw using a typed GPU vertex pointer and a GPU `uint16_t` index buffer.
A fullscreen deferred pass reads albedo, normal/roughness, and depth, reconstructs
world position from an interpolated camera ray and linearized hardware depth, and evaluates 3D
lighting into the swapchain. Its G-buffer command buffer is submitted before `acquire()`; a second
command buffer records the deferred drawable pass and is submitted by `submit_and_present()`. There
is no CPU image readback or platform blit. The WSI backend is Win32. Headless x86-64
GNU/Clang/AppleClang library builds remain available with `-DNOGRAPHICSAPI_BUILD_EXAMPLES=OFF`; creating
a device without a window creates no WSI state.

The examples do not explicitly process `WM_SIZE`; their window pump waits while minimized and thus
satisfies the non-zero drawable precondition of `acquire()`.
`get_drawable_extent()` reports a snapshot before acquire so applications can replace
resolution-dependent offscreen resources without first reserving a swapchain image. Acquire-side
out-of-date recreates and retries immediately. Present-side out-of-date makes the next `acquire()`
recreate before obtaining an image. Suboptimal does the same when the surface extent, transform, or
selected composite-alpha mode differs. Triangle has no dependent targets. Cube uses the acquired
frame extent for its shared depth target and projection. `deferred_renderer` replaces its G-buffer
targets, depth target, projection, deferred reconstruction rays, and small descriptor heap from the
earlier snapshot, then records and submits the G-buffer before acquiring. Its final pass rescales
pixel coordinates if the authoritative acquired extent changed meanwhile. Replacing the heap keeps
old descriptor bytes unchanged for in-flight frames. Deferred deletion retires the old resources
without an application timeline wait.

## Driver evidence

Support must always be decided by extension enumeration and feature queries, not vendor or version
strings. The following is primary-source evidence available on the status date, not a guarantee for
every GPU in a driver's product matrix.

| Driver/backend | Evidence as of 2026-08-15 |
|---|---|
| NVIDIA proprietary | NVIDIA's [Vulkan developer-driver history](https://developer.nvidia.com/vulkan-driver) added `VK_EXT_descriptor_heap` in Windows 582.30/Linux 580.94.16 on 2026-01-23 and `VK_KHR_device_address_commands` in Windows 595.92/Linux 595.44.03 on 2026-03-13. NVIDIA's [descriptor-heap guidance](https://developer.nvidia.com/blog/streamlining-resource-binding-with-end-to-end-support-for-vulkan-descriptor-heaps/) recommends driver 610+ for the full descriptor-heap/tooling path; earlier access is through the developer beta branch. |
| AMD Windows | AMD's [Vulkan driver support table](https://www.amd.com/en/resources/support-articles/release-notes/rn-rad-win-vulkan.html) lists `VK_EXT_descriptor_heap` in 25.30.17.02 and lists both it and `VK_KHR_device_address_commands` in Adrenalin 26.6.1 (2026-06-02). |
| Intel ANV / AMD RADV on Linux | [Mesa 26.1](https://docs.mesa3d.org/relnotes/26.1.0.html) introduced device-address commands on RADV and descriptor heaps behind `RADV_EXPERIMENTAL=heap`. [Mesa 26.2](https://docs.mesa3d.org/relnotes/26.2.0.html) enables descriptor heaps by default on ANV and RADV, and its change list records the ANV device-address-commands implementation. |
| Intel Windows | No Intel Windows release note naming the complete required extension set was found. Runtime enumeration remains authoritative. |

## Relationship to *No Graphics API*

The [design comparison](no-graphics-api-comparison.md) separates direct implementations of the
post's model from Vulkan/Slang adaptations, project policy, and features that are not implemented
yet. This document stays focused on exact Vulkan contracts and backend behavior.
