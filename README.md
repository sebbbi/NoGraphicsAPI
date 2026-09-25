# NoGraphicsAPI

NoGraphicsAPI is an experimental C++20 graphics library implementing the ideas in Sebastian Aaltonen's
[*No Graphics API*](https://www.sebastianaaltonen.com/blog/no-graphics-api) with Vulkan 1.4 and Slang.

The goal is to make GPU programming feel more like working with ordinary memory and data structures:
GPU pointers for data, heap indices for textures, and a small argument structure for each draw or dispatch.

The implemented backend is Vulkan, with windowed examples on Windows x86-64 and headless builds on
other supported x86-64 platforms. This is a prototype: expect API changes and please report bugs.

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
- **Root arguments instead of binding tables.** Each draw or dispatch receives one small structure
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
};
```

Fill it with GPU addresses and pass it to a draw:

```cpp
RootArguments root{
    .vertices = vertex_memory.gpu,
    .material = material_memory.gpu,
    .transform = transform,
};
gpu::draw(commands, root, vertex_count);
```

The shader reads `root.vertices[vertex_id]` and follows `root.material` directly. Larger data structures
remain behind pointers; there is no buffer-binding step for either field.

One deliberate difference from the blog: this implementation copies small root arguments (up to 256 bytes) per command
and shares them across graphics stages, rather than passing separate GPU root pointers for each stage.
See the [design comparison](docs/no-graphics-api-comparison.md) for the remaining differences and
the [shader guide](docs/slang.md) for complete examples.

## Threading

There are no internal mutexes. The application must externally synchronize each queue and command pool.
The recommended setup is one command pool per in-flight frame per recording thread.
Independent pools can record concurrently, and work can be submitted to multiple GPU queues.

The utility library's `BumpAllocator::allocate_atomic()` supports concurrent bump allocation using relaxed atomic operations.

## Hardware requirements

The library targets little-endian x86-64. Examples using the utility math library require AVX2 and FMA.
The GPU needs Vulkan 1.4 plus recent extensions. Three important extensions behind this API are:

- [`VK_EXT_descriptor_heap`][descriptor-heap] — required; application-owned descriptor heaps.
- [`VK_KHR_device_address_commands`][address-commands] — required; commands operate on GPU addresses.
- [`VK_KHR_unified_image_layouts`][unified-layouts] — optional; efficient texture access using a single image layout.

Descriptor heaps and device-address commands are brand-new 2026 extensions. Unified image layouts
was introduced in 2025. Missing required extensions are the main reason for unsupported GPUs below;
the remaining [requirements](docs/vulkan-support.md#vulkan-feature-surface) are more widely supported
on recent GPUs. Vulkan 1.4 support alone is not sufficient.

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

## Windows installation and quick start

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/vs/older-downloads/) with the
   [Desktop development with C++ workload](https://learn.microsoft.com/en-us/cpp/build/vscpp-step-0-installation?view=msvc-170),
   including the Windows SDK. The supplied `msvc` preset targets Visual Studio 2022 x64.
2. Install [CMake 3.24+](https://cmake.org/download/) and make `cmake` available on `PATH`.
3. Install the [Vulkan SDK 1.4.357+](https://vulkan.lunarg.com/sdk/home). Shader validation requires
   SPIRV-Tools 2026.3+; make the SDK's `Bin` directory, containing `spirv-val.exe`, available on `PATH`.
4. Use Slang 2026.13.1+ from the Vulkan SDK, or download a [standalone Windows x64 release](https://github.com/shader-slang/slang/releases),
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
- [Vulkan support and behavior](docs/vulkan-support.md)
- [Slang shaders and root ABI](docs/slang.md)
- [Public API](include/NoGraphicsAPI/NoGraphicsAPI.hpp)
- [Metal porting plan — not implemented](docs/metal-porting.md)

## License

NoGraphicsAPI and NoGraphicsAPIUtility use the [MIT License](LICENSE).
See [third-party notices](THIRD_PARTY_NOTICES.md) for bundled assets and dependencies.

[descriptor-heap]: https://www.khronos.org/blog/vulkan-introduces-roadmap-2026-and-new-descriptor-heap-extension
[address-commands]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_device_address_commands.html
[unified-layouts]: https://www.khronos.org/blog/so-long-image-layouts-simplifying-vulkan-synchronisation
[rdna2-rebar]: https://vulkan.gpuinfo.org/displayreport.php?id=42800
[rdna2-fixed]: https://vulkan.gpuinfo.org/displayreport.php?id=48951
[gtx16]: https://vulkan.gpuinfo.org/displayreport.php?id=51563
[turing]: https://vulkan.gpuinfo.org/displayreport.php?id=51475
[turing-rebar]: https://www.nvidia.com/en-us/geforce/graphics-cards/compare/?section=compare-specs
