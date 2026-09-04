# Slang shader contract

`NoGraphicsAPI` uses Slang to express the three data paths central to the
[*No Graphics API*](https://www.sebastianaaltonen.com/blog/no-graphics-api) model:

- a small shared root structure for each draw or dispatch;
- typed 64-bit GPU pointers for buffers and larger data structures; and
- integer indices into application-owned texture and sampler heaps.

The Vulkan backend has no application-visible descriptor sets, descriptor pools, buffer bindings,
or pipeline layouts.

## Toolchain and compilation

The examples require Slang 2026.14.1 or newer and SPIRV-Tools 2026.3 or newer. Slang 2026.14.1 is
the first supported release with the descriptor-heap lowering fixes needed by this project. Every
generated module is passed through `spirv-val` during the build.

The common Slang options are:

```sh
slangc shader.slang \
  -target spirv \
  -profile spirv_1_5 \
  -emit-spirv-directly \
  -fvk-use-entrypoint-name \
  -fvk-use-c-layout \
  -matrix-layout-row-major \
  -capability spvDescriptorHeapEXT \
  -entry fragmentMain \
  -stage fragment \
  -o shader.frag.spv
```

Only shaders that use shared matrices need `-matrix-layout-row-major`, and only shaders accessing a
descriptor heap need `spvDescriptorHeapEXT`. The significant choices are:

- `spirv_1_5` is the project baseline;
- the direct SPIR-V backend avoids an intermediate source-language translation;
- entry-point names are preserved because PSO creation expects `vertexMain`, `fragmentMain`,
  `meshMain`, or `computeMain`;
- C layout and row-major matrices establish the expected layout for supported shared types; and
- `spvDescriptorHeapEXT` enables native `ResourceDescriptorHeap` and `SamplerDescriptorHeap` syntax
  and brings in the required untyped-pointer capability.

Mesh shaders additionally request `spvMeshShadingEXT`. The current mesh path has no task stage and
expects triangle output.

## Root ABI

A root structure is declared once in a header included by C++ and Slang:

```cpp
struct RootArguments
{
    Vertex* vertices;
    float4x4 transform;
};
```

The CPU fills pointer fields with GPU virtual addresses and passes the structure directly to a draw
or dispatch:

```cpp
RootArguments root{
    .vertices = vertices.gpu,
    .transform = transform,
};
gpu::draw(commands, root, vertex_count);
```

The shader declares the same structure in push-constant storage:

```slang
[[vk::push_constant]]
ConstantBuffer<RootArguments> root;
```

Each draw or dispatch copies the complete root with `vkCmdPushDataEXT` immediately before the native
command. A rootless call passes `{}` and emits no push-data command. The CPU value can be stack-local
because its bytes are consumed during the call. Resources referenced by its pointer fields remain
live through submission, and mutable contents remain stable until the timeline point completes.

This differs deliberately from the blog's GPU-resident, stage-specific roots. Vulkan push data
cannot source its payload from GPU memory, so `NoGraphicsAPI` uses one small CPU root shared by the
active graphics stages. Arbitrary larger structures remain reachable through GPU pointers, without
a binding layout.

Root structures must be trivially copyable, use a byte size divisible by four, and fit
`DeviceCaps::max_push_data_size`.

## Typed GPU pointers

`gpu_malloc<T>()` returns both address domains when the allocation is CPU-visible:

```cpp
gpu::GpuAllocation<Vertex> vertices = gpu::gpu_malloc<Vertex>(device, vertex_count);
RootArguments root{.vertices = vertices.gpu};
```

The CPU writes through `vertices.cpu` but treats `vertices.gpu` only as GPU address bits. Slang sees
the shared field as an ordinary typed pointer:

```slang
Vertex vertex = root.vertices[vertex_id];
```

Slang lowers the pointer to SPIR-V physical-storage-buffer addressing. This is the project's buffer
model: there is no public buffer handle or shader buffer descriptor. Index buffers, indirect
arguments, and copies similarly use `GpuRange {gpu, size}`, which maps to Vulkan device-address
commands.

GPU pointers are intentionally low level:

- they carry no bounds and robust descriptor access does not protect an invalid dereference;
- pointee alignment, range validity, synchronization, and lifetime belong to the application;
- a GPU pointer must never be dereferenced by the CPU; and
- opaque textures and samplers are represented by heap indices rather than pointers.

Use typed pointer fields instead of round-tripping addresses through `uint64_t`; the latter can add
an unnecessary `shaderInt64` requirement.

## Application-owned descriptor heaps

Texture and sampler heaps are mapped `GpuAllocation` values. The application allocates their
storage, chooses slots, passes each mapped slot to the descriptor writer, and binds the GPU ranges
as command state:

```cpp
gpu::set_texture_heap(commands, gpu::gpu_range(texture_heap));
gpu::set_sampler_heap(commands, gpu::gpu_range(sampler_heap));
```

Shaders index the heaps directly:

```slang
Texture2D<float4> texture = ResourceDescriptorHeap[texture_index];
SamplerState sampler = SamplerDescriptorHeap[sampler_index];
```

Texture and sampler indices are separate 32-bit namespaces. Buffer descriptors are unnecessary
because buffer data uses GPU pointers. The texture declaration must match the descriptor's texture
shape, while descriptor creation selects the view needed by the application.

Heap sizing, slot allocation, and slot reuse are application policy. Heap and texture handles remain
live through submission, and mutable slots are not overwritten until the application's timeline
point completes. Destruction after submission uses deferred retirement. `SPV_EXT_descriptor_heap`
treats resource access as non-uniform by default; no conventional descriptor-set declaration or
manual `NonUniform` decoration is needed.

## Shared data layout

Shared headers include `<NoGraphicsAPI/shader_types.h>`, which provides matching plain scalar,
vector, and matrix types for C++ and Slang. Both languages use `T*` for GPU-address fields. CPU-only
math lives in `<NoGraphicsAPI/math.hpp>` and is not part of the shared ABI.

Compile every shader that reads a shared structure with `-fvk-use-c-layout`. Add
`-matrix-layout-row-major` when matrices are present. Use the supplied types and explicit padding,
and ensure every pointer field contains a GPU address rather than a host address.

Use integer fields for shared booleans; C++ and Slang boolean-vector layouts are not compatible. The
runtime enables scalar block layout plus 16-bit arithmetic and storage for root and BDA data. The
provided C++ `float16_t` is a bit container, not a host arithmetic type. Eight-bit root and BDA
members are not currently supported by the enabled Vulkan feature set.

## Current boundaries

- Root data is CPU push data, not a GPU-resident root pointer.
- Texture and sampler heaps use native `SPV_EXT_descriptor_heap`; conventional descriptor bindings
  are outside this shader model.
- BDA pointers are unbounded and cannot point to opaque textures.
- The API exposes no specialization-constant block. Per-draw variation belongs in the root; adding
  scalar PSO constants would require a separate explicit ABI.
- Mesh shaders are supported; task shaders are not.

## References

- [Slang command-line reference](https://github.com/shader-slang/slang/blob/master/docs/command-line-slangc-reference.md)
- [Slang direct descriptor-heap indexing][slang-heaps]
- [Slang pointers](https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md#pointers-limited)
- [Slang SPIR-V global-memory pointers](https://github.com/shader-slang/slang/blob/master/docs/user-guide/a2-01-spirv-target-specific.md#global-memory-pointers)
- [`SPV_EXT_descriptor_heap`](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_descriptor_heap.html)
- [`VK_EXT_descriptor_heap` push-data proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_descriptor_heap.adoc)

[slang-heaps]: https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md#direct-descriptor-heap-indexing
