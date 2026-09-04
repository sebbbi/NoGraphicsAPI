# Metal 4 porting design

Status: proposed and unimplemented. This document records the design reviewed on 2026-09-01.
It is not a claim that NoGraphicsAPI currently supports Metal.

The current implementation remains Vulkan-only. This document defines the smallest public API
change and the backend contracts required for a Metal 4 implementation that preserves the
low-level model of NoGraphicsAPI. Claims are classified as follows:

- **Documented** means the behavior follows an Apple or Slang document linked below.
- **Proposed** means it is the NoGraphicsAPI backend contract selected by this design.
- **Needs validation** means the behavior must be tested on shipping Apple GPU families before
  Metal can be advertised as supported.

The public API described here is the API in
[`include/NoGraphicsAPI/NoGraphicsAPI.hpp`](../include/NoGraphicsAPI/NoGraphicsAPI.hpp) at the review date. The
Metal backend and the public changes proposed by this document do not exist yet.

## Runtime and architecture requirements

The proposed backend uses the Metal 4 core API: `MTL4CommandQueue`, `MTL4CommandBuffer`,
`MTL4CommandAllocator`, Metal 4 render and compute command encoders, resource view pools,
residency sets, and Metal 4 barriers. It does not target the legacy Metal command-buffer API.

Apple documents the Metal 4 core feature set from Apple GPU family 7. That is the proposed
baseline for ordinary draw, compute, copy, view-pool, argument-table, allocator, and barrier
functionality. Individual formats and commands still have stricter family gates:

- `draw_meshlets_indirect` requires Apple GPU family 9. There is no faithful GPU-only fallback
  for the public operation on Apple 7 or Apple 8.
- BC texture compression support is family-dependent and is not part of the Apple 7 baseline.
- Every compressed, packed, and depth/stencil format remains subject to
  `supports_texture_format`.

The initial backend therefore has two viable device policies. It can require Apple 9 to preserve
the entire command surface, or it can expose a mesh-indirect capability and reject that operation
on Apple 7 and Apple 8. The first implementation should use the Apple 9 gate unless a public
capability is added; silently reading indirect arguments on the CPU is not equivalent.

The public header currently rejects all non-x86-64 builds. A Metal port must replace that guard
with an explicitly supported 64-bit Apple target while retaining the existing 64-bit address
assumptions. CPU-side helper code that assumes AVX2 also needs an ARM64 implementation or a
scalar fallback. Neither change is evidence of Metal support by itself.

## Public API compatibility summary

Most public concepts can be kept. Two public representations cannot: a texture descriptor heap
cannot remain an application-visible byte allocation, and a PSO cannot continue to accept
only SPIR-V.

| Public category | Metal 4 decision | Public API effect |
| --- | --- | --- |
| Opaque device, timeline, swapchain, texture, view, PSO, and command handles | Keep | None |
| `Span`, `Error`, `GpuRange`, `GpuAllocation`, and `TimelinePoint` | Keep | None |
| Device creation, capability query, and format query | Keep | Add Metal-specific validation; revise descriptor-size capabilities |
| Ordinary buffer allocation from `cpu_visible`, `gpu_only`, and `readback` heaps | Keep | None |
| Raw `texture_heap` allocation | Replace | Add an opaque, capacity-sized `TextureHeap` with indexed write/copy/bind operations |
| Logical sampler heap and `write_sampler_descriptor` | Keep | Back with a Tier-2 `MTLBuffer` argument buffer containing direct 64-bit sampler resource IDs |
| Texture creation, destruction, render views, and texture copies | Keep | Metal-specific alignment and format validation |
| Render and compute PSO descriptions | Change | Accept backend-neutral shader bundles rather than SPIR-V-only fields |
| Fixed raster, depth/stencil, blend, and attachment state | Keep | None |
| Per-command root arguments and the user root-data structure | Keep exactly | Snapshot each root into hidden Metal slot 0 |
| Direct, indexed, indirect, mesh, and compute commands | Keep | Apple 9 gate for indirect mesh |
| Global barrier API | Keep | Conservative Metal stage lowering; barriers remain illegal during rendering |
| Command batches, grouped submit, timeline signaling, and submit-and-present with a command-buffer batch | Keep | Metal queue and drawable mapping |
| Current omissions and single-threaded host contract | Keep | No new queue, ray-tracing, sparse, or threading surface |

The proposed texture-heap API is described below. No compatibility shim can make arbitrary GPU
stores to the old texture descriptor bytes update an `MTLTextureViewPool`; applications that
generate or copy texture descriptors on the GPU need an explicitly optional indirection path,
not the direct heap path specified here.

## Device, errors, capabilities, and formats

`create_device`, `destroy_device`, `get_device_caps`, and `supports_texture_format` retain their
roles. The backend selects one Metal device and one Metal 4 command queue.

The existing error model also remains:

- `create_device` reports recoverable device and optional window-system initialization errors
  through `Error`.
- The complete public error values remain `none`, `unsupported`, `device_lost`, and
  `driver_error`.
- Invalid descriptors, unsupported command use, allocator misuse, and recording-state errors are
  programming errors and follow the current assertion/fatal-error model.
- Metal object creation failures that can occur after device creation need the same explicit
  policy as the Vulkan backend; the port must not silently return partially initialized handles.

`DeviceCaps` maps as follows:

| Current capability | Proposed Metal meaning |
| --- | --- |
| `device_name` | `MTLDevice.name` stored with device lifetime |
| `max_push_data_size` | Maximum exact root-data byte count supported by the slot-0 ring ABI |
| `texture_descriptor_size` | Obsolete for an opaque `TextureHeap` |
| `texture_descriptor_stride` | Obsolete for an opaque `TextureHeap`; indices are logical slots |
| `sampler_descriptor_size` | Eight-byte direct sampler `gpuResourceID` entry |
| `sampler_descriptor_stride` | Eight-byte logical slot stride, subject to argument-buffer ABI validation |
| `texture_compression_bc` | True only when the selected family and format table support BC |
| `texture_compression_astc` | True only when the selected family and format table support ASTC |
| `storage_input_output16` | True only when the shader compiler and selected GPU support the required 16-bit storage behavior |

The two obsolete texture-descriptor fields should be removed or deprecated when `TextureHeap`
becomes opaque. A replacement maximum-capacity field is useful only if Metal reports a practical
view-pool limit that applications must plan around.

`supports_texture_format(device, format, usage)` remains authoritative for the public format/usage
combination. It validates storage access, filterability, renderability, compression family, and
depth/stencil role, but its signature has no texture type or dimensions. `create_texture`, view
creation, and texture-copy validation separately enforce type, dimensions, mip/layer bounds, and
subresource restrictions.

### Format coverage

The public `Format` enum contains the following complete families:

| NoGraphicsAPI formats | Proposed Metal treatment |
| --- | --- |
| `r8_srgb`, `rg8_srgb`, `rgba8_srgb`, `bgra8_srgb` | Map to the corresponding sRGB pixel format where supported |
| `rgba4_unorm`, `r5g5b5a1_unorm`, `r5g6b5_unorm` | Map only after channel-order and feature validation; otherwise reject |
| `r8_unorm`, `rg8_unorm`, `rgba8_unorm`, `bgra8_unorm`, `r16_unorm`, `rg16_unorm`, `rgba16_unorm` | Direct unorm-family mapping |
| `r8_uint`, `rg8_uint`, `rgba8_uint`, `r16_uint`, `rg16_uint`, `rgba16_uint`, `r32_uint`, `rg32_uint`, `rgba32_uint` | Direct unsigned-integer-family mapping |
| `bgra8_uint` | Metal has no direct BGRA8 unsigned-integer pixel format; reject unless a swizzled representation is validated for every declared use |
| `rgb32_uint` | No assumed portable three-channel texture mapping; reject unless validated |
| `r16_float`, `rg16_float`, `rgba16_float`, `r32_float`, `rg32_float`, `rgba32_float` | Direct floating-point-family mapping |
| `rgb32_float` | No assumed portable three-channel texture mapping; reject unless validated |
| `rgb10a2_unorm`, `rg11b10_float` | Map to the corresponding packed Metal format after component-order validation |
| `d16_unorm`, `d32_float`, `s8_uint`, `d32_float_s8_uint` | Map to corresponding depth/stencil formats subject to attachment and sampling support |
| `d24_unorm_s8_uint` | Capability-dependent; do not assume Apple-silicon support |
| `eac_rg` | Map to EAC RG when the selected family supports it |
| `astc_4x4_srgb`, `astc_4x4_unorm` | Map to ASTC 4x4 when supported |
| `bc3_srgb`, `bc3_unorm`, `bc5_rg`, `bc7_srgb`, `bc7_unorm` | Map only on families that expose the corresponding BC format |
| `undefined` | Invalid for texture creation |

This table is a mapping plan, not validation evidence. Packed 16-bit component order, depth/stencil
sampling, sRGB single- and dual-channel support, compressed copy pitches, and every
render-target/storage combination require device tests.

The constexpr `get_texture_format_info` helper, including block width, block height, byte count,
and depth/stencil flags, remains backend-neutral and unchanged.

### Texture types and usage

All current `TextureType` values remain in scope:

- `one_d`
- `two_d`
- `three_d`
- `cube`
- `two_d_array`
- `cube_array`

Metal texture types and array lengths provide direct representations. Cube-layer interpretation,
3D view restrictions, mip/layer bounds, and compressed-dimension rules need validation in the
common descriptor validator.

All current `TextureUsage` bits remain:

- `none`
- `sampled`
- `storage`
- `color_attachment`
- `depth_stencil_attachment`
- `transfer_source`
- `transfer_destination`

The four non-`none` shader/render usages map to Metal shader-read, shader-write, and render-target
usage as applicable.
Metal does not require a distinct creation-usage bit for every blit role, but NoGraphicsAPI must keep
the transfer bits for API validation and backend-independent intent. A texture descriptor must
not gain capabilities absent from its declared usage merely because Metal permits an operation.

The current API creates single-sample textures only. A Metal port does not add multisampling,
resolve attachments, planar/video formats, or texture buffers.

## Public operation mapping

The following table covers the current public operations and the proposed Metal implementation.
“Keep” means the C++ operation can retain its public shape unless another section explicitly
proposes a replacement.

| NoGraphicsAPI operation | Proposed Metal 4 implementation | Status |
| --- | --- | --- |
| `create_device` / `destroy_device` | Select `MTLDevice`, create one `MTL4CommandQueue`, allocators, residency set, caches, and backing heaps; when `DeviceDesc::window` is supplied, associate its `CAMetalLayer` and own the drawable state for the device lifetime | Proposed |
| `get_device_caps` | Query device/family/compiler behavior; publish only validated capabilities | Proposed |
| `supports_texture_format` | Table plus device-family and usage validation | Proposed |
| `get_texture_format_info` | Retain the constexpr format/block metadata table | Keep |
| `create_timeline_semaphore` / `destroy_timeline_semaphore` | Own an `MTLSharedEvent`, initialize `signaledValue`, and retain notification state | Proposed |
| `timeline_completed_value` | Read `MTLSharedEvent.signaledValue` | Documented mapping |
| `wait_timeline` | Register an event notification and block the host wait primitive until the requested value | Proposed |
| `get_drawable_extent` | Read `CAMetalLayer.drawableSize` without acquiring a drawable, allowing resolution-dependent offscreen work to be submitted first | Proposed |
| `acquire` | Wait for `nextDrawable` from the device-owned layer state; return a `SwapchainFrame` with its internal `RenderView`, width, and height | Proposed |
| `submit_and_present` | Finalize/group its command-buffer span, signal the timeline point, and present the acquired drawable | Proposed; needs validation |
| `gpu_malloc` | Suballocate ordinary buffers from placed `MTLBuffer` backing heaps; allocate sampler heaps as Tier-2 argument-buffer storage | Proposed |
| `gpu_free` | Invalidate the public allocation immediately and defer physical heap reuse on the backend retirement queue | Proposed |
| `gpu_range` | Preserve the allocation GPU pointer and byte size in the plain `GpuRange` aggregate | Keep |
| `create_texture` / `destroy_texture` | Create/destroy a placed `MTLTexture` at an explicit `MTLHeap` offset | Proposed |
| `create_render_view` / `destroy_render_view` | Retain view metadata and, when required, a Metal texture view | Proposed |
| `write_texture_descriptor` | Replace raw destination pointer with `TextureHeap` plus slot index and call the view pool | Public change |
| `copy_texture_descriptors` | Copy an explicit slot range between opaque texture heaps/view pools | New public operation |
| `write_sampler_descriptor` | Cache/retain `MTLSamplerState` and CPU-write its direct 64-bit `gpuResourceID` into one slot | Keep |
| `create_graphics_pso` / `create_mesh_pso` | Build Metal render pipeline and depth/stencil state from backend-neutral shader bundles and fixed state | Public shader-input change |
| `create_compute_pso` | Build Metal compute pipeline; retain local workgroup dimensions in the NoGraphicsAPI PSO | Public shader-input change |
| `destroy_pso` | Invalidate the public PSO immediately, then remove its allocation from residency and release it after backend retirement | Proposed |
| `begin_commands` | Reset one retired `MTL4CommandAllocator`, reuse a device-created `MTL4CommandBuffer`, and begin it with that allocator | Proposed |
| `bind_pso` | Update lazy graphics, mesh, or compute pipeline state; reapply when an encoder is created | Proposed |
| `set_texture_heap` | Bind one opaque texture view pool and publish its base resource ID in hidden slot 1 | Public signature change |
| `set_sampler_heap` | Bind the logical Tier-2 argument-buffer range as the typed slot-2 array | Keep |
| Root argument on every `draw*` / `dispatch*` command | Snapshot the exact user bytes into an internal shared-memory ring and bind them at slot 0 immediately before the command | Keep exactly |
| `begin_render_pass` / `end_render_pass` | Create/end an `MTL4RenderCommandEncoder` with attachment load/store/clear state | Proposed |
| `draw` | Direct non-indexed triangle draw | Direct mapping |
| `draw_indexed` | Pass the index data as raw `MTLGPUAddress`; apply `first_index` to that address | Direct mapping |
| `draw_indirect` | Pass raw argument `MTLGPUAddress`; emit `draw_count` commands using the public stride | Direct mapping |
| `draw_indexed_indirect` | Pass raw argument and index `MTLGPUAddress` values; emit `draw_count` commands | Direct mapping |
| `draw_meshlets` | Direct mesh-grid draw on supported Metal GPU families | Direct mapping |
| `draw_meshlets_indirect` | Indirect mesh draw using raw `MTLGPUAddress`; require Apple 9 | Documented family gate |
| `dispatch` | Dispatch the public group counts with workgroup size retained by the compute PSO | Direct mapping |
| `dispatch_indirect` | Pass the argument as raw `MTLGPUAddress` | Direct mapping |
| `copy_memory` | Resolve each ordinary `GpuRange` to `MTLBuffer` plus offset, then encode the native buffer copy | Proposed |
| `copy_memory_to_texture` / `copy_texture_to_memory` | Resolve the buffer range to `MTLBuffer` plus offset and encode the native buffer/texture copy | Proposed |
| `barrier` | Encode a producer queue barrier and split an active non-render encoder; use conservative visibility | Proposed |
| `submit` | Finalize and commit the ordered command-buffer span as one queue group, then signal the requested timeline point | Proposed |
| `wait_idle` | Wait for a queue completion event/value covering all prior groups | Proposed |

The current API has separate direct, indexed, indirect, mesh, and dispatch variants. The backend
must preserve their exact public argument layouts, draw counts, and strides; it must not translate
an indirect operation into a host readback.

## Ordinary memory, placement heaps, and GPU addresses

All NoGraphicsAPI-created buffers and textures use placement heaps in the proposed backend. There are
no standalone user resources.

### Buffer backing heaps

Each ordinary memory class owns one or more large placed `MTLHeap` objects and explicit
suballocations:

- `cpu_visible` uses host-visible buffer storage suitable for upload and root-data staging.
- `gpu_only` uses private GPU storage.
- `readback` uses host-visible storage suitable for GPU-to-host results.
- `sampler_heap` is a Tier-2 `MTLBuffer` argument buffer containing direct sampler resource IDs.
- `texture_heap` is removed from `gpu_malloc`; texture descriptors are allocated through opaque
  `TextureHeap` objects.

The intended backing range size is 256 MiB. A normal process is expected to have fewer than
roughly 50 live ordinary backing ranges, but that number is an implementation expectation, not an
API limit. The allocator may grow by adding placement heaps and buffers.

`GpuAllocation::gpu` and `GpuRange::gpu` continue to be real Metal GPU addresses for ordinary
buffer data. This preserves pointer arithmetic in user root structures and shaders.
`GpuAllocation::cpu` remains non-null only for host-visible memory classes. The owner/token fields
retain their current lifetime-validation purpose.

### Address-to-buffer lookup for copies

Metal 4 indexed draws and indirect draw/dispatch entry points accept `MTLGPUAddress` directly.
They do not require a buffer-object lookup.

Native Metal buffer copy methods instead accept `MTLBuffer` plus byte offset. Therefore
`copy_memory`, `copy_memory_to_texture`, and `copy_texture_to_memory` perform a backend lookup for
each buffer operand:

1. Find the live ordinary backing-buffer interval containing the complete `GpuRange`.
2. Convert the GPU address difference to an `MTLBuffer` offset.
3. Validate the range and encode the native copy.

A straightforward linear scan over fewer than roughly 50 backing ranges is the initial design.
The port should measure this before adding a more complex address index. Texture view heaps are
not buffer-copy operands. Sampler argument buffers may use the same lookup only if sampler-entry
copy semantics are validated.

### Alignment

The existing allocator already aligns whole allocations to at least the backend-required
alignment. Metal 4 requires 16-byte alignment for texture/buffer-copy offsets. Interior
`GpuRange`s passed to `copy_memory_to_texture` or `copy_texture_to_memory` must therefore also be
16-byte aligned. The first backend should validate and reject a misaligned range rather than
insert an implicit scratch copy.

All Metal size, row-pitch, image-pitch, mip, layer, and compressed-block constraints must be
checked before encoding. Integer overflow in `offset + size` or a computed image footprint is a
programming error.

### Lifetime and aliasing

`gpu_free`, `destroy_texture`, and object destruction follow the current immediate-logical,
deferred-physical contract. The backend records a monotonically increasing retirement value for
each submitted command group and processes an allocation-free FIFO from its oldest entry. Public
handles become invalid at the destroy call; Metal heap storage and objects are released or reused
only after their recorded GPU use has completed.

The public allocator does not expose simultaneous live aliases. Freed suballocations can be
reused only after completion. If explicit transient aliasing is added later, it needs
resource-aware heap activation/deactivation and alias visibility; the current global barrier API
alone is not a sufficient aliasing contract.

## Root data and hidden binding ABI

The root-data structure is intentionally unchanged. Metal buffer slot 0 points to a ring copy of
the exact user structure passed to each draw or dispatch: no wrapper, metadata header, descriptor
bases, or backend fields are inserted into it.

The generated Metal shader ABI has three fixed buffer bindings:

| Metal buffer slot | Contents | Visibility |
| --- | --- | --- |
| 0 | GPU address of ring storage containing the exact user root-data bytes | User-declared shader stages |
| 1 | Hidden pointer/constant containing the active `MTLTextureViewPool` 64-bit base resource ID | Stages that index textures |
| 2 | GPU address of a Tier-2 `MTLBuffer` argument buffer declared as the typed sampler array | Stages that index samplers |

Each rooted draw or dispatch snapshots its exact root bytes to an internal, persistently mapped
shared-memory ring and binds that allocation at slot 0 immediately before encoding the command.
The ring allocation remains alive through command-buffer completion and is reclaimed using the
queue timeline. A later host write to the source structure cannot change an already recorded
command. The single overload takes `ByteSpan` by value. A valid trivially copyable root whose byte size is a
multiple of four converts implicitly, storing its address and `sizeof(T)`; type-erased callers
explicitly construct `ByteSpan{byte_ptr, byte_size}`. Every byte size is validated against
`max_push_data_size`. Empty `{}` root data allocates and binds nothing;
a rootless shader declares no slot-0 root and ignores any physical binding left by earlier commands.

The backend binds heap slots 1 and 2 when it opens a render or compute encoder and reapplies them
after encoder transitions. Root slot 0 has no persistent public state: every rooted draw or dispatch
supplies and binds its own bytes.

The hidden bindings are target ABI, not members of the user structure. Vulkan target lowering can
continue to use its root/push-data and descriptor-buffer convention while Metal target lowering
emits the three-slot ABI. The same source-level root structure and texture/sampler indices are
therefore shared across backends.

## Texture heaps and `MTLTextureViewPool`

### Why the raw heap cannot be retained

The current public API allocates `MemoryType::texture_heap` bytes, exposes their CPU and GPU
addresses, writes implementation descriptor bytes through `write_texture_descriptor`, and binds a
`GpuRange`.

An `MTLTextureViewPool` is not a byte-addressable buffer. The CPU sets a texture view at a pool
index, and Metal assigns a resource ID in the pool's contiguous resource-ID range. A compute
shader or `memcpy` cannot write or copy arbitrary pool entries by storing descriptor bytes.
Preserving the old representation would require an index-to-resource-ID table, followed by the
resource-ID-to-texture interpretation. This design rejects that double indirection for the direct
path.

### Proposed public shape

The minimum public redesign is an opaque heap with indexed CPU operations. Exact spelling can be
settled during implementation, but the contract is:

```cpp
struct TextureHeap;

[[nodiscard]] TextureHeap*
create_texture_heap(Device* device, std::uint32_t capacity) noexcept;
void destroy_texture_heap(TextureHeap* heap) noexcept;

void write_texture_descriptor(
    TextureHeap* heap,
    std::uint32_t index,
    const Texture* texture,
    TextureDescriptorType type,
    const TextureDescriptorDesc& desc = {}) noexcept;

void copy_texture_descriptors(
    TextureHeap* destination,
    std::uint32_t destination_index,
    const TextureHeap* source,
    std::uint32_t source_index,
    std::uint32_t count) noexcept;

void set_texture_heap(CommandBuffer* commands, const TextureHeap* heap) noexcept;
```

The operation names may retain existing overloads during migration, but the portable contract is
heap plus index, never a writable descriptor pointer. Creation fixes a logical capacity.
Writes and copies validate complete slot ranges. As with the current raw heap, the caller must not
overwrite a slot, destroy its referenced texture, or destroy the heap until prior GPU use
completes; the backend does not snapshot a view pool per draw.

On Metal:

- one `TextureHeap` owns an `MTLTextureViewPool`;
- `write_texture_descriptor` creates/selects the required view and writes it at `index`;
- `copy_texture_descriptors` uses the pool range-copy operation where legal, or repeats indexed
  CPU pool writes while preserving reference lifetime;
- `set_texture_heap` records the pool and supplies `baseResourceID` through hidden slot 1.

On Vulkan, `TextureHeap` hides the existing descriptor-buffer allocation. Indexed writes use the
current descriptor encoding internally, indexed copies copy backend-owned descriptor slots, and
binding supplies the hidden descriptor-buffer address expected by Vulkan shader lowering. The
application no longer observes the representation.

### Shader lookup

Metal documents resource view IDs in a view pool as one contiguous range. Texture lookup is
therefore lowered as:

```text
resource_id = uint64(texture_view_pool_base) + uint64(texture_index)
texture = reinterpret_resource_id_as_the_required_texture_type(resource_id)
```

There is no `texture_index -> resource-ID table -> texture` path. Bounds and descriptor-type
correctness remain application responsibilities, subject to debug validation.

MSL does not provide Vulkan-style `ResourceDescriptorHeap[index]` syntax sugar for this ABI.
Slang must emit the base-ID addition and the correct typed texture reinterpretation for Metal.
That compiler work is the principal external blocker for the direct binding model.

### Descriptor generation and compatibility

GPU writes, GPU copies, and arbitrary host `memcpy` of texture descriptor bytes cease to be
portable. The indexed write/copy API is the only direct portable way to mutate a `TextureHeap`.
This restriction is specific to texture view-pool entries; sampler entries remain real
byte-addressable 64-bit values, subject to the sampler validation below.

An optional compatibility path could retain GPU-generated texture descriptor indices in an
ordinary buffer, then load a resource ID from an indirection table in the shader. It must be named
and selected explicitly because it adds the double lookup this direct design avoids. It is not
the default heap ABI and is not required for the first Metal backend.

`TextureDescriptorType::sampled` and `TextureDescriptorType::storage`,
`TextureDescriptorDesc::format`, mip range, and array-layer range remain part of each slot.
Metal view compatibility and writeable-format restrictions need validation for every accepted
combination.

## Sampler heap

The current material model uses global enum-like sampler indices. That remains the intended model:
materials store an integer, and the shader directly evaluates `sampler[index]`.
The motivating material configuration currently has 108 global sampler entries; the public model
must remain scalable beyond that count, not merely beyond the Metal direct-slot limit of 16.

The active logical sampler heap is exposed to generated Metal shaders as the typed sampler array
at hidden slot 2. The array is a real Tier-2 argument buffer backed by an `MTLBuffer`, with one
direct 64-bit `MTLSamplerState.gpuResourceID` value per logical entry. In shader source it is a
typed sampler array; the shader does not reinterpret a sampler resource ID manually.

The backend maintains a cache keyed by the complete `SamplerDesc`:

- `min_filter`, `mag_filter`, and `mip_filter`;
- `address_u`, `address_v`, and `address_w`;
- `anisotropic`, which selects the API's fixed 4x profile;
- `compare_enabled` and `compare`.

`write_sampler_descriptor` obtains or creates the corresponding `MTLSamplerState`, retains it in
the backend cache, and CPU-writes its direct 64-bit `gpuResourceID` into the destination slot.
The cache retains every created sampler state until device destruction. Raw sampler IDs can be
copied or GPU-written, so the backend cannot discover every surviving copy and cannot safely evict
a cache entry using only a timeline value. Earlier eviction would require a new explicit
heap/handle provenance contract.

The public sampler heap must scale beyond 16 entries. The design therefore does not use
`MTL4ArgumentTable` direct sampler slots, whose direct-slot count is limited. It uses the Tier-2
typed sampler argument buffer selected by shader lowering. No per-draw sampler setter,
per-material sampler handle, or second sampler-ID lookup is introduced.

The current `sampler_heap` allocation and `set_sampler_heap(GpuRange)` remain the logical
namespace. Because the Metal representation is an ordinary byte-addressable `MTLBuffer`, direct
64-bit entries can be written on the CPU. Copying those bytes or encoding entries on the GPU may
also be viable, unlike texture view-pool mutation, but it is not part of the support contract
until validation proves:

- the exact argument-buffer element layout and alignment;
- visibility requirements for GPU-written IDs before sampler use;
- lifetime/retention of every `MTLSamplerState` whose ID may be produced or copied;
- bounds and provenance validation for copied/generated entries;
- equivalent Vulkan behavior.

Until those tests pass, applications populate sampler entries through
`write_sampler_descriptor`. The restriction is provisional validation policy, not a claim that
sampler descriptors are categorically non-copyable.

The Metal Slang target must lower source sampler indexing to the typed slot-2 array. This work is
separate from the texture resource-ID arithmetic and must be validated with divergent indices and
heap sizes above 16.

## Textures, views, and copies

`TextureDesc` retains width, height, depth, logical `layer_count`, mip count, format, type, and
usage.
Every texture is created at an explicit offset in a placement `MTLHeap`; the backend never falls
back to a standalone user texture.

`RenderViewDesc` retains one mip level and one physical slice; cube faces are individual
slices. A NoGraphicsAPI
`RenderView` owns enough metadata to form the Metal render-pass attachment and retains a
Metal texture view when the selection cannot use the parent texture directly. Swapchain views are
owned by the swapchain/frame wrapper, not destroyed by the application.

`TextureCopyDesc` continues to describe one mip, a physical base slice and slice count, xyz
origin, width/height/depth, row pitch, and slice pitch. Zero counts/extents/pitches retain their
current “all remaining” or tightly packed defaults. The backend supports the same directions as
the current API:

- buffer to buffer through `copy_memory`;
- buffer to texture through `copy_memory_to_texture`;
- texture to buffer through `copy_texture_to_memory`.

The buffer side is resolved from raw GPU address to `MTLBuffer` plus offset as described above.
Texture/buffer offsets must be 16-byte aligned for Metal 4. Row/image pitches, compressed blocks,
3D slices, array layers, mip extents, and aspect restrictions are validated before encoding.

Copies use native Metal buffer/texture commands. They do not route through a compute shader, do
not reinterpret a raw GPU address as an `MTLBuffer`, and do not make texture-view-pool contents
byte-copyable.

## PSOs, shaders, and fixed state

### Backend-neutral shader artifacts

The current `GraphicsPSODesc` and `MeshPSODesc` explicitly contain `vertex_spirv`,
`fragment_spirv`, and `mesh_spirv`; `create_compute_pso` directly accepts a
`compute_spirv` span. Metal cannot consume those inputs directly.

The proposed API replaces a backend-specific bytecode field with a backend-neutral stage bundle.
A representative shape is:

```cpp
struct ShaderArtifactBundle
{
    Span<const std::uint32_t> spirv = {};
    Span<const std::byte> metallib = {};
    Span<const char> msl = {};
};
```

The final structure may use a separate stage descriptor and entry-point name, but it must support
at least:

- SPIR-V for the Vulkan backend;
- precompiled metallib for production Metal use;
- optional MSL source for development and validation.

The application or build system supplies the backend artifact. Runtime SPIR-V translation is not
an implicit NoGraphicsAPI contract. PSO descriptions retain stage identity and entry points while
replacing only the backend-specific payload.

Slang is the intended common source compiler. Its Metal output must implement the exact slot-0
root ABI, hidden slot-1 texture view-pool base, typed slot-2 sampler array, texture base-plus-index
resource-ID lowering, and existing raw buffer-pointer behavior. Current Slang descriptor-heap
convenience support does not by itself implement this Metal ABI. Compiler changes and upstream
coordination are the principal external dependency.

### Render state

The current render PSO state remains expressible:

- primitive topology is triangles through the current draw API;
- winding-based `CullMode::{none, clockwise, counter_clockwise}`;
- constant, clamp, and slope-scaled depth bias;
- depth clamp, polygon mode, line mode, and other unexposed raster controls remain out of scope;
- blend enable, source/destination color and alpha `BlendFactor`, and color/alpha `BlendOp`;
- per-attachment color write mask;
- depth test/write and all `CompareOp` values;
- global stencil read/write masks plus front/back references, comparisons, and all `StencilOp`
  values;
- `IndexType::{uint16, uint32}` selected by indexed draw;
- color/depth/stencil attachment formats supplied at PSO creation.

Metal represents depth/stencil state separately from the render pipeline. The NoGraphicsAPI PSO
object owns both and applies the immutable public front and back stencil references when bound,
even though Metal can set them dynamically.

Blend factors `zero`, `one`, source/destination color/alpha and their inverses, and blend
operations `add`, `subtract`, `reverse_subtract`, `minimum`, and `maximum` map directly after
enum-table validation. `source_alpha_saturate` also maps directly. Unsupported format/blend
combinations are rejected at PSO creation.

### Compute and mesh metadata

The public API does not carry compute local size separately; it is part of the compiled shader.
PSO creation must obtain it from metallib metadata/reflection or a backend-neutral artifact
field and retain it so `dispatch` can emit Metal threadgroups without reflection at recording
time. Mesh threadgroup metadata is likewise compiled metadata retained by the PSO. The
current `MeshPSODesc` has no object-shader input.

`draw_meshlets` is available only when the selected Metal family supports mesh shading.
`draw_meshlets_indirect` has the stricter Apple 9 gate described above. The backend must validate
the compiled metallib target and runtime GPU family together.

PSOs are `MTLAllocation` objects and participate in residency as described below.

## Rendering and attachments

`begin_render_pass` maps `RenderingDesc` to a Metal render-pass descriptor and
`MTL4RenderCommandEncoder`. The public attachment model remains:

- zero or more color attachments;
- an optional depth attachment;
- an optional stencil attachment;
- `LoadOp::{load, clear, discard}`;
- `StoreOp::{store, discard}`;
- clear color, clear depth, and clear stencil values.

The backend validates attachment extents/slices, formats, and PSO compatibility. The current
`RenderingDesc` has no render-area, layer-count, or view-mask field; rendering covers the selected
attachment views and does not add multiview. Separate NoGraphicsAPI depth and stencil attachment
fields may reference aspects of the same Metal depth/stencil view; inconsistent views are
rejected.

`LoadOp::discard` maps to a don't-care load action, and `StoreOp::discard` maps to a don't-care
store action when legal. The Vulkan backend already honors both store choices; the Metal backend
must do the same. A store operation controls whether attachment contents are preserved after the
pass. It is not a memory barrier and does not make later shader access visible.

Later sampling or storage access to an attachment requires `StoreOp::store`. `StoreOp::discard`
leaves the contents undefined, and no later barrier can recover them. On an Apple tile-based GPU,
the store action at render-pass completion preserves the result outside the tile/ROP domain; a
subsequent shader consumer still needs a queue dependency with device visibility.

The public API does not permit `barrier` while rendering is active. Metal keeps the same rule.
General render-target or depth/stencil producer hazards end the render pass before
synchronization.

## Barriers and resource state

No public barrier API change is required for Metal 4.

The current `Stage` values are:

- `none`
- `indirect`
- `index_input`
- `vertex`
- `mesh`
- `depth_stencil_tests`
- `fragment`
- `color_output`
- `compute`
- `transfer`
- `host`
- `all_commands`

The current `Access` values are:

- `none`
- `transfer_read` and `transfer_write`
- `shader_read` and `shader_write`
- `color_read` and `color_write`
- `depth_stencil_read` and `depth_stencil_write`
- `indirect_read`
- `index_read`
- `host_read`
- `descriptor_read`

They remain useful for Vulkan validation and for deriving conservative Metal stage and visibility
options. Metal barriers are global stage barriers; they do not take the NoGraphicsAPI resource or
byte-range list because NoGraphicsAPI does not expose one.

The public call remains
`barrier(commands, before, before_access, after, after_access)`. Stage and access values can be
combined with the existing bitwise-or operators. Validation continues to reject an access type
that is incompatible with its stage mask before backend lowering.

### Stage lowering

| NoGraphicsAPI `Stage` | Metal 4 stage selection |
| --- | --- |
| `none` | no stage |
| `indirect` | conservatively `dispatch | vertex | mesh | fragment` |
| `index_input` | conservatively `vertex | fragment` |
| `vertex` | `vertex` |
| `mesh` | `mesh` |
| `depth_stencil_tests` | `fragment` |
| `fragment` | `fragment` |
| `color_output` | `fragment` |
| `compute` | `dispatch` |
| `transfer` | `blit` |
| `host` | no GPU stage; handled at commit/completion boundaries |
| `all_commands` | `all` |

Metal does not expose separate index-input, indirect-argument, color-output, depth/stencil, or
host stages. The widening above is intentional. A future implementation may specialize
`indirect` after it sees whether the next operation is draw or dispatch, but that is an
optimization, not a public API requirement.
`all_commands` covers all stages performed by GPU commands and deliberately excludes the host
pseudo-stage. It is an independent flag, so a destination mask may combine it with `host`.
The `host` stage is valid only in a barrier destination together with `host_read`, for GPU-to-CPU
readback. Host writes to coherent mapped memory become available to GPU work at queue submission,
so NoGraphicsAPI rejects a host source stage and exposes no `host_write` access.

Apple's command-stage tables associate indexed and indirect draws with vertex and fragment stages.
Using only vertex as the consumer may be safe for some operations but is not the initial
contract. Compute indirect arguments consume at `dispatch`; copies consume/produce at `blit`.

### Access and visibility lowering

`Access` determines whether the barrier requires device-memory visibility:

- a source containing `transfer_write`, `shader_write`, `color_write`, or
  `depth_stencil_write`, followed by a GPU read or write, requests Metal device visibility;
- read-after-read and pure execution dependencies use `MTL4VisibilityOptionNone`;
- index, indirect, descriptor, shader, attachment, and transfer reads select validation and
  destination stages but are not distinct Metal visibility scopes;
- host writes to shared memory must finish before the queue commit that consumes them; they do not
  use a public barrier source;
- `host_read` becomes valid after timeline completion. The Apple-family-7-or-newer Metal 4 target
  does not add a legacy managed-storage synchronization path.

The first implementation should conservatively request device visibility whenever the source
access mask contains a GPU write and the destination contains any GPU access. It can narrow this
after hazard tests. `MTL4VisibilityOptionResourceAlias` is not used because simultaneous live
alias activation is not currently exposed.

### Placement and render hazards

Barriers are recorded outside rendering. The correctness-first lowering encodes
`barrier(afterStages:beforeQueueStages:visibilityOptions:)` at the public barrier point and ends
the active non-render encoder. That producer barrier covers matching work in the current and prior
passes, and blocks matching stages in subsequent queue passes. If no encoder is active, the
backend may keep the barrier pending in its originating command buffer, but must materialize it
before subsequent commands or command-buffer finalization as described below.

Keeping one compute encoder open across a public barrier is an optional optimization. It requires
both `barrier(afterQueueStages:beforeStages:visibilityOptions:)` for matching producers in prior
passes and `barrier(afterEncoderStages:beforeEncoderStages:visibilityOptions:)` for matching work
already encoded in the current pass. The intra-encoder form alone cannot implement the global
NoGraphicsAPI barrier. The backend derives all stage masks and visibility from the public barrier and
does not use the legacy render-encoder `MTLBarrierScope`.

Render-target/depth output followed by shader access is encoded as:

1. require `StoreOp::store` for every attachment whose contents are consumed later;
2. end the render pass;
3. encode a producer queue barrier for the `fragment` stage or a consumer queue barrier in the
   later shader encoder;
4. request device visibility and target the later consumer stages.

Apple tile-based GPU families do not support an intrapass barrier that waits on a `fragment` or
`tile` producer. General fragment/tile producer hazards therefore end the pass and use a queue
barrier instead. Vulkan follows the same NoGraphicsAPI rule because general barriers are not accepted
inside dynamic rendering.

Pending barrier state belongs to the command buffer in which `barrier` was recorded. It must
never be carried in recording-time global queue state across command buffers. Before encoding any
later commands, the backend consumes pending state with
`barrier(afterQueueStages:beforeStages:visibilityOptions:)` at the start of their encoder. If
`submit` or `submit_and_present` finalizes the command buffer first, the backend materializes a
producer queue barrier in a small empty encoder in that originating command buffer. The producer's
`afterStages`
covers prior passes, and its `beforeQueueStages` applies to later passes, including passes from
later command buffers on the same queue.

Residency keeps allocations addressable; it does not establish execution ordering or memory
visibility. Every dependency above is still required when all resources are resident.

## Command recording and execution

`begin_commands` acquires a retired `MTL4CommandAllocator` and a reusable, device-created
`MTL4CommandBuffer` from internal pools. After the prior GPU use has completed, it resets the
allocator and calls `beginCommandBufferWithAllocator:`. Submission or presentation calls
`endCommandBuffer` before commit. The allocator's command-memory heaps are not reset until the
timeline value covering their prior GPU use has completed. The API remains single-threaded.

PSO, texture-heap, and sampler-heap bindings are logical command-buffer state. The backend
creates render/compute encoders lazily, binds that active state, and reapplies it after copy or
barrier encoder transitions. Root data is instead supplied and bound by each draw or dispatch.

### Draw and dispatch addresses

Metal 4 render and compute entry points accept raw `MTLGPUAddress` for:

- indexed draw index data;
- draw indirect arguments;
- indexed-draw indirect arguments;
- mesh indirect arguments on supported families;
- compute dispatch indirect arguments.

Those operations use `GpuRange::gpu` directly after live-range and alignment validation.
`draw_indexed` advances the address by `first_index * index_element_size`; the vertex offset
remains the signed base-vertex argument. Indirect operations honor the `GpuRange` base address,
public stride, and draw count. If Metal exposes only a one-command call for a variant, the backend
emits a recording-time loop of GPU-indirect commands; it does not inspect argument contents on the
host.

Direct `draw` and `dispatch` map their counts without an address translation. Zero-count behavior,
overflow, and Metal maximums are validated consistently with Vulkan.

### Copy commands

Copy commands are the exception to direct raw-address use. They resolve ordinary GPU address
ranges to `MTLBuffer` plus offsets, as described in the memory section. Texture copies also
enforce the Metal 4 16-byte buffer-offset rule.

All three public copy operations use native copy methods on `MTL4ComputeCommandEncoder`; Metal
classifies those commands as the `blit` stage. `copy_memory` encodes buffer-to-buffer copy,
`copy_memory_to_texture` encodes buffer-to-texture copy, and `copy_texture_to_memory` encodes
texture-to-buffer copy. A compute shader `memcpy` is not a portable fallback for texture
descriptors or textures.

## Submission, timelines, and presentation

### Grouped submit

`submit` accepts the existing ordered `Span<CommandBuffer* const>` and `TimelinePoint`. The
proposed backend finalizes the command buffers and commits them as one ordered Metal queue group
using the queue's grouped commit operation. The timeline point is signaled only after the entire
group is complete.

The command buffers, command allocators, root-data ring segments, transient encoder objects, and
other backend-internal retired objects are associated with that completion value. Public
allocations, textures, views, PSOs, heaps, and descriptor contents are not implicitly
retained. Every value returned by `begin_commands` must appear exactly once in the next `submit`
or `submit_and_present` call, matching the current public contract; there is no separate
`end_commands`.

`wait_idle` waits for a shared-event value inserted after all previously committed work. It does
not busy-poll command-buffer status.

### Timeline

Each NoGraphicsAPI `TimelineSemaphore` owns an `MTLSharedEvent`:

- `create_timeline_semaphore` sets `signaledValue` to the public `initial_value`;
- `timeline_completed_value` reads `signaledValue`;
- `wait_timeline` registers notification for the requested value and blocks a host wait object;
- queue submission signals the value after its full command-buffer group;
- values must increase according to the current NoGraphicsAPI contract.

Backend-internal rings, allocator reclamation, and physical object destruction use completed
timeline values. Destruction calls invalidate public handles immediately and append their Metal
object pointer or allocator token to the fixed retirement FIFO; they do not insert a host wait.
A Metal barrier is not a substitute for host completion, and application-owned data still needs an
explicit point before CPU reuse.

The exact grouped-commit signal placement and error propagation must be validated with multiple
command buffers, multiple timeline points, and device-loss/error paths.

### Swapchain and platform handle

`DeviceDesc::window` is the platform escape hatch. For the initial Apple backend it is proposed to
mean a `CAMetalLayer*`, paired with `DeviceDesc::swapchain_format`. A higher-level NSView/UIView
adapter can install and configure the layer outside the core API; the core backend should not guess
the class behind `void*`.

`create_device` validates the supplied layer and applies `DeviceDesc::swapchain_format`.
`DeviceDesc::desired_swapchain_image_count` should select the layer's supported drawable count and
the number of active presentation contexts; display-sync policy remains a property of the supplied
layer. `get_drawable_extent` reads the layer size without consuming a drawable. `acquire` later
waits for the next drawable and returns a `SwapchainFrame` containing a device-owned render view
plus its width and height.

`submit_and_present` preserves the command-span/group semantics:

1. validate the acquired drawable and ordered command-buffer span;
2. call `waitForDrawable:` before committing any command buffer that targets the drawable;
3. finalize each command buffer with `endCommandBuffer`, then call `commit:count:` for the ordered
   group;
4. call `signalEvent:value:` for the requested NoGraphicsAPI completion point;
5. call `signalDrawable:` after all command buffers targeting the drawable have been committed;
6. call `[drawable present]`;
7. clear acquisition state and retire the frame wrapper when its CPU/GPU lifetime permits.

The timeline signal proves completion of the GPU command-buffer group; it does not prove that the
display has scanned out the drawable. The current call presents the device's one acquired drawable;
multi-window `PresentBatch` is not part of the public API.

No generic non-Apple WSI is added. The existing Win32/Vulkan path and proposed CAMetalLayer/Metal
path are platform implementations of the same opaque `window` field.

## Residency

Metal 4 makes residency explicit. The proposed backend creates one persistent, device-global
`MTLResidencySet`, attaches it to the NoGraphicsAPI command queue, and uses it for all
NoGraphicsAPI-owned allocations.

The set contains:

- every placement `MTLHeap` used for ordinary buffers;
- every placement `MTLHeap` used for textures;
- render and compute pipeline-state `MTLAllocation` objects.

Adding an `MTLHeap` makes the heap allocation resident as a whole; individual placed resources
are not separately added. Membership changes are committed according to the residency-set API.
A heap or PSO allocation is removed only after the last timeline value that can reference it
has completed.

Drawable textures are not standalone NoGraphicsAPI user resources and are not manually inserted into
the global set. For every live swapchain, the backend attaches that `CAMetalLayer` object's
`residencySet` to the Metal 4 command queue alongside the global NoGraphicsAPI set. It removes the
layer set only when swapchain work has completed and the swapchain is destroyed.

The backend must not create a standalone `MTLBuffer` or `MTLTexture` as a silent fallback when a
placement heap is exhausted. It grows the appropriate placement-heap pool or reports failure
according to the allocator policy. This keeps address lookup, budgeting, and residency uniform.

Residency is not hazard tracking. A resident texture written as an attachment still needs the
render-to-shader barrier described above, and a resident buffer written by dispatch still needs
the appropriate dispatch-to-consumer dependency.

## Current scope retained by the port

The Metal proposal does not broaden NoGraphicsAPI beyond its current public scope. In particular:

- the host API remains single-threaded and has no internal synchronization;
- there is one graphics/compute/copy-capable queue abstraction, not public multi-queue ownership;
- textures remain single-sample;
- there are no public sparse heaps or sparse resources;
- there is no ray tracing;
- there are mesh shaders but no separate public task-shader stage;
- there are no device-generated command graphs beyond the existing indirect operations;
- there is no capture/replay API;
- there is no persistent public pipeline-cache API;
- there are no public queries/timestamps;
- there are no explicit public resource objects for ordinary buffers, only GPU allocations/ranges;
- there is no public transient-alias activation model;
- swapchain integration remains platform-specific behind `void*`;
- Metal support does not imply support for legacy Intel Macs or the legacy Metal command API.

Adding one of these features requires a separate cross-backend contract. It must not be hidden in
the initial Metal port.

## Validation plan

Metal support must remain unadvertised until the following tests pass on representative Apple 7,
Apple 8, and Apple 9 hardware where applicable.

### Build and object model

- ARM64 header/build validation, including removal of the x86-only guard and replacement of AVX2
  helpers.
- Device creation/destruction, error paths, allocator growth, and leak checks.
- Placement-only buffer and texture creation at nonzero heap offsets.
- Residency-set membership, heap growth, PSO allocation residency, memory pressure, and
  completion-retired removal.

### Binding ABI

- Byte-for-byte slot-0 root structures containing scalars, nested structs, ordinary GPU pointers,
  texture indices, and sampler indices.
- Root-data ring wraparound and reuse only after timeline completion.
- Texture view-pool contiguous-ID validation and `base + index` lookup for every supported texture
  type, mip/layer view, sampled/storage type, and divergent index.
- Texture heap overwrite, range copy, destruction, and referenced-texture lifetime.
- Sampler arrays larger than 16, divergent sampler indices, all filter/address/compare modes,
  anisotropy, and backend cache lifetime.
- CPU-written sampler IDs plus GPU-copied and GPU-generated sampler entries, including all
  visibility and retention requirements.
- Per-command root-slot binding across render and compute encoder transitions.
- Vulkan and Metal output from the same Slang source and root-data definition.

### Commands and copies

- Direct and indexed draws using nonzero first index/base vertex.
- Every indirect layout, nonzero offset, padded stride, and multiple draw count.
- Direct and indirect compute dispatch with retained local size.
- Direct mesh draw on supported families and hard Apple 9 rejection/acceptance for indirect mesh.
- Address-to-`MTLBuffer` lookup at backing-range boundaries and with more than one 256 MiB range.
- Buffer, texture, and mixed copies for all supported dimensions, layers, mips, compressed blocks,
  pitches, and legal 16-byte-aligned offsets; deterministic rejection of misalignment.

### Rendering and synchronization

- Every load/store operation, including discard, and every clear value/aspect combination.
- Blend, color mask, culling, depth, stencil, and attachment-format compatibility.
- Dispatch-write to vertex/fragment/index/indirect/dispatch consumers.
- Blit-write to shader/index/indirect consumers.
- Color/depth/stencil attachment write followed by sampling or storage use in a later pass.
- Execution-only, write/read, write/write, and host completion cases.
- A barrier at the end of a command buffer, between command buffers in one group, and across
  groups, proving pending barriers never migrate to another command buffer.
- Validation that no general barrier is encoded while rendering is active.

### Timeline and presentation

- Ordered grouped commit with one and many command buffers.
- Timeline poll/wait/signal monotonicity, allocator retirement, timeout/error behavior, and
  `wait_idle`.
- CAMetalLayer resize, drawable exhaustion, occlusion, multiple sequential `submit_and_present`
  calls with one and many command buffers, and presentation ordering relative to timeline signals.

### Format and performance evidence

- `supports_texture_format` conformance for every `Format` and `TextureUsage` combination used by
  examples.
- Packed component order, RGB32 rejection, depth/stencil view behavior, sRGB filtering, storage
  formats, ASTC/EAC, and family-gated BC.
- CPU cost of the ordinary-address linear range lookup at the expected sub-50 backing-range
  scale.
- Shader and GPU cost of direct texture `base + index` versus an explicit resource-ID table,
  confirming that the selected ABI avoids the double indirection.
- Root-ring, sampler-table, view-pool, and residency-set churn under multi-frame workloads.

## Known blockers and open decisions

The principal blocker is Slang Metal lowering for the selected global binding ABI. The required
behavior is not merely compiling existing descriptor-heap source to MSL; it must produce:

- exact user root data at slot 0;
- hidden texture-pool base at slot 1;
- a hidden typed Tier-2 sampler argument buffer at slot 2;
- typed texture formation from contiguous `baseResourceID + index`;
- direct `sampler[index]`;
- ordinary raw GPU-pointer loads consistent with the current shader ABI.

Other decisions to close during implementation are:

- whether the initial device requirement is Apple 9 or a new mesh-indirect capability is public;
- the final `TextureHeap` C++ spelling and migration/deprecation period;
- the exact backend-neutral shader-artifact structure and entry-point representation;
- format-table results for packed, RGB32, depth24/stencil8, and compressed formats;
- the exact CAMetalLayer ownership contract and Metal 4 presentation sequence;
- whether debug validation tracks texture and sampler slot initialization/lifetime;
- whether GPU-written/copied sampler entries become a supported cross-backend contract;
- whether a separately named indirect texture-descriptor compatibility path is worth supporting.

None of these questions justify changing the user root structure or adding a resource-ID lookup
to the direct texture path.

## Primary references

Apple documentation used for the proposed mappings:

- [Understanding the Metal 4 core API](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api)
- [Metal feature set tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)
- [`MTL4CommandQueue`](https://developer.apple.com/documentation/metal/mtl4commandqueue)
- [`MTL4CommandBuffer`](https://developer.apple.com/documentation/metal/mtl4commandbuffer)
- [`MTL4CommandEncoder`](https://developer.apple.com/documentation/metal/mtl4commandencoder)
- [`MTL4RenderCommandEncoder`](https://developer.apple.com/documentation/metal/mtl4rendercommandencoder)
- [`MTL4ComputeCommandEncoder`](https://developer.apple.com/documentation/metal/mtl4computecommandencoder)
- [`MTLStages`](https://developer.apple.com/documentation/metal/mtlstages)
- [`MTL4VisibilityOptions`](https://developer.apple.com/documentation/metal/mtl4visibilityoptions)
- [Synchronizing stages within a pass](https://developer.apple.com/documentation/metal/synchronizing-stages-within-a-pass)
- [Resource synchronization](https://developer.apple.com/documentation/metal/resource-synchronization)
- [`MTLResourceViewPool`](https://developer.apple.com/documentation/metal/mtlresourceviewpool)
- [`MTLTextureViewPool`](https://developer.apple.com/documentation/metal/mtltextureviewpool)
- [`MTL4ArgumentTable`](https://developer.apple.com/documentation/metal/mtl4argumenttable)
- [`MTLSamplerState`](https://developer.apple.com/documentation/metal/mtlsamplerstate)
- [`MTLHeap`](https://developer.apple.com/documentation/metal/mtlheap) and
  [`MTLHeapType.placement`](https://developer.apple.com/documentation/metal/mtlheaptype/placement)
- [`MTLAllocation`](https://developer.apple.com/documentation/metal/mtlallocation)
- [`MTLResidencySet`](https://developer.apple.com/documentation/metal/mtlresidencyset)
- [Simplifying GPU resource management with residency sets](https://developer.apple.com/documentation/metal/simplifying-gpu-resource-management-with-residency-sets)
- [`MTLSharedEvent`](https://developer.apple.com/documentation/metal/mtlsharedevent)
- [`CAMetalLayer`](https://developer.apple.com/documentation/quartzcore/cametallayer)
- [Metal limits](https://developer.apple.com/metal/limits/)

Slang references:

- [Descriptor-heap convenience features](https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md)
- [Slang issue #11540: Metal resource descriptor heap support](https://github.com/shader-slang/slang/issues/11540)

The Apple references document Metal behavior. The Slang links establish current compiler context,
not a guarantee that the required NoGraphicsAPI ABI exists.

## Relationship to the existing backend

The [Vulkan support document](vulkan-support.md) remains the support contract for the implemented
backend. Vulkan can implement the opaque `TextureHeap` by hiding its descriptor buffer, so the
public change removes representation leakage without weakening the Vulkan implementation.

The [Slang integration document](slang.md) describes the current SPIR-V and root-data ABI. It
needs a Metal-target section only after the compiler lowering above is implemented and validated.

The [No Graphics API comparison](no-graphics-api-comparison.md) explains why NoGraphicsAPI exposes
GPU addresses, explicit allocation, global synchronization, and small command surfaces. This
Metal design preserves those goals while acknowledging the two places where Metal does not expose
the same representation: texture view-pool descriptors and native copy-command buffer operands.
