# Core Vulkan 1.3 concurrent queue device-loss repro

On Windows with an RTX 4090 and NVIDIA 596.99, this standalone repro intermittently loses the device using
standard Vulkan 1.3 with no instance or device extensions enabled (`--buffers --no-validation`).

The [source](../tests/repro_queue_device_lost.cpp) does not link or call NoGraphicsAPI. It includes only
the library's integer typedefs and links to Vulkan and the OS threading library. This manual target is
excluded from normal builds and CTest because it deliberately exercises a device-loss failure.

## Workload

Three workers use separate general, compute-only, and copy-only queues. Each owns a command pool,
command buffer, timeline semaphore, and three 256-byte buffers with separate memory allocations.
Only the device is shared; there are no cross-queue waits or shared buffer contents. Resources are created
before starting workers and destroyed after joining them and waiting for device idle. Buffers use
concurrent sharing and coherent host mappings.

Each iteration copies upload → scratch → readback, with transfer-write → transfer-read and
transfer-write → host-read barriers. The worker waits for its submission's timeline before reading
or reusing resources and resetting its pool. At most one submission is in flight per queue.
There are no shaders, PushData calls, textures, descriptors, timestamp writes, or query pools.

## Run on Windows

Building requires Vulkan SDK 1.4.357+ headers for the optional address-copy mode; the extension-free mode
uses only Vulkan 1.3 at runtime. The repro also requires a discrete GPU with all three queue families and
host-visible, coherent device-local memory. These commands use the existing MSVC configuration:

```powershell
cmake --build --preset msvc-release --target repro_queue_device_lost
$env:VK_LAYER_PATH = "$env:VULKAN_SDK\Bin"
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
for ($run = 1; $run -le 50; ++$run) {
    & .\build-msvc\tests\repro\Release\repro_queue_device_lost.exe --buffers --no-validation
    if ($LASTEXITCODE -ne 0) { break }
}
```

This configuration sets both instance and device `enabledExtensionCount` to zero. To repeat with core
and synchronization validation, omit `--no-validation`: it adds `VK_EXT_debug_utils` and
`VK_EXT_validation_features` at instance level only, with no device extensions. Validation is enabled
by default in both builds. Use `msvc-debug` and the `Debug` directory to test Debug; Vulkan failures are
checked independently of asserts.

- `--buffers`: use core `vkCmdCopyBuffer2`, with **no device extensions enabled**.
- Without `--buffers`: use `vkCmdCopyMemoryKHR` and enable `VK_KHR_device_address_commands`.
- `--no-validation`: disable the explicitly requested validation layer and both instance extensions.
- `--serial`: run the workers sequentially instead of on separate threads.
- `--queue-mask N`: select general=1, compute=2, copy=4; default 7 enables all three workers.
- `--host-fence`: insert an x86 store fence after CPU writes, as an ordering diagnostic.
- `--iterations N`: submissions per worker; default 256.

Do not externally force a validation layer when testing `--no-validation`.

## Observed on 2026-09-17

Windows, RTX 4090, NVIDIA 596.99, Vulkan validation layer 1.4.357. The failure is intermittent.

| Configuration | Observation |
| --- | --- |
| Release, core copies, validation + sync validation | Device lost on repetition 37; no preceding validation error |
| Release, core copies, no extensions or validation | Device lost on repetition 5 in the final check |
| Debug, core copies, validation + sync validation | 40 repetitions passed |
| Debug, core copies, no validation | A timeline wait timed out after five seconds on repetition 9 |
| Release, address copies, sequential workers | 40 repetitions passed |
| Release, address copies, explicit CPU store fence | Device loss still reproduced |

Each repetition is a fresh process launch, not a frame. One captured failure occurred on the compute
queue's first iteration, after a successful timeline wait:

```text
Family 2, iteration 1, word 0: expected 02000100, received cdcdcdcd.
Family 2: counter result 0, value 18446744073709551615, queue idle result -4; readback after idle cdcdcdcd.
```

Other runs return `VK_ERROR_DEVICE_LOST` directly from `vkQueueSubmit2`. The all-ones timeline value and
invalid readback can accompany device loss; they are not proof of a separate timeline-ordering bug.

As a validation check, temporarily removing the first transfer barrier immediately produced
`READ_AFTER_WRITE` with core copies. That deliberate error is not present in the source.

The NoGraphicsAPI implementation, device extensions, PushData, and timestamp resolution are not required
to trigger this failure. It points to an NVIDIA driver issue independent of the library implementation,
but the root cause is not vendor-confirmed. Clean validation does not prove the repro is free of all API
misuse, and passing controls do not establish a workaround. No library behavior has been changed to hide
the failure. No common root cause with bad_sdf's CPU-side driver crash has been established.
