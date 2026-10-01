# NoGraphicsAPI

NoGraphicsAPI is a C++20 graphics library for **Metal 3/4** devices and **Vulkan 1.4** devices with
**`VK_EXT_descriptor_heap`** and **`VK_EXT_mesh_shader`**. Metal 4 and Vulkan device-address commands are preferred when available.
It implements the ideas in Sebastian Aaltonen's [*No Graphics API*](https://www.sebastianaaltonen.com/blog/no-graphics-api)
with GPU pointers, descriptor heaps, and shared Slang shaders.

The goal is to make GPU programming feel more like working with ordinary memory and data structures:
GPU pointers for data, heap indices for textures, and one GPU pointer to the arguments of each draw or dispatch.

Metal and Vulkan are supported native backends. CMake selects Metal on macOS/iOS and Vulkan on Windows/Linux.
Both use the same C++ API and Slang sources. Windows and macOS include windowed examples; Linux currently supports headless use.

## What changes from classic rendering?

A conventional renderer creates buffer objects, describes vertex and resource-binding layouts, and
assembles bindings before drawing. The blog asks how much of this machinery modern bindless hardware
still needs. NoGraphicsAPI makes that alternative data model the foundation of the library:

- **Memory allocation without buffer objects.** Allocate GPU heaps and partition them with an
  application-side allocator. Mapped heaps provide CPU and GPU addresses, so the CPU can write data
  directly. Vertex data, constants, and arbitrary structures are allocations, not separate public buffer types.
- **Typed GPU pointers.** Shaders follow 64-bit pointers stored in shared C++/Slang structures.
  Arrays, pointer arithmetic, and nested data structures work without buffer descriptors or binding slots.
  Vertex shaders fetch their own vertices; there is no vertex-layout declaration.
- **Bindless textures and samplers.** The application owns descriptor heaps and chooses their indices.
  Materials carry those indices as data. Changing materials does not require constructing or rebinding
  per-material descriptor sets.
- **Root arguments instead of binding tables.** Each draw or dispatch receives a GPU pointer to a structure
  containing GPU pointers, texture indices, and constants. The CPU and shader share its declaration;
  there are no descriptor-set layouts or pipeline layouts to keep in agreement.
- **Less pipeline-state coupling.** Resource-binding and vertex layouts are absent from pipeline
  creation. Viewport, scissor, and depth/stencil state are set independently, reducing pipeline
  permutations. Rasterization, blending, and attachment formats still belong to pipeline objects.
- **Barriers without resource lists.** Synchronization describes which work produces and consumes
  data, not a list of buffer and image transitions. Applications do not track image layouts.

This is a low-level library: the application still owns allocation policy, resource lifetime, and
GPU synchronization. The optional NoGraphicsAPIUtility library supplies shared shader types, math,
allocators, upload queues, and deferred deletion without making them part of the graphics API.

### What a draw's data looks like

Declare the arguments once in a shared C++/Slang header:

```cpp
struct RootArguments
{
    Vertex* vertices;
    Material* material;
    float4x4 transform;
    uint32 texture_index;
};
```

Allocate a root from application-owned mapped storage, fill it through the CPU address, then pass its GPU address:

```cpp
const gpu::GpuCpuRange<RootArguments> root = bump_allocator.allocate<RootArguments>();
*root.cpu = {
    .vertices = vertex_memory.gpu,
    .material = material_memory.gpu,
    .transform = transform,
    .texture_index = texture_index,
};
gpu::draw(commands, root.gpu, vertex_count);
```

With `<NoGraphicsAPI/shader.slang>`, the shader accesses the same data directly:

```slang
GPU_ROOT(RootArguments, root);
Vertex vertex = root.vertices[vertex_id];
Material material = *root.material;
Texture2D<float4> texture = gpu_texture<Texture2D<float4>>(root.texture_index);
```

Draws and dispatches take GPU root pointers directly, including roots written by earlier GPU work. All graphics stages share the same root.
Keep each root allocation alive and stable until its GPU use completes.
See the [design comparison](docs/no-graphics-api-comparison.md) for the remaining differences and
the [shader guide](docs/slang.md) for complete examples.

## Native implementations

Metal exposes native GPU addresses. Metal 4 draw/dispatch commands accept those addresses directly;
Metal 3 maps command pointers to buffers and offsets. Both use `MTLTextureViewPool` for indexed textures
on OS 26+; older OS versions use texture resource-ID tables. Vulkan uses device-address commands when available
and the same buffer mapping otherwise. See [Metal implementation](docs/metal-support.md) and
[Vulkan implementation](docs/vulkan-support.md) for how these map to NoGraphicsAPI.

## Hardware requirements

### Metal

Requires macOS 15+ or iOS/iPadOS 18+ and Apple GPU family 7 or newer.
Metal 4 is selected on supported devices running OS 26+; otherwise the backend uses Metal 3.

| Platform | Supported devices |
| --- | --- |
| Mac | Apple silicon Macs (M1 and newer) |
| iPhone | iPhone 12 and newer (A14 and newer) |
| iPad Pro | 2021 and newer (M1 and newer) |
| iPad Air | 2020 and newer (A14 and newer) |
| iPad mini | 2021 and newer (A15 and newer) |
| iPad | 2022 and newer (A14 and newer) |

See [Apple's supported devices](https://support.apple.com/en-us/102894).
Direct task/mesh draws work throughout this baseline; indirect mesh draws require A17 Pro / M3 or newer.
BC texture compression is a separate capability. See [Metal implementation](docs/metal-support.md)
for these limits and [validation](docs/metal-validation.md) for tested hardware and known issues.
Intel Macs, Simulator, tvOS and visionOS are not supported targets.

### Vulkan 1.4

The Vulkan backend targets little-endian x86-64; utility math requires AVX2 and FMA.
Vulkan 1.4 alone is insufficient. Required extensions include:

- [`VK_EXT_descriptor_heap`][descriptor-heap] — application-owned descriptor heaps.
- [`VK_EXT_mesh_shader`][mesh-shader] — task and mesh shaders.
- [`VK_KHR_shader_untyped_pointers`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_untyped_pointers.html) — descriptor-heap shader support.

[`VK_KHR_device_address_commands`][address-commands] is optional: without it, command addresses resolve to backing buffers and offsets.
[`VK_KHR_unified_image_layouts`][unified-layouts] is optional and makes the backend's common image layout efficient.
See the [Vulkan implementation](docs/vulkan-support.md) for the complete feature contract.

Missing required extensions are the main reason for unsupported GPUs below; the remaining
[requirements](docs/vulkan-support.md#vulkan-feature-surface) are more widely supported on recent GPUs.

The table preserves the driver reports checked on **5 September 2026**. These are compatibility
snapshots, not a live driver list. The checked Windows packages were
[AMD Adrenalin 26.9.1](https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-WIN-26-9-1.html)
and [NVIDIA 616.64 WHQL](https://us.download.nvidia.com/Windows/616.64/616.64-win11-win10-release-notes.pdf).

| Architecture | Driver snapshot | Products | CPU-visible heap | Required extensions |
| --- | --- | --- | --- | --- |
| AMD RDNA 2 (dGPU) | Windows / Adrenalin 26.9.1 | [RX 6000][rdna2-rebar] | PCIe ReBAR or<br>🔴 [256 MiB fixed BAR][rdna2-fixed] | 🔴 Unsupported |
| AMD RDNA 2 (iGPU) | Windows / Adrenalin 26.9.1 | [600M](https://vulkan.gpuinfo.org/displayreport.php?id=47714) | UMA | 🔴 Unsupported |
| AMD RDNA 2 (iGPU) | Linux / Mesa RADV 26.2+ | [Steam Deck](https://vulkan.gpuinfo.org/displayreport.php?id=51189) | UMA | Supported |
| AMD RDNA 3 (dGPU) | Windows / Adrenalin 26.9.1 | [RX 7000](https://vulkan.gpuinfo.org/displayreport.php?id=51443) | PCIe ReBAR | Supported |
| AMD RDNA 3 (iGPU) | Windows / Adrenalin 26.9.1 | [700M](https://vulkan.gpuinfo.org/displayreport.php?id=49646) | UMA | Supported |
| AMD RDNA 4 (dGPU) | Windows / Adrenalin 26.9.1 | [RX 9000](https://vulkan.gpuinfo.org/displayreport.php?id=51293) | PCIe ReBAR | Supported |
| NVIDIA Turing | Windows / NVIDIA 616.64 | [GTX 16 series][gtx16] | 🔴 [256 MiB fixed BAR][turing-rebar] | Supported |
| NVIDIA Turing | Windows / NVIDIA 616.64 | [RTX 20 series][turing] | 🔴 [256 MiB fixed BAR][turing-rebar] | Supported |
| NVIDIA Ampere | Windows / NVIDIA 616.64 | [RTX 30 series](https://vulkan.gpuinfo.org/displayreport.php?id=51549) | PCIe ReBAR | Supported |
| NVIDIA Ada Lovelace | Windows / NVIDIA 616.64 | [RTX 40 series](https://vulkan.gpuinfo.org/displayreport.php?id=51469) | PCIe ReBAR | Supported |
| NVIDIA Blackwell | Windows / NVIDIA 616.64 | [RTX 50 series](https://vulkan.gpuinfo.org/displayreport.php?id=51573) | PCIe ReBAR | Supported |

🔴 marks missing extensions or a capacity-limited fixed BAR. Mapped heaps require coherent CPU-visible
GPU memory. Enable ReBAR where available on discrete GPUs; integrated GPUs use UMA. A fixed BAR can
still work, but limits mapped-heap capacity. Separate GPU-only allocations can use the remaining VRAM.
UMA heap sizes depend on system configuration.

The checked Windows RDNA 2 reports lack descriptor-heap support.
[Pascal / GTX 10](https://vulkan.gpuinfo.org/displayreport.php?id=51084) lacks the required extensions.
Intel Windows support was not verified; the checked
[Arc report](https://vulkan.gpuinfo.org/displayreport.php?id=51355) also lacks required extensions.
Mesa RADV and ANV [26.2+](https://docs.mesa3d.org/relnotes/26.2.0.html) expose the required extensions
on the reported Linux/SteamOS targets.

See [known driver issues](docs/known-driver-issues.md) for observed problems and workarounds.

## macOS installation and quick start

Install Xcode 26+ with its Metal compiler, CMake, and Slang 2026.18.2+. Build the examples and tests:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_SYSROOT=macosx -DNOGRAPHICSAPI_BUILD_EXAMPLES=ON \
  -DNOGRAPHICSAPI_BUILD_TESTS=ON -DNOGRAPHICSAPI_SLANGC=/path/to/slangc
cmake --build build
ctest --test-dir build --output-on-failure
build/examples/triangle/example_triangle
```

See [building and integration](docs/building.md) for iOS builds and
[Metal validation](docs/metal-validation.md) for known test limitations.

## Windows installation and quick start

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/vs/older-downloads/) with the
   [Desktop development with C++ workload](https://learn.microsoft.com/en-us/cpp/build/vscpp-step-0-installation?view=msvc-170),
   including the Windows SDK. The supplied `msvc` preset targets Visual Studio 2022 x64.
2. Install [CMake 3.24+](https://cmake.org/download/) and make `cmake` available on `PATH`.
3. Install the [Vulkan SDK 1.4.357+](https://vulkan.lunarg.com/sdk/home). Shader validation requires
   SPIRV-Tools 2026.3+; make the SDK's `Bin` directory, containing `spirv-val.exe`, available on `PATH`.
4. Use Slang 2026.14.1+ from the Vulkan SDK, or download a [standalone Windows x64 release](https://github.com/shader-slang/slang/releases),
   extract it, and add its `bin` directory to `PATH`.
5. Install a GPU driver meeting the hardware requirements above. The Vulkan SDK does not replace
   the GPU driver.

Clone the repository or download its source archive, then open PowerShell in the repository directory.
Configure, build, test, and run the triangle example:

```powershell
cmake --preset msvc
cmake --build --preset msvc-release
ctest --preset msvc-release
.\build-msvc\examples\triangle\Release\example_triangle.exe
```

To open the generated solution in Visual Studio, use `build-msvc/NoGraphicsAPI.sln`.
For a validation-enabled Debug build, use `msvc-debug` in the build and test commands.

If the shader tools are not on `PATH`, supply their locations when configuring. Adjust these example
paths to your installations:

```powershell
cmake --preset msvc -DNOGRAPHICSAPI_SLANGC=C:/Slang/2026.14.1/bin/slangc.exe -DNOGRAPHICSAPI_SPIRV_VAL=C:/VulkanSDK/1.4.357.0/Bin/spirv-val.exe
```

To install the Release libraries and headers locally:

```powershell
cmake --install build-msvc --config Release --prefix ./install
```

The install includes the independent NoGraphicsAPI and NoGraphicsAPIUtility CMake packages.
See [building and integration](docs/building.md) for using them in your own project or building
the library without examples and tests.

## Examples

- [Triangle](examples/triangle/triangle.cpp) — the smallest rendering example.
- [Cube](examples/cube/cube.cpp) — GPU-pointer vertex fetch and bindless textures.
- [Deferred renderer](examples/deferred_renderer/deferred_renderer.cpp) — compute simulation and mesh-shader rendering.

The executables are under `build-msvc/examples/<example>/Release` when using the supplied preset.

## Documentation

- [Comparison with *No Graphics API*](docs/no-graphics-api-comparison.md)
- [Vulkan implementation](docs/vulkan-support.md)
- [Metal implementation](docs/metal-support.md)
- [Slang shaders and root ABI](docs/slang.md)
- [Public API](include/NoGraphicsAPI/NoGraphicsAPI.hpp)
- [Metal validation and limitations](docs/metal-validation.md)
- [Building and integration](docs/building.md)

## License

NoGraphicsAPI and NoGraphicsAPIUtility use the [MIT License](LICENSE).
See [third-party notices](THIRD_PARTY_NOTICES.md) for bundled assets and dependencies.

[descriptor-heap]: https://www.khronos.org/blog/vulkan-introduces-roadmap-2026-and-new-descriptor-heap-extension
[mesh-shader]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_mesh_shader.html
[address-commands]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_device_address_commands.html
[unified-layouts]: https://www.khronos.org/blog/so-long-image-layouts-simplifying-vulkan-synchronisation
[rdna2-rebar]: https://vulkan.gpuinfo.org/displayreport.php?id=42800
[rdna2-fixed]: https://vulkan.gpuinfo.org/displayreport.php?id=48951
[gtx16]: https://vulkan.gpuinfo.org/displayreport.php?id=51563
[turing]: https://vulkan.gpuinfo.org/displayreport.php?id=51475
[turing-rebar]: https://www.nvidia.com/en-us/geforce/graphics-cards/compare/?section=compare-specs
