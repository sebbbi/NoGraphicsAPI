# Shared Slang shader contract

NoGraphicsAPI shaders share aligned root structures, real 64-bit GPU pointers, and separate
texture/sampler index namespaces across Vulkan and Metal 4. Include
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
a GPU pointer aligned to `DeviceCaps::root_alignment`, or `nullptr` for rootless shaders. No allocation or
root copy happens inside the graphics API. The root and its referenced data must stay valid until
their consuming submissions finish. For CPU-provided values that differ between commands, allocate separate records.

For transient CPU-written roots, use `BumpAllocator` over mapped frame storage:

```cpp
gpu::BumpAllocator frame_data(mapped_frame, gpu::get_device_caps(device).root_alignment);
const gpu::GpuCpuRange<ExampleRoot> root = frame_data.allocate_root<ExampleRoot>();
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
| Vulkan descriptor heap | Uniform buffer at binding 0, mapped to the 64-bit address in push data | Native resource descriptor heap | Native sampler descriptor heap |
| Vulkan descriptor buffer | Typed GPU pointer in eight bytes of push constants, loaded with the declared root alignment | Sampled/storage arrays at sets 1/2, binding 0 | Sampler array at set 3, binding 0 |
| Metal | `ConstantBuffer<T>` at buffer 0 in the `constant` address space | 64-bit texture-pool base at buffer 1 | Typed sampler entries at buffer 2 |

Metal 4 selects textures using `baseResourceID + index`.

Roots share a constant-buffer layout on both targets:

- Keep the root layout and total byte size 16-byte aligned. Each bound address must also meet the device's root alignment.
- Align 32-bit two-component vectors to eight bytes and four-component vectors to 16 bytes.
  Scalars and pointers use their natural alignment. Add explicit padding to the shared C++/Slang declaration.
- Use `float4`/`uint4` instead of three-component root vectors. Inline row-major matrices have four columns
  (`float3x4` or `float4x4`). Arrays use the native element stride, including four bytes for `uint32` arrays.
- Embedded structs follow the same rules. Reference ordinary packed data through pointers instead.

This contract applies only to roots. Pointed-to data keeps C POD layout, including packed `float3`,
`float3x3`, and arrays; pointers inside a Metal constant root still address ordinary `device` storage.
Root contents must remain unchanged during their shader use. GPU writes before the next use are supported
with the barriers described above, including rewriting the same root address between dispatches.

The optional third argument declares the root address alignment in bytes; it defaults to 16:

```slang
GPU_ROOT(ExampleRoot, root, 16);
```

Declare a power of two supported by the actual address. The shared ABI guarantees at least 16 bytes.
Metal applies `__builtin_assume` to the constant pointer. Vulkan's uniform-buffer binding is unchanged.
Neither backend allocates or copies root storage; all stages consume the application's GPU address directly.

On macOS, `tools/check_root_layouts.py` scans `GPU_ROOT` declarations and checks every field, nested field,
and array stride using compiled C++, SPIR-V reflection, and native Metal compiler assertions. Pass shader/header
directories, `--include` paths, `--slang /path/to/slangc`, and `--output build/root-layouts`. Use this after editing roots;
16-byte total size alone cannot detect incompatible field offsets.

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

Use `NoGraphicsAPI_compile_slang` with either `add_subdirectory(NoGraphicsAPI)` or the installed
`find_package(NoGraphicsAPI CONFIG REQUIRED)` package. It is available without enabling examples or tests.

```cmake
find_package(NoGraphicsAPI CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE NoGraphicsAPI::NoGraphicsAPI)
set(program "${CMAKE_CURRENT_BINARY_DIR}/shader.comp.${NOGRAPHICSAPI_SHADER_EXTENSION}")
NoGraphicsAPI_compile_slang("${program}" shader.slang computeMain compute)
target_sources(my_app PRIVATE "${program}")
```

The helper tracks included files and accepts `INCLUDE_DIRECTORIES`, `DEFINES`, `OPTIONS` (additional Slang flags),
and `DEPENDS` (for generated inputs). Build the application normally; shader variants need no separate targets.
Stage the resulting artifact with the application's other assets.
When independent executables share an artifact, give it one `add_custom_target(shaders DEPENDS "${program}")`
and make each executable depend on that target with `add_dependencies`; this prevents concurrent writes in parallel MSBuild builds.

Vulkan produces one `.shader` package containing native descriptor-heap and descriptor-buffer SPIR-V.
Both are compiled offline from the same source and validated. This still performs two internal compilations;
it preserves native shader performance without application-side variants, GPU detection, or shader changes.
PSO creation selects a borrowed span from the package, without allocation, compilation, or translation.

Load the artifact using ordinary file or asset I/O and pass its bytes in `ShaderStage::code`:

```cpp
gpu::PSO* pso = gpu::create_compute_pso(device, {.code = shader_bytes, .entry_point = "computeMain"});
```

Vulkan package storage must be four-byte aligned. Raw native-heap SPIR-V remains accepted on native-heap
devices; use the package for portable deployment. No application descriptor-set or pipeline-layout declarations
are needed. The descriptor-buffer backend uses private layouts without descriptor-set allocation, updates, or binds.

For build systems other than CMake, the same compiler is available as a command or Python function:

```sh
python NoGraphicsAPI/tools/compile_shader.py shader.slang -o shader.comp.shader \
  --entry computeMain --stage compute -- \
  -INoGraphicsAPI/include -INoGraphicsAPI/utility/include
```

Use `--slang`, `--spirv-val`, `--debug-level`, and `--depfile` as needed. Optional `--opt-config` and
`--spirv-opt` apply additional SPIRV-Tools passes to both variants. Failed compilation or validation preserves
the previous package. The installed script is beside `NoGraphicsAPIConfig.cmake`; installed headers share one
include directory. Vulkan requires Python 3.9+, Slang 2026.14.1+, and SPIRV-Tools 2026.3+ at shader build time.
Linking the library or loading precompiled packages needs none of these compiler tools.

On Apple platforms the CMake helper produces a `.metallib` using stock Slang 2026.18.2+ and Xcode 26+.
Compile Metal 4.0 libraries for macOS or iOS 26+; each metallib is specific to its target platform.
On Metal, compute, task, and mesh `threadgroup_size` must match `numthreads`; use the same shared constant
at shader and PSO call sites. Vulkan reads those dimensions from SPIR-V.

Both targets use row-major matrices; Vulkan additionally uses C-compatible layout. Entry names are preserved,
and the helper enables the Vulkan mesh capability for task and mesh stages. Assign whole vertex structs to
mesh outputs; Metal does not accept field-wise output writes. The library has no built-in shader programs;
`shader.slang` supplies helpers for application shaders.

## References

- [Slang target interoperation](https://shader-slang.org/slang/user-guide/a1-04-interop.html)
- [Slang Metal target](https://github.com/shader-slang/slang/blob/master/docs/user-guide/a2-02-metal-target-specific.md)
- [SPV_EXT_descriptor_heap](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_descriptor_heap.html)
- [Slang aligned loads](https://docs.shader-slang.org/en/latest/external/core-module-reference/global-decls/loadaligned-4.html)
- [Apple constant-address-space guidance](https://developer.apple.com/videos/play/wwdc2020/10632/)
