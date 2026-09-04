# `NoGraphicsAPI` compared with *No Graphics API*

[`No Graphics API`](https://www.sebastianaaltonen.com/blog/no-graphics-api) is a design proposal
for a graphics API built only for modern bindless hardware. Its API listing is illustrative: device
creation is deliberately omitted, presentation is not covered, and several pieces describe an ideal
cross-vendor shader language rather than an existing API.

`NoGraphicsAPI` is a concrete Vulkan and Slang experiment based on the same data model. It implements
the parts that current Vulkan extensions can express cleanly, chooses a few different policies where
the new extensions offer a useful alternative, and leaves much of the proposed feature breadth out
of this small prototype. This document distinguishes those cases; it does not treat the blog's
pseudocode as a normative ABI.

## At a glance

| Area | Blog model | `NoGraphicsAPI` and rationale |
|---|---|---|
| Backend requirements | Device creation is deliberately omitted; the proposal assumes modern bindless hardware | Requires Vulkan 1.4 plus the device extensions `VK_EXT_descriptor_heap`, `VK_EXT_mesh_shader`, `VK_KHR_shader_untyped_pointers`, `VK_KHR_device_address_commands`, and `VK_KHR_unified_image_layouts`. Win32 presentation additionally uses `VK_KHR_surface`, `VK_KHR_win32_surface`, `VK_KHR_get_surface_capabilities2`, `VK_EXT_surface_maintenance1`, `VK_KHR_swapchain`, and `VK_EXT_swapchain_maintenance1`. |
| Linear data | C/C++ structures reached through 64-bit GPU pointers; no public buffer objects | Same model, implemented with Vulkan 1.2 core `bufferDeviceAddress` and the SPIR-V physical-storage-buffer model (`SPV_KHR_physical_storage_buffer`). Public value memory is `GpuAllocation<T>`, shader structures contain typed GPU pointers, and vertex data is fetched in the shader. |
| Memory result | `gpuMalloc()` returns a raw pointer; mapped allocations need a later host-to-device address translation | Returns `GpuAllocation<T>` with CPU/GPU/size plus opaque owner/token release bookkeeping. Caching both address domains at allocation time removes a translation API and command-side lookup structure; `VK_KHR_device_address_commands` needs the stored size for its address-and-size operands. |
| Root data | A GPU pointer to GPU-resident root data; graphics has independent vertex and pixel roots | Every draw or dispatch receives CPU root bytes through one overload taking `ByteSpan` by value. A valid root structure converts implicitly, storing its address and `sizeof(T)`; type-erased callers explicitly construct `ByteSpan{byte_ptr, byte_size}`, and rootless calls pass `{}`. The command copies the structure into `VK_EXT_descriptor_heap` push-data state immediately before the native command. Shaders read fields directly; pushing only a GPU pointer would add another load. The adaptation limits root size, shares the root across graphics stages, and cannot provide GPU-generated roots. |
| Static/specialization data | A static-constant structure, potentially containing GPU pointers, specializes a PSO | No specialization structure is exposed. Slang specialization declarations are scalar or enum constant-ID leaves, not one C-compatible structure ABI. Vulkan support would therefore require a separate per-stage ID/offset/size map and value block at PSO creation, making the application redeclare the shader's static-input signature outside its shared C++/Slang structure. That duplicate PSO input signature conflicts with the design goal of declaring the data shape once in shared code instead of restating it in the API. Pointer fields do not recover the structure model: each address would be a `uint64_t` leaf that shader code must explicitly reconstruct with `Ptr<T>(address)`. Stable scalar PSO constants can be added later if a concrete use justifies that extra PSO interface. |
| Texture descriptors | One application-managed texture heap containing opaque descriptors at application-selected indices | `VK_EXT_descriptor_heap` calls this a resource heap; `NoGraphicsAPI` exposes its image-only use as a texture heap with caller-selected indices and direct CPU writes. Buffer descriptors are unsupported: buffers use device addresses and never occupy this heap. Shader-side `SPV_EXT_descriptor_heap` provides native heap indexing and implicitly declares `SPV_KHR_untyped_pointers`, requiring `VK_KHR_shader_untyped_pointers` at runtime. Vulkan defines descriptor size, alignment, and reserved storage, so allocation and descriptor encoding use thin driver-facing helpers. Views select a compatible format plus mip and logical layer ranges. Because the extension cannot encode sampled multisample descriptors and MSAA is uncommon in modern rendering pipelines, the adaptation deliberately supports only single-sample textures and rendering. |
| Samplers | Metal-style sampler values embedded in shader code | `VK_EXT_descriptor_heap` supplies a separate application-managed sampler heap. Vulkan embedded samplers require pipeline-layout knowledge; an explicit heap preserves binding-free pipeline creation. |
| Texture storage | The caller queries size/alignment, allocates private GPU memory, and places a texture in it | `create_texture()` owns placement and suballocates internal 256 MiB image-memory heaps. This keeps Vulkan memory-type, binding, and fragmentation policy out of the public API; `VK_KHR_unified_image_layouts` keeps all later normal image uses in `GENERAL`. |
| PSOs | No binding layout or vertex layout; vertex/pixel and mesh/pixel PSOs retain only necessary raster state | `VK_EXT_descriptor_heap` and Vulkan 1.4 core `maintenance5` allow descriptor-heap pipelines with `layout = VK_NULL_HANDLE`; `VK_EXT_mesh_shader` supplies mesh/fragment pipelines, and Vulkan 1.3 core `dynamicRendering` avoids render-pass and framebuffer objects. Color-target format/blend/write-mask spans plus optional depth/stencil formats remain because Vulkan still requires fixed-function pipeline compatibility. |
| Barriers | Producer/consumer stages plus flags for exceptional descriptor, indirect-argument, and depth/stencil hazards; no resource lists | Vulkan 1.3 core `synchronization2` maps producer/consumer `Stage` and `Access` masks to one global `VkMemoryBarrier2`, while `VK_KHR_unified_image_layouts` removes public image-layout state. Explicit accesses express Vulkan's descriptor, indirect, index, mesh, and depth/stencil target scopes. |
| Command and resource lifetime | One-shot command buffers backed by resettable pools per frame in flight; public queues and timeline semaphores are sketched | Each one-shot handle has its own resettable command pool and primary command buffer in a growable ring. A private device timeline advances once per submitted batch and retires every context plus FIFO deferred-deletion entries; completed contexts are reset and reused, while a busy cursor makes the ring grow instead of wait. The caller owns a separate Vulkan 1.2 timeline semaphore and monotonically increasing values for application data reuse. The single queue and a statically bounded pool of independently rotating WSI contexts stay internal; `DeviceDesc::desired_swapchain_image_count` selects how many are active. |
| Indirect work | GPU pointers for arguments, roots, root arrays, and GPU draw count | `VK_KHR_device_address_commands` consumes `GpuRange {gpu, size}` for indexed, mesh, compute, and ordinary indirect arguments plus copy operands. Indirect arguments are GPU-addressed, but roots are still CPU-pushed and draw count is CPU-supplied. |

## Memory and address ranges

Both designs assume a CPU-visible, device-local default heap supplied by UMA or PCIe ReBAR and a
private GPU-only heap for textures and large data. The blog additionally describes a CPU-cached
readback class. `NoGraphicsAPI` names the three ordinary classes `MemoryType::cpu_visible`,
`gpu_only`, and `readback`; `texture_heap` and `sampler_heap` select descriptor-heap
allocations. The implementation only *prefers* host-cached memory for `readback`. Device creation
rejects hardware
without coherent, host-visible device-local memory and a separate non-host-visible device-local
heap. There is no host-only or non-coherent fallback and therefore no flush/invalidate path.

The blog keeps the base allocator minimal: a default allocation returns a mapped CPU address and
`gpuHostToDevicePointer()` obtains its GPU address. Its example bump allocator calls that
translation once and caches the pair. `NoGraphicsAPI` moves that cache into the base allocation result:

```cpp
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

This difference has three practical reasons:

- CPU and GPU virtual addresses are distinct, and almost every real caller needs both.
- No address-translation hash map or tree is needed in the API or backend.
- `VK_KHR_device_address_commands` consumes address-and-size ranges for copies, index binding, and
  indirect work. `GpuRange {gpu, size}` carries that exact input without reintroducing a buffer
  handle or looking up an allocation while recording commands.

`GpuAllocation` is a plain caller-owned allocation handle; its owner/token fields are opaque,
must be copied unchanged and passed to `gpu_free(allocation)` exactly once, and are intentionally omitted from `GpuRange`. `GpuRange` is a non-owning view of GPU
addressable bytes. Neither has RAII behavior, exposes a Vulkan handle, retains resources for a
command buffer, or provides CPU dereference semantics for `gpu`. `gpu_free(allocation)` takes the
unchanged aggregate. Shader
pointer dereferences remain raw and unbounded; a command-side `GpuRange` does not add bounds checks
to shader code. `gpu_free()` is O(1), and no page-record vector exists. Command recording never
reads either opaque field and there is no separate address-translation path.

Ordinary allocations are internally suballocated with OffsetAllocator from 256 MiB pages. Each
page has one fully bound `VkBuffer` carrying every usage needed for shader addresses, index input,
indirect arguments, and transfers. That buffer is solely Vulkan backing machinery: no buffer handle
or buffer class is exposed to the application. The fixed page size and universal usage flags are
implementation policy, not requirements from the blog. Each memory pool tries an active page first,
falls back to a linear scan only when that page cannot fit the request, promotes a found or freed
page, and creates a new page only when none fit.

One ordinary allocation, including its alignment padding, must fit a single 256 MiB page. An
oversized request is a programming error caught by an assertion rather than spanning pages. This
hard per-allocation limit is a current policy difference from the blog's unconstrained allocator
sketch.

Vulkan exposes memory property bits, not the blog's exact write-combined cache policy. Consequently
`MemoryType::cpu_visible` guarantees device-local, host-visible, and host-coherent memory but does not promise a
specific CPU cache mode. Memory allocation failure is fatal and deliberately unchecked;
`gpu_malloc()` does not return a nullable out-of-memory result. A successful `gpu_only` allocation
has `cpu == nullptr` and a valid `gpu` address.

## Root data

This is the most important semantic difference.

The blog stores root data in GPU memory and passes a 64-bit GPU pointer with a dispatch. A graphics
draw passes separate vertex and pixel root pointers, allowing the two shader stages to use
independent structures. The root itself has no API size limit, can be prepared by the GPU, and can
point to arbitrary additional data.

`NoGraphicsAPI` instead passes root data to each draw or dispatch from an ordinary CPU value through a
single overload taking `ByteSpan` by value. A valid root structure converts implicitly; the span stores the
value's address and `sizeof(T)`. The command uses `vkCmdPushDataEXT` to copy those bytes immediately
before the native draw or dispatch:

```cpp
CubeRootArguments root{.vertices = vertices.gpu};
gpu::draw(commands, root, vertex_count);
```

`vkCmdPushDataEXT` copies ordinary CPU bytes into push-data state while recording. Vulkan does not
provide a command to load that state directly from GPU memory. Pushing only a 64-bit GPU pointer
would add a shader indirection: the shader would first read the pointer from push data and then
fetch the root through it. Copying the complete root into push data avoids that extra indirection.

The root can be stack-local and need not survive the draw or dispatch call. Type-erased callers
explicitly construct `ByteSpan{byte_ptr, byte_size}`, and a rootless shader passes `{}`, which
records no push-data command. As a secondary consequence, small per-draw roots need
no mapped ring allocation or CPU-to-GPU address translation. GPU pointers stored *inside* the root
remain ordinary 64-bit shader pointers, and their pointees must remain alive through execution.

The tradeoffs are explicit:

- root size is limited by `DeviceCaps::max_push_data_size`;
- vertex and fragment shaders see the same root block;
- indirect draws still use CPU-provided root bytes recorded ahead of the indirect command; and
- the GPU cannot generate or select root data for the current command API.

Large arrays and structures therefore belong behind GPU pointers in a small pushed root. A future
GPU-driven path would need separate GPU-root-pointer commands rather than silently changing the
root argument semantics of the current commands.

## Texture and sampler heaps

The central descriptor model is the same: the application owns a contiguous heap, chooses compact
32-bit indices, writes opaque descriptor bytes into its selected slots, and explicitly makes a heap
active for commands. There are no descriptor sets, binding layouts, descriptor pools, descriptor
tables, or backend hash maps.

`NoGraphicsAPI` exposes that model as:

- `gpu_malloc()` with `MemoryType::texture_heap` or `MemoryType::sampler_heap` for directly
  mapped heap storage;
- `write_texture_descriptor()` and `write_sampler_descriptor()` for one caller-selected CPU
  destination;
- `set_texture_heap()` and `set_sampler_heap()` for explicit command-state binding; and
- `DeviceCaps::texture_descriptor_stride` and `sampler_descriptor_stride` for slot arithmetic, with
  the corresponding raw descriptor sizes retained separately.

`gpu_malloc()` routes the descriptor-heap memory types to dedicated backing because
`VK_EXT_descriptor_heap` imposes heap-specific maximum sizes, alignments, and an
implementation-reserved suffix. Each returned heap therefore has a dedicated backing buffer with
any needed prefix alignment slack rather than consuming a 256 MiB ordinary allocation page. Only
the requested user bytes are visible in `GpuAllocation`, so slot zero is exactly the returned
CPU/GPU byte in its respective address domain. Both byte-count and typed allocations are supported,
so applications may represent fixed descriptor layouts with their own structures. An explicit
heap alignment on the byte-count overload applies to the GPU address.

`set_texture_heap()` and `set_sampler_heap()` must receive the unchanged complete user range from
the matching `texture_heap` or `sampler_heap` allocation, normally through `gpu_range(heap)`.
The backend derives the hidden reserved suffix from that base and size without looking up the
allocation. Binding an interior or shortened range would make that derived suffix invalid and is
outside the API contract.

The blog's texture descriptor is presented as a fixed-size 256-bit API value containing
hardware-specific bytes, produced by either a CPU helper or a hypothetical shader intrinsic.
Vulkan descriptor sizes and contents are implementation-defined. The current library uses the
driver to materialize a descriptor directly at the supplied host address; it exposes no portable
shader-side descriptor-construction operation. This is a current Vulkan and Slang capability
boundary, not a reason to return to retained descriptor objects. The texture heap defines
image-sized slots and contains texture descriptors only. Buffers use device addresses and are not
stored in this heap.

`TextureDescriptorDesc` selects a descriptor format, mip range, and layer range.
`Format::undefined` inherits the texture's format and a zero count means every remaining mip or logical layer. Cube and
cube-array layer ranges count complete cubes rather than their six physical faces. Vulkan requires
alternate compatible view formats to be declared when the texture is created. The backend handles
that implementation constraint by automatically declaring every compatible public format; it does
not add a Vulkan-specific creation-time field to `TextureDesc`.

`NoGraphicsAPI` is deliberately single-sample. `VK_EXT_descriptor_heap` does not provide sampled
multisample image descriptors, so MSAA would require a special-case attachment and resolve path
outside the otherwise uniform descriptor-heap model. MSAA is not commonly used in modern rendering
pipelines, so the API omits the corresponding texture, rasterization, and resolve controls.

The post's shader examples use Metal-style embedded sampler values and its API sketch has no
sampler heap. Vulkan immutable samplers require sampler declarations to participate in pipeline
layout creation, which conflicts with binding-free, `layout = VK_NULL_HANDLE` pipelines.
`NoGraphicsAPI` consequently gives samplers their own explicitly bound heap. The cube uses the simplest
possible policy: its one nearest/clamp sampler occupies slot zero. The core does not precreate or
name sampler slots; applications remain free to define their own sampler vocabulary.

## Textures and uploads

The blog deliberately separates storage allocation from texture identity:
query texture size/alignment, allocate `MEMORY_GPU`, create a placed texture, make a descriptor,
then copy linear upload data into the private swizzled/compressed representation.

Vulkan 1.3 can query image requirements from create information through
[`vkGetDeviceImageMemoryRequirements`](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceImageMemoryRequirements.html),
so a placed-texture surface could be built. `NoGraphicsAPI` deliberately keeps that policy internal
instead: `create_texture()` creates the image, queries its requirements, and normally suballocates a
256 MiB GPU-only image-memory heap with OffsetAllocator. This hides memory-type compatibility,
dedicated-allocation requirements, binding, and image-heap fragmentation from applications. Image
heaps use the same active-first/fallback-scan policy per Vulkan memory type, and retiring a freed
texture promotes its page.
Dedicated memory is used only when Vulkan requires it or the image cannot fit a page.

Every current texture is represented by an opaque `Texture*`, including sampled and storage textures.
That handle owns the Vulkan image and its allocation, carries the information needed for descriptor
generation and copies, and provides an explicit destruction point. It is never visible to a shader.
Sampled/storage descriptors are generated directly from texture creation metadata. Attachment use
instead creates a persistent `RenderView*` for one mip and physical slice. That view owns the
corresponding Vulkan `VkImageView` and can be reused across command buffers. Its parent texture's
live-view counter makes `destroy_texture()` terminate in every build until all of its views have been
logically destroyed. Once all referring command buffers have been submitted, destruction can be
requested immediately: the private device timeline defers the Vulkan view, image, and allocation
releases until GPU completion. Retaining `Texture*` for non-attachments is a current lifetime/API
choice rather than a fundamental requirement of the bindless model.

The upload path otherwise follows the post:

1. Allocate `MemoryType::cpu_visible` memory and write linear texels through `cpu`.
2. Create a GPU-only, optimal-tiled texture.
3. Call `copy_memory_to_texture()` with the upload allocation's `GpuRange`.
4. Issue a resource-free barrier before the texture's consumer.

`TextureType` exposes the blog's six shapes: `one_d`, `two_d`, `three_d`, `cube`,
`two_d_array`, and `cube_array`. `TextureDesc` also exposes mip and layer counts.
For cube types, `layer_count` counts logical cubes; the backend expands each cube to six Vulkan
array layers in +X, -X, +Y, -Y, +Z, -Z order.

The blog's prototype copy signatures do not define subresource regions or memory pitches.
`TextureCopyDesc` supplies those direct copy-command inputs: one mip, a texel offset and extent,
byte row and slice pitches, and a range of physical array slices. Cube uploads therefore address the
six faces directly through `base_slice` and `slice_count`. Zero extent, slice-count, or pitch fields
select the remaining subresource or tightly packed memory defaults. The `GpuRange` itself selects
the memory address, so an interior range expresses a byte offset without another descriptor field.
The core deliberately does not add a whole-chain mip construction command; applications can upload
individual levels directly, and higher-level generation policy belongs in an application or utility
layer.

`VK_KHR_unified_image_layouts` lets every normal image operation use
`VK_IMAGE_LAYOUT_GENERAL`, so no layout or image state appears in the public API. Vulkan still
requires a new image to start in `UNDEFINED`. The next `begin_commands()` batches that one-time
metadata initialization to `GENERAL`; it is not a user-visible usage transition. Swapchain images
are the sole exception: the backend privately transitions them between `GENERAL` and
`PRESENT_SRC_KHR` around presentation.

## PSOs and fixed-function rendering

Both designs remove shader binding declarations and vertex input layouts. A PSO does not know
about root structure fields, GPU pointer pointees, descriptor slots, or sampler slots. The current
Vulkan backend creates descriptor-heap pipelines with `layout = VK_NULL_HANDLE`, and the cube loads
vertices through a typed GPU pointer in Slang. The triangle's traditional vertex shader
emits three constant vertices from `SV_VertexID` without a vertex allocation. MRT/deferred
coverage lives in the separate `deferred_renderer` example.

`NoGraphicsAPI` still returns opaque `PSO*` handles because Vulkan must compile and retain
`VkPipeline` objects. `GraphicsPSODesc` carries vertex/fragment SPIR-V, cull winding, depth
bias, depth/stencil state, a span of per-target ordinary non-constant
blend/write-mask values, and optional depth and stencil formats. Graphics input assembly is
fixed to triangle-list with primitive restart disabled; rasterization is fixed to fill mode and line
width 1. `MeshPSODesc` replaces the vertex stage and input assembly with mesh SPIR-V that must
output triangles while retaining the remaining fixed-function and attachment state.
`create_compute_pso()` takes compute SPIR-V directly rather than
wrapping one span in a descriptor. PSO creation is synchronous and the prototype has no public
specialization-constant input or pipeline cache.

This covers the blog's vertex/pixel and mesh/pixel pipeline split, multiple color targets, and the
minimal immutable triangle-raster PSO state used by `NoGraphicsAPI`. The blog represents
depth/stencil and blend state as optional opaque objects; `NoGraphicsAPI` embeds plain
`DepthStencilState` and per-target `BlendState` values directly in each raster PSO descriptor.
Stencil read/write masks are shared, its compare/operations/reference remain per-face, and nonzero
depth-bias values enable baked bias. Only
viewport and scissor remain dynamic. Separate reusable blend objects and framebuffer-fetch paths
remain outside the current surface.

`RenderingDesc` mirrors the blog's render-pass description with a color-attachment span plus
optional depth and stencil entries. Each entry contains its texture, independent load/store
operations, and aspect-appropriate clear value. Vulkan dynamic rendering requires simultaneous
depth and stencil entries to name the same combined-aspect texture. Render-scope boundaries do not
implicitly solve application data hazards.

Other parts of the blog's broader surface remain outside this prototype:

- static-constant structures, including constants containing GPU pointers;
- task shaders, separate blend state, and shader framebuffer fetch;
- alternate primitive topologies, primitive restart, non-fill/line/point rasterization, depth clamp,
  rasterizer discard, depth bounds, sample shading/masks, alpha-to-coverage/one, logic operations,
  blend constants, constant blend factors, dual-source blending, and richer raster state beyond the
  current cull, color mask, and depth/stencil values; and
- separate vertex/mesh and pixel root pointers.

## Barriers and image writes

The core idea is preserved: public barriers contain no buffer list, texture list, subresource list,
or image layout transition. `barrier()` emits one global `VkMemoryBarrier2` and supplies zero Vulkan
buffer and image barriers. It synchronizes memory reached through GPU pointers, descriptor indices,
and image descriptors alike. The same call therefore covers buffer writes and image writes.

The blog uses producer/consumer stages plus exceptional hazard flags for descriptor-cache,
command-processor-prefetch, and depth-cache cases. `NoGraphicsAPI` maps the same intent more directly to
Vulkan synchronization2 by asking for both a `Stage` and an `Access` mask on each side:

```cpp
gpu::barrier(commands,
             gpu::Stage::compute,
             gpu::Access::shader_write,
             gpu::Stage::fragment,
             gpu::Access::shader_read);
```

Special consumers are expressed without resource objects:

- `Access::descriptor_read` maps to Vulkan resource-heap and sampler-heap read access;
- `Stage::indirect` plus `Access::indirect_read` targets indirect arguments and command-processor
  consumption;
- `Stage::index_input` plus `Access::index_read` globally targets index-input accesses; the index
  range itself is supplied separately to the draw;
- `Stage::mesh` covers mesh-shader reads and writes; and
- depth/stencil stage/access values cover both fixed-function aspects; and
- `Stage::host` with `Access::host_read` is destination-only for GPU-to-CPU readback. Coherent host
  writes are made available to GPU work by queue submission and need no public barrier source.

The index-input access is a concrete Vulkan extension of the blog's exceptional-consumer idea; the
post's named hazard flags do not include an index-buffer flag. Explicit source and destination
accesses make Vulkan validation precise and avoid a second library-specific hazard vocabulary, at
the cost of a slightly more verbose call.

A global dependency can order unrelated matching accesses, but it also avoids CPU-side resource
tracking and reflects the queue/stage granularity advocated by the post. An optional address-range
barrier could add precision later without making resource lists the default model.

The blog also sketches split GPU signal/wait commands operating on GPU addresses. Those are not
implemented. `NoGraphicsAPI` currently exposes only the unsplit global barrier inside a command buffer.

## Command recording, submission, and indirect work

`begin_commands()` returns a one-shot `CommandBuffer*`. Its context owns one transient command
pool and one primary Vulkan command buffer. The device starts with two contexts in a ring. When the
cursor context has completed, its pool is reset and the buffer is reused. If it is still recording or
executing, the ring grows by one context instead of waiting. Contexts and submit-info capacity remain
at that high-water mark. Starting a command buffer does not create a descriptor pool.

`submit()` and `submit_and_present()` take a nonempty span containing every command buffer begun
since the preceding submission exactly once. The span itself defines execution order. They end all
buffers and use one `vkQueueSubmit2` call with a work batch followed by an empty private-retirement batch,
consuming every submitted handle. The second batch orders retirement after all work-batch signals;
signals listed within a single batch would be unordered.
There is no separate command-buffer destruction or abandonment operation. Work can begin before
swapchain acquisition: `deferred_renderer` records and submits its G-buffer first, acquires late, then
records and presents a second drawable command buffer.

The caller creates a `TimelineSemaphore`, increments its own counter, and passes a `TimelinePoint`
to every submission or presentation batch. NoGraphicsAPI never retains or waits the caller's point for
internal command-context reuse. The device instead owns one private timeline semaphore. It advances
once per batch, and every command context in that batch receives the same retirement value. The
interactive examples use the explicit extent, acquire, command-recording, and presentation
operations as needed; none waits for device idle per frame. The core does not add a combined
synchronous convenience operation.

The point's semaphore must belong to the submitted command buffers' device, and its value must
strictly increase. Texture descriptor writes additionally validate texture/device ownership.

This realizes the blog's transient-command and explicit GPU-to-CPU timeline recommendation while
still hiding `GpuQueue`. `timeline_completed_value()` exposes a nonblocking progress query and
`wait_timeline()` blocks for one known submitted point, allowing applications to manage mutable data
and descriptor-slot reuse. Vulkan object destruction and allocation reuse instead use the private
command-retirement queue. The current implementation has one combined graphics/compute queue, one
all-live command-buffer batch at a time, a growable command ring, and single-threaded access. Public
queues, multiple queues, partial submissions of currently live command buffers, and GPU-to-GPU split
waits remain outside the first implementation.

Index binding, indirect draw/dispatch arguments, and memory-side copy operands use 64-bit GPU
addresses through `VK_KHR_device_address_commands`. They are `GpuRange` values because Vulkan needs
both address and extent. Recording passes them directly to Vulkan and performs no raw-pointer to
allocation lookup.

The current indirect functions can execute a CPU-specified count and stride from a GPU argument
range. They do not implement the blog's GPU-resident root arrays, independent vertex/pixel root
strides, GPU draw-count pointer, or GPU-generated root selection. In other words, indirect command
parameters are address-based today, but root transport remains the CPU push-data model described
above.

## C++ surface, errors, and lifetime

The blog presents a C-like sketch. `NoGraphicsAPI` keeps that character as free functions and opaque raw
pointer handles in namespace `gpu`, but it is a C++20 interface rather than a C ABI. Small templates
provide typed allocations and infer root size; `Span<T>` carries pointer/count arrays. Public
descriptor structures provide useful defaults and callers use designated initializers for only
non-default fields. The public surface has no ownership classes, inheritance, virtual functions,
PIMPL wrappers, exceptions, RTTI, mutexes, or atomic counters.

The post does not specify device-init errors, allocation failure, thread safety, or complete object
destruction. `NoGraphicsAPI` fills those gaps with project policy:

- `create_device()` accepts an optional native window and swapchain format and returns one
  handle/error aggregate because initialization can reasonably discover unsupported hardware or
  window-system compatibility;
- memory allocation failure and object creation failures after initialization are unrecoverable;
- programming errors use assertions, while normal post-init Vulkan operations assume success;
- memory allocation failures are deliberately unchecked;
- callers explicitly destroy handles after submitting their last referencing command buffer and
  while no command buffer is recording; backing Vulkan objects and allocation reuse are deferred
  internally, while application-owned descriptor bytes must remain unchanged or move to a fresh
  heap generation; and
- the first implementation is single-threaded and performs no internal locking.

## Deliberate scope limits

The following blog concepts are not in the current API: user-placed textures, GPU-generated
descriptors, static constant structures, task shaders, separate blend objects, framebuffer fetch,
split GPU-address signal/wait, GPU-root indirect draws, GPU draw-count pointers, public
queues/semaphores, and capture/replay allocation at a prescribed GPU virtual address.

This list describes prototype breadth, not a decision to restore legacy Vulkan abstractions. New
features should preserve the established invariants: no public buffer objects, no binding layouts,
no descriptor ownership hidden in the backend, no resource lists in normal barriers, and no legacy
hardware fallbacks.

The blog does not define presentation. `NoGraphicsAPI` adds a deliberately small Win32 WSI boundary.
Passing a native window and swapchain format to `create_device()` makes presentation state owned by
that device. `get_drawable_extent()` reports the current size without acquiring, `acquire()` returns
a swapchain-owned `RenderView*` plus its extent, and `submit_and_present()` submits the current
command-buffer batch. There is no public swapchain handle.
Acquisition remains separate from command recording: an application can query and rebuild
extent-dependent resources first, then record independent offscreen work before acquiring late. The
queried size is a snapshot and the acquired frame extent is authoritative if the surface changes in
between.
Binary semaphores are required by Vulkan acquire/present and remain internal. The device stores up
to eight presentation contexts, while `DeviceDesc::desired_swapchain_image_count` selects the active
count and defaults to two. Each context owns acquire/rendered semaphores and a per-present
maintenance fence. The same value is used as the best-effort swapchain image-count request after
clamping to the FIFO-specific surface limits; Vulkan may create additional images, though an actual
count above the backend capacity of eight is unsupported. Acquisition polls completed fences and
waits before reusing the next context in the active ring.
It requires a non-zero drawable extent and otherwise always returns a populated frame.
Replacement creates the new swapchain immediately with
the current generation as `oldSwapchain`; the retired generation remains alive until its present
fences signal. Drawable acquisition waits for an available image. Only
`acquire()` and `submit_and_present()` advance that independent ring, permitting safe WSI reuse,
recreation, and deferred destruction without coupling its cursor to command-context selection.
Caller-supplied timeline points are exclusively
application-visible completion markers. The examples retain their latest point; `deferred_renderer`
stores the exact G-buffer submission value for each of its two application data ranges, polls for a
completed range, and skips a loop iteration rather than waiting when both remain in flight. Other
platform WSI is still outside the current prototype.

`submit_and_present()` inserts no completion wait. `acquire()` may wait for a presentation context
or swapchain image, and Vulkan also permits `vkQueuePresentKHR` itself to block for a finite
implementation-dependent interval.

Acquire-side out-of-date recreates and retries within the same call. Present-side out-of-date marks
the swapchain stale, and the next `acquire()` recreates it before obtaining an image without
returning to the caller. Suboptimal requests recreation when the surface extent,
transform, or selected composite-alpha mode changed, avoiding recurring same-configuration rebuilds.
Cube sizes its depth attachment from the acquired frame. `deferred_renderer` replaces its G-buffer and
descriptor-heap generation from the earlier extent snapshot, records and submits the offscreen pass,
and only then acquires; its final pass rescales G-buffer pixel coordinates if the acquired extent
changed meanwhile. The old generations are retired by deferred deletion rather than an application
timeline wait.

## Related documentation

- [`NoGraphicsAPI.hpp`](../include/NoGraphicsAPI/NoGraphicsAPI.hpp) is the current public API.
- [Vulkan support and design mapping](vulkan-support.md) records exact extension contracts and
  backend behavior.
- [Slang integration and ABI](slang.md) records shader compilation, descriptor-heap syntax, shared
  structure layout, and pointer rules.
- The [triangle](../examples/triangle/triangle.cpp) is the deliberately trivial
  vertex/fragment sample, while the [cube](../examples/cube/cube.cpp) demonstrates typed GPU
  pointers plus application-managed texture and sampler heaps.
- [`deferred_renderer`](../examples/deferred_renderer/deferred_renderer.cpp) updates 1,048,576
  GPU-addressed object matrices and renders the cubes from GPU vertex/index data with one indexed
  instanced draw into two color targets plus depth, followed by deferred lighting that reconstructs
  world position from hardware depth.
