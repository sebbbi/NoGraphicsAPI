# `NoGraphicsAPI` compared with *No Graphics API*

Sebastian Aaltonen's [*No Graphics API*](https://www.sebastianaaltonen.com/blog/no-graphics-api)
proposes a graphics API designed around modern bindless hardware rather than legacy resource-binding
abstractions. `NoGraphicsAPI` is a small Vulkan and Slang implementation of that idea.

The project follows the post's central data model closely: shader-visible data is addressed with
64-bit GPU pointers, texture descriptors live in application-owned heaps, pipelines have no binding
layouts, and barriers describe global execution and memory dependencies instead of resource lists.
The largest semantic adaptation is root data: the post passes GPU pointers to roots, while this
implementation copies a small CPU root structure into Vulkan push-data state.

## Fidelity at a glance

| Area | Fidelity | `NoGraphicsAPI` |
|---|---|---|
| Linear data | Direct | No public buffer objects. Application-partitioned GPU heaps expose 64-bit addresses used directly by shaders and commands. |
| Vertex and structured data | Direct | Shaders follow typed pointers and fetch their own data; PSOs have no vertex layout. |
| Texture descriptors | Direct | The application allocates, fills, indexes, and binds the texture descriptor heap. |
| Samplers | Adapted | An application-owned sampler descriptor heap replaces the post's Metal-style embedded samplers. |
| Root data | Adapted | CPU root bytes are copied with `vkCmdPushDataEXT`; the post passes GPU root pointers. |
| Pipeline binding model | Direct | Pipelines use no descriptor-set layout, pipeline layout, or resource signature. |
| Raster state | Partial | Fixed render state remains baked into PSOs rather than split or dynamic as explored by the post. |
| Barriers | Direct in spirit | One global dependency names stages and accesses, with no resource or image-layout lists. |
| Textures | Mostly direct | The application places opaque textures in its own GPU-only texture heap, restricted to one device-selected memory type. |
| Commands and completion | Mostly direct | One-shot command buffers and caller-owned timeline points provide asynchronous reuse tracking. |
| Indirect work | Partial | Arguments and indices use GPU address ranges; root selection and draw count remain CPU-controlled. |

## Vulkan foundation

The backend requires Vulkan 1.4 and four device extensions:

- `VK_EXT_descriptor_heap` for application-owned texture and sampler heaps, null-layout pipelines,
  and push data;
- `VK_KHR_device_address_commands` for address-based index binding, indirect commands, and copies;
- `VK_KHR_shader_untyped_pointers` for the descriptor-heap shader model; and
- `VK_EXT_mesh_shader` for mesh pipelines and draws.

`VK_KHR_unified_image_layouts` is enabled when available to optimize the backend's single-layout
texture model. The public API does not expose layouts either way.

The Win32 presentation path additionally uses `VK_KHR_surface`, `VK_KHR_win32_surface`,
`VK_KHR_get_surface_capabilities2`, `VK_EXT_surface_maintenance1`, `VK_KHR_swapchain`, and
`VK_EXT_swapchain_maintenance1`. `VK_EXT_debug_utils` is enabled when available for validation
diagnostics.

Vulkan core features provide buffer device addresses, dynamic rendering, synchronization2, and
maintenance5. On the shader side, ordinary GPU pointers use the SPIR-V physical-storage-buffer
model (`SPV_KHR_physical_storage_buffer`), while heap indexing uses
`SPV_EXT_descriptor_heap`. The latter implies `SPV_KHR_untyped_pointers`.

This is an unusually direct Vulkan realization of the post: the implementation creates no
`VkDescriptorSetLayout`, `VkDescriptorPool`, `VkDescriptorSet`, or `VkPipelineLayout`.

## GPU pointers are the primary data model

The most important match is the absence of public buffer objects. `create_gpu_heap()` returns a raw
`GpuHeap` containing those addresses and the exact requested byte size in `GpuHeap::range`. The
application partitions that `GpuCpuRange<byte>` and places typed GPU pointers directly in shared C++/Slang
structures:

```cpp
struct RootArguments
{
    Vertex* vertices;
    ObjectData* objects;
};
```

The CPU writes GPU addresses into the structure; the shader dereferences them as pointers. Vertex
fetch, structured data, and pointer-linked data structures therefore need no buffer bindings or
descriptor entries. This is the key novel property of both the post and this implementation.

`GpuRange {gpu, size}` is the non-owning command-side form. Vulkan's device-address commands need
both an address and an extent, so copies, index binding, and indirect operations consume ranges
without reintroducing buffer handles. A subrange is expressed by adjusting the address and size.

`MemoryType` selects mapped CPU-visible memory, GPU-only memory, readback, or one of the two
descriptor-heap usages. Each call creates one whole backing buffer and memory allocation; the backend
has no suballocator. Applications can pass `heap.range` to the companion utility library's fixed
16-byte `BumpAllocator` or `HeapAllocator`. Raw allocation returns `GpuCpuRange<byte>`, while
`allocate<T>(element_count)` returns a range with typed CPU and GPU pointers.
Applications may also supply their own policy.

As in the post, GPU pointers are raw capabilities rather than tracked references. The application
must keep their backing heaps live while recording and must synchronize suballocation mutation or
reuse. Whole-heap destruction after submission is deferred by the backend; a `GpuRange` does not add
shader bounds checking or ownership.

## Application-owned descriptor heaps

The texture descriptor heap preserves the post's ownership model:

1. The application allocates heap storage.
2. It chooses compact integer slots and writes descriptors into them.
3. Root data or GPU data carries those indices.
4. The application binds the heap range before commands that use it.

There are no descriptor sets, descriptor tables, binding layouts, or backend-managed descriptor
caches. Buffers never occupy texture-descriptor slots because shaders reach them through GPU pointers.
Vulkan defines the native descriptor bytes and alignment; the API exposes descriptor sizes and
strides so the application can lay out its own heap.

`VK_EXT_descriptor_heap` separates resource and sampler heaps. `NoGraphicsAPI` creates exact-sized,
mapped `GpuHeap` values with `MemoryType::texture_descriptor_heap` and
`MemoryType::sampler_descriptor_heap`. The first is image-only. This differs from the post's
Metal-style embedded sampler values, but retains the more important property: slot ownership and
indexing stay with the application, and pipeline creation remains binding-free.

## Root ABI

The post's root is GPU-resident. A draw supplies independent vertex and pixel root pointers, and a
dispatch supplies a compute root pointer. This permits large roots, GPU-generated roots, and
GPU-selected roots without command-buffer rewriting.

`NoGraphicsAPI` instead gives every draw or dispatch one `ByteSpan` of CPU data. Immediately before
the native command, the backend copies the bytes with `vkCmdPushDataEXT`. Shaders read the root
fields directly from push-data state:

```cpp
RootArguments root{
    .vertices = vertex_gpu,
    .objects = object_gpu,
};
gpu::draw(commands, root, vertex_count);
```

The CPU value only needs to live through the API call. GPU pointers stored inside it retain their
normal GPU-execution lifetime requirements.

The exact root contract is:

- one root block is shared by all active graphics stages;
- the byte size is a multiple of four and does not exceed `DeviceCaps::max_push_data_size`;
- typed roots are trivially copyable;
- shared structures use C layout, with row-major layout enabled for matrices; and
- `{}` represents a rootless command and emits no push-data operation.

This is a deliberate performance-oriented adaptation. Pushing the entire small root lets the shader
read fields immediately; pushing only its GPU address would add another dependent memory load.
However, it cannot represent separate vertex and fragment roots, roots larger than the device's
push-data limit, or roots generated and selected by the GPU. A future fully GPU-driven path would
need explicit GPU-root commands rather than changing this ABI implicitly.

## Pipelines and rendering

The post removes binding layouts and vertex declarations from PSOs. `NoGraphicsAPI` does the same.
Vertex shaders fetch through GPU pointers, mesh shaders use the mesh path, and texture/sampler slots
are not declared to a pipeline. Descriptor-heap pipelines are created with a null Vulkan layout.

Opaque PSO handles remain because Vulkan still compiles pipeline objects. The current prototype
bakes raster, blend, depth/stencil, and attachment compatibility into those PSOs. That is more
conventional than the post's permutation-reduction direction, which separates or programs more of
this state. Dynamic rendering still removes render-pass and framebuffer objects.

Static-constant structures are not implemented. Slang/Vulkan specialization constants do not expose
the post's single shared C-compatible structure ABI.

## Global barriers and unified layouts

The synchronization model closely follows the post. A barrier specifies producer and consumer
stages plus access types, and the backend emits one global `VkMemoryBarrier2`:

```cpp
gpu::barrier(commands,
             gpu::Stage::compute, gpu::Access::shader_write,
             gpu::Stage::fragment, gpu::Access::shader_read);
```

There are no buffer lists, texture lists, or subresource lists. The dependency covers matching
accesses whether the resource was reached through a GPU pointer, a texture descriptor, or an
attachment. Descriptor reads, indirect arguments, index input, and depth/stencil accesses have
explicit stage/access representations for Vulkan's exceptional consumers.

Normal image use stays in `GENERAL`, so applications do not track layouts.
`VK_KHR_unified_image_layouts`, when available, makes that policy efficient; initial image setup and
swapchain presentation transitions remain backend responsibilities. This preserves the post's
simple synchronization surface at the cost of global dependencies that may cover unrelated work.

The post's split barriers signal a token through a GPU pointer and wait on that address later, with
configurable operations such as atomic maximum. Vulkan has neither that GPU-address signal/wait
mechanism nor configurable token operations, so the current API exposes only unsplit global barriers.

## Commands, timelines, and indirect work

Command buffers are one-shot handles backed by reusable Vulkan contexts. Each submission consumes all
command buffers begun since the preceding submission, in the supplied order.

Applications provide a monotonically increasing `TimelinePoint` with every submission. Polling or
waiting that point controls reuse of application-owned heap ranges, texture placements, descriptor
slots, and readback data. Internal command-context and Vulkan-object retirement uses a separate
private timeline. This keeps frames asynchronous and matches the post's recommendation that
completion be explicit.

`VK_KHR_device_address_commands` extends the GPU-pointer model into command processing. Index data,
indirect argument records, and copy operands are supplied as `GpuRange` values and passed to Vulkan
without an allocation lookup.

The current indirect commands still receive root bytes and draw count from the CPU. They do not yet
implement the post's GPU root arrays, independent vertex/pixel root strides, GPU draw-count pointer,
or GPU-selected roots. Thus indirect arguments are address-based, but the complete GPU-driven model
from the post is only partially present.

## Textures and presentation

Textures remain opaque objects because Vulkan images need creation metadata, binding, views, and
lifetime management, but their storage placement is application-owned. A `TextureHeap` value owns
one raw allocation of a device-selected GPU-only image-memory type, separate from the texture descriptor
heap. `DeviceCaps::texture_heap_alignment` is a worst-case alignment for every supported texture, so
the utility `TextureAllocator` can suballocate without leading alignment padding. It owns the placement
policy for one texture heap: `allocate()` queries `SizeAlign` and hides element rounding and the Vulkan
offset, while `free()` destroys the `PlacedTexture` and returns its range. The backend contains no
texture suballocator.

There are no CPU-visible or readback texture heaps. Texture upload and readback use
`copy_memory_to_texture()` and `copy_texture_to_memory()` with buffer-backed `GpuRange` storage.
Destroying the wrapper does not make an in-flight placement reusable; the application retires that
range with its submission timeline. Every sampled or storage descriptor remains separately owned
and placed by the application.

Presentation is outside the post. The project adds a compact Win32 swapchain boundary with explicit
extent query, acquire, and present operations. WSI synchronization, transitions, and recreation stay
internal. Other window systems are outside the current prototype.

## Deliberate adaptations and remaining gaps

The implementation deliberately uses CPU push-data roots, one shared graphics root, a separate
sampler descriptor heap, one GPU-only texture memory type, and Vulkan stage/access masks. These
preserve the post's main model while adapting it to `VK_EXT_descriptor_heap` and synchronization2.

**Prototype scope.** Vulkan already supports task shaders, public queue selection and multiple queues,
GPU-addressed indirect draw counts through `vkCmdDraw*IndirectCount2KHR`, and optional BDA
capture/replay address preservation. This backend does not expose them. Fixed render and attachment
state in PSOs is also largely a current implementation choice.

**Available through an additional extension.** GPU-root indirect work has an existing but heavier
path: `VK_EXT_device_generated_commands` and its descriptor-heap push-data tokens can source
per-sequence root data from GPU memory. That optional extension is outside this backend. Vulkan
specialization constants already provide the optimization behind static constants; exposing the
post's shared C-compatible structure ABI is library and Slang work rather than a missing Vulkan
primitive.

**Vulkan limitations.** Two features from the post have no faithful Vulkan representation:

- descriptor heaps are GPU-writable and copyable, but native descriptors can only be constructed by
  host calls; a shader descriptor-construction intrinsic needs new Vulkan and SPIR-V support; and
- Vulkan has no synchronization command that signals or waits on a token at a GPU address, or selects
  atomic and comparison operations such as atomic maximum and greater-or-equal. The post's split
  barrier model needs a new synchronization extension.

These two Vulkan gaps are not prototype backlog. Future work should preserve the defining invariants:
GPU pointers for linear data, application-owned descriptor slots, pipelines without binding layouts,
and synchronization without resource lists.

## Related documentation

- [`NoGraphicsAPI.hpp`](../include/NoGraphicsAPI/NoGraphicsAPI.hpp) is the public API.
- [Vulkan support](vulkan-support.md) records extension contracts and backend behavior.
- [Slang shader contract](slang.md) records compilation, heap syntax, shared layout, and pointer rules.
