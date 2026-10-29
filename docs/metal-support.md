# Metal implementation

Metal implements the same GPU-pointer and descriptor-heap model as [Vulkan](vulkan-support.md).
The backend uses native Metal commands and precompiled metallibs. Applications own allocation,
descriptor indices, resource lifetime and synchronization.
Requires Metal 4, macOS, iOS or iPadOS 26+, and Apple GPU family 7 or newer.
See [supported devices](../README.md#metal) for the hardware baseline.

## GPU pointers and address-based commands

`GpuHeap` exposes real 64-bit addresses from Metal buffers. Applications partition this storage and
put typed GPU pointers in shared CPU/shader structures. Shaders dereference them directly, including
vertex fetch, without buffer descriptors or per-resource binding calls. CPU-visible heaps use shared
storage; GPU-only heaps use private storage. Textures occupy application-managed placement heaps.

Texture creation takes a device and records no GPU commands. `initialize_texture()` is a no-op on Metal;
portable applications call it before first GPU use and after writes through an overlapping texture.
Textures may share placement storage while their GPU uses are disjoint. The caller synchronizes prior
storage accesses with ordinary barriers or timeline waits, then initializes and clears or overwrites
the incoming texture. Views and descriptors stay resident. This uses ordinary placement heap aliasing,
without `makeAliasable()` or sparse-resource visibility operations.

Metal 4 also accepts GPU addresses in its command API:

| NoGraphicsAPI operation | Metal 4 implementation |
| --- | --- |
| Indexed drawing | `drawIndexedPrimitives` receives the index GPU address and byte length. |
| Indirect drawing | `drawPrimitives` / `drawIndexedPrimitives` receive the argument GPU address. |
| Indirect dispatch | `dispatchThreadgroupsWithIndirectBuffer` receives the argument GPU address. |
| Indirect mesh drawing | `drawMeshThreadgroupsWithIndirectBuffer` receives the argument GPU address. |

Despite the `Buffer` names, these Metal 4 arguments are addresses, not `MTLBuffer` objects. The backend
passes them through directly. Copy commands resolve GPU ranges to backing buffers and offsets.
The shared address table supports at most 64 live GPU heaps per device;
each command range must fit in one heap. Lookup uses binary search and atomic version checks without mutexes.
Table and residency-set updates are serialized; ordinary command recording and submission are not.

## Application-owned descriptor heaps

Texture descriptor heaps use `MTLTextureViewPool`, which provides
application-selected slots with contiguous resource IDs. A shader selects a texture by forming a handle from
**`baseResourceID + index`**. Buffer slot 1 contains the 64-bit pool base ID.
Sampler heaps use an indexed array of eight-byte resource IDs.

Metal does not expose writable texture-descriptor bytes. The common API therefore exposes opaque
texture and sampler heaps with indexed write/copy operations; Vulkan keeps its mapped descriptor
storage behind the same interface. Applications still own the slots and must retain their contents
and referenced textures until GPU use finishes.

Stock Slang has no texture-pool indexing operator. The shared shader header supplies the handle
reinterpretation, following the tested [MSL heap experiment](https://github.com/sebbbi/msl_heap).
`gpu_texture<T>(index)` and `gpu_sampler(index)` inline on both backends; no Slang fork is required.

## Root ABI and shared shaders

Each draw or dispatch takes an application-owned GPU root pointer and places that address in the
Metal 4 argument table. All graphics stages share the root.
The backend neither allocates root storage nor copies its contents. Roots can be CPU-written through mapped memory or produced by GPU work.
See [root allocation, lifetime and synchronization](slang.md#root-and-pointer-layout).

| Metal buffer slot | Contents |
| --- | --- |
| 0 | Root arguments |
| 1 | 64-bit texture-view-pool base ID |
| 2 | Sampler resource IDs |

`GPU_ROOT` reads the application's buffer through Metal's `constant` address space. Root fields follow the
[shared constant-buffer layout](slang.md#root-and-pointer-layout); pointed-to data retains C POD layout.
The same Slang sources compile to metallib and SPIR-V. The backend mirrors viewport Y, so vertex and
mesh clip positions need no platform wrapper. See the [shader guide](slang.md) for layout and compilation.

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

Metal 4 presents directly with framebuffer-only drawables by default. On macOS, set
`DeviceDesc::steam_overlay_compatibility = true` to allow drawable texture views and use Steam's classic Metal presentation hook.
Rendering stays on Metal 4. GPU-only events hand work to the classic presentation buffer and back to the render queue,
which signals drawable readiness after the overlay hook's commands finish. A following classic buffer signals completion so
writes appended by the `commit` hook are included. Unlike a shared-event wait, this allows presentation to be scheduled before
rendering completes. Application completion timelines still cover application rendering.
This adds two submissions per frame and gives up framebuffer-only drawable optimizations. `wait_idle()` also drains the presentation queue.
The flag has no effect on Vulkan, iOS or headless devices.
Use three drawables to keep scanout from starving a two-frame renderer on Apple displays.

## Synchronization

Compute encoders open on the first dispatch or copy and remain open across barriers until a
render pass, timestamp or command-buffer end closes them. Independent dispatches can overlap;
dependent accesses need explicit barriers. Additional compute queues are supported, with timeline waits between queues.
Compute bindings survive internal encoder splits, but are cleared at explicit render-pass boundaries.

Barriers combine intra-encoder dependencies with queue scopes covering earlier and later encoders, including subsequent submissions.
Between passes, barriers accumulate by destination stage and visibility until the next real encoder starts;
only a trailing barrier with no encoder requires its own encoder. Fragment, depth and color destinations wait
at fragment/tile stages, allowing independent vertex, object and mesh work to overlap previous passes.
Compute and copy work use their own scopes. Write dependencies request device visibility;
execution-only dependencies do not flush caches.

Suspended render passes use native suspend/resume encoding; requested load/clear and store operations apply
to the first and last segments respectively.

Metal 4 command pools map to reusable command allocators.
Pools and queues are externally synchronized; independent pools can record concurrently.
GPU-signaled shared events provide cross-queue synchronization and CPU completion waits.
Resource destruction and command-pool reset require completion of submitted uses; destruction remains immediate.

Each render pass starts with no pipeline bound, including resumed segments. Bind a graphics pipeline inside the pass before drawing.

Metal 4 timestamps use native counter heaps. After the existing completion wait, `read_timestamps(pool)`
retrieves results into CPU memory before pool reset. It adds no GPU resolve, barrier or wait, preserving
the opportunity for overlap between frames. Outside-render markers are approximate all-commands timings;
inside-render markers have a [known driver limitation](repro-metal-render-timestamps.md).

## References

- [Metal 4 core API](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api)
- [Texture view pools](https://developer.apple.com/documentation/metal/mtltextureviewpool)
- [Metal feature tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)
- [Validation and limitations](metal-validation.md)
