# Comparison with *No Graphics API*

Sebastian Aaltonen's [*No Graphics API*](https://www.sebastianaaltonen.com/blog/no-graphics-api)
proposes GPU pointers, application-owned descriptor heaps and synchronization without resource lists.
NoGraphicsAPI implements that model with Vulkan 1.4 and Metal 3/4. One GPU root is shared across
graphics stages; each draw or dispatch takes an application-owned GPU pointer directly.

| Area | NoGraphicsAPI |
| --- | --- |
| Linear data | Application-partitioned GPU heaps expose 64-bit pointers; there are no public buffer objects. |
| Vertex data | Shaders fetch through pointers; PSOs have no vertex layout. |
| Textures and samplers | Separate application-owned indexed heaps; native descriptor bytes remain private. |
| Root data | One GPU pointer passed to each draw/dispatch, shared across active graphics stages; no backend allocation or copy. |
| Pipelines | No application binding layout; rasterization, blending and attachment formats remain in PSOs. |
| Dynamic state | Viewport, scissor and exposed depth/stencil state are command state. |
| Barriers | Global stage/access dependencies, without resource or image-layout lists. |
| Texture storage | Application places opaque textures in GPU-only texture heaps. |
| Commands | Reusable command pools, one-shot buffers, multiple queues and caller-owned timeline points. |
| Indirect work | Address-based arguments and GPU-written roots; root binding and draw count remain CPU-controlled. |

## GPU pointers and descriptor heaps

`create_gpu_heap()` provides mapped CPU-visible, GPU-only or readback storage. Applications partition
it and put typed GPU pointers directly in shared CPU/shader structures. Buffer data occupies no
descriptor slots. `GpuRange {gpu, size}` supplies a non-owning address interval to copy, index and
indirect commands. The optional utility library provides allocation policies; the graphics API does
not suballocate application data or track pointer lifetimes.

Textures and samplers occupy application-selected descriptor indices. Opaque heaps provide indexed
writes, range copies and binding, without descriptor sets, per-material binding tables or pipeline
resource signatures. Metal uses texture-view pools on OS 26+, texture resource-ID tables on older OS versions,
and sampler resource IDs. Vulkan uses native `VK_EXT_descriptor_heap` storage. The public contract exposes CPU descriptor operations, not arbitrary
CPU/GPU descriptor-byte mutation. This retains slot ownership while supporting both representations.

Unlike the post's embedded sampler values, sampler indices refer to a separate application-owned
heap. Texture views can select compatible formats, mip/layer ranges and aspects. Applications own
slot reuse and referenced texture lifetimes.

## Root ABI and pipelines

Each draw or dispatch takes a GPU pointer to an application-owned root. Pointer fields reference GPU
storage, and descriptor fields hold indices. The root and referenced resources survive GPU completion.
Mapped frame storage and `BumpAllocator::allocate<T>()` provide transient roots without per-command driver allocation.

Roots use a shared aligned layout and row-major matrices; pointed-to data keeps C POD layout. Vulkan pushes the root address, Metal 4 updates
its argument table, and Metal 3 binds the backing buffer and offset. GPU work can write root contents before
later commands consume them. All graphics stages share one root; separate stage roots and GPU-generated binding commands are not exposed.

`ShaderStage` contains precompiled bytes, an entry name and compute/task/mesh threadgroup dimensions.
Vulkan accepts SPIR-V; Metal accepts metallib. Both use the same Slang source and root structures.
There is no runtime shader compilation or SPIR-V translation.

PSOs retain rasterization, blend and attachment compatibility because the native APIs compile those
states into pipelines. Viewport, scissor and exposed depth/stencil state remain independent. The
post's shared static-constant structure ABI is not implemented; specialization constants alone do
not provide that common C-compatible interface.

## Synchronization and submission

A barrier names stages and accesses without identifying resources:

```cpp
gpu::barrier(commands, gpu::Stage::compute, gpu::Access::shader_write,
             gpu::Stage::fragment, gpu::Access::shader_read);
```

Vulkan emits a global memory barrier and keeps ordinary images in `GENERAL`; optional unified image
layouts optimize this policy. Initial texture and presentation transitions remain internal. Metal 4 maps the same scopes
to native stage dependencies; Metal 3 uses encoder fences. Applications describe actual hazards and use timeline waits between queues.

The post's split barriers can signal and wait on tokens at GPU addresses. That interface is not
exposed: Vulkan has no equivalent command with the proposed configurable atomic/comparison operations.

Applications record through externally synchronized command pools and submit ordered buffers to
selected queues. General, compute and copy queue families are supported. Completion timeline points
control command-pool reset and reuse of upload ranges, descriptors, indirect arguments and texture
placements. Resource destruction is immediate; deferred deletion is an optional utility policy.

Index data, indirect arguments and copies use GPU ranges. Vulkan device-address commands consume
them directly when available. Otherwise Vulkan and Metal 3 resolve backing buffers and offsets through
the same table used for Metal copies. Metal 4 draws and dispatches retain native address commands. Indirect
mesh drawing is optional on Metal Apple7/Apple8 and reported by `DeviceCaps::indirect_mesh_draw`.
GPU draw-count pointers, root arrays and per-stage root strides remain outside the public interface.

## Textures and presentation

Opaque textures retain native creation metadata and views, but placement belongs to the application.
`TextureHeap` is storage, separate from a texture descriptor heap. Vulkan selects one compatible
GPU-only texture memory type; Metal uses private placement heaps. Placement requirements come from
`get_texture_size_align`; the utility `TextureAllocator` manages reusable placements.

Textures have no CPU mapping. Upload and readback use GPU address ranges, and their lifetimes follow
submission timelines. The backend does not track texture-to-heap or texture-to-descriptor dependencies.

Presentation is outside the post. NoGraphicsAPI provides a Win32 Vulkan swapchain and Metal
`CAMetalLayer` acquisition/presentation. Applications own native windows/layers; the library handles
native presentation synchronization and image transitions.

## Native API requirements and limits

Vulkan requires `VK_EXT_descriptor_heap`, `VK_KHR_shader_untyped_pointers`
and `VK_EXT_mesh_shader`, plus the core features documented in
[Vulkan support](vulkan-support.md). Descriptor-heap pipelines use no `VkDescriptorSetLayout`,
`VkDescriptorPool`, `VkDescriptorSet` or `VkPipelineLayout`.

The Metal mapping and hardware limits are documented in [Metal support](metal-support.md). Neither backend exposes every
possible native operation. GPU-generated binding commands would need an additional interface, such as
Vulkan device-generated commands; they do not follow implicitly from GPU-written root contents.

- [Public API](../include/NoGraphicsAPI/NoGraphicsAPI.hpp)
- [Shared shader contract](slang.md)
