# Metal validation

Tested on Apple M3 Max, macOS 26.6.2, Xcode 27 and stock Slang 2026.18.2. The integrating bad_sdf
application also runs on iPhone 15 Pro. M1/M2 hardware execution remains unverified.
These results cover Metal 4. Metal 3 native compilation and runtime validation remain unverified.

## Running the checks

Follow the [macOS build instructions](../README.md#macos-installation-and-quick-start), then run:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --test-dir build --output-on-failure
```

The 27 September 2026 Release run passes **29 of 30 CTests** with API validation.
`test_render_continuation` exposes the [native render-timestamp failure](repro-metal-render-timestamps.md).
Add `-E '^test_render_continuation$'` to run the remaining checks separately.

Coverage includes shared root layouts, GPU pointers, descriptor indexing/copies, direct and indirect
commands, texture transfers, viewport orientation, presentation and command-pool reuse. Triangle,
cube and deferred-renderer examples run with Metal API validation.
The `_buffer_commands` tests force Metal 3 command encoding and exercise texture-view pools on OS 26+,
or texture resource-ID tables on older supported OS versions.

## Concurrency and lifetime

Tests exercise independent recording, cross-queue waits, heap creation/destruction, concurrent
updates to disjoint descriptor slots, timestamp allocation and CPU retrieval. Address-index tests
reach the 64-heap limit and repeatedly reuse entries. The shared CPU address-map test also exercises concurrent
lookups during registration and removal. The prior Metal 4 GPU checks pass with API and shader
validation. See [the implementation](metal-support.md) for synchronization responsibilities.

## Stage dependencies and overlap

Barrier tests check compute-to-fragment visibility, geometry side effects, GPU-generated index and
indirect arguments, and attachment readback. Bounded probes observe later vertex/mesh work overlapping
earlier fragment work, both within a submission and across submissions with timestamps enabled.
Broad-barrier controls wait as expected; output checks pass.

A three-drawable presentation probe also observes overlap between frames. This establishes a scheduling
opportunity on M3 Max, not a guaranteed speedup or iPhone result. Drawable availability can still stall
presentation. CPU timestamp retrieval adds no GPU wait.

## MetalTools limitations

- Concurrent sampler creation/destruction can crash API validation in `MTLSamplerDescriptorHashMap`.
  A native Metal-only stress test reproduces it; uninstrumented execution passes.
- Shader validation cannot reliably enumerate placed resources through heap residency. In that mode,
  the backend registers individual resources and serializes MetalTools residency enumeration.
- Shader instrumentation of native indirect mesh commands can crash in `resolvedSharedPacketData`
  or leave readbacks unwritten. Direct commands pass with instrumentation; indirect commands pass
  with API validation. CTest exercises these separately.

To reproduce the indirect-mesh tool failure:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 build/tests/test_metal_mesh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 build/tests/test_task_shader
```

The corresponding `--direct-only` runs pass. These diagnostic accommodations do not change production commands.
