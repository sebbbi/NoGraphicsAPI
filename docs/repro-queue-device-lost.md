# Core Vulkan 1.3 concurrent queue device-loss repro

On Windows with an RTX 4090 and NVIDIA 617.14, this standalone repro intermittently loses the device using
standard Vulkan 1.3 with no instance or device extensions enabled (`--buffers --no-validation`).
The failure previously observed on NVIDIA 596.99 persists in the 2026-10-07 retest.

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

## Retested on 2026-10-07

Windows, RTX 4090, NVIDIA 617.14, Vulkan 1.4.351, Vulkan validation layer 1.4.357.
Each configuration ran up to 50 fresh process launches, stopping at its first failure, with 256 iterations
per worker. Validation configurations include core and synchronization validation.

| Configuration | Observation |
| --- | --- |
| Release, core copies, validation | 50 launches passed |
| Release, core copies, no extensions or validation | Device lost on launch 28; stale readback and all-ones timeline counter |
| Debug, core copies, validation | 50 launches passed |
| Debug, core copies, no extensions or validation | `vkQueueSubmit2` returned `VK_ERROR_DEVICE_LOST` on launch 4 |
| Release, address copies, validation | Device lost on launch 40; stale readback and all-ones timeline counter |
| Release, address copies, no validation | Device lost on launch 14; stale readback and all-ones timeline counter |
| Release, address copies, sequential workers, validation | 50 launches passed |
| Release, address copies, explicit CPU store fence, validation | `vkQueueSubmit2` returned `VK_ERROR_DEVICE_LOST` on launch 39 |

The stale-readback failures retained `cdcdcdcd`, reported timeline counter `18446744073709551615`,
and returned `VK_ERROR_DEVICE_LOST` from `vkQueueWaitIdle`. These can accompany device loss;
they are not proof of a separate timeline-ordering bug. Local logs are in `build-msvc/driver-retest-61714`.

With the existing texture-readback workarounds, clean Release and Debug builds each passed all 40 CTest
tests once. Repeating the enabled `test_queue_family_threads` test still produced buffer-readback
failures in Release; one passing suite does not rule out this intermittent issue.

The failure does not require NoGraphicsAPI, device extensions, PushData, or timestamp resolution.
The root cause is not vendor-confirmed, and the passing validation and sequential controls do not establish
a workaround. No library behavior has been changed to hide the failure.
