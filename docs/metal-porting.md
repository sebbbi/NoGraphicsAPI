# Metal 4 porting design

Status: proposed and unimplemented. NoGraphicsAPI remains Vulkan-only.

This document asks whether a Metal 4 backend can preserve the important ideas from
[*No Graphics API*](https://www.sebastianaaltonen.com/blog/no-graphics-api), rather than merely
translate the current Vulkan implementation. The target model is:

- ordinary shader data is reached through 64-bit GPU pointers, without public buffer objects;
- applications manage global texture and sampler namespaces by index;
- each draw or dispatch receives the application's root structure directly;
- allocation, synchronization, and submission remain explicit;
- the API stays small and avoids binding layouts, resource lists, and per-draw descriptor work.

Metal 4 supports most of that model directly. The difficult parts are texture descriptors, the
shader ABI that turns indices into Metal resources, and Metal's explicit residency rules. Those
areas need compatibility mechanisms, but they do not require changing the application's root
structure or abandoning GPU pointers.

The terms used below are deliberate:

- **Documented** behavior follows an Apple or Slang reference linked at the end.
- **Proposed** behavior is the selected NoGraphicsAPI backend design.
- **Needs validation** means shipping Apple GPUs must prove the behavior before Metal support is
  advertised.

## Design summary

| Area | Metal 4 result |
| --- | --- |
| Ordinary data | Preserve real 64-bit GPU addresses and pointer arithmetic. |
| Index and indirect arguments | Pass `MTLGPUAddress` directly to Metal 4 commands. |
| Native copies | Resolve a GPU address internally to its backing `MTLBuffer` and offset. |
| Texture storage | Preserve application placement in GPU-only `TextureHeap` objects backed by `MTLHeap`. |
| Texture descriptors | Replace the public descriptor-byte `GpuHeap` with an opaque indexed heap backed by `MTLTextureViewPool`. |
| Sampler heap | Keep the logical heap and expose a typed Tier-2 argument-buffer array to shaders. |
| Root data | Keep the exact user structure; snapshot it at Metal buffer slot 0 for each command. |
| Shader artifacts | Replace SPIR-V-only PSO inputs with backend-neutral stage artifacts. |
| Barriers | Keep the global barrier API and lower it conservatively to Metal 4 stage and visibility barriers. |
| Residency | Track placed heaps and pipeline allocations in queue-attached residency sets. |
| Submission | Map command batches and timeline points to grouped queue commits and `MTLSharedEvent`. |
| Presentation | Use a supplied `CAMetalLayer` without adding a second public swapchain model. |

The texture descriptor heap and shader artifacts are the only required public representation
changes. A capability decision may also be needed for indirect mesh drawing.

## Platform baseline

The proposed backend targets the Metal 4 core API: `MTL4CommandQueue`, command buffers and
allocators, render and compute encoders, resource view pools, barriers, and residency sets. It does
not target the legacy Metal command-buffer API.

Apple GPU family 7 is the baseline for the Metal 4 core feature set. Important qualifications are:

- `draw_meshlets_indirect` requires Apple GPU family 9. The first backend should either require
  Apple 9 or expose a capability and reject that operation on Apple 7 and 8. CPU readback of the
  indirect arguments is not an equivalent fallback.
- Texture formats remain device- and usage-dependent. BC compression in particular is not part of
  the Apple 7 baseline.
- The current x86-64 header restriction must admit supported 64-bit Apple targets, and AVX2-only
  helpers need ARM64 or scalar implementations.

`supports_texture_format` remains authoritative. The port should publish only combinations proved
by Metal queries and device tests; it should not promise a format merely because a similarly named
`MTLPixelFormat` exists.

## GPU pointers and placed memory

This is the strongest direct match with *No Graphics API*.

Ordinary `GpuHeap` and `GpuRange` values continue to carry real GPU virtual addresses. Metal placed
buffers expose `MTLGPUAddress`, so application root structures and shader structures can retain
typed pointers and pointer arithmetic. No public `MTLBuffer` handle, binding index, or
address-translation API is introduced.

Metal 4 accepts raw addresses for the command operands that matter most to the design:

- index data;
- draw and indexed-draw indirect arguments;
- mesh indirect arguments on supported families;
- compute dispatch indirect arguments.

These commands consume `GpuRange::gpu` directly under the caller's normal bounds and alignment
contract. Multiple indirect commands may be encoded without reading their argument contents on the
CPU.

Native Metal copy commands are the exception. They take an `MTLBuffer` and byte offset rather than
an arbitrary GPU address. The backend therefore keeps an internal address-range index for its
ordinary backing buffers. A copy resolves the complete input range to one backing buffer and
offset before encoding. This is compatibility machinery for Metal's copy API, not a change to the
public pointer model or to shader access.

The public memory classes retain their intent:

- `cpu_visible` provides coherent mapped upload memory with a GPU address;
- `gpu_only` provides private GPU memory;
- `readback` provides mapped memory suitable for completed GPU results;
- `texture_descriptor_heap` and `sampler_descriptor_heap` provide the descriptor namespaces
  described below.

`GpuHeap` is an exact application-sized buffer block. GPU-only `TextureHeap` values wrap texture
memory, `SizeAlign` reports placement requirements, and the application chooses offsets and block
growth. Destruction invalidates the public object immediately, while native objects are retired only
after the last relevant timeline value has completed. The initial port does not add simultaneous
live aliasing.

## Root and binding ABI

The user-visible root ABI remains unchanged. Each rooted draw or dispatch copies the exact bytes of
the supplied root structure into an internal shared-memory ring. Metal buffer slot 0 points at that
snapshot. There is no wrapper, metadata header, or backend field in the structure.

Two hidden bindings complete the Metal target ABI:

| Metal buffer slot | Meaning |
| --- | --- |
| 0 | Exact user root-data bytes for the current draw or dispatch. |
| 1 | Base resource ID of the active texture view pool. |
| 2 | Typed sampler array backed by the active Tier-2 argument buffer. |

Slots 1 and 2 are encoder state; slot 0 is supplied for each command. All ring storage survives
until the submitted work completes, so later CPU writes cannot alter an already recorded command.
Rootless shaders simply do not declare the slot-0 root.

These hidden bindings are a compiler target ABI, not additions to the shared C++/Slang root
structure. Vulkan can retain its push-data and descriptor-heap lowering while Metal emits the
three-slot form. This preserves one source-level data layout across both backends.

## Texture descriptor heap compatibility

The current Vulkan texture descriptor heap is a byte-addressable allocation containing implementation
descriptor bytes. `MTLTextureViewPool` is not byte-addressable: the CPU assigns a texture view to a
pool index, and Metal assigns the corresponding resource ID inside a contiguous range. Arbitrary
CPU or GPU copies of descriptor bytes cannot update the pool.

The portable texture descriptor heap must therefore become opaque. The proposed contract provides:

- capacity-based descriptor-heap creation and destruction;
- an indexed texture-view write;
- an indexed range copy between compatible heaps;
- command-buffer binding of one active heap.

The exact C++ spelling remains open, but the destination is always a heap and slot index, never a
writable descriptor pointer. Vulkan can implement the same abstraction by hiding its current
descriptor-heap allocation and translating indexed operations internally.

Metal shader lookup remains direct. `MTLTextureViewPool` assigns a contiguous resource-ID range, so
the shader forms a typed texture from:

```text
resource_id = texture_view_pool_base + texture_index
```

There is no `index -> resource-ID table -> texture` lookup in the main ABI. Avoiding that extra
load preserves the direct global texture namespace intended by the blog post.

The tradeoff is explicit: GPU-generated texture descriptors and arbitrary descriptor `memcpy` are
not portable in this design. A separately named compatibility mode could store resource IDs in an
ordinary buffer and add shader indirection. It is optional, slower by construction, and not part of
the first Metal backend.

Each slot still describes its view format, mip/layer range, aspect, and sampled/storage role. The API
does not retain or track the referenced texture; its lifetime remains the application's responsibility.

## Sampler heap

The sampler namespace can stay closer to the current API. Metal exposes a sampler state's 64-bit
`gpuResourceID`, and a Tier-2 `MTLBuffer` argument buffer can hold a typed sampler array. The active
sampler heap is bound at hidden slot 2, and shaders retain direct `sampler[index]` access.

This avoids `MTL4ArgumentTable`'s small direct-sampler limit and preserves material systems with
large global sampler tables. `write_sampler_descriptor` obtains the matching `MTLSamplerState`,
retains it, and writes its ID into the logical slot.

The initial supported path is CPU population through `write_sampler_descriptor`. Raw copying or
GPU generation of sampler IDs may be supportable, but needs proof of argument-buffer layout,
visibility, provenance, and sampler lifetime on both backends. Until then it is validation work,
not part of the portable contract.

## Shaders and pipelines

The current PSO descriptions contain SPIR-V directly, which Metal cannot consume. They must accept
a backend-neutral stage artifact capable of carrying:

- SPIR-V for Vulkan;
- a precompiled metallib for production Metal builds;
- optionally MSL source for development and validation.

Runtime SPIR-V translation is not an implicit API promise. The application or build system selects
and supplies the appropriate artifact. Pipeline descriptions otherwise retain the fixed state and
attachment compatibility needed by the backend; this document does not redefine every descriptor
field.

Slang is the intended common source compiler. Its Metal target must produce all of the following:

- the exact user root structure at slot 0;
- the hidden texture-pool base at slot 1;
- the hidden typed sampler array at slot 2;
- typed texture construction from `baseResourceID + index`;
- direct sampler indexing;
- ordinary raw GPU-pointer loads with the same data layout used by Vulkan.

Existing descriptor-heap syntax does not establish this ABI by itself. Implementing and validating
this lowering is the principal external blocker for the port.

Compute and mesh workgroup sizes are compiled shader metadata. The Metal PSO retains the values
needed to encode dispatches; command recording does not reflect shader metadata repeatedly.

## Commands, rendering, and synchronization

The public command surface maps without structural changes:

- direct, indexed, indirect, mesh, and compute work use Metal 4 render or compute commands;
- render-pass attachments map to Metal load, store, and clear actions;
- buffer and texture copies use native Metal copy commands;
- command-buffer batches map to ordered grouped queue commits.

The public global barrier also remains. Metal does not expose every NoGraphicsAPI stage separately,
so index, indirect, color-output, and depth/stencil scopes are widened to conservative Metal render
stages. Transfer maps to the blit stage and compute maps to dispatch. Access masks determine whether
a GPU write needs device-memory visibility before later GPU access.

General barriers remain outside rendering. Attachment data read later must be stored, and a
render-to-shader hazard ends the pass before a queue barrier. Host reads still wait for timeline
completion, while residency provides no execution or visibility dependency. The first backend may
split encoders conservatively around barriers; preserving dependencies matters more than encoder
coalescing.

## Timelines, presentation, and lifetime

Each public timeline semaphore maps to `MTLSharedEvent`. Submission signals the requested value only
after its entire ordered command-buffer group. Polling, host waiting, `wait_idle`, allocator reuse,
root-ring reuse, and deferred destruction all use completed event values.

For a windowed device, `DeviceDesc::window` is proposed to identify a `CAMetalLayer`. A platform
adapter may create and configure that layer, but the core API should not guess whether the opaque
value is an `NSView`, `UIView`, or some other wrapper.

The existing flow remains recognizable:

1. `get_drawable_extent` reads the layer size without acquiring a drawable.
2. `acquire` obtains a drawable and returns its render view and extent.
3. `submit_and_present` waits for the drawable, commits the ordered command group, signals its
   timeline point and the drawable, then presents it in Metal's required order.

The timeline proves GPU command completion, not display scanout. Exact layer ownership, drawable
wait behavior, resizing, occlusion, and error propagation still need platform validation.

## Residency

Metal 4 requires explicit residency. The proposed backend attaches one persistent
`MTLResidencySet` to its command queue. It contains the placement heaps used for ordinary buffers
and textures, plus render and compute pipeline allocations. A windowed device also attaches the
`CAMetalLayer` residency set required for drawable textures.

Heap membership is tracked at heap granularity. A heap or pipeline allocation leaves the set only
after all submitted work that may reference it has completed. The application responds to
exhaustion by creating another explicit heap.

Residency is backend machinery and requires no public resource list. It guarantees addressability,
not ordering, visibility, or lifetime beyond the application's existing timeline contract.

## Proposed public API impact

Most of the API remains intact: exact GPU heaps and ranges, placed texture heaps, opaque resource
handles, root arguments, draw/dispatch/copy calls, global barriers, command batches, timelines, and
presentation flow.

Required changes are limited to:

1. Adapt Vulkan descriptor-heap buffers to Metal texture and sampler indexing while preserving the
   application-owned logical namespaces.
2. Replace SPIR-V-only PSO inputs with backend-neutral shader-stage artifacts.
3. Remove or deprecate texture descriptor byte-size capabilities once descriptor heaps are
   opaque.

A fourth change is conditional: expose indirect-mesh support as a capability if the initial backend
does not simply require Apple GPU family 9.

No public change is proposed for ordinary GPU pointers, root-data layout, sampler indices, render
descriptions, barriers, or command submission.

## Deliberate scope

The port does not expand the existing API. It remains single-threaded, uses one general command
queue abstraction, and adds no public sparse resources, ray tracing, task shaders, command graphs,
queries, pipeline cache, transient-alias activation, or legacy Metal path. Such features need their
own cross-backend contracts rather than Metal-only escape hatches.

## Blockers and open decisions

The principal blocker is the Slang Metal lowering for the three-slot root and heap ABI. Without it,
Metal can execute commands but cannot preserve the data model that makes this API interesting.

Implementation must also settle:

- Apple 9 as the baseline versus a public indirect-mesh capability;
- the final texture descriptor-heap API spelling and migration from raw descriptor storage;
- the backend-neutral shader artifact and entry-point representation;
- tested format and view support for each advertised Apple GPU family;
- the `CAMetalLayer` ownership and presentation contract;
- whether copied or GPU-generated sampler entries become portable;
- whether the optional indirect texture-descriptor path is worth exposing.

None of these decisions requires adding backend metadata to user root structures or inserting a
resource-ID table into the direct texture path.

## Validation plan

Metal support remains unadvertised until representative Apple 7, 8, and 9 systems validate the
applicable paths.

The focused test plan is:

- build and run the public surface on ARM64, then compare Vulkan and Metal output from shared Slang
  sources and data structures;
- prove typed GPU pointers, address-based commands, and copy-address lookup at allocation boundaries;
- verify byte-exact roots plus direct texture and sampler indexing across encoder transitions;
- exercise rendering, copy, compute, barrier, residency, and deferred-lifetime cases;
- test grouped timelines, drawable acquisition, resizing, occlusion, and presentation order; and
- measure the direct texture path before considering an optional indirection mode.

Format coverage, alignment limits, and descriptor-view conformance should be generated from the
implementation's tables and tests rather than duplicated as a second API specification here.

## Primary references

Apple:

- [Understanding the Metal 4 core API](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api)
- [Metal feature set tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)
- [`MTLTextureViewPool`](https://developer.apple.com/documentation/metal/mtltextureviewpool)
- [`MTLHeap`](https://developer.apple.com/documentation/metal/mtlheap)
- [`MTLResidencySet`](https://developer.apple.com/documentation/metal/mtlresidencyset)
- [`MTLSharedEvent`](https://developer.apple.com/documentation/metal/mtlsharedevent)
- [`MTL4CommandQueue signalDrawable`](<https://developer.apple.com/documentation/metal/mtl4commandqueue/signaldrawable(_:)>)
- [`CAMetalLayer`](https://developer.apple.com/documentation/quartzcore/cametallayer)

Slang:

- [Descriptor-heap convenience features](https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md)
- [Metal resource descriptor heap issue](https://github.com/shader-slang/slang/issues/11540)

The Apple links document platform behavior. The Slang links describe current compiler context, not
evidence that the proposed NoGraphicsAPI Metal ABI already exists.

## Relationship to the implemented backend

The [Vulkan support document](vulkan-support.md) remains the contract for the only implemented
backend. Vulkan can adopt an opaque texture descriptor-heap API without changing the GPU-only
`TextureHeap` used for placed image storage; the change simply stops exposing descriptor bytes that
Metal cannot share.

The [No Graphics API comparison](no-graphics-api-comparison.md) explains how the current API follows
the blog proposal. This Metal design preserves its most important properties: real GPU pointers,
application-selected global resource indices, exact user root structures, explicit synchronization,
and a small command surface. The unavoidable adaptations are confined to texture view-pool writes,
native copy operands, shader artifacts, and residency.
