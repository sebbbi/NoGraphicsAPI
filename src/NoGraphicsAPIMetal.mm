#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <NoGraphicsAPI/shader_shared.h>
#include "BufferAddressMap.hpp"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NDEBUG)
#undef assert
#define assert(expression) ((void)sizeof(static_cast<bool>(expression)))
#endif

namespace gpu
{
namespace
{
constexpr MTLRenderStages render_stages = MTLRenderStageVertex | MTLRenderStageFragment | MTLRenderStageObject | MTLRenderStageMesh;

uint64 align_up(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }
bool has_usage(TextureUsage usage, TextureUsage bit) { return (static_cast<uint32>(usage) & static_cast<uint32>(bit)) != 0; }

MTLStages barrier_stages(Stage stages, bool source)
{
    const uint64 mask = static_cast<uint64>(stages);
    if (mask & static_cast<uint64>(Stage::all_commands)) return MTLStageAll;
    constexpr MTLStages raster = MTLStageFragment | MTLStageTile;
    constexpr MTLStages geometry = MTLStageVertex | MTLStageObject | MTLStageMesh;
    MTLStages result = 0;
    // Preserve Vulkan's logical execution scopes without joining its alternative graphics/compute branches.
    if (mask & static_cast<uint64>(Stage::indirect)) result |= geometry | MTLStageDispatch | (source ? 0 : raster);
    if (mask & static_cast<uint64>(Stage::index_input | Stage::vertex)) result |= MTLStageVertex | (source ? 0 : raster);
    if (mask & static_cast<uint64>(Stage::task)) result |= MTLStageObject | (source ? 0 : MTLStageMesh | raster);
    if (mask & static_cast<uint64>(Stage::mesh)) result |= MTLStageMesh | (source ? MTLStageObject : raster);
    if (mask & static_cast<uint64>(Stage::depth_stencil_tests | Stage::fragment | Stage::color_output)) result |= raster | (source ? geometry : 0);
    if (mask & static_cast<uint64>(Stage::compute)) result |= MTLStageDispatch;
    if (mask & static_cast<uint64>(Stage::transfer)) result |= MTLStageBlit;
    return result;
}

MTLPixelFormat pixel_format(Format format)
{
    switch (format)
    {
    case Format::r8_srgb: return MTLPixelFormatR8Unorm_sRGB;
    case Format::rg8_srgb: return MTLPixelFormatRG8Unorm_sRGB;
    case Format::rgba8_srgb: return MTLPixelFormatRGBA8Unorm_sRGB;
    case Format::bgra8_srgb: return MTLPixelFormatBGRA8Unorm_sRGB;
    case Format::r8_unorm: return MTLPixelFormatR8Unorm;
    case Format::rg8_unorm: return MTLPixelFormatRG8Unorm;
    case Format::rgba8_unorm: return MTLPixelFormatRGBA8Unorm;
    case Format::bgra8_unorm: return MTLPixelFormatBGRA8Unorm;
    case Format::r16_unorm: return MTLPixelFormatR16Unorm;
    case Format::rg16_unorm: return MTLPixelFormatRG16Unorm;
    case Format::rgba16_unorm: return MTLPixelFormatRGBA16Unorm;
    case Format::r8_uint: return MTLPixelFormatR8Uint;
    case Format::rg8_uint: return MTLPixelFormatRG8Uint;
    case Format::rgba8_uint: return MTLPixelFormatRGBA8Uint;
    case Format::r16_uint: return MTLPixelFormatR16Uint;
    case Format::rg16_uint: return MTLPixelFormatRG16Uint;
    case Format::rgba16_uint: return MTLPixelFormatRGBA16Uint;
    case Format::r32_uint: return MTLPixelFormatR32Uint;
    case Format::rg32_uint: return MTLPixelFormatRG32Uint;
    case Format::rgba32_uint: return MTLPixelFormatRGBA32Uint;
    case Format::r16_float: return MTLPixelFormatR16Float;
    case Format::rg16_float: return MTLPixelFormatRG16Float;
    case Format::rgba16_float: return MTLPixelFormatRGBA16Float;
    case Format::r32_float: return MTLPixelFormatR32Float;
    case Format::rg32_float: return MTLPixelFormatRG32Float;
    case Format::rgba32_float: return MTLPixelFormatRGBA32Float;
    case Format::rgb10a2_unorm: return MTLPixelFormatRGB10A2Unorm;
    case Format::rg11b10_float: return MTLPixelFormatRG11B10Float;
    case Format::d16_unorm: return MTLPixelFormatDepth16Unorm;
    case Format::d32_float: return MTLPixelFormatDepth32Float;
    case Format::s8_uint: return MTLPixelFormatStencil8;
    case Format::d32_float_s8_uint: return MTLPixelFormatDepth32Float_Stencil8;
    case Format::eac_rg: return MTLPixelFormatEAC_RG11Unorm;
    case Format::astc_4x4_srgb: return MTLPixelFormatASTC_4x4_sRGB;
    case Format::astc_4x4_unorm: return MTLPixelFormatASTC_4x4_LDR;
    case Format::bc3_srgb: return MTLPixelFormatBC3_RGBA_sRGB;
    case Format::bc3_unorm: return MTLPixelFormatBC3_RGBA;
    case Format::bc5_rg: return MTLPixelFormatBC5_RGUnorm;
    case Format::bc6h_ufloat: return MTLPixelFormatBC6H_RGBUfloat;
    case Format::bc6h_sfloat: return MTLPixelFormatBC6H_RGBFloat;
    case Format::bc7_srgb: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
    case Format::bc7_unorm: return MTLPixelFormatBC7_RGBAUnorm;
    case Format::rgba4_unorm: return MTLPixelFormatABGR4Unorm;
    case Format::r5g5b5a1_unorm: return MTLPixelFormatA1BGR5Unorm;
    case Format::r5g6b5_unorm: return MTLPixelFormatB5G6R5Unorm;
    case Format::bgra8_uint:
    case Format::d24_unorm_s8_uint:
    case Format::undefined: return MTLPixelFormatInvalid;
    }
    return MTLPixelFormatInvalid;
}

MTLTextureType texture_type(TextureType type)
{
    switch (type)
    {
    case TextureType::one_d: return MTLTextureType1D;
    case TextureType::two_d: return MTLTextureType2D;
    case TextureType::three_d: return MTLTextureType3D;
    case TextureType::cube: return MTLTextureTypeCube;
    case TextureType::two_d_array: return MTLTextureType2DArray;
    case TextureType::cube_array: return MTLTextureTypeCubeArray;
    }
    return MTLTextureType2D;
}

MTLTextureDescriptor* texture_descriptor(const TextureDesc& desc)
{
    MTLTextureDescriptor* result = [MTLTextureDescriptor new];
    result.textureType = texture_type(desc.type);
    result.pixelFormat = pixel_format(desc.format);
    result.width = desc.extent.x;
    result.height = desc.extent.y;
    result.depth = desc.extent.z;
    result.mipmapLevelCount = desc.mip_levels;
    result.arrayLength = desc.type == TextureType::cube_array ? desc.layer_count / 6 :
                         desc.type == TextureType::two_d_array ? desc.layer_count : 1;
    result.storageMode = MTLStorageModePrivate;
    result.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    result.usage = MTLTextureUsageUnknown;
    if (has_usage(desc.usage, TextureUsage::sampled)) result.usage |= MTLTextureUsageShaderRead;
    if (has_usage(desc.usage, TextureUsage::storage)) result.usage |= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    if (has_usage(desc.usage, TextureUsage::color_attachment) || has_usage(desc.usage, TextureUsage::depth_stencil_attachment))
        result.usage |= MTLTextureUsageRenderTarget;
    if (desc.mutable_format || get_texture_format_info(desc.format).stencil) result.usage |= MTLTextureUsagePixelFormatView;
    return result;
}

void report_error(const char* operation, NSError* error)
{
    fprintf(stderr, "NoGraphicsAPI Metal: %s: %s\n", operation, error ? error.localizedDescription.UTF8String : "creation failed");
}

MTLSize metal_size(uint32x3 size) { return MTLSizeMake(size.x, size.y, size.z); }
}

struct GpuHeapOwner
{
    Device* device = nullptr;
    id<MTLHeap> heap = nil;
    id<MTLBuffer> buffer = nil;
    detail::BufferRecord* record = nullptr;
};

struct TextureHeapOwner
{
    Device* device = nullptr;
    id<MTLHeap> heap = nil;
};

struct Texture
{
    Device* device = nullptr;
    id<MTLTexture> texture = nil;
    TextureDesc desc = {};
};

struct RenderView
{
    id<MTLTexture> texture = nil;
    uint32 mip = 0;
    uint32 slice = 0;
};

struct TextureDescriptorHeap
{
    Device* device = nullptr;
    id<MTLTextureViewPool> pool = nil;
    id<MTLBuffer> base = nil;
    id<MTLTexture>* views = nullptr;
    uint32 capacity = 0;
};

struct SamplerDescriptorHeap
{
    Device* device = nullptr;
    id<MTLBuffer> buffer = nil;
    id<MTLSamplerState>* samplers = nullptr;
    uint32 capacity = 0;
};

struct TimelineSemaphore
{
    Device* device = nullptr;
    id<MTLSharedEvent> event = nil;
};

struct PSO
{
    Device* device = nullptr;
    id<MTLRenderPipelineState> render = nil;
    id<MTLComputePipelineState> compute = nil;
    RasterizationState rasterization = {};
    MTLSize threads = {1, 1, 1};
    MTLSize object_threads = {1, 1, 1};
    bool mesh = false;
};

struct CounterPage
{
    id<MTL4CounterHeap> heap = nil;
    CounterPage* next = nullptr;
    uint64 occupied[64] = {};
};

struct TimestampSlot
{
    id<MTL4CounterHeap> heap = nil;
    CounterPage* page = nullptr;
    uint64* destination = nullptr;
    uint32 index = 0;
};

struct NativeCommandBuffer
{
    id<MTL4CommandBuffer> buffer = nil;
    id<MTL4CommandAllocator> allocator = nil;
    NativeCommandBuffer* next = nullptr;
};

struct CommandBuffer
{
    Device* device = nullptr;
    CommandPool* pool = nullptr;
    CommandBuffer* next = nullptr;
    id<MTL4CommandBuffer> commands = nil;
    id<MTLCommandBuffer> commands3 = nil;
    NativeCommandBuffer* native_buffers = nullptr;
    NativeCommandBuffer* native = nullptr;
    uint32 native_count = 0;
    id<MTL4ArgumentTable> arguments = nil;
    id compute = nil;
    id render = nil;
    id<MTLBlitCommandEncoder> blit = nil;
    MTL4RenderPassDescriptor* pass = nil;
    MTLRenderPassDescriptor* pass3 = nil;
    id<MTLBuffer> bindings[3] = {};
    uint64 binding_offsets[3] = {};
    TimestampSlot* timestamps = nullptr;
    uint32 timestamp_count = 0;
    id<MTLSharedEvent> retirement = nil;
    uint64 retirement_value = 0;
    bool recording = false;
    bool ended = false;
    bool render_continuation = false;
    bool acquired = false;
    const PSO* pso = nullptr;
};

struct DepthStateEntry
{
    DepthStencilState desc = {};
    id<MTLDepthStencilState> state = nil;
};

struct DepthStateChunk
{
    DepthStateEntry entries[64] = {};
    uint32 count = 0;
    DepthStateChunk* next = nullptr;
};

enum class QueueKind : uint8 { general, compute, copy };

struct Queue
{
    id<MTL4CommandQueue> queue = nil;
    id<MTLCommandQueue> queue3 = nil;
    // Metal 3 fences collect producer passes and publish explicit barriers in submission order.
    id<MTLFence> producers = nil;
    id<MTLFence> dependencies = nil;
    dispatch_queue_t feedback = nullptr;
    id<MTLSharedEvent> completion = nil;
    uint64 submitted_value = 0;
    id<MTL4CommandBuffer>* submission = nullptr;
    size_t submission_capacity = 0;
    QueueKind kind = QueueKind::general;
};

struct CommandPool
{
    Device* device = nullptr;
    Queue* queue = nullptr;
    QueueKind kind = QueueKind::general;
    CommandBuffer* first = nullptr;
    CommandBuffer* last = nullptr;
    CommandBuffer* next_buffer = nullptr;
    DepthStateChunk* depth_states = nullptr;
};

struct Device
{
    id<MTLDevice> metal = nil;
    Queue* queues = nullptr;
    id<MTL4Compiler> compiler = nil;
    id<MTLResidencySet> residency = nil;
    CAMetalLayer* layer = nil;
    id<CAMetalDrawable> drawable = nil;
    CommandBuffer* acquired = nullptr;
    RenderView drawable_view = {};
    DeviceCaps caps = {};
    char name[256] = {};
    os_unfair_lock residency_lock = OS_UNFAIR_LOCK_INIT;
    detail::BufferAddressMap buffer_map = {};
    CounterPage* counter_pages = nullptr;
    uint32 counter_page_count = 0;
    uint32 timestamp_query_count = 0;
    bool shader_validation = false;
    bool metal4 = false;
    bool presenting = false;
};

namespace
{
void add_resident(Device* device, id<MTLAllocation> allocation)
{
    os_unfair_lock_lock(&device->residency_lock);
    [device->residency addAllocation:allocation];
    [device->residency commit];
    os_unfair_lock_unlock(&device->residency_lock);
}

void remove_resident(Device* device, id<MTLAllocation> allocation)
{
    os_unfair_lock_lock(&device->residency_lock);
    [device->residency removeAllocation:allocation];
    [device->residency commit];
    os_unfair_lock_unlock(&device->residency_lock);
}

bool allocate_timestamps(Device* device, TimestampSlot* slots)
{
    uint32 allocated = 0;
    for (;;)
    {
        CounterPage* head = __atomic_load_n(&device->counter_pages, __ATOMIC_ACQUIRE);
        uint32 published = 0;
        for (CounterPage* page = head; page; page = page->next)
        {
            ++published;
            for (uint32 word = 0; word < 64; ++word)
            {
                uint64 occupied = __atomic_load_n(&page->occupied[word], __ATOMIC_RELAXED);
                while (occupied != ~uint64{0})
                {
                    uint64 available = ~occupied;
                    uint64 claim = 0;
                    for (uint32 i = allocated; i < device->timestamp_query_count && available; ++i)
                    {
                        claim |= uint64{1} << __builtin_ctzll(available);
                        available &= available - 1;
                    }
                    if (!__atomic_compare_exchange_n(&page->occupied[word], &occupied, occupied | claim, true,
                                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
                    while (claim)
                    {
                        const uint32 bit = static_cast<uint32>(__builtin_ctzll(claim));
                        slots[allocated++] = {.heap = page->heap, .page = page, .index = word * 64 + bit};
                        claim &= claim - 1;
                    }
                    if (allocated == device->timestamp_query_count) return true;
                    break;
                }
            }
        }
        if (__atomic_load_n(&device->counter_pages, __ATOMIC_ACQUIRE) != head) continue;
        uint32 page_count = __atomic_load_n(&device->counter_page_count, __ATOMIC_RELAXED);
        if (page_count == 32)
        {
            if (published != 32) continue;
            report_error("timestamp storage capacity exhausted", nil);
            return false;
        }
        if (!__atomic_compare_exchange_n(&device->counter_page_count, &page_count, page_count + 1, true,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED)) continue;
        if (__atomic_load_n(&device->counter_pages, __ATOMIC_ACQUIRE) != head)
        {
            __atomic_fetch_sub(&device->counter_page_count, 1u, __ATOMIC_RELAXED);
            continue;
        }
        NSError* error = nil;
        MTL4CounterHeapDescriptor* counters = [MTL4CounterHeapDescriptor new];
        counters.type = MTL4CounterHeapTypeTimestamp;
        counters.count = 4096;
        CounterPage* page = new CounterPage{.heap = [device->metal newCounterHeapWithDescriptor:counters error:&error]};
        [counters release];
        if (!page->heap)
        {
            __atomic_fetch_sub(&device->counter_page_count, 1u, __ATOMIC_RELAXED);
            report_error("timestamp storage", error);
            delete page;
            return false;
        }
        // Reserve native capacity before creation, and retain every successfully created page after publication races.
        do { page->next = head; }
        while (!__atomic_compare_exchange_n(&device->counter_pages, &head, page, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    }
}

void destroy_context(Device* device, CommandBuffer* commands)
{
    for (NativeCommandBuffer* native = commands->native_buffers; native;)
    {
        NativeCommandBuffer* next = native->next;
        [native->buffer release];
        [native->allocator release];
        delete native;
        native = next;
    }
    [commands->arguments release];
    [commands->pass release];
    [commands->pass3 release];
    [commands->commands3 release];
    for (uint32 i = 0; i < device->timestamp_query_count && commands->timestamps && commands->timestamps[i].page;)
    {
        CounterPage* page = commands->timestamps[i].page;
        const uint32 word = commands->timestamps[i].index / 64;
        uint64 release = 0;
        do { release |= uint64{1} << (commands->timestamps[i++].index % 64); }
        while (i < device->timestamp_query_count && commands->timestamps[i].page == page && commands->timestamps[i].index / 64 == word);
        __atomic_fetch_and(&page->occupied[word], ~release, __ATOMIC_RELEASE);
    }
    delete[] commands->timestamps;
    delete commands;
}

CommandBuffer* create_context(CommandPool* pool)
{
    Device* device = pool->device;
    CommandBuffer* result = new CommandBuffer{.device = device, .pool = pool};
    if (!device->metal4)
    {
        result->pass3 = [MTLRenderPassDescriptor new];
        return result;
    }
    NSError* error = nil;
    result->native_buffers = new NativeCommandBuffer{.buffer = [device->metal newCommandBuffer], .allocator = [device->metal newCommandAllocator]};
    result->commands = result->native_buffers->buffer;
    MTL4ArgumentTableDescriptor* arguments = [MTL4ArgumentTableDescriptor new];
    arguments.maxBufferBindCount = 3;
    arguments.initializeBindings = YES;
    result->arguments = [device->metal newArgumentTableWithDescriptor:arguments error:&error];
    [arguments release];
    result->pass = [MTL4RenderPassDescriptor new];
    if (device->timestamp_query_count)
    {
        result->timestamps = new TimestampSlot[device->timestamp_query_count]{};
        if (!allocate_timestamps(device, result->timestamps))
        {
            destroy_context(device, result);
            return nullptr;
        }
    }
    if (!result->commands || !result->native_buffers->allocator || !result->arguments)
    {
        report_error("command context", error);
        destroy_context(device, result);
        return nullptr;
    }
    return result;
}

id<MTLDepthStencilState> depth_state(CommandPool* pool, const DepthStencilState& state);

void end_compute(CommandBuffer* commands)
{
    if (commands->compute)
    {
        if (!commands->device->metal4) [(id<MTLComputeCommandEncoder>)commands->compute updateFence:commands->pool->queue->producers];
        [commands->compute endEncoding];
        [commands->compute release];
        commands->compute = nil;
    }
    if (commands->blit)
    {
        [commands->blit updateFence:commands->pool->queue->producers];
        [commands->blit endEncoding];
        [commands->blit release];
        commands->blit = nil;
    }
}

id<MTL4CommandBuffer> native_commands(CommandBuffer* commands)
{
    if (!commands->commands)
    {
        if (!commands->native->next)
            commands->native->next = new NativeCommandBuffer{.buffer = [commands->device->metal newCommandBuffer],
                                                           .allocator = [commands->device->metal newCommandAllocator]};
        commands->native = commands->native->next;
        commands->commands = commands->native->buffer;
        ++commands->native_count;
        [commands->commands beginCommandBufferWithAllocator:commands->native->allocator];
    }
    return commands->commands;
}

id compute_encoder(CommandBuffer* commands)
{
    assert(commands->recording && !commands->render);
    if (!commands->compute)
    {
        end_compute(commands);
        if (commands->device->metal4)
        {
            commands->compute = [[native_commands(commands) computeCommandEncoder] retain];
            [commands->compute setArgumentTable:commands->arguments];
        }
        else
        {
            commands->compute = [[commands->commands3 computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent] retain];
            [(id<MTLComputeCommandEncoder>)commands->compute waitForFence:commands->pool->queue->dependencies];
            for (uint32 i = 0; i < 3; ++i)
                [(id<MTLComputeCommandEncoder>)commands->compute setBuffer:commands->bindings[i] offset:commands->binding_offsets[i] atIndex:i];
        }
        if (commands->pso && commands->pso->compute) [commands->compute setComputePipelineState:commands->pso->compute];
    }
    return commands->compute;
}

id copy_encoder(CommandBuffer* commands)
{
    if (commands->device->metal4) return compute_encoder(commands);
    assert(commands->recording && !commands->render);
    if (!commands->blit)
    {
        end_compute(commands);
        commands->blit = [[commands->commands3 blitCommandEncoder] retain];
        [commands->blit waitForFence:commands->pool->queue->dependencies];
    }
    return commands->blit;
}

void bind_buffer(CommandBuffer* commands, id<MTLBuffer> buffer, uint64 offset, uint32 index)
{
    commands->bindings[index] = buffer;
    commands->binding_offsets[index] = offset;
    if (commands->compute) [(id<MTLComputeCommandEncoder>)commands->compute setBuffer:buffer offset:offset atIndex:index];
    if (commands->render)
    {
        [(id<MTLRenderCommandEncoder>)commands->render setVertexBuffer:buffer offset:offset atIndex:index];
        [(id<MTLRenderCommandEncoder>)commands->render setFragmentBuffer:buffer offset:offset atIndex:index];
        [(id<MTLRenderCommandEncoder>)commands->render setObjectBuffer:buffer offset:offset atIndex:index];
        [(id<MTLRenderCommandEncoder>)commands->render setMeshBuffer:buffer offset:offset atIndex:index];
    }
}

id<MTLBuffer> resolve_buffer(Device* device, GpuRange range, uint64* offset)
{
    return reinterpret_cast<id<MTLBuffer>>(device->buffer_map.resolve(range, offset));
}
}

DeviceInit create_device(const DeviceDesc& desc) noexcept
{
    @autoreleasepool
    {
        Device* device = new Device{};
        // MetalTools on macOS 26.6.2 cannot enumerate placed resources through heap residency.
        const char* shader_validation = getenv("MTL_SHADER_VALIDATION");
        device->shader_validation = shader_validation && atoi(shader_validation) != 0;
        device->metal = MTLCreateSystemDefaultDevice();
        if (!device->metal || ![device->metal supportsFamily:MTLGPUFamilyApple7] || ![device->metal supportsFamily:MTLGPUFamilyMetal3])
        {
            destroy_device(device);
            return {.error = Error::unsupported};
        }
        if (@available(macOS 26.0, iOS 26.0, *)) device->metal4 = [device->metal supportsFamily:MTLGPUFamilyMetal4];
#if defined(NOGRAPHICSAPI_TEST_BUFFER_COMMANDS)
        device->metal4 = false;
#endif
        NSError* error = nil;
        assert(desc.desired_queue_count);
        device->caps.general_queue_count = desc.desired_queue_count;
        device->caps.compute_queue_count = desc.desired_compute_queue_count;
        device->caps.copy_queue_count = desc.desired_copy_queue_count;
        device->caps.queue_count = desc.desired_queue_count + desc.desired_compute_queue_count + desc.desired_copy_queue_count;
        device->queues = new Queue[device->caps.queue_count]{};
        for (uint32 i = 0; i < device->caps.queue_count; ++i)
        {
            Queue& queue = device->queues[i];
            queue.kind = i < desc.desired_queue_count ? QueueKind::general :
                         i < desc.desired_queue_count + desc.desired_compute_queue_count ? QueueKind::compute : QueueKind::copy;
            if (device->metal4)
            {
                queue.feedback = dispatch_queue_create("NoGraphicsAPI Metal feedback", DISPATCH_QUEUE_SERIAL_WITH_AUTORELEASE_POOL);
                MTL4CommandQueueDescriptor* descriptor = [MTL4CommandQueueDescriptor new];
                descriptor.feedbackQueue = queue.feedback;
                queue.queue = [device->metal newMTL4CommandQueueWithDescriptor:descriptor error:&error];
                // This SDK's destructor releases its assign-only feedbackQueue; retain ownership in Queue.
                descriptor.feedbackQueue = nullptr;
                [descriptor release];
            }
            else
            {
                queue.queue3 = [device->metal newCommandQueueWithMaxCommandBufferCount:4096];
                queue.producers = [device->metal newFence];
                queue.dependencies = [device->metal newFence];
            }
            queue.completion = [device->metal newSharedEvent];
            if ((!queue.queue && !queue.queue3) || !queue.completion || (!device->metal4 && (!queue.producers || !queue.dependencies)))
            {
                report_error("command queue", error);
                destroy_device(device);
                return {.error = Error::driver_error};
            }
        }
        if (device->metal4)
        {
            MTL4CompilerDescriptor* compiler = [MTL4CompilerDescriptor new];
            device->compiler = [device->metal newCompilerWithDescriptor:compiler error:&error];
            [compiler release];
        }
        MTLResidencySetDescriptor* residency = [MTLResidencySetDescriptor new];
        residency.initialCapacity = 256;
        device->residency = [device->metal newResidencySetWithDescriptor:residency error:&error];
        [residency release];
        if ((device->metal4 && !device->compiler) || !device->residency)
        {
            report_error("device", error);
            destroy_device(device);
            return {.error = Error::driver_error};
        }
        for (uint32 i = 0; i < device->caps.queue_count; ++i)
        {
            [device->queues[i].queue addResidencySet:device->residency];
            [device->queues[i].queue3 addResidencySet:device->residency];
        }
        device->timestamp_query_count = device->metal4 ? desc.timestamp_query_count : 0;
        snprintf(device->name, sizeof(device->name), "%s", device->metal.name.UTF8String);
        mach_timebase_info_data_t timebase = {};
        mach_timebase_info(&timebase);
        device->caps = {
            .device_name = device->name,
            .queue_count = device->caps.queue_count,
            .general_queue_count = desc.desired_queue_count,
            .compute_queue_count = desc.desired_compute_queue_count,
            .copy_queue_count = desc.desired_copy_queue_count,
            .max_push_data_size = 256,
            .texture_heap_alignment = 16384,
            .timestamp_period_ns = device->metal4 ? static_cast<float>(timebase.numer) / static_cast<float>(timebase.denom) : 0.0f,
            .sub_texel_precision_bits = 8,
            .texture_compression_bc = device->metal.supportsBCTextureCompression,
            .texture_compression_astc = true,
            .storage_input_output16 = true,
            .indirect_mesh_draw = [device->metal supportsFamily:MTLGPUFamilyApple9],
        };
        if (desc.window)
        {
            device->layer = [(CAMetalLayer*)desc.window retain];
            device->layer.device = device->metal;
            device->layer.pixelFormat = pixel_format(desc.swapchain_format == Format::undefined ? Format::bgra8_unorm : desc.swapchain_format);
            device->layer.maximumDrawableCount = desc.desired_swapchain_image_count < 3 ? 2 : 3;
            if (device->metal4) [device->queues[0].queue addResidencySet:device->layer.residencySet];
        }
        return {.device = device};
    }
}

void destroy_device(Device* device) noexcept
{
    @autoreleasepool
    {
        if (!device) return;
        assert(!device->drawable);
        if (device->layer && device->metal4) [device->queues[0].queue removeResidencySet:device->layer.residencySet];
        [device->drawable release];
        [device->layer release];
        for (uint32 i = 0; i < device->caps.queue_count; ++i)
        {
            Queue& queue = device->queues[i];
            assert(queue.completion.signaledValue >= queue.submitted_value);
            if (device->residency) [queue.queue removeResidencySet:device->residency];
            if (device->residency) [queue.queue3 removeResidencySet:device->residency];
            [queue.completion release];
            [queue.queue release];
            [queue.queue3 release];
            [queue.producers release];
            [queue.dependencies release];
            if (queue.feedback) dispatch_release(queue.feedback);
            free(queue.submission);
        }
        delete[] device->queues;
        for (CounterPage* page = device->counter_pages; page;)
        {
            CounterPage* next = page->next;
            for (uint32 word = 0; word < 64; ++word) assert(!__atomic_load_n(&page->occupied[word], __ATOMIC_RELAXED));
            [page->heap release];
            delete page;
            page = next;
        }
        [device->residency release];
        [device->compiler release];
        [device->metal release];
        delete device;
    }
}

const DeviceCaps& get_device_caps(const Device* device) noexcept { return device->caps; }

bool supports_texture_format(const Device* device, Format format, TextureUsage usage) noexcept
{
    if (pixel_format(format) == MTLPixelFormatInvalid || format == Format::d24_unorm_s8_uint) return false;
    const TextureFormatInfo info = get_texture_format_info(format);
    if (info.depth && info.stencil &&
        (has_usage(usage, TextureUsage::transfer_source) || has_usage(usage, TextureUsage::transfer_destination))) return false;
    const bool compressed = info.block_extent.x > 1;
    const bool bc = format >= Format::bc3_srgb && format <= Format::bc7_unorm;
    if (bc && !device->caps.texture_compression_bc) return false;
    if (has_usage(usage, TextureUsage::storage))
    {
        if (compressed || info.depth || info.stencil || format == Format::r8_srgb || format == Format::rg8_srgb ||
            format == Format::rgba8_srgb || format == Format::bgra8_srgb || format == Format::rgba4_unorm ||
            format == Format::r5g5b5a1_unorm || format == Format::r5g6b5_unorm) return false;
    }
    if (has_usage(usage, TextureUsage::color_attachment) && (compressed || info.depth || info.stencil)) return false;
    if (has_usage(usage, TextureUsage::depth_stencil_attachment) && !(info.depth || info.stencil)) return false;
    return true;
}

uint32x2 get_drawable_extent(Device* device) noexcept
{
    @autoreleasepool
    {
        if (!device->layer) return {};
        return {.x = static_cast<uint32>(device->layer.drawableSize.width), .y = static_cast<uint32>(device->layer.drawableSize.height)};
    }
}

TimelineSemaphore* create_timeline_semaphore(Device* device, uint64 initial_value) noexcept
{
    @autoreleasepool
    {
        TimelineSemaphore* result = new TimelineSemaphore{.device = device, .event = [device->metal newSharedEvent]};
        if (!result->event) { report_error("timeline", nil); delete result; return nullptr; }
        result->event.signaledValue = initial_value;
        return result;
    }
}

void destroy_timeline_semaphore(TimelineSemaphore* semaphore) noexcept
{
    @autoreleasepool
    {
        if (!semaphore) return;
        [semaphore->event release];
        delete semaphore;
    }
}

uint64 timeline_completed_value(const TimelineSemaphore* semaphore) noexcept
{
    @autoreleasepool
    {
        return semaphore->event.signaledValue;
    }
}

void wait_timeline(TimelinePoint point) noexcept
{
    @autoreleasepool
    {
        assert(point.semaphore);
        while (![point.semaphore->event waitUntilSignaledValue:point.value timeoutMS:1000]) {}
    }
}

void wait_idle(Device* device) noexcept
{
    @autoreleasepool
    {
        if (device->metal4)
            for (uint32 i = 0; i < device->caps.queue_count; ++i)
                [device->queues[i].queue signalEvent:device->queues[i].completion value:++device->queues[i].submitted_value];
        for (uint32 i = 0; i < device->caps.queue_count; ++i)
            while (![device->queues[i].completion waitUntilSignaledValue:device->queues[i].submitted_value timeoutMS:1000]) {}
    }
}

GpuHeap create_gpu_heap(Device* device, uint64 byte_count, MemoryType memory) noexcept
{
    @autoreleasepool
    {
        MTLResourceOptions options = memory == MemoryType::gpu_only ? MTLResourceStorageModePrivate : MTLResourceStorageModeShared;
        if (memory == MemoryType::cpu_visible) options |= MTLResourceCPUCacheModeWriteCombined;
        options |= MTLResourceHazardTrackingModeUntracked;
        const MTLSizeAndAlign requirements = [device->metal heapBufferSizeAndAlignWithLength:byte_count options:options];
        MTLHeapDescriptor* desc = [MTLHeapDescriptor new];
        desc.type = MTLHeapTypePlacement;
        desc.size = align_up(requirements.size, requirements.align);
        desc.resourceOptions = options;
        GpuHeapOwner* owner = new GpuHeapOwner{.device = device, .heap = [device->metal newHeapWithDescriptor:desc]};
        [desc release];
        owner->buffer = [owner->heap newBufferWithLength:byte_count options:options offset:0];
        if (!owner->buffer)
        {
            report_error("GPU heap", nil);
            [owner->heap release];
            delete owner;
            return {};
        }
        owner->record = device->buffer_map.insert(owner->buffer.gpuAddress, byte_count, reinterpret_cast<uintptr>(owner->buffer));
        add_resident(device, device->shader_validation ? (id<MTLAllocation>)owner->buffer : (id<MTLAllocation>)owner->heap);
        return {.range = {.cpu = memory == MemoryType::gpu_only ? nullptr : static_cast<byte*>(owner->buffer.contents),
                          .gpu = reinterpret_cast<byte*>(owner->buffer.gpuAddress), .size = byte_count},
                .owner = owner};
    }
}

void destroy_gpu_heap(const GpuHeap& heap) noexcept
{
    @autoreleasepool
    {
        if (!heap.owner) return;
        GpuHeapOwner* owner = const_cast<GpuHeapOwner*>(heap.owner);
        owner->device->buffer_map.remove(owner->record);
        remove_resident(owner->device, owner->device->shader_validation ? (id<MTLAllocation>)owner->buffer : (id<MTLAllocation>)owner->heap);
        [owner->buffer release];
        [owner->heap release];
        delete owner;
    }
}

TextureHeap create_texture_heap(Device* device, uint64 byte_count) noexcept
{
    @autoreleasepool
    {
        MTLHeapDescriptor* desc = [MTLHeapDescriptor new];
        desc.type = MTLHeapTypePlacement;
        desc.size = byte_count;
        desc.storageMode = MTLStorageModePrivate;
        desc.hazardTrackingMode = MTLHazardTrackingModeUntracked;
        TextureHeapOwner* owner = new TextureHeapOwner{.device = device, .heap = [device->metal newHeapWithDescriptor:desc]};
        [desc release];
        if (!owner->heap) { report_error("texture heap", nil); delete owner; return {}; }
        if (!device->shader_validation) add_resident(device, owner->heap);
        return {.size = byte_count, .owner = owner};
    }
}

void destroy_texture_heap(const TextureHeap& heap) noexcept
{
    @autoreleasepool
    {
        if (!heap.owner) return;
        if (!heap.owner->device->shader_validation) remove_resident(heap.owner->device, heap.owner->heap);
        [heap.owner->heap release];
        delete heap.owner;
    }
}

SizeAlign get_texture_size_align(Device* device, const TextureDesc& desc) noexcept
{
    @autoreleasepool
    {
        if (!supports_texture_format(device, desc.format, desc.usage)) return {};
        MTLTextureDescriptor* descriptor = texture_descriptor(desc);
        const MTLSizeAndAlign result = [device->metal heapTextureSizeAndAlignWithDescriptor:descriptor];
        [descriptor release];
        assert(device->caps.texture_heap_alignment % result.align == 0);
        return {.size = result.size, .align = result.align};
    }
}

Texture* create_texture(CommandBuffer* commands, const TextureDesc& desc, const TextureHeap& heap, uint64 offset) noexcept
{
    @autoreleasepool
    {
        Device* device = commands->device;
        assert(commands->recording && !commands->render && heap.owner && heap.owner->device == device);
        MTLTextureDescriptor* descriptor = texture_descriptor(desc);
        id<MTLTexture> texture = [heap.owner->heap newTextureWithDescriptor:descriptor offset:offset];
        [descriptor release];
        if (!texture) { report_error("texture", nil); return nullptr; }
        if (device->shader_validation) add_resident(device, texture);
        return new Texture{.device = device, .texture = texture, .desc = desc};
    }
}

void destroy_texture(Texture* texture) noexcept
{
    @autoreleasepool
    {
        if (!texture) return;
        if (texture->device->shader_validation) remove_resident(texture->device, texture->texture);
        [texture->texture release];
        delete texture;
    }
}

RenderView* create_render_view(Texture* texture, const RenderViewDesc& desc) noexcept
{
    @autoreleasepool
    {
        return new RenderView{.texture = [texture->texture retain], .mip = desc.mip_level, .slice = desc.slice};
    }
}

void destroy_render_view(RenderView* render_view) noexcept
{
    @autoreleasepool
    {
        if (!render_view) return;
        [render_view->texture release];
        delete render_view;
    }
}

TextureDescriptorHeap* create_texture_descriptor_heap(Device* device, uint32 capacity) noexcept
{
    @autoreleasepool
    {
        NSError* error = nil;
        TextureDescriptorHeap* result = new TextureDescriptorHeap{.device = device, .capacity = capacity};
        GPUTextureHeap header = {};
        if (@available(macOS 26.0, iOS 26.0, *))
        {
            MTLResourceViewPoolDescriptor* desc = [MTLResourceViewPoolDescriptor new];
            desc.resourceViewCount = capacity;
            result->pool = [device->metal newTextureViewPoolWithDescriptor:desc error:&error];
            [desc release];
            if (!result->pool) { report_error("texture descriptor heap", error); delete result; return nullptr; }
            header.base = result->pool.baseResourceID._impl;
            assert(header.base);
        }
        else result->views = new id<MTLTexture>[capacity]{};
        result->base = [device->metal newBufferWithLength:sizeof(header) + (result->pool ? 0 : capacity * sizeof(MTLResourceID))
                                                options:MTLResourceStorageModeShared];
        if (!result->base)
        {
            report_error("texture pool base", nil);
            [result->pool release];
            delete[] result->views;
            delete result;
            return nullptr;
        }
        memset(result->base.contents, 0, result->base.length);
        if (!result->pool) header.ids = reinterpret_cast<uint64*>(result->base.gpuAddress + sizeof(header));
        memcpy(result->base.contents, &header, sizeof(header));
        add_resident(device, result->base);
        return result;
    }
}

void destroy_texture_descriptor_heap(TextureDescriptorHeap* heap) noexcept
{
    @autoreleasepool
    {
        if (!heap) return;
        if (heap->views)
        {
            for (uint32 i = 0; i < heap->capacity; ++i) [heap->views[i] release];
            delete[] heap->views;
        }
        remove_resident(heap->device, heap->base);
        [heap->base release];
        [heap->pool release];
        delete heap;
    }
}

void write_texture_descriptor(TextureDescriptorHeap* heap, uint32 index, const Texture* texture, TextureDescriptorType type,
                              const TextureDescriptorDesc& desc) noexcept
{
    @autoreleasepool
    {
        assert(index < heap->capacity && texture->device == heap->device);
        if (!heap->pool)
        {
            MTLPixelFormat format = pixel_format(desc.format == Format::undefined ? texture->desc.format : desc.format);
            if (desc.aspect == TextureAspect::stencil && texture->desc.format == Format::d32_float_s8_uint) format = MTLPixelFormatX32_Stencil8;
            MTLTextureType view_type = texture->texture.textureType;
            if (type == TextureDescriptorType::storage && (view_type == MTLTextureTypeCube || view_type == MTLTextureTypeCubeArray))
                view_type = MTLTextureType2DArray;
            id<MTLTexture> view = [texture->texture newTextureViewWithPixelFormat:format textureType:view_type
                                      levels:NSMakeRange(desc.base_mip, desc.mip_count ? desc.mip_count : texture->desc.mip_levels - desc.base_mip)
                                      slices:NSMakeRange(desc.base_layer, desc.layer_count ? desc.layer_count : texture->desc.layer_count - desc.base_layer)];
            [heap->views[index] release];
            heap->views[index] = view;
            static_cast<MTLResourceID*>(static_cast<void*>(static_cast<byte*>(heap->base.contents) + sizeof(GPUTextureHeap)))[index] = view.gpuResourceID;
            return;
        }
        MTLTextureViewDescriptor* view = [MTLTextureViewDescriptor new];
        view.pixelFormat = pixel_format(desc.format == Format::undefined ? texture->desc.format : desc.format);
        if (desc.aspect == TextureAspect::stencil && texture->desc.format == Format::d32_float_s8_uint)
            view.pixelFormat = MTLPixelFormatX32_Stencil8;
        view.textureType = texture->texture.textureType;
        if (type == TextureDescriptorType::storage &&
            (view.textureType == MTLTextureTypeCube || view.textureType == MTLTextureTypeCubeArray)) view.textureType = MTLTextureType2DArray;
        view.levelRange = NSMakeRange(desc.base_mip, desc.mip_count ? desc.mip_count : texture->desc.mip_levels - desc.base_mip);
        view.sliceRange = NSMakeRange(desc.base_layer, desc.layer_count ? desc.layer_count : texture->desc.layer_count - desc.base_layer);
        const MTLResourceID resource = [heap->pool setTextureView:texture->texture descriptor:view atIndex:index];
        assert(resource._impl == heap->pool.baseResourceID._impl + index);
        [view release];
    }
}

void copy_texture_descriptors(const TextureDescriptorHeap* source, uint32 source_index, TextureDescriptorHeap* destination,
                              uint32 destination_index, uint32 count) noexcept
{
    @autoreleasepool
    {
        assert(source->device == destination->device && source_index + count <= source->capacity && destination_index + count <= destination->capacity);
        if (!count || (source == destination && source_index == destination_index)) return;
        if (!source->pool)
        {
            const bool backwards = source == destination && destination_index > source_index;
            for (uint32 n = 0; n < count; ++n)
            {
                const uint32 i = backwards ? count - 1 - n : n;
                id<MTLTexture> view = [source->views[source_index + i] retain];
                [destination->views[destination_index + i] release];
                destination->views[destination_index + i] = view;
            }
            memmove(static_cast<byte*>(destination->base.contents) + sizeof(GPUTextureHeap) + destination_index * sizeof(MTLResourceID),
                    static_cast<const byte*>(source->base.contents) + sizeof(GPUTextureHeap) + source_index * sizeof(MTLResourceID),
                    count * sizeof(MTLResourceID));
            return;
        }
        if (source == destination && destination_index > source_index && destination_index < source_index + count)
        {
            for (uint32 i = count; i != 0; --i)
                [destination->pool copyResourceViewsFromPool:source->pool sourceRange:NSMakeRange(source_index + i - 1, 1)
                                           destinationIndex:destination_index + i - 1];
        }
        else if (source == destination && source_index > destination_index && source_index < destination_index + count)
        {
            for (uint32 i = 0; i < count; ++i)
                [destination->pool copyResourceViewsFromPool:source->pool sourceRange:NSMakeRange(source_index + i, 1)
                                           destinationIndex:destination_index + i];
        }
        else [destination->pool copyResourceViewsFromPool:source->pool sourceRange:NSMakeRange(source_index, count) destinationIndex:destination_index];
    }
}

SamplerDescriptorHeap* create_sampler_descriptor_heap(Device* device, uint32 capacity) noexcept
{
    @autoreleasepool
    {
        SamplerDescriptorHeap* result = new SamplerDescriptorHeap{.device = device, .capacity = capacity};
        result->buffer = [device->metal newBufferWithLength:capacity * sizeof(MTLResourceID) options:MTLResourceStorageModeShared];
        if (!result->buffer) { report_error("sampler heap", nil); delete result; return nullptr; }
        result->samplers = new id<MTLSamplerState>[capacity]{};
        memset(result->buffer.contents, 0, capacity * sizeof(MTLResourceID));
        add_resident(device, result->buffer);
        return result;
    }
}

void destroy_sampler_descriptor_heap(SamplerDescriptorHeap* heap) noexcept
{
    @autoreleasepool
    {
        if (!heap) return;
        for (uint32 i = 0; i < heap->capacity; ++i) [heap->samplers[i] release];
        delete[] heap->samplers;
        remove_resident(heap->device, heap->buffer);
        [heap->buffer release];
        delete heap;
    }
}

void write_sampler_descriptor(SamplerDescriptorHeap* heap, uint32 index, const SamplerDesc& desc) noexcept
{
    @autoreleasepool
    {
        assert(index < heap->capacity);
        const MTLSamplerAddressMode address_modes[] = {MTLSamplerAddressModeRepeat, MTLSamplerAddressModeMirrorRepeat, MTLSamplerAddressModeClampToEdge};
        MTLSamplerDescriptor* sampler = [MTLSamplerDescriptor new];
        sampler.minFilter = desc.min_filter == Filter::linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sampler.magFilter = desc.mag_filter == Filter::linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sampler.mipFilter = desc.mip_filter == Filter::linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        sampler.sAddressMode = address_modes[static_cast<uint32>(desc.address_u)];
        sampler.tAddressMode = address_modes[static_cast<uint32>(desc.address_v)];
        sampler.rAddressMode = address_modes[static_cast<uint32>(desc.address_w)];
        sampler.maxAnisotropy = desc.anisotropic ? 4 : 1;
        sampler.compareFunction = desc.compare_enabled ? static_cast<MTLCompareFunction>(desc.compare) : MTLCompareFunctionNever;
        sampler.supportArgumentBuffers = YES;
        id<MTLSamplerState> state = [heap->device->metal newSamplerStateWithDescriptor:sampler];
        [sampler release];
        [heap->samplers[index] release];
        heap->samplers[index] = state;
        static_cast<MTLResourceID*>(heap->buffer.contents)[index] = state.gpuResourceID;
    }
}

void copy_sampler_descriptors(const SamplerDescriptorHeap* source, uint32 source_index, SamplerDescriptorHeap* destination,
                              uint32 destination_index, uint32 count) noexcept
{
    @autoreleasepool
    {
        assert(source->device == destination->device && source_index + count <= source->capacity && destination_index + count <= destination->capacity);
        if (!count || (source == destination && source_index == destination_index)) return;
        const bool backwards = source == destination && destination_index > source_index;
        for (uint32 n = 0; n < count; ++n)
        {
            const uint32 i = backwards ? count - 1 - n : n;
            id<MTLSamplerState> state = [source->samplers[source_index + i] retain];
            [destination->samplers[destination_index + i] release];
            destination->samplers[destination_index + i] = state;
            static_cast<MTLResourceID*>(destination->buffer.contents)[destination_index + i] =
                static_cast<const MTLResourceID*>(source->buffer.contents)[source_index + i];
        }
    }
}

namespace
{
MTL4LibraryFunctionDescriptor* shader_function(Device* device, const ShaderStage& stage)
{
    if (!stage.code.size) return nil;
    assert(stage.code.data && stage.code.size >= 4 && "PSO stage requires a metallib signature");
    assert(memcmp(stage.code.data, "MTLB", 4) == 0 && "PSO stage does not contain a metallib binary");
    NSError* error = nil;
    dispatch_data_t data = dispatch_data_create(stage.code.data, stage.code.size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> library = [device->metal newLibraryWithData:data error:&error];
    dispatch_release(data);
    if (!library) { report_error("shader library", error); return nil; }
    MTL4LibraryFunctionDescriptor* function = [MTL4LibraryFunctionDescriptor new];
    function.library = library;
    function.name = [NSString stringWithUTF8String:stage.entry_point];
    [library release];
    return function;
}

id<MTLFunction> shader_function3(Device* device, const ShaderStage& stage)
{
    if (!stage.code.size) return nil;
    assert(stage.code.data && stage.code.size >= 4 && memcmp(stage.code.data, "MTLB", 4) == 0);
    NSError* error = nil;
    dispatch_data_t data = dispatch_data_create(stage.code.data, stage.code.size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> library = [device->metal newLibraryWithData:data error:&error];
    dispatch_release(data);
    if (!library) { report_error("shader library", error); return nil; }
    id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:stage.entry_point]];
    [library release];
    if (!function) report_error("shader function", nil);
    return function;
}

MTLBlendFactor blend_factor(BlendFactor factor)
{
    const MTLBlendFactor factors[] = {
        MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
        MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor, MTLBlendFactorSourceAlpha,
        MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha,
        MTLBlendFactorSourceAlphaSaturated,
    };
    return factors[static_cast<uint32>(factor)];
}

void color_targets(MTL4RenderPipelineColorAttachmentDescriptorArray* attachments, Span<const ColorTargetDesc> targets)
{
    for (uint32 i = 0; i < targets.size; ++i)
    {
        const ColorTargetDesc& target = targets.data[i];
        attachments[i].pixelFormat = pixel_format(target.format);
        attachments[i].writeMask = static_cast<MTLColorWriteMask>(((target.write_mask & 1) << 3) | ((target.write_mask & 2) << 1) |
                                                               ((target.write_mask & 4) >> 1) | ((target.write_mask & 8) >> 3));
        attachments[i].blendingState = target.blend.enabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
        attachments[i].sourceRGBBlendFactor = blend_factor(target.blend.color.source);
        attachments[i].destinationRGBBlendFactor = blend_factor(target.blend.color.destination);
        attachments[i].rgbBlendOperation = static_cast<MTLBlendOperation>(target.blend.color.operation);
        attachments[i].sourceAlphaBlendFactor = blend_factor(target.blend.alpha.source);
        attachments[i].destinationAlphaBlendFactor = blend_factor(target.blend.alpha.destination);
        attachments[i].alphaBlendOperation = static_cast<MTLBlendOperation>(target.blend.alpha.operation);
    }
}

void color_targets3(MTLRenderPipelineColorAttachmentDescriptorArray* attachments, Span<const ColorTargetDesc> targets)
{
    for (uint32 i = 0; i < targets.size; ++i)
    {
        const ColorTargetDesc& target = targets.data[i];
        attachments[i].pixelFormat = pixel_format(target.format);
        attachments[i].writeMask = static_cast<MTLColorWriteMask>(((target.write_mask & 1) << 3) | ((target.write_mask & 2) << 1) |
                                                               ((target.write_mask & 4) >> 1) | ((target.write_mask & 8) >> 3));
        attachments[i].blendingEnabled = target.blend.enabled;
        attachments[i].sourceRGBBlendFactor = blend_factor(target.blend.color.source);
        attachments[i].destinationRGBBlendFactor = blend_factor(target.blend.color.destination);
        attachments[i].rgbBlendOperation = static_cast<MTLBlendOperation>(target.blend.color.operation);
        attachments[i].sourceAlphaBlendFactor = blend_factor(target.blend.alpha.source);
        attachments[i].destinationAlphaBlendFactor = blend_factor(target.blend.alpha.destination);
        attachments[i].alphaBlendOperation = static_cast<MTLBlendOperation>(target.blend.alpha.operation);
    }
}
}

PSO* create_graphics_pso(Device* device, const GraphicsPSODesc& desc) noexcept
{
    assert(desc.vertex.code.size && "Graphics PSOs require a vertex stage");
    @autoreleasepool
    {
        if (!device->metal4)
        {
            MTLRenderPipelineDescriptor* pipeline = [MTLRenderPipelineDescriptor new];
            id<MTLFunction> vertex = shader_function3(device, desc.vertex);
            id<MTLFunction> fragment = shader_function3(device, desc.fragment);
            if (!vertex || (desc.fragment.code.size && !fragment))
            {
                [vertex release]; [fragment release]; [pipeline release]; return nullptr;
            }
            pipeline.vertexFunction = vertex;
            pipeline.fragmentFunction = fragment;
            pipeline.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
            pipeline.depthAttachmentPixelFormat = pixel_format(desc.depth_format);
            pipeline.stencilAttachmentPixelFormat = pixel_format(desc.stencil_format);
            color_targets3(pipeline.colorAttachments, desc.color_targets);
            NSError* error = nil;
            id<MTLRenderPipelineState> state = [device->metal newRenderPipelineStateWithDescriptor:pipeline error:&error];
            [vertex release]; [fragment release]; [pipeline release];
            if (!state) { report_error("graphics pipeline", error); return nullptr; }
            return new PSO{.device = device, .render = state, .rasterization = desc.rasterization};
        }
        MTL4RenderPipelineDescriptor* pipeline = [MTL4RenderPipelineDescriptor new];
        MTL4LibraryFunctionDescriptor* vertex = shader_function(device, desc.vertex);
        MTL4LibraryFunctionDescriptor* fragment = shader_function(device, desc.fragment);
        if (!vertex || (desc.fragment.code.size && !fragment))
        {
            [vertex release]; [fragment release]; [pipeline release]; return nullptr;
        }
        pipeline.vertexFunctionDescriptor = vertex;
        pipeline.fragmentFunctionDescriptor = fragment;
        pipeline.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
        color_targets(pipeline.colorAttachments, desc.color_targets);
        NSError* error = nil;
        id<MTLRenderPipelineState> state = [device->compiler newRenderPipelineStateWithDescriptor:pipeline compilerTaskOptions:nil error:&error];
        [vertex release];
        [fragment release];
        [pipeline release];
        if (!state) { report_error("graphics pipeline", error); return nullptr; }
        add_resident(device, state);
        return new PSO{.device = device, .render = state, .rasterization = desc.rasterization};
    }
}

PSO* create_mesh_pso(Device* device, const MeshPSODesc& desc) noexcept
{
    assert(desc.mesh.code.size && "Mesh PSOs require a mesh stage");
    @autoreleasepool
    {
        if (!device->metal4)
        {
            MTLMeshRenderPipelineDescriptor* pipeline = [MTLMeshRenderPipelineDescriptor new];
            id<MTLFunction> task = shader_function3(device, desc.task);
            id<MTLFunction> mesh = shader_function3(device, desc.mesh);
            id<MTLFunction> fragment = shader_function3(device, desc.fragment);
            if (!mesh || (desc.task.code.size && !task) || (desc.fragment.code.size && !fragment))
            {
                [task release]; [mesh release]; [fragment release]; [pipeline release]; return nullptr;
            }
            pipeline.objectFunction = task;
            pipeline.meshFunction = mesh;
            pipeline.fragmentFunction = fragment;
            pipeline.depthAttachmentPixelFormat = pixel_format(desc.depth_format);
            pipeline.stencilAttachmentPixelFormat = pixel_format(desc.stencil_format);
            color_targets3(pipeline.colorAttachments, desc.color_targets);
            NSError* error = nil;
            id<MTLRenderPipelineState> state = [device->metal newRenderPipelineStateWithMeshDescriptor:pipeline options:MTLPipelineOptionNone
                                                                                         reflection:nil error:&error];
            [task release]; [mesh release]; [fragment release]; [pipeline release];
            if (!state) { report_error("mesh pipeline", error); return nullptr; }
            return new PSO{.device = device, .render = state, .rasterization = desc.rasterization,
                           .threads = metal_size(desc.mesh.threadgroup_size), .object_threads = metal_size(desc.task.threadgroup_size), .mesh = true};
        }
        MTL4MeshRenderPipelineDescriptor* pipeline = [MTL4MeshRenderPipelineDescriptor new];
        MTL4LibraryFunctionDescriptor* task = shader_function(device, desc.task);
        MTL4LibraryFunctionDescriptor* mesh = shader_function(device, desc.mesh);
        MTL4LibraryFunctionDescriptor* fragment = shader_function(device, desc.fragment);
        if (!mesh || (desc.task.code.size && !task) || (desc.fragment.code.size && !fragment))
        {
            [task release]; [mesh release]; [fragment release]; [pipeline release]; return nullptr;
        }
        pipeline.objectFunctionDescriptor = task;
        pipeline.meshFunctionDescriptor = mesh;
        pipeline.fragmentFunctionDescriptor = fragment;
        pipeline.requiredThreadsPerMeshThreadgroup = metal_size(desc.mesh.threadgroup_size);
        if (task) pipeline.requiredThreadsPerObjectThreadgroup = metal_size(desc.task.threadgroup_size);
        color_targets(pipeline.colorAttachments, desc.color_targets);
        NSError* error = nil;
        id<MTLRenderPipelineState> state = [device->compiler newRenderPipelineStateWithDescriptor:pipeline compilerTaskOptions:nil error:&error];
        [task release]; [mesh release]; [fragment release]; [pipeline release];
        if (!state) { report_error("mesh pipeline", error); return nullptr; }
        add_resident(device, state);
        return new PSO{.device = device, .render = state, .rasterization = desc.rasterization,
                       .threads = metal_size(desc.mesh.threadgroup_size), .object_threads = metal_size(desc.task.threadgroup_size), .mesh = true};
    }
}

PSO* create_compute_pso(Device* device, const ShaderStage& stage) noexcept
{
    assert(stage.code.size && "Compute PSOs require a compute stage");
    @autoreleasepool
    {
        if (!device->metal4)
        {
            id<MTLFunction> function = shader_function3(device, stage);
            if (!function) return nullptr;
            NSError* error = nil;
            id<MTLComputePipelineState> state = [device->metal newComputePipelineStateWithFunction:function error:&error];
            [function release];
            if (!state) { report_error("compute pipeline", error); return nullptr; }
            return new PSO{.device = device, .compute = state, .threads = metal_size(stage.threadgroup_size)};
        }
        MTL4LibraryFunctionDescriptor* function = shader_function(device, stage);
        if (!function) return nullptr;
        MTL4ComputePipelineDescriptor* desc = [MTL4ComputePipelineDescriptor new];
        desc.computeFunctionDescriptor = function;
        desc.requiredThreadsPerThreadgroup = metal_size(stage.threadgroup_size);
        NSError* error = nil;
        id<MTLComputePipelineState> state = [device->compiler newComputePipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error];
        [function release];
        [desc release];
        if (!state) { report_error("compute pipeline", error); return nullptr; }
        add_resident(device, state);
        return new PSO{.device = device, .compute = state, .threads = metal_size(stage.threadgroup_size)};
    }
}

void destroy_pso(PSO* pso) noexcept
{
    @autoreleasepool
    {
        if (!pso) return;
        if (pso->device->metal4 && pso->render) remove_resident(pso->device, pso->render);
        if (pso->device->metal4 && pso->compute) remove_resident(pso->device, pso->compute);
        [pso->render release];
        [pso->compute release];
        delete pso;
    }
}

CommandPool* create_command_pool(Device* device, uint32 queue_index) noexcept
{
    @autoreleasepool
    {
        assert(queue_index < device->caps.queue_count);
        CommandPool* pool = new CommandPool{.device = device, .queue = &device->queues[queue_index], .kind = device->queues[queue_index].kind};
        depth_state(pool, {});
        for (uint32 compare = 0; compare < 8; ++compare)
            for (uint32 write = 0; write < 2; ++write)
                depth_state(pool, {.depth_test = true, .depth_write = write != 0, .depth_compare = static_cast<CompareOp>(compare)});
        return pool;
    }
}

void reset_command_pool(CommandPool* pool) noexcept
{
    @autoreleasepool
    {
        for (CommandBuffer* commands = pool->first; commands; commands = commands->next)
        {
            assert(!commands->acquired && (!commands->retirement || commands->retirement.signaledValue >= commands->retirement_value));
            if (commands->render) end_render_pass(commands);
            end_compute(commands);
            if (commands->recording) [commands->commands endCommandBuffer];
            [commands->commands3 release];
            commands->commands3 = nil;
            commands->recording = false;
            commands->ended = false;
            commands->retirement = nil;
            commands->retirement_value = 0;
            for (NativeCommandBuffer* native = commands->native_buffers; native; native = native->next) [native->allocator reset];
        }
        pool->next_buffer = pool->first;
    }
}

void destroy_command_pool(CommandPool* pool) noexcept
{
    if (!pool) return;
    @autoreleasepool
    {
        reset_command_pool(pool);
        for (CommandBuffer* commands = pool->first; commands;)
        {
            CommandBuffer* next = commands->next;
            destroy_context(pool->device, commands);
            commands = next;
        }
        for (DepthStateChunk* chunk = pool->depth_states; chunk;)
        {
            DepthStateChunk* next = chunk->next;
            for (uint32 i = 0; i < chunk->count; ++i) [chunk->entries[i].state release];
            delete chunk;
            chunk = next;
        }
        delete pool;
    }
}

CommandBuffer* begin_commands(CommandPool* pool) noexcept
{
    @autoreleasepool
    {
        CommandBuffer* context = pool->next_buffer;
        if (context) pool->next_buffer = context->next;
        else
        {
            context = create_context(pool);
            if (!context) return nullptr;
            if (pool->last) pool->last->next = context;
            else pool->first = context;
            pool->last = context;
        }
        assert(!context->recording && !context->ended);
        if (pool->device->metal4)
        {
            context->native = context->native_buffers;
            context->commands = context->native->buffer;
            context->native_count = 1;
            [context->commands beginCommandBufferWithAllocator:context->native->allocator];
            [context->arguments setAddress:0 atIndex:0];
            [context->arguments setAddress:0 atIndex:1];
            [context->arguments setAddress:0 atIndex:2];
        }
        else
        {
            context->commands3 = [[pool->queue->queue3 commandBufferWithUnretainedReferences] retain];
            for (uint32 i = 0; i < 3; ++i)
            {
                context->bindings[i] = nil;
                context->binding_offsets[i] = 0;
            }
        }
        context->timestamp_count = 0;
        context->pso = nullptr;
        context->recording = true;
        context->render_continuation = false;
        return context;
    }
}

void end_commands(CommandBuffer* commands) noexcept
{
    @autoreleasepool
    {
        assert(commands->recording && !commands->render);
        end_compute(commands);
        [commands->commands endCommandBuffer];
        commands->recording = false;
        commands->ended = true;
    }
}

void submit(Device* device, const SubmitDesc& desc, uint32 queue_index) noexcept
{
    @autoreleasepool
    {
        assert(queue_index < device->caps.queue_count && desc.completion.semaphore && desc.completion.semaphore->device == device);
        Queue& queue = device->queues[queue_index];
        if (!device->metal4)
        {
            id<MTLCommandBuffer> prologue = [queue.queue3 commandBufferWithUnretainedReferences];
            for (size_t i = 0; i < desc.waits.size; ++i)
            {
                const TimelinePoint& wait = desc.waits.data[i];
                assert(wait.semaphore && wait.semaphore->device == device);
                [prologue encodeWaitForEvent:wait.semaphore->event value:wait.value];
            }
            id<MTLBlitCommandEncoder> entry = [prologue blitCommandEncoder];
            [entry updateFence:queue.dependencies];
            [entry updateFence:queue.producers];
            [entry endEncoding];
            if (device->shader_validation) os_unfair_lock_lock(&device->residency_lock);
            [prologue commit];
            for (size_t i = 0; i < desc.commands.size; ++i)
            {
                CommandBuffer* commands = desc.commands.data[i];
                assert(commands->device == device && commands->pool->queue == &queue && commands->ended && !commands->retirement);
                commands->retirement = queue.completion;
                commands->retirement_value = queue.submitted_value + 1;
                [commands->commands3 commit];
            }
            id<MTLCommandBuffer> epilogue = [queue.queue3 commandBufferWithUnretainedReferences];
            id<MTLBlitCommandEncoder> exit = [epilogue blitCommandEncoder];
            [exit waitForFence:queue.dependencies];
            [exit waitForFence:queue.producers];
            [exit endEncoding];
            [epilogue encodeSignalEvent:queue.completion value:++queue.submitted_value];
            [epilogue encodeSignalEvent:desc.completion.semaphore->event value:desc.completion.value];
            if (queue_index == 0 && device->presenting) [epilogue presentDrawable:device->drawable];
            [epilogue commit];
            if (device->shader_validation) os_unfair_lock_unlock(&device->residency_lock);
            return;
        }
        size_t count = desc.commands.size;
        for (size_t i = 0; i < desc.commands.size; ++i)
            count += desc.commands.data[i]->native_count - 1;
        if (queue.submission_capacity < count)
        {
            queue.submission_capacity = count;
            queue.submission = static_cast<id<MTL4CommandBuffer>*>(realloc(queue.submission, count * sizeof(id<MTL4CommandBuffer>)));
        }
        for (size_t i = 0, native_index = 0; i < desc.commands.size; ++i)
        {
            CommandBuffer* commands = desc.commands.data[i];
            assert(commands->device == device && commands->pool->kind == queue.kind && commands->ended && !commands->retirement);
            commands->retirement = queue.completion;
            commands->retirement_value = queue.submitted_value + 1;
            NativeCommandBuffer* native = commands->native_buffers;
            for (uint32 j = 0; j < commands->native_count; ++j, native = native->next) queue.submission[native_index++] = native->buffer;
        }
        for (size_t i = 0; i < desc.waits.size; ++i)
        {
            const TimelinePoint& wait = desc.waits.data[i];
            assert(wait.semaphore && wait.semaphore->device == device);
            [queue.queue waitForEvent:wait.semaphore->event value:wait.value];
        }
        // MetalTools enumerates residency on the CPU during commit without synchronizing concurrent updates.
        if (device->shader_validation) os_unfair_lock_lock(&device->residency_lock);
        if (count) [queue.queue commit:queue.submission count:count];
        if (device->shader_validation) os_unfair_lock_unlock(&device->residency_lock);
        [queue.queue signalEvent:queue.completion value:++queue.submitted_value];
        [queue.queue signalEvent:desc.completion.semaphore->event value:desc.completion.value];
    }
}

SwapchainFrame acquire(CommandBuffer* commands) noexcept
{
    @autoreleasepool
    {
        Device* device = commands->device;
        assert(commands->recording && !commands->render && commands->pool->kind == QueueKind::general && device->layer && !device->drawable);
        const uint32x2 size = get_drawable_extent(device);
        if (!size.x || !size.y) return {};
        device->drawable = [[device->layer nextDrawable] retain];
        if (!device->drawable) return {};
        device->acquired = commands;
        commands->acquired = true;
        device->drawable_view = {.texture = device->drawable.texture};
        return {.render_view = &device->drawable_view,
                .extent = {.x = static_cast<uint32>(device->drawable.texture.width), .y = static_cast<uint32>(device->drawable.texture.height)}};
    }
}

void submit_and_present(Device* device, const SubmitDesc& desc) noexcept
{
    @autoreleasepool
    {
        assert(device->drawable && device->acquired);
        bool found = false;
        for (size_t i = 0; i < desc.commands.size; ++i) found |= desc.commands.data[i] == device->acquired;
        assert(found);
        if (device->metal4) [device->queues[0].queue waitForDrawable:device->drawable];
        device->presenting = true;
        submit(device, desc, 0);
        device->presenting = false;
        if (device->metal4)
        {
            [device->queues[0].queue signalDrawable:device->drawable];
            [device->drawable present];
        }
        [device->drawable release];
        device->acquired->acquired = false;
        device->acquired = nullptr;
        device->drawable = nil;
        device->drawable_view = {};
    }
}

void set_texture_descriptor_heap(CommandBuffer* commands, TextureDescriptorHeap* heap) noexcept
{
    @autoreleasepool
    {
        assert(commands->recording && heap->device == commands->device);
        if (commands->device->metal4) [commands->arguments setAddress:heap->base.gpuAddress atIndex:1];
        else bind_buffer(commands, heap->base, 0, 1);
    }
}

void set_sampler_descriptor_heap(CommandBuffer* commands, SamplerDescriptorHeap* heap) noexcept
{
    @autoreleasepool
    {
        assert(commands->recording && heap->device == commands->device);
        if (commands->device->metal4) [commands->arguments setAddress:heap->buffer.gpuAddress atIndex:2];
        else bind_buffer(commands, heap->buffer, 0, 2);
    }
}

void copy_memory(CommandBuffer* commands, GpuRange source, GpuRange destination) noexcept
{
    @autoreleasepool
    {
        assert(source.size <= destination.size);
        uint64 source_offset = 0;
        uint64 destination_offset = 0;
        id<MTLBuffer> source_buffer = resolve_buffer(commands->device, source, &source_offset);
        id<MTLBuffer> destination_buffer = resolve_buffer(commands->device, {.gpu = destination.gpu, .size = source.size}, &destination_offset);
        [copy_encoder(commands) copyFromBuffer:source_buffer sourceOffset:source_offset toBuffer:destination_buffer
                               destinationOffset:destination_offset size:source.size];
    }
}

namespace
{
struct TextureCopy
{
    MTLOrigin origin = {};
    MTLSize extent = {};
    uint64 row_pitch = 0;
    uint64 image_pitch = 0;
    uint64 slice_pitch = 0;
    uint64 size = 0;
    uint32 slices = 0;
};

TextureCopy texture_copy(const Texture* texture, const TextureCopyDesc& desc)
{
    const TextureFormatInfo format = get_texture_format_info(texture->desc.format);
    const uint32 width = texture->desc.extent.x >> desc.mip_level;
    const uint32 height = texture->desc.extent.y >> desc.mip_level;
    const uint32 depth = texture->desc.extent.z >> desc.mip_level;
    TextureCopy copy = {
        .origin = MTLOriginMake(desc.offset.x, desc.offset.y, desc.offset.z),
        .extent = MTLSizeMake(desc.extent.x ? desc.extent.x : (width ? width : 1) - desc.offset.x,
                              desc.extent.y ? desc.extent.y : (height ? height : 1) - desc.offset.y,
                              desc.extent.z ? desc.extent.z : (depth ? depth : 1) - desc.offset.z),
        .slices = desc.slice_count ? desc.slice_count : texture->desc.layer_count - desc.base_slice,
    };
    const uint64 row_bytes = ((copy.extent.width + format.block_extent.x - 1) / format.block_extent.x) * format.bytes_per_block;
    const uint64 rows = (copy.extent.height + format.block_extent.y - 1) / format.block_extent.y;
    copy.row_pitch = desc.row_pitch_bytes ? desc.row_pitch_bytes : row_bytes;
    copy.image_pitch = copy.row_pitch * rows;
    copy.slice_pitch = desc.slice_pitch_bytes ? desc.slice_pitch_bytes : copy.image_pitch * copy.extent.depth;
    if (texture->desc.type == TextureType::three_d && desc.slice_pitch_bytes) copy.image_pitch = desc.slice_pitch_bytes;
    copy.size = copy.slice_pitch * (copy.slices - 1) + copy.image_pitch * (copy.extent.depth - 1) + copy.row_pitch * (rows - 1) + row_bytes;
    return copy;
}
}

void copy_memory_to_texture(CommandBuffer* commands, GpuRange source, Texture* destination, const TextureCopyDesc& desc) noexcept
{
    @autoreleasepool
    {
        const TextureCopy copy = texture_copy(destination, desc);
        assert(source.size >= copy.size);
        uint64 offset = 0;
        id<MTLBuffer> buffer = resolve_buffer(commands->device, {.gpu = source.gpu, .size = copy.size}, &offset);
        for (uint32 slice = 0; slice < copy.slices; ++slice)
            [copy_encoder(commands) copyFromBuffer:buffer sourceOffset:offset + slice * copy.slice_pitch sourceBytesPerRow:copy.row_pitch
                                 sourceBytesPerImage:copy.image_pitch sourceSize:copy.extent toTexture:destination->texture
                                    destinationSlice:desc.base_slice + slice destinationLevel:desc.mip_level destinationOrigin:copy.origin];
    }
}

void copy_texture_to_memory(CommandBuffer* commands, Texture* source, GpuRange destination, const TextureCopyDesc& desc) noexcept
{
    @autoreleasepool
    {
        const TextureCopy copy = texture_copy(source, desc);
        assert(destination.size >= copy.size);
        uint64 offset = 0;
        id<MTLBuffer> buffer = resolve_buffer(commands->device, {.gpu = destination.gpu, .size = copy.size}, &offset);
        for (uint32 slice = 0; slice < copy.slices; ++slice)
            [copy_encoder(commands) copyFromTexture:source->texture sourceSlice:desc.base_slice + slice sourceLevel:desc.mip_level sourceOrigin:copy.origin
                                           sourceSize:copy.extent toBuffer:buffer destinationOffset:offset + slice * copy.slice_pitch
                               destinationBytesPerRow:copy.row_pitch destinationBytesPerImage:copy.image_pitch];
    }
}

void barrier(CommandBuffer* commands, Stage before, Access before_access, Stage after, Access after_access) noexcept
{
    @autoreleasepool
    {
        assert(!commands->render);
        (void)after_access;
        const MTLStages source = barrier_stages(before, true);
        const MTLStages destination = barrier_stages(after, false);
        // Shared-event completion provides host visibility; a host-only destination blocks no future GPU stages.
        if (!source || !destination) return;
        if (!commands->device->metal4)
        {
            end_compute(commands);
            id<MTLBlitCommandEncoder> bridge = [commands->commands3 blitCommandEncoder];
            [bridge waitForFence:commands->pool->queue->producers];
            [bridge updateFence:commands->pool->queue->dependencies];
            [bridge endEncoding];
            return;
        }
        const uint64 writes = static_cast<uint64>(Access::transfer_write | Access::shader_write | Access::color_write | Access::depth_stencil_write);
        const MTL4VisibilityOptions visibility = (static_cast<uint64>(before_access) & writes) ? MTL4VisibilityOptionDevice : MTL4VisibilityOptionNone;
        [compute_encoder(commands) barrierAfterStages:source beforeQueueStages:destination visibilityOptions:visibility];
        end_compute(commands);
    }
}

void write_timestamp(CommandBuffer* commands, uint64* cpu_destination, Stage stage) noexcept
{
    @autoreleasepool
    {
        if (!commands->device->timestamp_query_count) return;
        assert(commands->recording && cpu_destination && commands->timestamp_count < commands->device->timestamp_query_count);
        const uint32 index = commands->timestamp_count++;
        commands->timestamps[index].destination = cpu_destination;
        if (commands->render)
        {
            MTLRenderStages after = MTLRenderStageFragment;
            if (stage == Stage::vertex || stage == Stage::index_input) after = MTLRenderStageVertex;
            if (stage == Stage::task) after = MTLRenderStageObject;
            if (stage == Stage::mesh) after = MTLRenderStageMesh;
            [commands->render writeTimestampWithGranularity:MTL4TimestampGranularityPrecise afterStage:after
                                                  intoHeap:commands->timestamps[index].heap atIndex:commands->timestamps[index].index];
        }
        else
        {
            end_compute(commands);
            [native_commands(commands) writeTimestampIntoHeap:commands->timestamps[index].heap atIndex:commands->timestamps[index].index];
        }
    }
}

void read_timestamps(CommandPool* pool) noexcept
{
    @autoreleasepool
    {
        for (CommandBuffer* commands = pool->first; commands; commands = commands->next)
        {
            if (!commands->retirement || !commands->timestamp_count) continue;
            assert(commands->retirement.signaledValue >= commands->retirement_value);
            for (uint32 i = 0; i < commands->timestamp_count;)
            {
                const TimestampSlot& first = commands->timestamps[i];
                uint32 count = 1;
                while (i + count < commands->timestamp_count && commands->timestamps[i + count].heap == first.heap &&
                       commands->timestamps[i + count].index == first.index + count) ++count;
                NSData* data = [first.heap resolveCounterRange:NSMakeRange(first.index, count)];
                assert(data.length == count * sizeof(MTL4TimestampHeapEntry));
                const MTL4TimestampHeapEntry* values = static_cast<const MTL4TimestampHeapEntry*>(data.bytes);
                for (uint32 j = 0; j < count; ++j) *commands->timestamps[i + j].destination = values[j].timestamp;
                i += count;
            }
            commands->timestamp_count = 0;
        }
    }
}

namespace
{
MTLLoadAction load_action(LoadOp load)
{
    const MTLLoadAction actions[] = {MTLLoadActionLoad, MTLLoadActionClear, MTLLoadActionDontCare};
    return actions[static_cast<uint32>(load)];
}

void render_attachment(MTLRenderPassAttachmentDescriptor* attachment, RenderView* view, LoadOp load, StoreOp store)
{
    attachment.texture = view ? view->texture : nil;
    if (!view) return;
    attachment.level = view->mip;
    attachment.slice = view->texture.textureType == MTLTextureType3D ? 0 : view->slice;
    attachment.depthPlane = view->texture.textureType == MTLTextureType3D ? view->slice : 0;
    attachment.loadAction = load_action(load);
    attachment.storeAction = store == StoreOp::store ? MTLStoreActionStore : MTLStoreActionDontCare;
}

bool same_stencil(const StencilFaceState& a, const StencilFaceState& b)
{
    return a.compare == b.compare && a.fail == b.fail && a.pass == b.pass && a.depth_fail == b.depth_fail;
}

bool same_depth(const DepthStencilState& a, const DepthStencilState& b)
{
    return a.depth_test == b.depth_test && a.depth_write == b.depth_write && a.depth_compare == b.depth_compare && a.stencil_test == b.stencil_test &&
           a.stencil_read_mask == b.stencil_read_mask && a.stencil_write_mask == b.stencil_write_mask &&
           same_stencil(a.front, b.front) && same_stencil(a.back, b.back);
}

void stencil_descriptor(MTLStencilDescriptor* desc, const StencilFaceState& face, const DepthStencilState& state)
{
    desc.stencilCompareFunction = static_cast<MTLCompareFunction>(face.compare);
    desc.stencilFailureOperation = static_cast<MTLStencilOperation>(face.fail);
    desc.depthFailureOperation = static_cast<MTLStencilOperation>(face.depth_fail);
    desc.depthStencilPassOperation = static_cast<MTLStencilOperation>(face.pass);
    desc.readMask = state.stencil_read_mask;
    desc.writeMask = state.stencil_write_mask;
}

id<MTLDepthStencilState> depth_state(CommandPool* pool, const DepthStencilState& state)
{
    Device* device = pool->device;
    DepthStateChunk** chunk = &pool->depth_states;
    while (*chunk)
    {
        for (uint32 i = 0; i < (*chunk)->count; ++i)
            if (same_depth((*chunk)->entries[i].desc, state)) return (*chunk)->entries[i].state;
        if ((*chunk)->count < 64) break;
        chunk = &(*chunk)->next;
    }
    if (!*chunk) *chunk = new DepthStateChunk{};
    MTLDepthStencilDescriptor* desc = [MTLDepthStencilDescriptor new];
    desc.depthCompareFunction = state.depth_test ? static_cast<MTLCompareFunction>(state.depth_compare) : MTLCompareFunctionAlways;
    desc.depthWriteEnabled = state.depth_write && state.depth_test;
    if (state.stencil_test)
    {
        MTLStencilDescriptor* front = [MTLStencilDescriptor new];
        MTLStencilDescriptor* back = [MTLStencilDescriptor new];
        stencil_descriptor(front, state.front, state);
        stencil_descriptor(back, state.back, state);
        desc.frontFaceStencil = front;
        desc.backFaceStencil = back;
        [front release]; [back release];
    }
    id<MTLDepthStencilState> result = [device->metal newDepthStencilStateWithDescriptor:desc];
    [desc release];
    (*chunk)->entries[(*chunk)->count++] = {.desc = state, .state = result};
    return result;
}
}

void begin_render_pass(CommandBuffer* commands, const RenderingDesc& desc, RenderingFlags flags) noexcept
{
    @autoreleasepool
    {
        assert(commands->recording && !commands->render && commands->pool->kind == QueueKind::general && desc.colors.size <= 8);
        end_compute(commands);
        MTLRenderPassDescriptor* pass = commands->device->metal4 ? (MTLRenderPassDescriptor*)commands->pass : commands->pass3;
        RenderView* area = desc.colors.size ? desc.colors.data[0].render_view :
                           desc.depth.render_view ? desc.depth.render_view : desc.stencil.render_view;
        for (uint32 i = 0; i < 8; ++i)
        {
            if (i < desc.colors.size)
            {
                const ColorAttachment& color = desc.colors.data[i];
                render_attachment(pass.colorAttachments[i], color.render_view, color.load, color.store);
                pass.colorAttachments[i].clearColor = MTLClearColorMake(color.clear.x, color.clear.y, color.clear.z, color.clear.w);
                if (!area) area = color.render_view;
            }
            else pass.colorAttachments[i].texture = nil;
        }
        render_attachment(pass.depthAttachment, desc.depth.render_view, desc.depth.load, desc.depth.store);
        pass.depthAttachment.clearDepth = desc.depth.clear;
        render_attachment(pass.stencilAttachment, desc.stencil.render_view, desc.stencil.load, desc.stencil.store);
        pass.stencilAttachment.clearStencil = desc.stencil.clear;
        assert(area);
        const NSUInteger width = area->texture.width >> area->mip;
        const NSUInteger height = area->texture.height >> area->mip;
        pass.renderTargetWidth = width ? width : 1;
        pass.renderTargetHeight = height ? height : 1;
        commands->render_continuation = flags != RenderingFlags::none;
        MTL4RenderEncoderOptions options = (static_cast<uint32>(flags) & static_cast<uint32>(RenderingFlags::suspending)) != 0 ?
                                          MTL4RenderEncoderOptionSuspending : MTL4RenderEncoderOptionNone;
        if ((static_cast<uint32>(flags) & static_cast<uint32>(RenderingFlags::resuming)) != 0) options |= MTL4RenderEncoderOptionResuming;
        if (commands->device->metal4)
        {
            commands->render = [[native_commands(commands) renderCommandEncoderWithDescriptor:commands->pass options:options] retain];
            [commands->render setArgumentTable:commands->arguments atStages:render_stages];
        }
        else
        {
            const bool suspending = (static_cast<uint32>(flags) & static_cast<uint32>(RenderingFlags::suspending)) != 0;
            const bool resuming = (static_cast<uint32>(flags) & static_cast<uint32>(RenderingFlags::resuming)) != 0;
            for (uint32 i = 0; i < 8; ++i)
            {
                if (resuming) pass.colorAttachments[i].loadAction = MTLLoadActionLoad;
                if (suspending) pass.colorAttachments[i].storeAction = MTLStoreActionStore;
            }
            if (resuming) { pass.depthAttachment.loadAction = MTLLoadActionLoad; pass.stencilAttachment.loadAction = MTLLoadActionLoad; }
            if (suspending) { pass.depthAttachment.storeAction = MTLStoreActionStore; pass.stencilAttachment.storeAction = MTLStoreActionStore; }
            commands->render = [[commands->commands3 renderCommandEncoderWithDescriptor:pass] retain];
            [(id<MTLRenderCommandEncoder>)commands->render waitForFence:commands->pool->queue->dependencies beforeStages:render_stages];
            if (resuming) [(id<MTLRenderCommandEncoder>)commands->render waitForFence:commands->pool->queue->producers beforeStages:render_stages];
            for (uint32 i = 0; i < 3; ++i) bind_buffer(commands, commands->bindings[i], commands->binding_offsets[i], i);
        }
        [commands->render setFrontFacingWinding:MTLWindingCounterClockwise];
        [commands->render setViewport:MTLViewport{0, double(height ? height : 1), double(width ? width : 1), -double(height ? height : 1), 0, 1}];
        [commands->render setScissorRect:MTLScissorRect{0, 0, width ? width : 1, height ? height : 1}];
        [commands->render setDepthStencilState:depth_state(commands->pool, {})];
        if (commands->pso && commands->pso->render) bind_pso(commands, commands->pso);
    }
}

void end_render_pass(CommandBuffer* commands) noexcept
{
    @autoreleasepool
    {
        assert(commands->render);
        MTLRenderPassDescriptor* pass = commands->device->metal4 ? (MTLRenderPassDescriptor*)commands->pass : commands->pass3;
        if (!commands->device->metal4)
            [(id<MTLRenderCommandEncoder>)commands->render updateFence:commands->pool->queue->producers afterStages:render_stages];
        [commands->render endEncoding];
        [commands->render release];
        commands->render = nil;
        if (commands->render_continuation && commands->device->metal4)
        {
            [commands->commands endCommandBuffer];
            commands->commands = nil;
        }
        for (uint32 i = 0; i < 8; ++i) pass.colorAttachments[i].texture = nil;
        pass.depthAttachment.texture = nil;
        pass.stencilAttachment.texture = nil;
    }
}

void set_viewport(CommandBuffer* commands, const Viewport& viewport) noexcept
{
    @autoreleasepool
    {
        assert(commands->recording);
        if (!commands->render) return;
        [commands->render setViewport:MTLViewport{viewport.x, double(viewport.y) + viewport.height, viewport.width, -double(viewport.height),
                                                viewport.min_depth, viewport.max_depth}];
    }
}

void set_scissor(CommandBuffer* commands, const Scissor& scissor) noexcept
{
    @autoreleasepool
    {
        assert(commands->recording);
        if (!commands->render) return;
        [commands->render setScissorRect:MTLScissorRect{static_cast<NSUInteger>(scissor.x), static_cast<NSUInteger>(scissor.y), scissor.width, scissor.height}];
    }
}

void set_depth_stencil(CommandBuffer* commands, const DepthStencilState& state) noexcept
{
    @autoreleasepool
    {
        assert(commands->recording);
        id<MTLDepthStencilState> depth = depth_state(commands->pool, state);
        if (!commands->render) return;
        [commands->render setDepthStencilState:depth];
        [commands->render setStencilFrontReferenceValue:state.front.reference backReferenceValue:state.back.reference];
    }
}

void bind_pso(CommandBuffer* commands, const PSO* pso) noexcept
{
    @autoreleasepool
    {
        assert(commands->recording && pso->device == commands->device);
        commands->pso = pso;
        if (pso->compute)
        {
            assert(!commands->render);
            if (commands->compute) [commands->compute setComputePipelineState:pso->compute];
            else compute_encoder(commands);
        }
        else if (commands->render)
        {
            [commands->render setRenderPipelineState:pso->render];
            [commands->render setCullMode:pso->rasterization.cull == CullMode::none ? MTLCullModeNone :
                                         pso->rasterization.cull == CullMode::clockwise ? MTLCullModeBack : MTLCullModeFront];
            [commands->render setDepthBias:pso->rasterization.depth_bias_constant slopeScale:pso->rasterization.depth_bias_slope
                                    clamp:pso->rasterization.depth_bias_clamp];
        }
    }
}

namespace
{
void root_data(CommandBuffer* commands, const void* root)
{
    if (!root) return;
    assert((reinterpret_cast<uintptr>(root) & 15u) == 0);
    if (commands->device->metal4) [commands->arguments setAddress:reinterpret_cast<uintptr>(root) atIndex:0];
    else
    {
        uint64 offset = 0;
        id<MTLBuffer> buffer = resolve_buffer(commands->device, {.gpu = const_cast<void*>(root), .size = 1}, &offset);
        bind_buffer(commands, buffer, offset, 0);
    }
}
}

void draw(CommandBuffer* commands, const void* root, uint32 vertex_count, uint32 instance_count, uint32 first_vertex, uint32 first_instance) noexcept
{
    @autoreleasepool
    {
        assert(commands->render && commands->pso && !commands->pso->mesh);
        root_data(commands, root);
        [commands->render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:first_vertex vertexCount:vertex_count instanceCount:instance_count
                           baseInstance:first_instance];
    }
}

void draw_indexed(CommandBuffer* commands, const void* root, GpuRange indices, IndexType type, uint32 index_count, uint32 instance_count,
                  uint32 first_index, int32 vertex_offset, uint32 first_instance) noexcept
{
    @autoreleasepool
    {
        assert(commands->render && commands->pso && !commands->pso->mesh);
        root_data(commands, root);
        const uint64 offset = first_index * (type == IndexType::uint16 ? 2u : 4u);
        if (!commands->device->metal4)
        {
            uint64 buffer_offset = 0;
            id<MTLBuffer> buffer = resolve_buffer(commands->device, indices, &buffer_offset);
            [(id<MTLRenderCommandEncoder>)commands->render drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:index_count
                                     indexType:type == IndexType::uint16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                                   indexBuffer:buffer indexBufferOffset:buffer_offset + offset
                                 instanceCount:instance_count baseVertex:vertex_offset baseInstance:first_instance];
            return;
        }
        [commands->render drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:index_count
                                     indexType:type == IndexType::uint16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                                   indexBuffer:reinterpret_cast<uintptr>(indices.gpu) + offset indexBufferLength:indices.size - offset
                                 instanceCount:instance_count baseVertex:vertex_offset baseInstance:first_instance];
    }
}

void draw_indirect(CommandBuffer* commands, const void* root, GpuRange arguments, uint32 draw_count, uint32 stride) noexcept
{
    @autoreleasepool
    {
        assert(commands->render && commands->pso && !commands->pso->mesh);
        root_data(commands, root);
        if (!stride) stride = sizeof(MTLDrawPrimitivesIndirectArguments);
        if (!commands->device->metal4)
        {
            uint64 offset = 0;
            id<MTLBuffer> buffer = resolve_buffer(commands->device, arguments, &offset);
            for (uint32 i = 0; i < draw_count; ++i)
                [(id<MTLRenderCommandEncoder>)commands->render drawPrimitives:MTLPrimitiveTypeTriangle
                                                              indirectBuffer:buffer indirectBufferOffset:offset + i * stride];
            return;
        }
        for (uint32 i = 0; i < draw_count; ++i)
            [(id<MTL4RenderCommandEncoder>)commands->render drawPrimitives:MTLPrimitiveTypeTriangle
                                                          indirectBuffer:reinterpret_cast<uintptr>(arguments.gpu) + i * stride];
    }
}

void draw_indexed_indirect(CommandBuffer* commands, const void* root, GpuRange indices, IndexType type, GpuRange arguments, uint32 draw_count,
                           uint32 stride) noexcept
{
    @autoreleasepool
    {
        assert(commands->render && commands->pso && !commands->pso->mesh);
        root_data(commands, root);
        if (!stride) stride = sizeof(MTLDrawIndexedPrimitivesIndirectArguments);
        if (!commands->device->metal4)
        {
            uint64 index_offset = 0;
            uint64 argument_offset = 0;
            id<MTLBuffer> index_buffer = resolve_buffer(commands->device, indices, &index_offset);
            id<MTLBuffer> argument_buffer = resolve_buffer(commands->device, arguments, &argument_offset);
            for (uint32 i = 0; i < draw_count; ++i)
                [(id<MTLRenderCommandEncoder>)commands->render drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                         indexType:type == IndexType::uint16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                                       indexBuffer:index_buffer indexBufferOffset:index_offset
                                    indirectBuffer:argument_buffer indirectBufferOffset:argument_offset + i * stride];
            return;
        }
        for (uint32 i = 0; i < draw_count; ++i)
            [commands->render drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexType:type == IndexType::uint16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                                       indexBuffer:reinterpret_cast<uintptr>(indices.gpu) indexBufferLength:indices.size
                                    indirectBuffer:reinterpret_cast<uintptr>(arguments.gpu) + i * stride];
    }
}

void dispatch(CommandBuffer* commands, const void* root, uint32x3 group_count) noexcept
{
    @autoreleasepool
    {
        assert(commands->pso && commands->pso->compute);
        root_data(commands, root);
        [compute_encoder(commands) dispatchThreadgroups:metal_size(group_count) threadsPerThreadgroup:commands->pso->threads];
    }
}

void dispatch_indirect(CommandBuffer* commands, const void* root, GpuRange arguments) noexcept
{
    @autoreleasepool
    {
        assert(commands->pso && commands->pso->compute);
        root_data(commands, root);
        if (commands->device->metal4)
            [(id<MTL4ComputeCommandEncoder>)compute_encoder(commands) dispatchThreadgroupsWithIndirectBuffer:reinterpret_cast<uintptr>(arguments.gpu)
                                                                                     threadsPerThreadgroup:commands->pso->threads];
        else
        {
            uint64 offset = 0;
            id<MTLBuffer> buffer = resolve_buffer(commands->device, arguments, &offset);
            [(id<MTLComputeCommandEncoder>)compute_encoder(commands) dispatchThreadgroupsWithIndirectBuffer:buffer indirectBufferOffset:offset
                                                                                   threadsPerThreadgroup:commands->pso->threads];
        }
    }
}

void draw_meshlets(CommandBuffer* commands, const void* root, uint32x3 group_count) noexcept
{
    @autoreleasepool
    {
        assert(commands->render && commands->pso && commands->pso->mesh);
        root_data(commands, root);
        [commands->render drawMeshThreadgroups:metal_size(group_count) threadsPerObjectThreadgroup:commands->pso->object_threads
                     threadsPerMeshThreadgroup:commands->pso->threads];
    }
}

void draw_meshlets_indirect(CommandBuffer* commands, const void* root, GpuRange arguments, uint32 draw_count, uint32 stride) noexcept
{
    @autoreleasepool
    {
        assert(commands->render && commands->pso && commands->pso->mesh);
        assert(commands->device->caps.indirect_mesh_draw);
        root_data(commands, root);
        if (!stride) stride = sizeof(MTLDispatchThreadgroupsIndirectArguments);
        if (!commands->device->metal4)
        {
            uint64 offset = 0;
            id<MTLBuffer> buffer = resolve_buffer(commands->device, arguments, &offset);
            for (uint32 i = 0; i < draw_count; ++i)
                [(id<MTLRenderCommandEncoder>)commands->render drawMeshThreadgroupsWithIndirectBuffer:buffer indirectBufferOffset:offset + i * stride
                                             threadsPerObjectThreadgroup:commands->pso->object_threads threadsPerMeshThreadgroup:commands->pso->threads];
            return;
        }
        for (uint32 i = 0; i < draw_count; ++i)
            [commands->render drawMeshThreadgroupsWithIndirectBuffer:reinterpret_cast<uintptr>(arguments.gpu) + i * stride
                                        threadsPerObjectThreadgroup:commands->pso->object_threads threadsPerMeshThreadgroup:commands->pso->threads];
    }
}

} // namespace gpu
