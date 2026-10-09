# Metal implementation

Metal implements the same GPU-pointer and descriptor-heap model as [Vulkan](vulkan-support.md).
The backend uses native Metal commands and precompiled metallibs. Applications own allocation,
descriptor indices, resource lifetime and synchronization.
See [supported devices](../README.md#metal) for the hardware baseline. Metal 4 is preferred on supported OS 26+ devices;
Metal 3 supports macOS 15+ and iOS/iPadOS 18+ on the same Apple GPU baseline.

## GPU pointers and address-based commands

`GpuHeap` exposes real 64-bit addresses from Metal buffers. Applications partition this storage and
put typed GPU pointers in shared CPU/shader structures. Shaders dereference them directly, including
vertex fetch, without buffer descriptors or per-resource binding calls. CPU-visible heaps use shared
storage; GPU-only heaps use private storage. Textures occupy application-managed placement heaps.

`TextureDesc::aliasable` permits resident textures to share a placement while their GPU uses are
disjoint. `activate_texture_alias()` issues an all-command barrier with device visibility before
the incoming alias is cleared or overwritten; prior contents are discarded by contract. Views and
descriptors stay resident. Cross-queue use requires timeline waits. This uses ordinary placement
heap aliasing, without `makeAliasable()` or sparse-resource visibility operations.

Metal 4 also accepts GPU addresses in its command API:

| NoGraphicsAPI operation | Metal 4 implementation |
| --- | --- |
| Indexed drawing | `drawIndexedPrimitives` receives the index GPU address and byte length. |
| Indirect drawing | `drawPrimitives` / `drawIndexedPrimitives` receive the argument GPU address. |
| Indirect dispatch | `dispatchThreadgroupsWithIndirectBuffer` receives the argument GPU address. |
| Indirect mesh drawing | `drawMeshThreadgroupsWithIndirectBuffer` receives the argument GPU address. |

Despite the `Buffer` names, these Metal 4 arguments are addresses, not `MTLBuffer` objects. The backend
passes them through directly. Metal 3 resolves root, index and indirect addresses to `MTLBuffer` objects and offsets.
Both versions use this mapping for copies. The shared table supports at most 64 live GPU heaps per device;
each command range must fit in one heap. Lookup uses binary search and atomic version checks without mutexes.
Table and residency-set updates are serialized; ordinary command recording and submission are not.

## Application-owned descriptor heaps

On OS 26+, both Metal 3 and Metal 4 texture descriptor heaps use `MTLTextureViewPool`, which provides
application-selected slots with contiguous resource IDs. A shader selects a texture by forming a handle from
**`baseResourceID + index`**, without a per-texture ID-table load. Older OS versions store texture-view resource IDs in an indexed GPU table.
Both paths use the same shader ABI: buffer slot 1 contains a pool base ID or a pointer to the ID table.
Sampler heaps use an indexed array of eight-byte resource IDs on both versions.

Metal does not expose writable texture-descriptor bytes. The common API therefore exposes opaque
texture and sampler heaps with indexed write/copy operations; Vulkan keeps its mapped descriptor
storage behind the same interface. Applications still own the slots and must retain their contents
and referenced textures until GPU use finishes.

Stock Slang has no texture-pool indexing operator. The shared shader header supplies the handle
reinterpretation, following the tested [MSL heap experiment](https://github.com/sebbbi/msl_heap).
`gpu_texture<T>(index)` and `gpu_sampler(index)` inline on both backends; no Slang fork is required.

## Root ABI and shared shaders

Each draw or dispatch takes an application-owned GPU root pointer and places that address in the
Metal 4 argument table, or binds its backing buffer and offset on Metal 3. All graphics stages share the root.
The backend neither allocates root storage nor copies its contents. Roots can be CPU-written through mapped memory or produced by GPU work.
See [root allocation, lifetime and synchronization](slang.md#root-and-pointer-layout).

| Metal buffer slot | Contents |
| --- | --- |
| 0 | Root arguments |
| 1 | `GPUTextureHeap`: texture-view-pool base ID or texture-ID table pointer |
| 2 | Sampler resource IDs |

`GPU_ROOT` preserves the shared C layout instead of Metal's ordinary constant-buffer vector alignment.
The same Slang sources compile to metallib and SPIR-V. The backend mirrors viewport Y, so vertex and
mesh clip positions need no platform wrapper. See the [shader guide](slang.md) for layout and compilation.
Rebuild existing Metal shaders for the shared 16-byte texture-heap header.

## Mesh work and format support

Task shaders map to Metal's object stage. Direct task/mesh drawing works throughout the supported
baseline. Native indirect mesh drawing requires Apple9 (A17 Pro / M3) or newer and is reported through
`DeviceCaps::indirect_mesh_draw`. M1/M2 applications can launch direct task groups that read GPU-produced
counts; NoGraphicsAPI does not emulate an indirect command.

Metal 4 does not imply BC texture compression support. Query the required texture formats before
choosing assets; ASTC is available throughout the baseline.

Presentation currently supports only `ColorSpace::srgb`. `create_device()` and `set_swapchain_format()`
return `Error::unsupported` for `ColorSpace::extended_srgb_linear`; EDR layer configuration is not implemented.
After `wait_idle()`, with no acquired frame, the setter accepts the current format or switches between
`Format::bgra8_unorm` and `Format::bgra8_srgb` without replacing application resources.

On macOS, Metal 4 rendering hands presentation to a classic Metal command buffer after a GPU shared-event wait.
This exposes the presentation hook used by Steam's Metal overlay; drawables allow texture views for overlay composition.
`wait_idle()` drains that presentation queue as well as rendering. iOS retains direct Metal 4 presentation.
Use three drawables to keep scanout from starving a two-frame renderer on Apple displays.

## Synchronization

On Metal 4, resource-free barriers map to Metal producer barriers. Fragment, depth and color destinations wait
at fragment/tile stages, allowing independent vertex, object and mesh work to overlap previous passes.
Compute and copy work use their own scopes. Write dependencies request device visibility;
execution-only dependencies do not flush caches.

Metal 3 uses native encoder fences for resource-free dependencies, including across command buffers;
these dependencies synchronize whole encoders and can reduce overlap compared with Metal 4 stage barriers.
Suspended render passes preserve attachment contents through native store/load operations between segments;
requested load/clear and store operations apply to the first and last segments respectively.

Metal 4 command pools map to reusable command allocators. Metal 3 records classic command buffers from
the pool's selected queue and must submit to that exact queue. Each Metal 3 queue has 4096 native command-buffer slots.
An unsubmitted or uncompleted application command buffer occupies one slot; each uncompleted submission also
uses two internal slots. Keep enough capacity available to submit recorded work; native allocation blocks when the queue is full.
Pools and queues are externally synchronized; independent pools can record concurrently.
`MTLSharedEvent` supplies cross-queue waits and CPU completion.
Resource destruction and pool reset require completion of their submitted uses.

Metal 4 timestamps use native counter heaps. After the existing completion wait, `read_timestamps(pool)`
retrieves results into CPU memory before pool reset. It adds no GPU resolve, barrier or wait, preserving
the opportunity for overlap between frames. Outside-render markers are approximate all-commands timings;
inside-render markers have a [known driver limitation](repro-metal-render-timestamps.md).
Metal 3 reports `timestamp_period_ns = 0`; timestamp calls leave their destinations unchanged because
its stage-boundary counters cannot implement arbitrary markers inside render passes.

## References

- [Metal 4 core API](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api)
- [Texture view pools](https://developer.apple.com/documentation/metal/mtltextureviewpool)
- [Metal feature tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)
- [Validation and limitations](metal-validation.md)
