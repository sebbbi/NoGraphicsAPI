# Slang shader contract

`NoGraphicsAPI` uses Slang to express the shader contract behind the
[*No Graphics API*](https://www.sebastianaaltonen.com/blog/no-graphics-api) model:

- a small shared root structure for each draw or dispatch;
- typed 64-bit GPU pointers for buffers and larger data structures;
- integer indices into application-owned texture and sampler heaps; and
- C-compatible layout for every POD structure shared by C++ and Slang, selected with
  `-fvk-use-c-layout`.

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

Every shader requires `-matrix-layout-row-major` and `spvDescriptorHeapEXT`. Matrices and descriptor
heaps are part of the expected shader model, not opt-in cases. The significant choices are:

- `spirv_1_5` is the project baseline;
- the direct SPIR-V backend avoids an intermediate source-language translation;
- entry-point names are preserved because PSO creation expects `vertexMain`, `fragmentMain`,
  `meshMain`, or `computeMain`;
- `-fvk-use-c-layout` is required to give shared POD structures the C-compatible layout expected by
  C++;
- row-major matrix layout establishes the expected representation for matrix-bearing shared
  structures; and
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
or dispatch.

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

CPU-visible `GpuCpuRange<T>` values provide typed CPU and GPU addresses; their `size` remains a byte
count. The CPU writes through the CPU address and copies the GPU address into shared data. Slang sees
that field as an ordinary typed pointer:

```slang
Vertex vertex = root.vertices[vertex_id];
```

Slang lowers the pointer to SPIR-V physical-storage-buffer addressing. There is no public buffer
handle or shader buffer descriptor. GPU pointers carry no bounds, must not be dereferenced by the
CPU, and remain the application's alignment, range, synchronization, and lifetime responsibility.
Opaque textures and samplers use heap indices instead.

Use typed pointer fields instead of round-tripping addresses through `uint64_t`; the latter can add
an unnecessary `shaderInt64` requirement.

## Application-owned descriptor heaps

The application chooses slots in separate mapped texture and sampler descriptor heaps, writes them,
and binds their GPU ranges with `set_texture_descriptor_heap()` and
`set_sampler_descriptor_heap()`. Slot addresses use the corresponding descriptor size from
`DeviceCaps`.

Shaders index the heaps directly:

```slang
Texture2D<float4> texture = ResourceDescriptorHeap[texture_index];
SamplerState sampler = SamplerDescriptorHeap[sampler_index];
```

Texture and sampler indices are separate 32-bit namespaces. The texture declaration must match the
descriptor view; `TextureDescriptorDesc` selects its format, mip/layer range, and aspect. Buffer data
uses GPU pointers and needs no descriptor entry. Slot lifetime and reuse remain application policy.
`SPV_EXT_descriptor_heap` treats resource access as non-uniform by default, so no conventional
descriptor-set declaration or manual `NonUniform` decoration is needed.

## Shared data layout

Shared headers include `<NoGraphicsAPIUtility/shader_types.h>`, which provides matching plain
scalar, vector, and matrix types for C++ and Slang. Both languages use `T*` for GPU-address fields.

Compile every shader with the required `-fvk-use-c-layout` and `-matrix-layout-row-major` options.
Use the supplied types and explicit padding, and ensure every pointer field contains a GPU address
rather than a host address.

Use integer fields for shared booleans; C++ and Slang boolean-vector layouts are not compatible. The
runtime enables scalar block layout plus 16-bit arithmetic and storage for root and BDA data. The
provided C++ `float16_t` is a bit container, not a host arithmetic type. Eight-bit root and BDA
members are not currently supported by the enabled Vulkan feature set.

## Current boundaries

- Root data is CPU push data, not a GPU-resident root pointer.
- Texture and sampler heaps use native `SPV_EXT_descriptor_heap`; conventional descriptor bindings
  are outside this shader model.
- BDA pointers are unbounded and cannot point to opaque textures.
- `NoGraphicsAPI` does not support specialization constants because Slang and Vulkan do not expose
  them as a single C-compatible POD structure.
- Mesh shaders are supported; task shaders are not.

## References

- [Slang command-line reference](https://github.com/shader-slang/slang/blob/master/docs/command-line-slangc-reference.md)
- [Slang direct descriptor-heap indexing][slang-heaps]
- [Slang pointers](https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md#pointers-limited)
- [Slang SPIR-V global-memory pointers](https://github.com/shader-slang/slang/blob/master/docs/user-guide/a2-01-spirv-target-specific.md#global-memory-pointers)
- [`SPV_EXT_descriptor_heap`](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_descriptor_heap.html)
- [`VK_EXT_descriptor_heap` push-data proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_descriptor_heap.adoc)

[slang-heaps]: https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md#direct-descriptor-heap-indexing
