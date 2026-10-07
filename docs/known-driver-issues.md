# Known driver issues

These are locally reproduced observations, not vendor-confirmed root causes.

## NVIDIA 617.14: stale address-based texture readback

The failure observed on NVIDIA 596.99 persists on an RTX 4090 with NVIDIA 617.14, retested on 2026-10-07.
An upload through `vkCmdCopyMemoryToImageKHR` followed by `vkCmdCopyImageToMemoryKHR` returns incorrect
data despite a transfer-write to transfer-read barrier. Removing the intermediate timeline wait from
`test_texture_copy` fails for 3D, array, cube, and BC7 textures in both Release and Debug.
The buffer-command controls pass.

A parallel-recording control using two command buffers in one submission, with a transfer barrier and
no intermediate timeline wait, passed 50 runs in each Debug/Release address-command and buffer-command variant.
A timeline wait between separate upload and readback submissions works. The parallel texture and
pitch-roundtrip tests retain this workaround. No driver-specific barrier widening is applied by the graphics API.

## NVIDIA 617.14: core Vulkan 1.3 concurrent copies lose the device

The failure observed on NVIDIA 596.99 persists on Windows with an RTX 4090 and NVIDIA 617.14.
Buffer copies submitted to separate general, compute, and copy queues from three CPU threads
intermittently return `VK_ERROR_DEVICE_LOST`.

The failure reproduces independently of NoGraphicsAPI using standard Vulkan 1.3, with no instance or
device extensions enabled (`--buffers --no-validation`). The standalone repro does not link or call
NoGraphicsAPI; it uses ordinary `vkCmdCopyBuffer2`, synchronization2 barriers, and timeline semaphores.
There are no shaders, PushData calls, descriptor heaps, or timestamp queries.

The 2026-10-07 retest reproduced core-copy device loss without validation in both Release and Debug.
Address copies also failed with and without synchronization validation. Core-copy validation runs and
sequential address-copy runs passed 50 repetitions each; these controls do not establish a workaround.

This points to an NVIDIA driver issue independent of the library implementation, not a confirmed
NoGraphicsAPI defect. The root cause is not vendor-confirmed, and no workaround is established.
See the [standalone repro and test results](repro-queue-device-lost.md).

## Metal 4 / macOS 26.6.2: render timestamps leave counter entries unwritten

On Apple M3 Max with macOS 26.6.2 and Xcode 27, ten native render timestamp writes interleaved with
draws return five nonzero entries and five zeros after GPU completion. A standalone native Metal repro
fails with and without API validation, using one ordinary render pass and no Slang or NoGraphicsAPI calls.

This points to a Metal driver/runtime issue; the root cause is not vendor-confirmed. Outside-render
markers work in the tested configuration, but no workaround for in-render markers is established.
`test_render_continuation` retains the failing check. See the [repro and results](repro-metal-render-timestamps.md).

## MetalTools validation limitations

MetalTools has separate limitations with concurrent sampler lifetime changes, placed-resource
residency, and indirect mesh instrumentation. See [Metal validation](metal-validation.md#metaltools-limitations)
for reproductions and tool-specific handling.
