# Vulkan implementation

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
| [`VK_EXT_descriptor_heap`][descriptor-heap] with [`VK_KHR_shader_untyped_pointers`][untyped-pointers], or `VK_EXT_descriptor_buffer` | Native heaps use layout-free pipelines and `vkCmdPushDataEXT`; descriptor buffers use private layouts and an eight-byte root push constant. |
| [`VK_KHR_device_address_commands`][address-commands] (optional) | Index, indirect, and copy commands consume GPU addresses directly. Otherwise addresses resolve to buffers and offsets. |
| [`VK_KHR_unified_image_layouts`][unified-layouts] (optional) | Optimizes ordinary `GENERAL` image use; the public model is unchanged without it. |
| [`VK_EXT_mesh_shader`][mesh-shader] | Required task and mesh shader support for direct and indirect workgroup draws. |
| Buffer device address | Gives GPU heaps 64-bit shader addresses for their lifetime and enables typed pointer fields in shared structures. |
| Vulkan 1.3 `synchronization2` and `dynamicRendering` | Resource-free barriers and rendering without render-pass or framebuffer objects. |
| Core Vulkan dynamic state | Command-set viewport, scissor, and exposed depth/stencil state. |
| Timeline semaphores | Application-visible completion points and cross-queue waits; private swapchain retirement. |
| 64-bit timestamps (optional) | Supported queues capture GPU markers; `read_timestamps(pool)` retrieves them into CPU memory after submission completion. |
| Shader and layout features | Scalar layout, float16, 16-bit storage access, draw parameters, and independent blending. |
| Texture features | Compression is reported through `DeviceCaps`; exact format and usage support is queried with `supports_texture_format`. |
| Win32 WSI | `VK_KHR_surface`, `VK_KHR_win32_surface`, `VK_KHR_get_surface_capabilities2`, and `VK_KHR_swapchain`. |

Windowed devices use `VK_KHR_surface_maintenance1` and [`VK_KHR_swapchain_maintenance1`][swapchain-maintenance]
when available, with the original EXT variants also accepted. Swapchain maintenance supplies a completion
fence for each presentation. Drivers without maintenance use core presentation with one render-finished
semaphore per swapchain image and a presentation-queue wait at swapchain replacement and shutdown.
Debug builds enable `VK_EXT_debug_utils` and the Khronos validation layer when available.

`DeviceCaps::storage_input_output16` and `storage_push_constant16` reflect the advertised Vulkan features
and are enabled only when supported. Neither is required to create a device. The former permits 16-bit
shader stage interfaces; the latter describes Vulkan push-constant storage, not pointed-to root members.
Metal reports 16-bit stage interfaces and leaves the Vulkan-only push-constant flag false.

Storage-image usage requires per-format read/write-without-format support. Vulkan 1.3+ permits these
[SPIR-V capabilities](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html) independently of the legacy
`shaderStorageImageReadWithoutFormat` and `shaderStorageImageWriteWithoutFormat` feature bits; those bits
are neither required nor enabled. BC/ASTC compression is also optional for device creation; applications
must check their asset formats before using them.

## GPU heaps and application-side allocation

The backend requires device-local host-visible coherent memory for upload heaps and descriptor
heaps, plus compatible device-local memory for GPU-only heaps and textures. Readback heaps require
host-visible coherent memory and prefer host-cached types, including system RAM on discrete GPUs.
They stay mapped; callers must complete GPU writes and make them available to host reads before accessing them.
Coherent memory needs no explicit mapped-range invalidation. GPU-only selections
prefer a non-host-visible type, but can use a host-visible UMA type without mapping or exposing it.
Upload and descriptor heaps have no host-only fallback. Non-coherent mapped memory is not used.

`create_gpu_heap()` returns one application-sized block. `cpu_visible`, `gpu_only`, and `readback`
provide addressable data storage. Opaque texture and sampler descriptor heaps use separate creation APIs.
`GpuCpuRange<T>::size` is always bytes, regardless of `T`, and a GPU-only heap has a null CPU pointer.

When device-address commands are unavailable, index, indirect and copy operations resolve addresses through
the same table used by Metal. Each command range must fit in one GPU heap; at most 64 GPU heaps may be live
per device. Lookups use binary search with atomic version checks; only table updates serialize.
Devices with the extension pass command addresses directly and do not allocate the table.

Application data has no backend suballocator. The optional utility library provides fixed-16-byte
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

Vulkan always reports `DeviceCaps::indirect_mesh_draw`; the required mesh extension provides both address-based and buffer-based indirect commands.

Address commands use fully bound storage-buffer ranges. Interior ranges are valid, so applications
can suballocate and pass only the relevant byte interval. Interior-pointer alignment, bounds, and
lifetime remain the caller's contract.

Conventional vertex-input bindings are absent. Vertex shaders normally load through a typed pointer
in root data; indexed draws bind only the index address range required by Vulkan.

## Application-owned descriptor heaps

Texture and sampler descriptors live in opaque `TextureDescriptorHeap` and `SamplerDescriptorHeap`
objects created with application-selected slot capacities. `write_texture_descriptor(heap, index, ...)`
and `write_sampler_descriptor(heap, index, ...)` encode descriptors in a cached CPU array allocated with the heap,
then copy those bytes to private mapped storage. `copy_texture_descriptors()` and `copy_sampler_descriptors()`
copy indexed ranges through that CPU array, including overlapping ranges, without reading mapped GPU memory.
Native bytes, strides, reserved regions, and GPU addresses remain private.

`set_texture_descriptor_heap()` and `set_sampler_descriptor_heap()` bind those heaps. Shared `gpu_texture`
and `gpu_sampler` helpers use native [`SPV_EXT_descriptor_heap`][spirv-heap] access or descriptor-buffer
arrays selected at shader compilation. The latter keeps separate sampled/storage planes with the same
application indices, and retains native image views and samplers until their final descriptor copy is overwritten or destroyed.
Descriptor writes encode into cached CPU storage before copying to write-only GPU mappings on both paths.
Arbitrary GPU writes or copies of descriptor bytes are not part of this portable contract.

Texture views can select a compatible format only when `TextureDesc::mutable_format` is enabled;
they can select a mip/layer range and color, depth, or stencil aspect. The default
`TextureAspect::automatic` preserves format-derived selection. Sampler descriptors are encoded
directly, so there is no public sampler object. Buffers use GPU pointers instead of resource-heap
entries.

The application owns descriptor-slot reuse. Concurrent destinations must be disjoint and copy
sources stable. A slot cannot be overwritten while in-flight work may read it. This is intentionally the same explicit lifetime model used for mapped heap suballocations.

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
`TextureAllocator` uses that granularity and returns a POD `PlacedTexture` containing an uninitialized texture. Pass the aggregate back to
the same allocator after GPU use has completed. The API does not track texture-to-heap or
texture-to-descriptor dependencies.

Texture upload and readback go through buffer-backed `GpuRange` values with
`copy_memory_to_texture()` and `copy_texture_to_memory()`. The application retires texture-heap ranges
with its submission timeline.

`create_texture(device, desc, heap, offset)` creates and binds an image without recording GPU commands.
Views and descriptors can be created immediately. Before first GPU use, call `initialize_texture(commands, texture)`
outside rendering to initialize its metadata with a whole-image `UNDEFINED` to `GENERAL` transition.
This discards contents; clear or overwrite the texture before reading it.

Textures can share a placement with disjoint periods of GPU use. Reserve enough bytes and alignment
for every image, and reinitialize the incoming texture after writes through another alias. The caller
must synchronize prior storage accesses and make writes available with ordinary barriers or timeline
waits before initialization. Initialization orders its metadata work before subsequent texture accesses;
it does not issue a global memory barrier. Views and descriptors remain resident, and the backend has no alias registry.
See Vulkan's [memory aliasing rules](https://docs.vulkan.org/spec/latest/chapters/resources.html#resources-memory-aliasing).

## Root ABI

Each draw, mesh draw, and dispatch accepts a GPU root pointer. The backend pushes that eight-byte
address with `vkCmdPushDataEXT` on native heaps or `vkCmdPushConstants` on descriptor buffers;
it neither allocates storage nor copies the root structure. Only the address occupies push constants, so
16-bit root members are loaded from GPU memory independently of `DeviceCaps::storage_push_constant16`.
One root is shared by all active graphics stages. Roots may contain typed GPU pointers, texture and
sampler indices, and scalar, vector and matrix values.

Application-owned roots must be 16-byte aligned and remain live until GPU use finishes. For transient
CPU-written roots, use a `BumpAllocator` over mapped storage protected by a frame timeline. GPU shaders
can produce roots for later draws/dispatches; synchronize writes with `Access::shader_read` at the
consuming stages. That access includes uniform-buffer reads. See [the root contract](slang.md#root-and-pointer-layout).

Native heap pipelines use `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` and a null pipeline layout.
Descriptor-buffer pipelines use `VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT`, fixed internal descriptor layouts,
and descriptor-buffer binding/offset commands. Neither path allocates, updates or binds descriptor sets.

On native heaps, the shader's binding-zero uniform buffer maps directly to the address in push data through
[`VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT`](https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorSetAndBindingMappingEXT.html).
This preserves uniform-buffer access instead of physical-storage-buffer loads through a BDA pointer,
allowing the driver to use its constant-buffer optimizations and prefetch paths.
The descriptor-buffer variant loads the same root through a typed GPU pointer with the declared alignment.
BDA can receive equivalent optimizations, so any performance advantage depends on the GPU, driver and workload.
There is no root descriptor or descriptor-set binding. The shader ABI changes from a full push-data
structure to one address; rebuild existing Vulkan shader binaries. Separate roots per stage and
GPU-generated binding commands are not exposed. The GPU can select additional data through pointers stored inside the root.

Shared C++/Slang structures use C layout and row-major matrices. Slang 2026.14.1+ and
SPIRV-Tools 2026.3+ are required; see [the shader contract](slang.md).

## Resource-free barriers and unified image layout

`barrier()` combines consecutive global dependencies into one `VkMemoryBarrier2`, using the union of their stage/access masks.
This preserves dependency chains; separate entries in one Vulkan call would be independent. The combined dependency emits before
the next draw, dispatch, copy, render-pass boundary, timestamp or command-buffer end. Bindings do not split the batch, repeated masks
add no work, and each command buffer needs only one pending dependency. The acquire transition joins the next batch.
Texture metadata initialization flushes preceding dependencies before its transition because it can access aliased storage.
The call has no texture or buffer parameter. The application describes the real
producer/consumer hazard, while resource identity and image-layout bookkeeping disappear from the
public API.

Every ordinary texture use—sampling, storage access, attachment access, and copies—uses
`VK_IMAGE_LAYOUT_GENERAL`. `VK_KHR_unified_image_layouts`, when available, makes that choice
efficient rather than merely legal.

The backend privately handles the two unavoidable cases:

- `initialize_texture()` transitions a newly created or reused image from `UNDEFINED` to `GENERAL`;
- swapchain images transition between `GENERAL` and `PRESENT_SRC_KHR` around presentation.

Resource-free barriers do not imply automatic synchronization. Applications still issue a barrier
for actual read/write hazards and use timeline points for reuse of mutable CPU/GPU data.

## Pipelines, rendering, and mesh work

Graphics, mesh, and compute PSOs accept `ShaderStage` with a `.shader` shader package in byte-sized `code`,
and an explicit `entry_point` (default `main`). Vulkan ignores `threadgroup_size`; SPIR-V carries that metadata.
The [public shader build helper](slang.md#build-and-stage-artifacts) packages both binding variants from one source.
PSO creation selects the device's variant without runtime compilation or translation. Native heaps retain native SPIR-V;
descriptor-buffer devices use the separately compiled variant. Raw native-heap SPIR-V remains supported on native heaps.
Vulkan 1.4 maintenance5 lets pipeline
stages take inline `VkShaderModuleCreateInfo`, and descriptor-heap pipelines need no pipeline
layout. Graphics and mesh PSOs retain attachment compatibility, rasterization, and blend state;
viewport, scissor, and the public depth/stencil state are set independently on the command buffer.

Rendering uses `vkCmdBeginRendering` and `vkCmdEndRendering`. `RenderView` represents the persistent
image view needed for attachment use, but there is no render-pass or framebuffer object. Attachment
load/store state and clears are specified at the rendering call. Barriers remain explicit and are
not implied by rendering boundaries. Each `begin_render_pass()` sets a full render-area viewport and
scissor and disables depth/stencil, preventing state from leaking between passes. Applications call
`set_viewport()`, `set_scissor()`, or `set_depth_stencil()` after beginning a pass to override those defaults.
Bind a graphics or mesh PSO inside each pass before drawing, including resumed segments; relying on a
previous pass's pipeline binding is outside the API contract.
Rebind compute before dispatching after a render pass as well. The wrapper stores no selected PSO and never restores one;
only explicit `bind_pso()` calls emit `vkCmdBindPipeline`. Vulkan preserves native bindings across rendering boundaries,
so semantic invalidation does not clear native state or diagnose a missing bind.

`RenderingFlags::suspending` and `resuming` split one pass across independently recorded command buffers.
Each segment repeats the rendering description and has its own begin/end pair; only the first loads
or clears, and only the last stores. Submit the full chain in order in one batch, without action
commands, synchronization, or other passes between segments. Each buffer sets its own bindings.

The raster path has empty fixed vertex input because shaders fetch through GPU pointers. Mesh PSOs
use `VK_EXT_mesh_shader`, with task and mesh support enabled as part of the fixed device baseline.
`MeshPSODesc::task` adds a task stage before `mesh`; direct and indirect draw counts then launch
task workgroups, which cull or expand work through their payload and mesh dispatch. Empty task code
launches mesh workgroups directly. `Stage::task` names task-stage hazards in barriers. All graphics
stages share the same root ABI and descriptor heaps.

## Submission, presentation, and lifetime

`write_timestamp(commands, cpu_destination)` captures a 64-bit timestamp, defaulting to `Stage::all_commands`.
`DeviceDesc::timestamp_query_count` sets each command buffer's capacity and defaults to 256. Zero disables timestamps and their pool/storage allocation.
Timestamp calls are ignored and leave their destinations unchanged when disabled or when the command buffer's queue lacks 64-bit counters
or host query reset support. These capabilities do not restrict device or queue selection, and unsupported queues allocate no timestamp storage.
After every submission from a pool completes, call `read_timestamps(pool)` before resetting it. The backend batches
`vkGetQueryPoolResults` without `VK_QUERY_RESULT_WAIT_BIT` and writes the results to the supplied CPU destinations.
There are no timestamp GPU copies, resolve barriers, or additional waits. Destinations remain valid and distinct until retrieval.
Unread results are discarded on pool reset; unsubmitted buffers are ignored. Markers are valid inside each rendering segment,
but not between suspension and resumption. Multiply unsigned tick differences by `DeviceCaps::timestamp_period_ns` to obtain nanoseconds.
Timestamp storage follows command-pool reuse and destruction.

Command buffers are one-shot recording handles allocated from explicit command pools. End each buffer
before submitting any subset to a selected queue. Other pools can continue recording independently.
Reset a pool only after all its submitted work completes; reset discards unsubmitted work and invalidates
its command-buffer handles. Pools retain storage for reuse until destruction.

Device creation requests general, compute-only, and copy-only queue counts, each capped to its family's capacity.
A nonzero request requires a matching family; queue indices run general first, then compute, then copy.
`DeviceCaps` reports the actual counts. `create_command_pool(device, queue_index)` selects the recording family;
submit its command buffers only to queues in that family. Queue zero remains the general/presentation queue.
All buffers, textures, and swapchain images use concurrent sharing across the enabled families, so ownership
transfers are unnecessary. With only one family, Vulkan's exclusive mode already covers all its queues.
`submit(device, desc, queue_index)` selects a queue by index and defaults to zero.
Submission accepts timeline waits covering all command stages and signals the caller's completion point.
Use waits for cross-queue hazards; an ordinary barrier only synchronizes work on its own queue.
Prefer one signaling timeline per queue, or explicitly order signals to a shared timeline.

Copy-only texture transfers follow `DeviceCaps::copy_texture_granularity`; depth/stencil copies require a general queue.
`UploadQueue` accepts any queue index. Its tightly packed texture helper respects copy granularity, provided staging
fits one granularity block at the mip edge. Compute upload callbacks require a general or compute queue.

Each `(device, queue_index)` and command pool is externally synchronized, including command recording within the pool.
Distinct resource creation, immutable queries, timeline waits, and descriptor writes to disjoint slots
can run concurrently on one device. Texture creation records its `UNDEFINED` to `GENERAL` transition
in the supplied command buffer; every use must follow that initialization. Resource lifetime and
mutable utility allocators remain caller-synchronized. No internal queue, pool, or device-wide locks are used.

Public timeline semaphores are the application's reuse mechanism. Poll or wait for the point that
last used mutable upload data, readback storage, a texture placement, indirect argument memory, or a
descriptor slot before modifying or recycling it.

The optional utility `DeleteQueue` runs deferred callbacks once their nondecreasing application
timeline values complete. Applications normally tick it once per frame. At shutdown, call
`wait_idle()` and then drain the queue before destroying the device.

The backend does not recycle application allocator entries or descriptor slots; `wait_idle()` remains
an intentional whole-device drain.

For presentation, `acquire(commands)` returns a swapchain-owned `RenderView` and extent, or an empty frame
while the drawable extent is zero. Later command buffers in the same submission may also access the image.
Submit through `submit_and_present(device, desc)`, which always uses queue zero and transitions the image
back to presentation after all submitted command buffers.
Windowed device creation/destruction, drawable queries, acquire, and presentation stay on the native
message-pump thread; other work follows the threading rules above. Binary WSI semaphores remain private, while
`VK_KHR_swapchain_maintenance1` present fences support safe reuse and swapchain replacement without
draining unrelated queue work.
Core presentation reuses an image's render-finished semaphore after that image is acquired again. Its
resize/shutdown path uses device idle and has no maintenance present-completion fence.

`DeviceDesc::swapchain_color_space` defaults to `ColorSpace::srgb`. Windowed devices enable
`VK_EXT_swapchain_colorspace` when advertised, including devices starting in SDR. Extended output
requires that extension and an advertised format/color-space pair; use `Format::rgba16_float` with
`ColorSpace::extended_srgb_linear` for FP16 scRGB output. Unsupported requests fail device creation.

`set_swapchain_format()` changes the pair while retaining the device and application resources.
Call it on the presentation thread after `wait_idle()`, with no acquired frame. Unsupported pairs
leave the previous mode usable; a zero-size drawable defers recreation until its extent is nonzero.
Resize preserves the selected pair. The application owns display/HDR-state detection and must supply
pixels in the selected color space; the backend does not change OS display settings or tone-map output.

## Validation

The tests cover the public CPU-facing contracts, while the examples exercise representative GPU
paths. Debug builds enable Vulkan validation when it is installed. Runtime extension and feature
queries remain authoritative.

See [known driver issues](known-driver-issues.md) for observed driver-specific behavior and workarounds.

See [No Graphics API comparison](no-graphics-api-comparison.md) for the feature-by-feature assessment
of direct matches, Vulkan adaptations, and intentionally unsupported areas.

[descriptor-heap]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_heap.html
[untyped-pointers]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_untyped_pointers.html
[address-commands]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_device_address_commands.html
[unified-layouts]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_unified_image_layouts.html
[mesh-shader]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_mesh_shader.html
[swapchain-maintenance]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_swapchain_maintenance1.html
[spirv-heap]: https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_descriptor_heap.html
