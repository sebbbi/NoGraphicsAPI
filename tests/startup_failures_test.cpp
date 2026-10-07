#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>
#include <NoGraphicsAPIUtility/upload_queue.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#endif
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
struct NativeResources
{
    int32 instances, devices, buffers, memory, semaphores, images, views, pools, queries, pipelines;
};
NativeResources live{};
thread_local const char* failure_operation = nullptr;
thread_local VkResult failure_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
thread_local uint32 failure_skip = 0;
uint32 command_allocations = 0, command_frees = 0;
uint32 failures = 0, reports = 0;
bool reported_fatal = false;
char diagnostic[1024]{};

bool inject(const char* operation) noexcept
{
    if (!failure_operation || strcmp(failure_operation, operation)) return false;
    if (failure_skip) { --failure_skip; return false; }
    failure_operation = nullptr;
    return true;
}

#define TEST_CREATE(name, parameters, arguments, counter) \
    VkResult VKAPI_CALL test_##name parameters \
    { \
        if (inject(#name)) return failure_result; \
        const VkResult result = name arguments; \
        if (result == VK_SUCCESS) ++live.counter; \
        return result; \
    }
#define TEST_DESTROY(name, parameters, arguments, handle, counter) \
    void VKAPI_CALL test_##name parameters \
    { \
        if (handle) --live.counter; \
        name arguments; \
    }
#define TEST_RESULT(name, parameters, arguments) \
    VkResult VKAPI_CALL test_##name parameters \
    { \
        return inject(#name) ? failure_result : name arguments; \
    }

TEST_CREATE(vkCreateInstance, (const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* instance),
    (info, allocator, instance), instances)
TEST_CREATE(vkCreateDevice, (VkPhysicalDevice physical, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator, VkDevice* device),
    (physical, info, allocator, device), devices)
TEST_CREATE(vkCreateBuffer, (VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* allocator, VkBuffer* buffer),
    (device, info, allocator, buffer), buffers)
TEST_CREATE(vkAllocateMemory, (VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* allocator, VkDeviceMemory* memory),
    (device, info, allocator, memory), memory)
TEST_CREATE(vkCreateSemaphore, (VkDevice device, const VkSemaphoreCreateInfo* info, const VkAllocationCallbacks* allocator, VkSemaphore* semaphore),
    (device, info, allocator, semaphore), semaphores)
TEST_CREATE(vkCreateImage, (VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator, VkImage* image),
    (device, info, allocator, image), images)
TEST_CREATE(vkCreateImageView, (VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* allocator, VkImageView* view),
    (device, info, allocator, view), views)
TEST_CREATE(vkCreateCommandPool, (VkDevice device, const VkCommandPoolCreateInfo* info, const VkAllocationCallbacks* allocator, VkCommandPool* pool),
    (device, info, allocator, pool), pools)
TEST_CREATE(vkCreateQueryPool, (VkDevice device, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks* allocator, VkQueryPool* pool),
    (device, info, allocator, pool), queries)
TEST_CREATE(vkCreateComputePipelines, (VkDevice device, VkPipelineCache cache, uint32 count, const VkComputePipelineCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines), (device, cache, count, info, allocator, pipelines), pipelines)
TEST_CREATE(vkCreateGraphicsPipelines, (VkDevice device, VkPipelineCache cache, uint32 count, const VkGraphicsPipelineCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines), (device, cache, count, info, allocator, pipelines), pipelines)
TEST_RESULT(vkBindBufferMemory, (VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset), (device, buffer, memory, offset))
TEST_RESULT(vkBindImageMemory, (VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset), (device, image, memory, offset))
TEST_RESULT(vkMapMemory, (VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** data),
    (device, memory, offset, size, flags, data))
TEST_DESTROY(vkDestroyInstance, (VkInstance instance, const VkAllocationCallbacks* allocator), (instance, allocator), instance, instances)
TEST_DESTROY(vkDestroyDevice, (VkDevice device, const VkAllocationCallbacks* allocator), (device, allocator), device, devices)
TEST_DESTROY(vkDestroyBuffer, (VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator), (device, buffer, allocator), buffer, buffers)
TEST_DESTROY(vkFreeMemory, (VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator), (device, memory, allocator), memory, memory)
TEST_DESTROY(vkDestroySemaphore, (VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks* allocator),
    (device, semaphore, allocator), semaphore, semaphores)
TEST_DESTROY(vkDestroyImage, (VkDevice device, VkImage image, const VkAllocationCallbacks* allocator), (device, image, allocator), image, images)
TEST_DESTROY(vkDestroyImageView, (VkDevice device, VkImageView view, const VkAllocationCallbacks* allocator), (device, view, allocator), view, views)
TEST_DESTROY(vkDestroyCommandPool, (VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator), (device, pool, allocator), pool, pools)
TEST_DESTROY(vkDestroyQueryPool, (VkDevice device, VkQueryPool pool, const VkAllocationCallbacks* allocator), (device, pool, allocator), pool, queries)
TEST_DESTROY(vkDestroyPipeline, (VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator),
    (device, pipeline, allocator), pipeline, pipelines)

VkResult VKAPI_CALL test_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* commands)
{
    if (inject("vkAllocateCommandBuffers")) return failure_result;
    const VkResult result = vkAllocateCommandBuffers(device, info, commands);
    if (result == VK_SUCCESS) command_allocations += info->commandBufferCount;
    return result;
}

void VKAPI_CALL test_vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32 count, const VkCommandBuffer* commands)
{
    command_frees += count;
    vkFreeCommandBuffers(device, pool, count, commands);
}

#undef TEST_CREATE
#undef TEST_DESTROY
#undef TEST_RESULT
}

// Inject only the test translation unit's native calls; normal calls still use the real driver.
#define vkCreateInstance test_vkCreateInstance
#define vkCreateDevice test_vkCreateDevice
#define vkCreateBuffer test_vkCreateBuffer
#define vkAllocateMemory test_vkAllocateMemory
#define vkBindBufferMemory test_vkBindBufferMemory
#define vkMapMemory test_vkMapMemory
#define vkCreateSemaphore test_vkCreateSemaphore
#define vkCreateImage test_vkCreateImage
#define vkBindImageMemory test_vkBindImageMemory
#define vkCreateImageView test_vkCreateImageView
#define vkCreateCommandPool test_vkCreateCommandPool
#define vkAllocateCommandBuffers test_vkAllocateCommandBuffers
#define vkCreateQueryPool test_vkCreateQueryPool
#define vkCreateComputePipelines test_vkCreateComputePipelines
#define vkCreateGraphicsPipelines test_vkCreateGraphicsPipelines
#define vkDestroyInstance test_vkDestroyInstance
#define vkDestroyDevice test_vkDestroyDevice
#define vkDestroyBuffer test_vkDestroyBuffer
#define vkFreeMemory test_vkFreeMemory
#define vkDestroySemaphore test_vkDestroySemaphore
#define vkDestroyImage test_vkDestroyImage
#define vkDestroyImageView test_vkDestroyImageView
#define vkDestroyCommandPool test_vkDestroyCommandPool
#define vkFreeCommandBuffers test_vkFreeCommandBuffers
#define vkDestroyQueryPool test_vkDestroyQueryPool
#define vkDestroyPipeline test_vkDestroyPipeline
#include "../src/NoGraphicsAPI.cpp"

namespace
{
void check(bool condition, const char* message) noexcept
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s [%s]\n", message, diagnostic);
    ++failures;
}

void receive_error(const char* message, bool fatal) noexcept
{
    ++reports;
    reported_fatal |= fatal;
    snprintf(diagnostic, sizeof(diagnostic), "%s", message);
}

void arm(const char* operation, VkResult result = VK_ERROR_OUT_OF_DEVICE_MEMORY, uint32 skip = 0) noexcept
{
    failure_operation = operation;
    failure_result = result;
    failure_skip = skip;
    reports = 0;
    reported_fatal = false;
    diagnostic[0] = '\0';
}

void unchanged(const NativeResources& before) noexcept
{
    check(live.instances == before.instances && live.devices == before.devices && live.buffers == before.buffers && live.memory == before.memory
        && live.semaphores == before.semaphores && live.images == before.images && live.views == before.views && live.pools == before.pools
        && live.queries == before.queries && live.pipelines == before.pipelines, "partial native resources are released");
}

void reported(const char* operation, const NativeResources& before, bool bytes = false) noexcept
{
    char result[64];
    snprintf(result, sizeof(result), "Vulkan result %d", int(failure_result));
    check(!failure_operation, "the selected native failure was reached");
    check(reports == 1 && !reported_fatal, "one nonfatal diagnostic accompanies the returned failure");
    check(strstr(diagnostic, operation) && strstr(diagnostic, result), "diagnostic identifies the failing native operation and result");
    if (bytes) check(strstr(diagnostic, "MiB requested") != nullptr, "allocation diagnostics include the requested memory size");
    unchanged(before);
}

void heap_failures(gpu::Device* device) noexcept
{
    const char* operations[]{"vkCreateBuffer", "vkAllocateMemory", "vkBindBufferMemory", "vkMapMemory"};
    for (const char* operation : operations)
    {
        const NativeResources before = live;
        arm(operation, strcmp(operation, "vkMapMemory") == 0 ? VK_ERROR_MEMORY_MAP_FAILED : VK_ERROR_OUT_OF_DEVICE_MEMORY);
        const gpu::GpuHeap failed = gpu::create_gpu_heap(device, 1024 * 1024, gpu::MemoryType::cpu_visible);
        check(!failed.owner && !failed.range.cpu && !failed.range.gpu && !failed.range.size, "failed mapped heap returns an empty result");
        reported(operation, before, true);
        const gpu::GpuHeap retry = gpu::create_gpu_heap(device, 1024 * 1024, gpu::MemoryType::cpu_visible);
        check(retry.owner && retry.range.cpu && retry.range.gpu && retry.range.size == 1024 * 1024, "mapped heap creation succeeds after failure");
        gpu::destroy_gpu_heap(retry);
        unchanged(before);
    }
    const NativeResources before = live;
    arm("vkAllocateMemory");
    check(!gpu::create_texture_descriptor_heap(device, 4), "descriptor heap propagates backing allocation failure");
    reported("vkAllocateMemory", before, true);
    gpu::TextureDescriptorHeap* textures = gpu::create_texture_descriptor_heap(device, 4);
    check(textures != nullptr, "texture descriptor heap creation recovers");
    gpu::destroy_texture_descriptor_heap(textures);
    arm("vkMapMemory", VK_ERROR_MEMORY_MAP_FAILED);
    check(!gpu::create_sampler_descriptor_heap(device, 4), "sampler heap propagates backing map failure");
    reported("vkMapMemory", before, true);
    gpu::SamplerDescriptorHeap* samplers = gpu::create_sampler_descriptor_heap(device, 4);
    check(samplers != nullptr, "sampler descriptor heap creation recovers");
    gpu::destroy_sampler_descriptor_heap(samplers);
    unchanged(before);
}

void texture_failures(gpu::Device* device) noexcept
{
    const gpu::TextureDesc desc{.extent = {.x = 16, .y = 16, .z = 1}, .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment};
    const gpu::SizeAlign size = gpu::get_texture_size_align(device, desc);
    check(size.size != 0, "ordinary texture requirements are available");
    if (!size.size) return;
    const NativeResources initial = live;
    arm("vkAllocateMemory");
    const gpu::TextureHeap failed = gpu::create_texture_heap(device, size.size);
    check(!failed.owner && !failed.size, "texture heap allocation failure returns an empty result");
    reported("vkAllocateMemory", initial, true);
    const gpu::TextureHeap heap = gpu::create_texture_heap(device, size.size);
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    gpu::CommandBuffer* commands = pool ? gpu::begin_commands(pool) : nullptr;
    check(heap.owner && commands, "texture creation resources recover");
    if (heap.owner && commands)
    {
        const NativeResources before = live;
        const char* operations[]{"vkCreateImage", "vkBindImageMemory"};
        for (const char* operation : operations)
        {
            arm(operation);
            check(!gpu::create_texture(commands, desc, heap, 0), "image creation or binding failure returns null");
            reported(operation, before);
            gpu::Texture* retry = gpu::create_texture(commands, desc, heap, 0);
            check(retry != nullptr, "image creation succeeds after failure");
            gpu::end_commands(commands);
            gpu::reset_command_pool(pool);
            gpu::destroy_texture(retry);
            commands = gpu::begin_commands(pool);
            unchanged(before);
        }
        gpu::Texture* texture = gpu::create_texture(commands, desc, heap, 0);
        check(texture != nullptr, "render view fixture texture is available");
        if (texture)
        {
            const NativeResources before_view = live;
            arm("vkCreateImageView");
            check(!gpu::create_render_view(texture), "render view creation failure returns null");
            reported("vkCreateImageView", before_view);
            gpu::RenderView* view = gpu::create_render_view(texture);
            check(view != nullptr, "render view creation succeeds after failure");
            gpu::destroy_render_view(view);
            gpu::end_commands(commands);
            gpu::reset_command_pool(pool);
            gpu::destroy_texture(texture);
            commands = gpu::begin_commands(pool);
        }
        gpu::end_commands(commands);
    }
    gpu::destroy_command_pool(pool);
    gpu::destroy_texture_heap(heap);
    unchanged(initial);
}

void utility_failures(gpu::Device* device) noexcept
{
    const NativeResources initial = live;
    const char* upload_operations[]{"vkCreateBuffer", "vkAllocateMemory", "vkMapMemory", "vkCreateSemaphore", "vkCreateCommandPool",
        "vkAllocateCommandBuffers", "vkCreateQueryPool"};
    for (uint32 index = 0; index < sizeof(upload_operations) / sizeof(upload_operations[0]); ++index)
    {
        arm(upload_operations[index], index == 2 ? VK_ERROR_MEMORY_MAP_FAILED : VK_ERROR_OUT_OF_DEVICE_MEMORY, index >= 4 ? 1 : 0);
        gpu::UploadQueue failed(device, 65536);
        check(!failed.valid() && !failed.stats().capacity && !failed.stats().pending_batches, "failed uploader constructor leaves an empty invalid queue");
        reported(upload_operations[index], initial, index <= 2);
        failed.destroy();
        unchanged(initial);
        gpu::UploadQueue retry(device, 65536);
        check(retry.valid() && retry.stats().capacity == 65536, "uploader construction succeeds after native failure");
        gpu::UploadQueue moved(static_cast<gpu::UploadQueue&&>(retry));
        check(moved.valid() && !retry.valid(), "uploader validity follows ownership after a move");
        moved.destroy();
        check(!moved.valid(), "destroying the uploader clears validity");
        unchanged(initial);
    }

    const gpu::TextureDesc desc{.extent = {.x = 16, .y = 16, .z = 1}};
    const gpu::SizeAlign size = gpu::get_texture_size_align(device, desc);
    const uint64 alignment = gpu::get_device_caps(device).texture_heap_alignment;
    const gpu::TextureHeap heap = gpu::create_texture_heap(device, (size.size + alignment - 1) & ~(alignment - 1));
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    check(heap.owner && pool, "texture allocator failure fixture is available");
    if (heap.owner && pool)
    {
        gpu::TextureAllocator allocator(device, heap, 1);
        const char* operations[]{"vkCreateImage", "vkBindImageMemory"};
        for (const char* operation : operations)
        {
            gpu::CommandBuffer* commands = gpu::begin_commands(pool);
            check(commands != nullptr, "texture allocator initialization commands are available");
            if (!commands) break;
            const NativeResources before = live;
            arm(operation);
            gpu::PlacedTexture failed = allocator.allocate(commands, desc);
            check(!failed.texture && !failed.token, "failed texture creation returns an empty allocator result");
            reported(operation, before);
            gpu::PlacedTexture retry = allocator.allocate(commands, desc);
            check(retry.texture != nullptr, "failed texture reservation is reusable in a one-slot heap");
            check(!allocator.allocate(commands, desc).texture, "successful retry consumes the only texture reservation");
            gpu::end_commands(commands);
            gpu::reset_command_pool(pool);
            allocator.free(retry);
            check(!retry.texture && !retry.token, "freeing the successful retry restores an empty allocation");
            unchanged(before);
        }
    }
    gpu::destroy_command_pool(pool);
    gpu::destroy_texture_heap(heap);
    unchanged(initial);
}

void command_failures(gpu::Device* device) noexcept
{
    const NativeResources before = live;
    arm("vkCreateSemaphore");
    check(!gpu::create_timeline_semaphore(device), "timeline creation failure returns null");
    reported("vkCreateSemaphore", before);
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    check(timeline != nullptr, "timeline creation succeeds after failure");
    gpu::destroy_timeline_semaphore(timeline);
    arm("vkCreateCommandPool");
    check(!gpu::create_command_pool(device), "command pool creation failure returns null");
    reported("vkCreateCommandPool", before);
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    check(pool != nullptr, "command pool creation succeeds after failure");
    gpu::destroy_command_pool(pool);
    const char* operations[]{"vkAllocateCommandBuffers", "vkCreateQueryPool"};
    for (const char* operation : operations)
    {
        pool = gpu::create_command_pool(device);
        check(pool != nullptr, "command failure fixture pool is available");
        if (!pool) continue;
        const NativeResources before_commands = live;
        const uint32 allocations = command_allocations, frees = command_frees;
        arm(operation);
        check(!gpu::begin_commands(pool), "first command recording failure returns null");
        reported(operation, before_commands);
        check(command_allocations - allocations == command_frees - frees, "failed first command context frees its native command buffer");
        gpu::CommandBuffer* commands = gpu::begin_commands(pool);
        check(commands != nullptr, "first command recording succeeds after failure");
        if (commands) gpu::end_commands(commands);
        gpu::destroy_command_pool(pool);
        unchanged(before);
    }
}

gpu::ByteSpan read_shader(const char* path) noexcept
{
    FILE* file = fopen(path, "rb");
    check(file != nullptr, "compiled pipeline fixture is available");
    if (!file) return {};
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    byte* data = static_cast<byte*>(malloc(size_t(size)));
    const bool valid = size > 0 && fread(data, 1, size_t(size), file) == size_t(size);
    fclose(file);
    check(valid, "compiled pipeline fixture loads completely");
    if (!valid) { free(data); return {}; }
    return {data, uint64(size)};
}

void pipeline_failures(gpu::Device* device) noexcept
{
    const gpu::ByteSpan compute = read_shader(NOGRAPHICSAPI_FAILURE_COMPUTE);
    const gpu::ByteSpan vertex = read_shader(NOGRAPHICSAPI_FAILURE_VERTEX);
    const gpu::ByteSpan fragment = read_shader(NOGRAPHICSAPI_FAILURE_FRAGMENT);
    const NativeResources before = live;
    if (compute.data)
    {
        const gpu::ShaderStage stage{.code = compute, .entry_point = "computeMain"};
        arm("vkCreateComputePipelines");
        check(!gpu::create_compute_pso(device, stage), "compute pipeline failure returns null");
        reported("vkCreateComputePipelines", before);
        gpu::PSO* pso = gpu::create_compute_pso(device, stage);
        check(pso != nullptr, "compute pipeline creation succeeds after failure");
        gpu::destroy_pso(pso);
    }
    if (vertex.data && fragment.data)
    {
        const gpu::ColorTargetDesc target{.format = gpu::Format::rgba8_unorm};
        const gpu::GraphicsPSODesc desc{.vertex = {.code = vertex, .entry_point = "vertexMain"},
            .fragment = {.code = fragment, .entry_point = "fragmentMain"}, .color_targets = {&target, 1}};
        arm("vkCreateGraphicsPipelines");
        check(!gpu::create_graphics_pso(device, desc), "graphics pipeline failure returns null");
        reported("vkCreateGraphicsPipelines", before);
        gpu::PSO* pso = gpu::create_graphics_pso(device, desc);
        check(pso != nullptr, "graphics pipeline creation succeeds after failure");
        gpu::destroy_pso(pso);
    }
    free(const_cast<byte*>(compute.data));
    free(const_cast<byte*>(vertex.data));
    free(const_cast<byte*>(fragment.data));
    unchanged(before);
}

#if defined(_WIN32)
DWORD WINAPI callback_thread(void*) noexcept
{
    arm("vkCreateInstance", VK_ERROR_OUT_OF_HOST_MEMORY);
    const gpu::DeviceInit init = gpu::create_device();
    return !init.device && init.error == gpu::Error::out_of_memory && !failure_operation ? 0 : 1;
}
#endif
}

int main()
{
    gpu::set_error_callback(receive_error);
    arm("vkCreateInstance", VK_ERROR_OUT_OF_HOST_MEMORY);
    gpu::DeviceInit init = gpu::create_device();
    check(!init.device && init.error == gpu::Error::out_of_memory, "instance allocation failure returns an error instead of terminating");
    reported("vkCreateInstance", {});
    arm(nullptr);
    init = gpu::create_device({.timestamp_query_count = 8});
    if (init.error == gpu::Error::unsupported)
    {
        printf("SKIP: required Vulkan device features are unavailable.\n");
        return 77;
    }
    check(init.device && init.error == gpu::Error::none, "device creation succeeds after instance failure");
    if (!init.device) return 1;
    gpu::Device* device = init.device;
    const NativeResources before_device = live;
    arm("vkCreateDevice");
    init = gpu::create_device();
    check(!init.device && init.error == gpu::Error::out_of_memory, "logical device allocation failure returns an error");
    reported("vkCreateDevice", before_device);
    heap_failures(device);
    texture_failures(device);
    command_failures(device);
    utility_failures(device);
    pipeline_failures(device);
    arm("vkCreateBuffer");
    gpu::set_error_callback(nullptr);
    const gpu::GpuHeap silent = gpu::create_gpu_heap(device, 1024);
    check(!silent.owner && !failure_operation && reports == 0, "clearing the callback suppresses diagnostics without changing failure returns");
    gpu::set_error_callback(receive_error);
#if defined(_WIN32)
    arm(nullptr);
    HANDLE worker = CreateThread(nullptr, 0, callback_thread, nullptr, 0, nullptr);
    check(worker != nullptr, "callback isolation worker starts");
    if (worker)
    {
        WaitForSingleObject(worker, INFINITE);
        DWORD result = 1;
        GetExitCodeThread(worker, &result);
        CloseHandle(worker);
        check(result == 0 && reports == 0, "startup callback is not inherited by another thread");
    }
    arm("vkCreateBuffer");
    check(!gpu::create_gpu_heap(device, 1024).owner, "main thread callback remains installed after the worker finishes");
    reported("vkCreateBuffer", before_device, true);
#endif
    gpu::wait_idle(device);
    gpu::destroy_device(device);
    gpu::set_error_callback(nullptr);
    unchanged({});
    printf("Startup failure returns and cleanup: %u failures\n", failures);
    return failures ? 1 : 0;
}
