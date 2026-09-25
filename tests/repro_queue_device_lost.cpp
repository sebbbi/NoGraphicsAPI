#include <NoGraphicsAPI/types.h>
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <immintrin.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace
{

constexpr uint32 buffer_size = 256;

[[noreturn]] void stop(uint32 code)
{
    fflush(stderr);
#if defined(_WIN32)
    // Avoid DLL teardown deadlocks when other workers are still inside a lost device's driver.
    TerminateProcess(GetCurrentProcess(), code);
#endif
    _Exit(static_cast<int>(code));
}

void check(VkResult result, const char* operation, uint32 line)
{
    if (result == VK_SUCCESS) return;
    fprintf(stderr, "Line %u: %s returned %d%s.\n", line, operation, result, result == VK_ERROR_DEVICE_LOST ? " (VK_ERROR_DEVICE_LOST)" : "");
    stop(1);
}

#define VK_CHECK(operation) check(operation, #operation, __LINE__)

VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    fprintf(stderr, "Validation: %s\n", data->pMessage);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) stop(2);
    return VK_FALSE;
}

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    uint32* mapped = nullptr;
};

struct Worker
{
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    PFN_vkCmdCopyMemoryKHR copy_memory = nullptr;
    Buffer upload{};
    Buffer scratch{};
    Buffer readback{};
    uint32 family = 0;
    uint32 iterations = 256;
    bool buffer_copies = false;
    bool host_fence = false;
};

Buffer create_buffer(VkDevice device, const VkPhysicalDeviceMemoryProperties& properties, const uint32* families, bool mapped)
{
    Buffer result{};
    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer_size,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = 3,
        .pQueueFamilyIndices = families,
    };
    VK_CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer));
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
    const VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        (mapped ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0);
    uint32 memory_type = 0;
    for (; memory_type < properties.memoryTypeCount; ++memory_type)
    {
        if ((requirements.memoryTypeBits & (1u << memory_type)) && (properties.memoryTypes[memory_type].propertyFlags & required) == required)
            break;
    }
    if (memory_type == properties.memoryTypeCount)
    {
        fputs("Required memory type is unavailable.\n", stderr);
        exit(77);
    }
    const VkMemoryAllocateFlagsInfo flags{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    const VkMemoryAllocateInfo memory_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    VK_CHECK(vkAllocateMemory(device, &memory_info, nullptr, &result.memory));
    VK_CHECK(vkBindBufferMemory(device, result.buffer, result.memory, 0));
    if (mapped)
    {
        void* mapping = nullptr;
        VK_CHECK(vkMapMemory(device, result.memory, 0, VK_WHOLE_SIZE, 0, &mapping));
        result.mapped = static_cast<uint32*>(mapping);
    }
    const VkBufferDeviceAddressInfo address_info{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = result.buffer};
    result.address = vkGetBufferDeviceAddress(device, &address_info);
    return result;
}

void record_copy(const Worker& worker, const Buffer& source, const Buffer& destination)
{
    if (worker.buffer_copies)
    {
        const VkBufferCopy2 region{.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2, .size = buffer_size};
        const VkCopyBufferInfo2 info{
            .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
            .srcBuffer = source.buffer,
            .dstBuffer = destination.buffer,
            .regionCount = 1,
            .pRegions = &region,
        };
        vkCmdCopyBuffer2(worker.commands, &info);
    }
    else
    {
        const VkDeviceMemoryCopyKHR region{
            .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_COPY_KHR,
            .srcRange = {.address = source.address, .size = buffer_size},
            .srcFlags = VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR,
            .dstRange = {.address = destination.address, .size = buffer_size},
            .dstFlags = VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR,
        };
        const VkCopyDeviceMemoryInfoKHR info{.sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_INFO_KHR, .regionCount = 1, .pRegions = &region};
        worker.copy_memory(worker.commands, &info);
    }
}

void run(Worker& worker)
{
    for (uint32 iteration = 1; iteration <= worker.iterations; ++iteration)
    {
        for (uint32 word = 0; word < buffer_size / sizeof(uint32); ++word)
            worker.upload.mapped[word] = worker.family * 0x1000000u + iteration * 0x100u + word;
        memset(worker.readback.mapped, 0xcd, buffer_size);
        if (worker.host_fence) _mm_sfence();
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VK_CHECK(vkBeginCommandBuffer(worker.commands, &begin));
        record_copy(worker, worker.upload, worker.scratch);
        VkMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        };
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(worker.commands, &dependency);
        record_copy(worker, worker.scratch, worker.readback);
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        vkCmdPipelineBarrier2(worker.commands, &dependency);
        VK_CHECK(vkEndCommandBuffer(worker.commands));
        const VkCommandBufferSubmitInfo command_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = worker.commands, .deviceMask = 1,
        };
        const VkSemaphoreSubmitInfo signal{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = worker.timeline,
            .value = iteration,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        const VkSubmitInfo2 submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &command_info,
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos = &signal,
        };
        VK_CHECK(vkQueueSubmit2(worker.queue, 1, &submit, VK_NULL_HANDLE));
        const uint64 value = iteration;
        const VkSemaphoreWaitInfo wait{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &worker.timeline, .pValues = &value,
        };
        const VkResult result = vkWaitSemaphores(worker.device, &wait, 5000000000ull);
        if (result != VK_SUCCESS)
        {
            fprintf(stderr, "Family %u, iteration %u: ", worker.family, iteration);
            check(result, "vkWaitSemaphores", __LINE__);
        }
        for (uint32 word = 0; word < buffer_size / sizeof(uint32); ++word)
        {
            if (worker.readback.mapped[word] == worker.upload.mapped[word]) continue;
            fprintf(stderr, "Family %u, iteration %u, word %u: expected %08x, received %08x.\n",
                    worker.family, iteration, word, worker.upload.mapped[word], worker.readback.mapped[word]);
            uint64 completed = 0;
            const VkResult counter_result = vkGetSemaphoreCounterValue(worker.device, worker.timeline, &completed);
            const VkResult idle_result = vkQueueWaitIdle(worker.queue);
            fprintf(stderr, "Family %u: counter result %d, value %llu, queue idle result %d; readback after idle %08x.\n",
                    worker.family, counter_result, completed, idle_result, worker.readback.mapped[word]);
            stop(3);
        }
        VK_CHECK(vkResetCommandPool(worker.device, worker.pool, 0));
    }
    printf("Family %u: %u iterations passed.\n", worker.family, worker.iterations);
}

#if defined(_WIN32)
DWORD WINAPI run_thread(void* argument)
#else
void* run_thread(void* argument)
#endif
{
    run(*static_cast<Worker*>(argument));
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool validation = true;
    bool serial = false;
    bool buffer_copies = false;
    bool host_fence = false;
    uint32 queue_mask = 7;
    uint32 iterations = 256;
    for (int index = 1; index < argc; ++index)
    {
        if (strcmp(argv[index], "--no-validation") == 0) validation = false;
        else if (strcmp(argv[index], "--serial") == 0) serial = true;
        else if (strcmp(argv[index], "--buffers") == 0) buffer_copies = true;
        else if (strcmp(argv[index], "--host-fence") == 0) host_fence = true;
        else if (strcmp(argv[index], "--queue-mask") == 0 && index + 1 < argc) queue_mask = static_cast<uint32>(atoi(argv[++index]));
        else if (strcmp(argv[index], "--iterations") == 0 && index + 1 < argc) iterations = static_cast<uint32>(atoi(argv[++index]));
        else
        {
            fputs("Usage: repro_queue_device_lost [--no-validation] [--serial] [--buffers] [--host-fence] [--queue-mask 1..7] [--iterations N]\n", stderr);
            return 1;
        }
    }
    if (queue_mask == 0 || queue_mask > 7 || iterations == 0)
    {
        fputs("Queue mask must be 1..7 and iteration count must be positive.\n", stderr);
        return 1;
    }
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* instance_extensions[]{VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    const VkValidationFeatureEnableEXT validation_feature = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    const VkValidationFeaturesEXT validation_features{
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &validation_feature,
    };
    const VkDebugUtilsMessengerCreateInfoEXT messenger_info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = validation_message,
    };
    const VkApplicationInfo application{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "queue device-loss repro", .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo instance_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = validation ? &validation_features : nullptr,
        .pApplicationInfo = &application,
        .enabledLayerCount = validation ? 1u : 0u,
        .ppEnabledLayerNames = &layer,
        .enabledExtensionCount = validation ? 2u : 0u,
        .ppEnabledExtensionNames = instance_extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&instance_info, nullptr, &instance));
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (validation)
        VK_CHECK(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"))(
            instance, &messenger_info, nullptr, &messenger));

    VkPhysicalDevice physical_devices[16]{};
    uint32 device_count = 16;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &device_count, physical_devices));
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    for (uint32 index = 0; index < device_count; ++index)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_devices[index], &properties);
        if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
        physical_device = physical_devices[index];
        printf("GPU: %s; driver raw version: %u; validation + sync: %s\n", properties.deviceName, properties.driverVersion, validation ? "on" : "off");
        break;
    }
    if (!physical_device) return 77;
    VkQueueFamilyProperties families[32]{};
    uint32 family_count = 32;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families);
    uint32 selected_families[3]{~0u, ~0u, ~0u};
    for (uint32 family = 0; family < family_count; ++family)
    {
        if (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            if (selected_families[0] == ~0u) selected_families[0] = family;
        }
        else if (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)
        {
            if (selected_families[1] == ~0u) selected_families[1] = family;
        }
        else if (families[family].queueFlags & VK_QUEUE_TRANSFER_BIT)
        {
            if (selected_families[2] == ~0u) selected_families[2] = family;
        }
    }
    for (uint32 family : selected_families) if (family == ~0u) return 77;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queues[3]{};
    for (uint32 index = 0; index < 3; ++index)
        queues[index] = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = selected_families[index],
                         .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR address_commands{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR, .deviceAddressCommands = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = buffer_copies ? nullptr : &address_commands, .synchronization2 = VK_TRUE,
    };
    const VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &features13, .timelineSemaphore = VK_TRUE, .bufferDeviceAddress = VK_TRUE,
    };
    const char* extension = VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME;
    const VkDeviceCreateInfo device_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &features12, .queueCreateInfoCount = 3, .pQueueCreateInfos = queues,
        .enabledExtensionCount = buffer_copies ? 0u : 1u, .ppEnabledExtensionNames = &extension,
    };
    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(physical_device, &device_info, nullptr, &device));
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
    Worker workers[3]{};
    for (uint32 index = 0; index < 3; ++index)
    {
        Worker& worker = workers[index];
        worker.device = device;
        worker.family = selected_families[index];
        worker.iterations = iterations;
        worker.buffer_copies = buffer_copies;
        worker.host_fence = host_fence;
        worker.copy_memory = reinterpret_cast<PFN_vkCmdCopyMemoryKHR>(vkGetDeviceProcAddr(device, "vkCmdCopyMemoryKHR"));
        vkGetDeviceQueue(device, worker.family, 0, &worker.queue);
        const VkCommandPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = worker.family,
        };
        VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &worker.pool));
        const VkCommandBufferAllocateInfo commands{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = worker.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
        };
        VK_CHECK(vkAllocateCommandBuffers(device, &commands, &worker.commands));
        const VkSemaphoreTypeCreateInfo timeline{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &timeline};
        VK_CHECK(vkCreateSemaphore(device, &semaphore_info, nullptr, &worker.timeline));
        worker.upload = create_buffer(device, memory, selected_families, true);
        worker.scratch = create_buffer(device, memory, selected_families, false);
        worker.readback = create_buffer(device, memory, selected_families, true);
    }
    printf("Copies: %s; host: %s; queues: %u (1=general, 2=compute, 4=copy); no query pools.\n",
           buffer_copies ? "vkCmdCopyBuffer2" : "vkCmdCopyMemoryKHR", serial ? "serial" : "parallel", queue_mask);
#if defined(_WIN32)
    HANDLE threads[3]{};
#else
    pthread_t threads[3]{};
#endif
    for (uint32 index = 0; index < 3; ++index)
    {
        if (!(queue_mask & (1u << index))) continue;
        if (serial) { run(workers[index]); continue; }
#if defined(_WIN32)
        threads[index] = CreateThread(nullptr, 0, run_thread, workers + index, 0, nullptr);
        if (!threads[index]) stop(1);
#else
        if (pthread_create(threads + index, nullptr, run_thread, workers + index) != 0) stop(1);
#endif
    }
    if (!serial)
    {
        for (uint32 index = 0; index < 3; ++index)
        {
            if (!(queue_mask & (1u << index))) continue;
#if defined(_WIN32)
            if (WaitForSingleObject(threads[index], 30000) != WAIT_OBJECT_0) { fputs("Worker timed out.\n", stderr); stop(4); }
            CloseHandle(threads[index]);
#else
            if (pthread_join(threads[index], nullptr) != 0) return 4;
#endif
        }
    }
    VK_CHECK(vkDeviceWaitIdle(device));
    for (Worker& worker : workers)
    {
        vkDestroyCommandPool(device, worker.pool, nullptr);
        vkDestroySemaphore(device, worker.timeline, nullptr);
        const Buffer buffers[]{worker.upload, worker.scratch, worker.readback};
        for (const Buffer& buffer : buffers)
        {
            if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
            vkDestroyBuffer(device, buffer.buffer, nullptr);
            vkFreeMemory(device, buffer.memory, nullptr);
        }
    }
    vkDestroyDevice(device, nullptr);
    if (messenger)
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"))(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
