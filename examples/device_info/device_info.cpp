// Creates a headless device and reports its Vulkan identity, memory information, and operational
// limits and features exposed through NoGraphicsAPI.

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <inttypes.h>
#include <stdio.h>

const char* device_type_name(uint32 device_type)
{
    switch (device_type)
    {
    case 0: return "other";
    case 1: return "integrated GPU";
    case 2: return "discrete GPU";
    case 3: return "virtual GPU";
    case 4: return "CPU";
    default: return "unknown";
    }
}

void print_bytes(const char* label, uint64 bytes)
{
    if (bytes > 1024ull * 1024ull * 1024ull)
        printf("%-26s%" PRIu64 " bytes (%.2f GiB)\n", label, bytes, static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    else if (bytes > 1024ull * 1024ull)
        printf("%-26s%" PRIu64 " bytes (%.2f MiB)\n", label, bytes, static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes > 1024ull)
        printf("%-26s%" PRIu64 " bytes (%.2f KiB)\n", label, bytes, static_cast<double>(bytes) / 1024.0);
    else
        printf("%-26s%" PRIu64 " bytes\n", label, bytes);
}

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
        return 1;
    }

    const gpu::DeviceInfo& info = gpu::get_device_info(device_init.device);
    const gpu::DeviceMemoryInfo& memory = gpu::get_device_memory_info(device_init.device);
    const gpu::DeviceCaps& caps = gpu::get_device_caps(device_init.device);
    printf(
        "Vulkan API                0x%08" PRIx32 " (%" PRIu32 ".%" PRIu32 ".%" PRIu32 ")\n",
        info.api_version,
        info.api_version >> 22u,
        (info.api_version >> 12u) & 0x3ffu,
        info.api_version & 0xfffu
    );
    printf("device                    %s\n", info.device_name);
    printf("vendor id                 0x%04" PRIx32 "\n", info.vendor_id);
    printf("device id                 0x%04" PRIx32 "\n", info.device_id);
    printf("device type               %" PRIu32 " (%s)\n", info.device_type, device_type_name(info.device_type));
    printf("driver                    %s\n", info.driver_name);
    printf("driver info               %s\n", info.driver_info);
    printf("driver version            0x%08" PRIx32 "\n", info.driver_version);
    print_bytes("max push data size", caps.max_push_data_size);
    print_bytes("texture heap alignment", caps.texture_heap_alignment);
    print_bytes("texture descriptor size", caps.texture_descriptor_size);
    print_bytes("sampler descriptor size", caps.sampler_descriptor_size);
    print_bytes("image descriptor align", caps.image_descriptor_alignment);
    print_bytes("sampler descriptor align", caps.sampler_descriptor_alignment);
    print_bytes("resource heap alignment", caps.resource_heap_alignment);
    print_bytes("sampler heap alignment", caps.sampler_heap_alignment);
    print_bytes("min resource heap range", caps.min_resource_heap_reserved_range);
    print_bytes("min sampler heap range", caps.min_sampler_heap_reserved_range);
    print_bytes("device-local memory", memory.device_local_memory_size);
    print_bytes("host-visible memory", memory.host_visible_memory_size);
    print_bytes("host-visible device-local", memory.host_visible_device_local_memory_size);
    printf("BC texture compression    %s\n", caps.texture_compression_bc ? "yes" : "no");
    printf("ASTC texture compression  %s\n", caps.texture_compression_astc ? "yes" : "no");
    printf("ETC2 texture compression  %s\n", caps.texture_compression_etc2 ? "yes" : "no");
    printf("16-bit storage in/out     %s\n", caps.storage_input_output16 ? "yes" : "no");
    printf("unified image layouts     %s\n", caps.unified_image_layouts ? "yes" : "no");
    printf("swapchain maintenance 1   %s\n", caps.swapchain_maintenance1 ? "yes" : "no");

    gpu::wait_idle(device_init.device);
    gpu::destroy_device(device_init.device);
    return 0;
}
