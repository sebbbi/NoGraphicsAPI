# Known driver issues

These are locally reproduced observations, not vendor-confirmed root causes.

## NVIDIA 596.99: stale address-based texture readback

Observed on an RTX 4090 with NVIDIA 596.99, with both sequential and parallel command recording.
An upload through `vkCmdCopyMemoryToImageKHR` followed by `vkCmdCopyImageToMemoryKHR` returned stale
data despite a transfer-write to transfer-read barrier. The issue also reproduced without validation.

A timeline wait between separate upload and readback submissions works. A full Vulkan memory
dependency also worked in isolation; native `vkCmdCopyImageToBuffer2` readback worked in the comparison.
The parallel texture test uses an explicit timeline wait. No driver-specific barrier widening is applied
by the graphics API.

## NVIDIA 596.99: copy-queue timestamp resolution loses the device

On an RTX 4090, resolving timestamps with `vkCmdCopyQueryPoolResultsToMemoryKHR` on a copy-only queue
returns `VK_ERROR_DEVICE_LOST`, with or without validation. Timestamp writes without the resolve complete;
buffer/texture transfers and general/compute-queue timestamp resolution also pass.
The [Vulkan command contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyQueryPoolResultsToMemoryKHR.html)
permits transfer-only queues.

Avoid `write_timestamp` on copy-only queues with this driver. No workaround or silent timestamp suppression
is applied by the library. The regular queue-family tests omit copy-queue markers; reproduce the failure
explicitly with `build-msvc/tests/Debug/test_queue_families.exe --copy-timestamps`.

## NVIDIA 596.99: core Vulkan 1.3 concurrent copies lose the device

On Windows with an RTX 4090, buffer copies submitted to separate general, compute, and copy queues from
three CPU threads intermittently return `VK_ERROR_DEVICE_LOST`.

The failure reproduces independently of NoGraphicsAPI using standard Vulkan 1.3, with no instance or
device extensions enabled (`--buffers --no-validation`). The standalone repro does not link or call
NoGraphicsAPI; it uses ordinary `vkCmdCopyBuffer2`, synchronization2 barriers, and timeline semaphores.
There are no shaders, PushData calls, descriptor heaps, or timestamp queries.

It also fails with core and synchronization validation enabled, without a preceding validation error.
That mode adds only the debug/validation instance extensions, not device extensions.

This points to an NVIDIA driver issue independent of the library implementation, not a confirmed
NoGraphicsAPI defect. The root cause is not vendor-confirmed, and no workaround is established.
See the [standalone repro and test results](repro-queue-device-lost.md).
