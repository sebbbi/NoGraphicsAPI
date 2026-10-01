# Shared Slang shader contract

NoGraphicsAPI shaders share C-compatible root structures, real 64-bit GPU pointers, and separate
texture/sampler index namespaces across Vulkan and Metal 3/4. Include
`<NoGraphicsAPI/shader.slang>` for the target ABI and
`<NoGraphicsAPIUtility/shader_types.h>` in CPU/GPU shared data headers.

```slang
#include <NoGraphicsAPI/shader.slang>
#include "example_shared.h"

GPU_ROOT(ExampleRoot, root);

// Ordinary data remains pointer-based; only textures and samplers occupy heap slots.
Vertex vertex = root.vertices[vertex_index];
Texture2D<float4> texture = gpu_texture<Texture2D<float4>>(root.texture_index);
SamplerState sampler = gpu_sampler(root.sampler_index);
```

Use `gpu_sampler<SamplerComparisonState>(index)` for comparison samplers. Texture types must match
the indexed view. Helpers inline on both backends; `gpu_nonuniform_index(index)` preserves explicit
nonuniform annotations. See [Metal implementation](metal-support.md) for texture-pool indexing.

## Root and pointer layout

Define each root once in the shader's matching shared header. All draw and dispatch variants take
a 16-byte-aligned GPU pointer to that structure, or `nullptr` for rootless shaders. No allocation or
root copy happens inside the graphics API. The root and its referenced data must stay valid until
their consuming submissions finish. For CPU-provided values that differ between commands, allocate separate records.

For transient CPU-written roots, use `BumpAllocator` over mapped frame storage:

```cpp
const gpu::GpuCpuRange<ExampleRoot> root = frame_data.allocate<ExampleRoot>();
*root.cpu = {.vertices = vertices.gpu, .texture_index = texture_index};
gpu::dispatch(commands, root.gpu, {.x = groups, .y = 1, .z = 1});
```

Typed `allocate<T>()` and `allocate_atomic<T>()` default to one element. Reclaim frame storage only
after its timeline completes. Use ordinary allocation for a single recording thread; concurrent
workers can reserve disjoint chunks atomically and use a separate ordinary bump allocator per chunk.
The allocator is linear, not a wrapping ring, and never grows its storage.

GPU shaders can also produce the root contents. Synchronize writes to the consuming shader stages
with `Access::shader_read`, then pass that GPU address directly to a draw or dispatch. There is no
256-byte API limit; roots obey native shader resource limits. The CPU selects the bound address;
additional pointers inside the root can be selected by GPU work.

`GPU_ROOT(Type, name)` selects these bindings:

| Target | Root | Texture namespace | Sampler namespace |
| --- | --- | --- | --- |
| Vulkan | Uniform buffer at binding 0, mapped to the 64-bit address in push data | Native resource descriptor heap | Native sampler descriptor heap |
| Metal | `StructuredBuffer<T>` at buffer 0, preserving C layout | `GPUTextureHeap` at buffer 1 | Typed sampler entries at buffer 2 |

`GPUTextureHeap` contains a 64-bit pool base and a GPU pointer to resource IDs. Both Metal 3 and Metal 4 use
the base on OS 26+; older OS versions use the table. Rebuild Metal shaders to adopt this shared 16-byte header.

`GPU_ROOT` preserves the shared C++ layout, including vectors, matrices and pointers, without adding
backend fields. Plain Metal `ConstantBuffer<Type>` can use different alignment.

The optional third argument declares the root address alignment in bytes; it defaults to four:

```slang
GPU_ROOT(ExampleRoot, root, 16);
```

Use a power of two at least four and satisfy both that promise and the type's natural alignment.
This does not change field offsets or allocate memory. The draw/dispatch API still requires a
16-byte-aligned root address, so its callers can specify 16 explicitly.

Vulkan uses a uniform-buffer binding; the alignment argument does not change its code generation.

Metal keeps an ordinary typed load for the two-argument default; explicit alignment hints use
`__builtin_assume` on the generated packed storage pointer.
The root remains in the `device` address space. `constant` can enable uniform-register preloading,
but Slang's `ConstantBuffer` lowering currently changes the shared vector/matrix layout, including
when `ScalarDataLayout` is requested. The alignment hint preserves that layout; it does not request
constant-address-space access or guarantee vector loads. Source generation is checked with Slang
2026.18.2; native root-data and ABI tests pass on M3 Max. No Metal performance gain is claimed.

Recompile Vulkan shaders when adopting this ABI: the push payload is now eight bytes, not the root
structure itself. The binding uses `VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT`; no buffer descriptor is allocated.

`GPU_ADDRESS(value)` forms an address for shared code such as
`loadAligned<16>(GPU_ADDRESS(root.camera->position))`. It preserves Vulkan's explicit load/store
alignment and selects Slang's internal address operation on Metal. Integer address round-trips are
unnecessary. Use shared integer fields for booleans and retain CPU size/offset checks.

Write vertex and mesh `SV_Position` outputs directly. The Metal backend maps each viewport
`(x, y, width, height)` to `(x, y + height, width, -height)`, preserving Vulkan's screen coordinates.
This applies to default and explicit viewports; shaders need no Y-flip helper or compiler option.
`gpu_wave_prefix_count_bits` covers `WavePrefixCountBits`, whose Metal definition is absent.

## Build and stage artifacts

Vulkan requires Slang 2026.14.1+ and SPIRV-Tools 2026.3+. Metal requires stock Slang 2026.18.2+ and
Xcode 26+; compile Metal 3.0 metallibs for macOS 15+ or iOS 18+ to support both command backends.
No Slang fork or generated-source rewriting is required.
Both targets use `-matrix-layout-row-major`; Vulkan additionally requires `-fvk-use-c-layout`.

```sh
slangc shader.slang -target spirv -profile spirv_1_5 -emit-spirv-directly \
  -fvk-use-entrypoint-name -fvk-use-c-layout -matrix-layout-row-major \
  -capability spvDescriptorHeapEXT -entry computeMain -stage compute -o shader.comp.spv
spirv-val --target-env vulkan1.4 --scalar-block-layout shader.comp.spv

slangc shader.slang -target metal -DNOGRAPHICSAPI_METAL -matrix-layout-row-major \
  -entry computeMain -stage compute -o shader.comp.metal
xcrun -sdk macosx metal -std=metal3.0 -target air64-apple-macos15.0 -c shader.comp.metal -o shader.comp.air
xcrun -sdk macosx metallib shader.comp.air -o shader.comp.metallib
```

For iOS 18+ device libraries, use `-sdk iphoneos` in both Xcode commands and replace the target with
`-target air64-apple-ios18.0`. Shared sources and shader metadata stay the same;
the resulting metallib is specific to its target platform.

Task and mesh Vulkan stages also require `spvMeshShadingEXT`. Entry names are preserved on both
targets. Assign whole vertex structs to mesh outputs; Metal does not accept field-wise output writes.

`ShaderStage` carries bytes, an entry name, and compiled threadgroup size. Apple devices accept
precompiled metallib; Vulkan accepts SPIR-V. Debug builds assert the expected artifact header.
On Metal, compute, task and mesh `threadgroup_size` must match `numthreads`; use the same shared
constant at shader and PSO call sites. Vulkan reads the dimensions from SPIR-V. Shader compilation
belongs to the build system; the API performs no runtime SPIR-V translation. The library has no built-in shader programs; `shader.slang` supplies helpers for application shaders.

## References

- [Slang target interoperation](https://shader-slang.org/slang/user-guide/a1-04-interop.html)
- [Slang Metal target](https://github.com/shader-slang/slang/blob/master/docs/user-guide/a2-02-metal-target-specific.md)
- [SPV_EXT_descriptor_heap](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_descriptor_heap.html)
- [Slang aligned loads](https://docs.shader-slang.org/en/latest/external/core-module-reference/global-decls/loadaligned-4.html)
- [Apple constant-address-space guidance](https://developer.apple.com/videos/play/wwdc2020/10632/)
