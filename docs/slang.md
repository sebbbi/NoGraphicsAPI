# Slang shader contract

`NoGraphicsAPI` uses Slang as the source language for a Vulkan-only binding model:

- every draw or dispatch carries one C-compatible CPU root structure copied into command-buffer
  push data;
- buffers and larger structures are reached through 64-bit typed buffer device address
  (BDA) pointers stored in that root; and
- textures and samplers are selected by integer indices into the
  application-owned texture and sampler heaps.

There are no application-visible descriptor sets, descriptor pools, or pipeline
layouts in the shader compilation path described below.

The host side uses free functions in namespace `gpu` over opaque `Device*`, `Texture*`,
`RenderView*`, `PSO*`, and `CommandBuffer*` handles. Textures, render views,
PSOs, and devices are destroyed explicitly;
submitting or presenting a span consumes every command-buffer handle in that batch. Repeated
`begin_commands()` calls acquire contexts from a growable ring; each context owns one transient
Vulkan command pool and one primary command buffer. A private device timeline retires all contexts in
a batch together, after which their pools can be reset and reused. The backend retains the contexts
and submit metadata at their high-water mark, but it retains no shader resource referenced by those
commands. These are raw handles rather than RAII wrappers.
PSO descriptions carry SPIR-V words in the custom `Span<const uint32_t>` pointer/count type.
Graphics PSOs pair `vertexMain` with `fragmentMain`; mesh PSOs pair `meshMain` with
`fragmentMain`; and compute PSOs use `computeMain`.
The span owns nothing and is consumed during the creation call. It accepts a pointer and count, a C
array, or a contiguous container with compatible `data()` and `size()` members, including
`std::vector` without adding that dependency to `NoGraphicsAPI.hpp`. `Span<const T>` also accepts an
initializer list used directly as a call argument. Temporary-container and initializer-list storage
lasts through the containing full expression, so it is valid for a directly consuming call but must
not be retained afterward.

## Supported Slang version

**Slang v2026.14.1 or newer is required.** The cube uses native descriptor-heap syntax.

**SPIRV-Tools v2026.3 or newer is also required.** Every generated module is
validated during the build, including native `SPV_EXT_descriptor_heap` use.

Slang v2026.14 added direct `ResourceDescriptorHeap` and
`SamplerDescriptorHeap` input syntax. The initial v2026.14.0 release had a
descriptor-heap `ConstantBuffer<T>` lowering bug: it emitted a storage-buffer
descriptor instead of a uniform-buffer descriptor. That bug was fixed in
v2026.14.1. The build requests `spvDescriptorHeapEXT` directly and fails if the
installed compiler does not provide it.

- [Direct descriptor-heap input support](https://github.com/shader-slang/slang/pull/11798)
- [v2026.14 `ConstantBuffer` issue](https://github.com/shader-slang/slang/issues/12226)
- [v2026.14.1 fix](https://github.com/shader-slang/slang/pull/12256)
- [Slang releases](https://github.com/shader-slang/slang/releases)

## Exact compiler options

[`examples/triangle/CMakeLists.txt`](../examples/triangle/CMakeLists.txt) invokes
the following command for the traditional vertex entry point:

```sh
slangc examples/triangle/triangle.slang \
  -target spirv \
  -profile spirv_1_5 \
  -emit-spirv-directly \
  -fvk-use-entrypoint-name \
  -entry vertexMain \
  -stage vertex \
  -o triangle.vert.spv
```

The fragment invocation changes the entry point, stage, and output to
`fragmentMain`, `fragment`, and `triangle.frag.spv`. The minimal triangle has no
compute stage and no descriptor-heap access, so its compiler command requests no
descriptor-heap capability. `SV_VertexID` selects three positions and colors from shader
constants.

[`examples/cube/CMakeLists.txt`](../examples/cube/CMakeLists.txt) adds the native
descriptor-heap options to otherwise equivalent vertex and fragment commands:

```sh
slangc examples/cube/cube.slang \
  -target spirv \
  -profile spirv_1_5 \
  -emit-spirv-directly \
  -fvk-use-entrypoint-name \
  -fvk-use-c-layout \
  -matrix-layout-row-major \
  -capability spvDescriptorHeapEXT \
  -I examples/cube \
  -I include \
  -entry fragmentMain \
  -stage fragment \
  -o cube.frag.spv
```

The cube vertex invocation changes the entry point, stage, and output to
`vertexMain`, `vertex`, and `cube.vert.spv`.

[`examples/deferred_renderer/CMakeLists.txt`](../examples/deferred_renderer/CMakeLists.txt) compiles
the G-buffer and deferred-lighting sources for their respective `vertexMain` and `fragmentMain`
entry points. Each source includes its own shared ABI header. The G-buffer vertex shader indexes
GPU `GBufferVertex*` and `ObjectData*` arrays while its indexed draw reads a GPU `uint16_t`
index buffer. The deferred vertex shader generates its fullscreen triangle from `SV_VertexID`,
and the deferred fragment reads the application heap. All four modules request
`spvDescriptorHeapEXT`.

The significant options are:

- `-target spirv -profile spirv_1_5` targets a SPIR-V version supported by the
  Vulkan 1.4 baseline and makes Slang lower `discard` to core `OpKill`. A 1.6
  profile instead emits `OpDemoteToHelperInvocation` and would add an otherwise
  unused optional device-feature requirement.
- `-emit-spirv-directly` selects Slang's direct SPIR-V backend explicitly.
- `-fvk-use-entrypoint-name` preserves names such as `vertexMain`; PSO
  creation uses those names rather than `main`.
- `-fvk-use-c-layout` makes the shader representation agree with the C++ POD
  representation described below.
- `-matrix-layout-row-major` selects the matrix ABI used by the C++ `float4x4` POD.
- `-capability spvDescriptorHeapEXT` enables the cube's native
  `SPV_EXT_descriptor_heap` lowering. In Slang's capability definitions this
  also brings in `SPV_KHR_untyped_pointers`.

The texture heap contains only image descriptors and uses
`VkPhysicalDeviceDescriptorHeapPropertiesEXT::imageDescriptorSize`; samplers
have a separate heap. Buffer descriptors are unsupported because buffers use BDA
pointers instead. Consequently the build does not use Slang's
`-spirv-resource-heap-stride`, `-spirv-sampler-heap-stride`, or
`-spirv-unified-descriptor-heap-stride` options.

Primary references:

- [Slang command-line reference](https://github.com/shader-slang/slang/blob/master/docs/command-line-slangc-reference.md)
- [Slang capability definition](https://github.com/shader-slang/slang/blob/master/source/slang/slang-capabilities.capdef)
- [`SPV_EXT_descriptor_heap`](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_descriptor_heap.html)
- [`SPV_EXT_mesh_shader`](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_mesh_shader.html)

## Mesh shader contract

`create_mesh_pso()` consumes mesh and fragment SPIR-V modules. The mesh module must expose
`meshMain`, and the fragment module must expose `fragmentMain`; use
`-fvk-use-entrypoint-name` so those names survive SPIR-V generation. A matching Slang mesh-stage
invocation uses `-entry meshMain -stage mesh -capability spvMeshShadingEXT` together with the
common options above. Also add `-capability spvDescriptorHeapEXT` when that shader accesses either
application-owned descriptor heap.

`MeshPSODesc` has no primitive-topology field; the mesh shader must declare triangle output.
Its `color_targets` span orders fragment outputs by location and supplies each attachment format and
per-target blend/write-mask state. `depth_format`, `stencil_format`, `rasterization`, and
`depth_stencil` have the same meaning as they do for a vertex graphics PSO.
The current mesh path has no task-shader stage.

`draw_meshlets()` supplies X, Y, and Z mesh-workgroup counts. `draw_meshlets_indirect()` reads one
or more mesh-workgroup-count records from a `GpuRange`; a zero stride selects the tightly packed
record size. Both forms take the same CPU root convention as the other draw functions, so
one root structure may contain BDA pointers used by the mesh or fragment stage. Use `Stage::mesh`
when a barrier needs to name mesh-shader execution.

## Specialization constants

Slang's `[vk::constant_id(...)]` applies to non-`static` scalar or enum declarations, not to an
entire structure or typed pointer. A shader can still assemble a local structure from separately
declared scalar specialization constants, but Vulkan must map every leaf independently with one
`VkSpecializationMapEntry` and an explicit ID, offset, and size. A global structure cannot be
initialized from those specialization values; construct it locally or in a helper function.
Applying the attribute to structure fields does not create a structure-shaped specialization ABI;
the current compiler folds those fields as ordinary constants instead.

A typed BDA pointer cannot itself be a specialization declaration because Slang requires a scalar
or enum type. Its address can instead be represented as a `uint64_t` specialization constant, but
Slang does not implicitly convert that integer to a pointer: shader code must explicitly reconstruct
it with `Ptr<T>(address)` or an equivalent cast. This still does not provide a shared structure ABI.

`NoGraphicsAPI` therefore keeps BDA pointers in the per-draw root data and does not expose
specialization constants in the public API. Vulkan support would require a per-stage
ID/offset/size map plus a caller-owned POD value block in each PSO description, forcing the
application to redeclare the shader's static-input signature separately from its shared C++/Slang
structure. That duplicate PSO input signature conflicts with the design goal of declaring data
shapes once in shared code instead of restating them in the API. If stable scalar PSO constants
become necessary, they can be added when a concrete use justifies that extra interface.

Primary references:

- [Slang SPIR-V specialization constants](https://github.com/shader-slang/slang/blob/master/docs/user-guide/a2-01-spirv-target-specific.md#specialization-constants)
- [`VkSpecializationInfo`](https://docs.vulkan.org/refpages/latest/refpages/source/VkSpecializationInfo.html)
- [`VkSpecializationMapEntry`](https://docs.vulkan.org/refpages/latest/refpages/source/VkSpecializationMapEntry.html)

## Descriptor-heap syntax

The fragment shader in
[`examples/cube/cube.slang`](../examples/cube/cube.slang) is:

```slang
Texture2D<float4> texture = ResourceDescriptorHeap[0];
SamplerState sampler = SamplerDescriptorHeap[0];
```

The application chooses both kinds of index by choosing descriptor byte offsets. Texture and
sampler indices are separate namespaces even though both are represented as `uint32`. The cube
writes its texture and its one nearest/clamp sampler into slot zero of their separate heaps. The
deferred example instead places its three G-buffer textures in caller-defined texture slots and
names their fixed indices with the shared `GBufferTexture` enum. Its `DeferredRoot` carries only
the camera position, ray basis, depth-linearization coefficients, and G-buffer-to-drawable pixel
scale used by the lighting pass.

The resource declaration determines the descriptor's texture dimensionality. Its 1D, 2D, 3D, cube,
and array shape must match the `TextureType` used to create the texture and the kind of descriptor
written into the heap. The example uses `Texture2D<float4>` because its source is a
`TextureType::two_d` texture.

## Application-owned descriptor heaps

Descriptor heaps use explicit `MemoryType` modes because Vulkan gives descriptor-capable buffers
heap-specific size, alignment, and reserved-range rules:

```cpp
const gpu::DeviceCaps& caps = gpu::get_device_caps(device);
gpu::GpuAllocation<std::byte> texture_heap = gpu::gpu_malloc(
    device, caps.texture_descriptor_stride, gpu::MemoryType::texture_heap);
gpu::GpuAllocation<std::byte> sampler_heap = gpu::gpu_malloc(
    device, caps.sampler_descriptor_stride, gpu::MemoryType::sampler_heap);
```

`MemoryType::texture_heap` selects the NoGraphicsAPI texture heap, backed by Vulkan's natively named
resource heap and bound by `set_texture_heap()`;
`MemoryType::sampler_heap` selects the heap bound by `set_sampler_heap()`.
Both allocations return directly usable coherent `cpu` and `gpu` values. Their reported
`size` is exactly the requested user byte count. Vulkan requires an implementation-reserved region;
NoGraphicsAPI appends it as a hidden suffix outside the returned range, so user descriptor slot zero is
exactly the returned pointer. Slot `i` begins at `cpu + i * descriptor_stride`; the stride is the
raw descriptor size rounded up to the corresponding Vulkan descriptor alignment.
The example uses the byte-count overload because descriptor stride is a runtime device property.
The typed `gpu_malloc<T>()` overload supports both descriptor-heap memory types as well, allowing a
fixed descriptor layout to be represented by an application-defined structure. The byte-count
overload's optional alignment constrains the GPU address.
Each heap allocation owns one descriptor-capable `VkBuffer` and one dedicated coherent mapped
device-memory allocation. Its private size includes any prefix alignment slack, suffix padding, and
the hidden reserved region; descriptor heaps are not suballocated from the ordinary 256 MiB buffer
pages.

`write_texture_descriptor()` takes the device first and writes a sampled or storage descriptor to a
caller-selected CPU address. `TextureDescriptorDesc` selects its format, mip range, and layer range.
`Format::undefined` inherits the texture's creation format; a zero mip or layer count selects every
remaining entry from its corresponding base. Cube and cube-array layer ranges count logical cubes,
not individual faces. The backend automatically declares every compatible public view format while
creating the texture, so the application selects the format only when writing a texture descriptor.
The same texture can therefore occupy multiple caller-selected heap slots with different compatible
formats or subresource ranges.

`NoGraphicsAPI` supports only single-sample texture declarations and rendering.
`VK_EXT_descriptor_heap` cannot encode sampled multisample image descriptors, so MSAA would require
a special-case attachment and resolve path outside this descriptor model. Because MSAA is not
commonly used in modern rendering pipelines, the API deliberately omits that surface.

`write_sampler_descriptor()` writes a sampler descriptor without creating a public sampler
object. A command buffer that executes a shader using either heap binds the user ranges explicitly:

```cpp
gpu::set_texture_heap(commands, gpu::gpu_range(texture_heap));
gpu::set_sampler_heap(commands, gpu::gpu_range(sampler_heap));
```

Heaps are command state, not device-global retained objects. The backend neither owns descriptor
slots nor discovers heap references from copied root data. The application must keep the heap
allocations, textures named by descriptors, and all other referenced public objects alive until
every command buffer using them has been submitted. It must keep mutable descriptor and allocation
contents unchanged through GPU completion. After submission, destroy/free calls may invalidate the
public values immediately because the backend defers their physical Vulkan backing.

Slang can also materialize a resource-specific `.Handle` from a heap access for
storage or passing through user code. Internally descriptor handles use a
`uint2` representation. A combined texture/sampler handle uses `.x` for the
texture heap and `.y` for the sampler heap.

Under `SPV_EXT_descriptor_heap`, dynamically selected heap descriptors are
non-uniform by default. No conventional descriptor-set declarations or
`NonUniform` decoration are needed for legality.

See [Slang direct descriptor-heap indexing](https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md#direct-descriptor-heap-indexing)
and [the SPIR-V-specific lowering](https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md#spir-v-with-spvdescriptorheapext).

## Shared root ABI and `vkCmdPushDataEXT`

Root data is an ordinary CPU POD. Pass it to a draw, meshlet draw, or dispatch; the command template
infers its complete size:

```cpp
const RootArguments root{.vertices = vertices.gpu};

gpu::draw(commands, root, vertex_count);
// gpu::dispatch(commands, root, x, y, z);
```

A rootless shader passes `{}` for its root `ByteSpan`.

Each draw or dispatch has a single overload taking `ByteSpan` by value. A valid root structure converts
implicitly; the span stores its address and `sizeof(T)`. Type-erased callers explicitly construct
`ByteSpan{byte_ptr, byte_size}`. The command records `vkCmdPushDataEXT` immediately before the
native draw or dispatch when the span is nonempty. Passing `{}` records no push-data command.
Vulkan copies the CPU bytes
while recording and cannot load push-data state directly from GPU memory. The shader therefore reads
root fields directly from push data instead of reading a GPU address and then fetching the root
through it. The root may be stack-local and may be changed or destroyed as soon as the draw or
dispatch call returns. Its type must be trivially copyable; its size must be a multiple of four
bytes and must not exceed `DeviceCaps::max_push_data_size`.

The shader reads the same structure directly from the ordinary SPIR-V `PushConstant` storage class:

```slang
[[vk::push_constant]]
ConstantBuffer<RootArguments> root;
```

`vkCmdPushDataEXT` needs no new Slang storage kind. GPU pointers copied as fields of the root still
refer to GPU allocations, so their physical backing must survive execution even though the CPU root
itself has no post-call lifetime requirement. Once all referencing command buffers are submitted,
`gpu_free()` provides that physical lifetime through deferred retirement. Do not mix this model with `vkCmdPushConstants`
or push descriptors because the Vulkan commands invalidate one another's push-data state.

- [Slang push constants](https://github.com/shader-slang/slang/blob/master/docs/user-guide/a2-01-spirv-target-specific.md#push-constants)
- [`VK_EXT_descriptor_heap` proposal, including push data](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_descriptor_heap.adoc)

## GPU pointers backed by buffer device addresses

Shared structures use an ordinary pointer type for a device pointer. The cube root is one example:

```c
struct CubeRootArguments
{
    CubeVertex* vertices;
    float4x4 transform;
};
```

Both C++ and Slang see the cube's typed `CubeVertex*`; its texture and sampler use fixed heap slot
zero. `gpu_malloc<T>()` returns a
`GpuAllocation<T>` aggregate containing `cpu`, `gpu`, `size`, and two opaque fields used only
by `gpu_free()`. The default `MemoryType::cpu_visible` and readback
allocations provide both addresses from coherent mapped device-local memory. The allocation holds the vertices; the root itself remains an
ordinary CPU value:

```cpp
gpu::GpuAllocation<CubeVertex> vertices = gpu::gpu_malloc<CubeVertex>(device, vertex_count);
CubeRootArguments root{.vertices = vertices.gpu};
gpu::draw(commands, root, vertex_count);
```

The CPU pointer names the persistently mapped bytes and can be written directly.
The GPU pointer is only a carrier for GPU virtual-address bits on the CPU and
must not be dereferenced there. The shader can use normal typed access:

```slang
CubeVertex vertex = root.vertices[vertex_id];
```

Slang lowers such pointers to the SPIR-V `PhysicalStorageBuffer` storage class,
the `PhysicalStorageBufferAddresses` capability, and the
`PhysicalStorageBuffer64` addressing model. A more explicit shader-only spelling
is `Ptr<T, Access.Read, AddressSpace.Device>`, but the native `T*` spelling keeps
the shared header simple.

`gpu_malloc()` with `MemoryType::gpu_only` still returns the same aggregate, but `cpu`
is null because the allocation is not host-visible; `gpu` and `size` remain
valid. `gpu_free(allocation)` takes the complete, unchanged `GpuAllocation` by reference after it is
returned by `gpu_malloc()`, not a `Device*`, one of the allocation's pointers, or a manually
constructed range.
Its opaque owner/token fields must also remain unchanged, and the allocation must be freed exactly once.

Memory allocation failure is fatal and deliberately unchecked; `gpu_malloc()` does not return a
nullable out-of-memory result. A successful `MemoryType::gpu_only` allocation still has a null
`cpu` because it is not host-visible.

Address-based command APIs use a second small aggregate,
`GpuRange {gpu, size}`. Commands take ranges by value. Call `gpu_range(allocation)` for a whole allocation, or supply an interior
GPU pointer and the exact byte count for
that subrange. The backend passes this address/size pair directly to the Vulkan
device-address command. Command recording performs no allocation lookup and does not retain the
allocation; validity, bounds, and lifetime are the application's responsibility.

Internally the Vulkan runtime enables `bufferDeviceAddress`, backs allocations
with 256 MiB heap pages, and gives each page one fully bound universal buffer
created for shader-address, storage, index, indirect, and transfer use. It obtains
the page's base address with `vkGetBufferDeviceAddress` and suballocates it with the pinned
[OffsetAllocator](https://github.com/sebbbi/OffsetAllocator) revision. Additional
pages are created on exhaustion. Each memory pool normally probes one active page; only a miss
scans older pages, and a successful fallback or free promotes that page. The opaque allocation
owner/token makes `gpu_free()` O(1). `MemoryType::cpu_visible` is not a host-only staging class: device
creation rejects hardware without coherent host-visible memory on a device-local heap. Descriptor
heaps use their dedicated buffers described above. No Vulkan buffer handle or owning allocation
class is visible to the application; `GpuAllocation` is a plain value handle and `GpuRange` is a
plain address/size carrier.

Important pointer limitations are:

- a BDA pointer has no bound, so robust buffer access cannot make an out-of-range
  dereference safe;
- the CPU representation carries a GPU address and cannot be dereferenced by host code;
- the application must keep every GPU allocation referenced through the root alive through GPU
  completion and satisfy the pointee's alignment; the CPU root itself is copied during recording;
- opaque textures cannot be pointees—write their descriptors into a texture heap;
- sampler state is written directly into a sampler heap rather than represented by a public
  sampler object;
- Slang's Vulkan pointer support is intentionally smaller than C++ pointer
  semantics, including restrictions on pointers to local variables, `const`
  syntax, custom alignment, and inheritance; and
- storing the address as `uint64_t` and casting it in shader code can introduce
  a `shaderInt64` requirement. A typed pointer field does not require that
  integer feature merely to carry the address.

- [Slang pointer support and limitations](https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md#pointers-limited)
- [Slang SPIR-V global-memory pointers](https://github.com/shader-slang/slang/blob/master/docs/user-guide/a2-01-spirv-target-specific.md#global-memory-pointers)
- [`SPV_KHR_physical_storage_buffer`](https://github.khronos.org/SPIRV-Registry/extensions/KHR/SPV_KHR_physical_storage_buffer.html)
- [Vulkan buffer device address guide](https://docs.vulkan.org/guide/latest/buffer_device_address.html)
- [BDA alignment requirements](https://docs.vulkan.org/guide/latest/buffer_device_address_alignment.html)

## Shared C/C++ POD layout

Shader and CPU code include the same application structure header when they share data, as the cube
does with [`examples/cube/cube_shared.h`](../examples/cube/cube_shared.h). It first includes
`<NoGraphicsAPI/shader_types.h>`.

Slang predefines `__SLANG__`, which `shader_types.h` uses to select its shader branch. That branch
uses Slang's native `float2`, `float3`, `float4`, `int2`, `uint4`, `float16_t2`, `int16_t3`,
`uint16_t4`, `float3x4`, and similar types. On the C++ branch it declares matching scalar-member
POD types. CPU-only operators and math functions live in `<NoGraphicsAPI/math.hpp>`, which shared
Slang headers do not include.

The C++ branch deliberately omits `bool2`, `bool3`, and `bool4`. Slang boolean vector elements
occupy four bytes, while C++ `bool` can occupy one byte, so equivalent-looking structures are not
ABI-compatible. Represent shared flags with `uint32` or an unsigned integer vector and compare
them with zero in shader code.

`-fvk-use-c-layout` is required because it applies C/C++ layout rules to values
accessed through `ConstantBuffer`, `ParameterBlock`, `StructuredBuffer`,
`ByteAddressBuffer`, and general pointers. Every shader sharing C++ POD structures is compiled
with this option. Structures containing matrices also require
`-matrix-layout-row-major`. The provided matrix C++ PODs match Slang's built-ins and store
rows directly. Slang names matrix dimensions as rows by columns, so `float4x4` contains four
`float4` rows. Shared structures do not
support an alternate layout mode.

The runtime requires and enables Vulkan `scalarBlockLayout`. Keep shared structures simple:

- use the scalar-member types supplied by `shader_types.h`, not host SIMD types
  with platform-specific alignment;
- make padding explicit when the ABI is externally visible;
- use the supplied matrix PODs with the row-major compiler option; the cube passes its full
  `float4x4` model-view-projection transform; and
- reserve ordinary pointer fields for GPU addresses, and keep host pointers,
  references, containers, constructors, and virtual
  members out of shared structures.

Primary references:

- [`-fvk-use-c-layout`](https://github.com/shader-slang/slang/blob/master/docs/command-line-slangc-reference.md#fvk-use-c-layout)
- [Slang C-layout regression test](https://github.com/shader-slang/slang/blob/master/tests/spirv/c-layout-buffer.slang)
- [Slang's `__SLANG__` predefined macro](https://github.com/shader-slang/slang/blob/master/source/slang/slang-translation-unit.cpp)
- [Vulkan scalar block layout](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_scalar_block_layout.html)

## 16-bit values

Slang's true 16-bit scalar and vector names include `float16_t`, `float16_t2`,
`int16_t`, `uint16_t`, and `uint16_t4`. The C++ `float16_t` in
`shader_types.h` is deliberately an opaque `uint16` bit container; the shared
ABI preserves all binary16 bit patterns but does not perform host-side numeric
conversion.

Neither current example places a 16-bit value in its root or vertex data.
`NoGraphicsAPI` nevertheless currently requires and enables:

- `shaderFloat16` for FP16 operations and conversions;
- `shaderInt16` for 16-bit integer shader values;
- `storagePushConstant16` for 16-bit values copied directly into a root; and
- `storageBuffer16BitAccess` for 16-bit values reached through BDA-backed
  storage.

The root itself uses push-constant storage, while a BDA pointee uses storage-buffer access. Both
locations therefore support 16-bit values through their corresponding enabled feature.

The runtime does not enable `uniformAndStorageBuffer16BitAccess`, because the
current API does not expose descriptor-backed uniform/storage buffers. It also
does not enable the Vulkan 8-bit storage or `shaderInt8` features, even though
the shared type header provides 8-bit CPU/Slang aliases. Do not place 8-bit
members in root or BDA data until those Vulkan requirements are added.

- [Vulkan 16-bit storage](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_16bit_storage.html)
- [`VkPhysicalDevice16BitStorageFeatures`](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDevice16BitStorageFeatures.html)
- [Slang BDA-pointer capability regression test](https://github.com/shader-slang/slang/blob/master/tests/spirv/bda-pointer-no-spurious-storage-capability.slang)

## Summary of current boundaries

- Slang v2026.14.1+ and native `SPV_EXT_descriptor_heap` compilation are required.
- Every shared shader/CPU structure uses `-fvk-use-c-layout`; no alternate layout mode is supported.
- Texture-heap indices address image-sized slots; buffer and other non-texture descriptors are not
  supported.
- Texture and sampler heap indices are separate namespaces.
- Texture and sampler heaps are application-owned `GpuAllocation` values, must be bound explicitly,
  and must remain live until every referencing command buffer is submitted. Their contents must
  remain unchanged through GPU completion unless a fresh generation is used.
- Texture heap declarations must match the descriptor's 1D, 2D, 3D, cube, and array shape.
  The backend declares compatible public view formats at texture creation; each descriptor may then
  select its format plus mip and logical layer ranges.
- The texture and rendering API is deliberately single-sample.
- Every vertex, indexed, meshlet, or compute command receives its root explicitly and copies the
  typed CPU structure through `vkCmdPushDataEXT` immediately before the native command.
- BDA pointers are unbounded and require valid lifetime and alignment.
- Shared structures must use compatible plain C layout.
- Direct 16-bit root fields and 16-bit BDA pointee data use the corresponding Vulkan features listed
  above.
- The runtime is single-threaded and performs no mutex or atomic synchronization; do not record or
  submit through one device concurrently.
