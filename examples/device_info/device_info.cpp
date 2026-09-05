// Headless device report. Creates a device with no window and prints what the backend selected,
// so the required extension and feature set can be checked on a machine without running a
// windowed example.

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <cinttypes>
#include <cstdio>

int main()
{
    const gpu::DeviceInit device_init = gpu::create_device();
    if (device_init.error != gpu::Error::none)
    {
        const char* reason = "unknown";
        switch (device_init.error)
        {
        case gpu::Error::none: break;
        case gpu::Error::unsupported: reason = "unsupported: no device exposes every required extension and feature"; break;
        case gpu::Error::device_lost: reason = "device lost"; break;
        case gpu::Error::driver_error: reason = "driver error"; break;
        }
        printf("create_device failed: %s\n", reason);
        gpu::destroy_device(device_init.device);
        return 1;
    }

    const gpu::DeviceCaps& caps = gpu::get_device_caps(device_init.device);
    printf("device                    %s\n", caps.device_name);
    printf("max push data size        %" PRIu64 " bytes\n", caps.max_push_data_size);
    printf("texture heap alignment    %" PRIu64 " bytes\n", caps.texture_heap_alignment);
    printf("texture descriptor size   %" PRIu64 " bytes\n", caps.texture_descriptor_size);
    printf("sampler descriptor size   %" PRIu64 " bytes\n", caps.sampler_descriptor_size);
    printf("BC texture compression    %s\n", caps.texture_compression_bc ? "yes" : "no");
    printf("ASTC texture compression  %s\n", caps.texture_compression_astc ? "yes" : "no");
    printf("16-bit storage in/out     %s\n", caps.storage_input_output16 ? "yes" : "no");

    gpu::destroy_device(device_init.device);
    return 0;
}
