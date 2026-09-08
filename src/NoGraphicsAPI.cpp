#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#endif

#include <vulkan/vulkan.h>

#include <NoGraphicsAPI/bit.hpp>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Keep assert expressions type-checked in release without evaluating them.
#if defined(NDEBUG)
#undef assert
#define assert(expression) ((void)sizeof(static_cast<bool>(expression)))
#endif

namespace gpu
{

namespace
{

constexpr uint32 max_device_extensions = 512;
constexpr uint32 max_instance_extensions = 256;
constexpr uint32 max_instance_layers = 64;
constexpr uint32 max_physical_devices = 32;
constexpr uint32 max_queue_families = 64;
constexpr uint32 max_color_attachments = 8;
constexpr uint32 image_barrier_batch_size = 64;
constexpr uint32 initial_command_context_count = 2;
constexpr uint32 max_swapchain_images = 8;
constexpr VkPresentModeKHR swapchain_present_mode = VK_PRESENT_MODE_FIFO_KHR;
constexpr uint32 gpu_allocation_alignment = 16;
constexpr uint32 max_surface_formats = 64;
constexpr uint32 format_count = static_cast<uint32>(Format::undefined);

[[nodiscard]] Error error_from_vk(VkResult result) noexcept
{
    switch (result)
    {
    case VK_SUCCESS: return Error::none;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_TOO_MANY_OBJECTS: abort();
    case VK_ERROR_DEVICE_LOST: return Error::device_lost;
    case VK_ERROR_LAYER_NOT_PRESENT:
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_INCOMPATIBLE_DRIVER:
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return Error::unsupported;
    default: return Error::driver_error;
    }
}

[[noreturn]] void abort_vk_failure(VkResult result) noexcept
{
    (void)result;
    assert(result == VK_SUCCESS && "unexpected Vulkan failure");
    abort();
}

void require_vk(VkResult result) noexcept
{
    if (result != VK_SUCCESS)
        abort_vk_failure(result);
}

void assert_vk(VkResult result) noexcept
{
    assert(result == VK_SUCCESS && "unexpected Vulkan failure");
    (void)result;
}

void require_error(Error error) noexcept
{
    if (error != Error::none)
    {
        assert(false && "unexpected graphics API failure");
        abort();
    }
}

template<typename T>
T load_instance_proc(VkInstance instance, const char* name) noexcept
{
    const PFN_vkVoidFunction proc = vkGetInstanceProcAddr(instance, name);
    return reinterpret_cast<T>(proc);
}

template<typename T>
T load_device_proc(VkDevice device, const char* name) noexcept
{
    const PFN_vkVoidFunction proc = vkGetDeviceProcAddr(device, name);
    return reinterpret_cast<T>(proc);
}

template<typename T>
T align_up(T value, T alignment)
{
    assert(alignment != 0);
    return ((value + alignment - 1) / alignment) * alignment;
}

template<typename T>
constexpr bool has_flag(T value, T flag)
{
    using U = __underlying_type(T);
    return (static_cast<U>(value) & static_cast<U>(flag)) != 0;
}

bool has_name(Span<const VkExtensionProperties> values, const char* name)
{
    for (size_t index = 0; index < values.size; ++index)
    {
        if (strcmp(values.data[index].extensionName, name) == 0)
            return true;
    }
    return false;
}

#if !defined(NDEBUG)
bool has_name(Span<const VkLayerProperties> values, const char* name)
{
    for (size_t index = 0; index < values.size; ++index)
    {
        if (strcmp(values.data[index].layerName, name) == 0)
            return true;
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* callback_data, void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && callback_data && callback_data->pMessage)
    {
        // Keep the library callback dependency-free. Applications can still install
        // their own messenger; this one makes validation failures debugger-visible.
        fputs("NoGraphicsAPI validation: ", stderr);
        fputs(callback_data->pMessage, stderr);
        fputc('\n', stderr);
    }
    return VK_FALSE;
}
#endif

VkFormat to_vk(Format format)
{
    switch (format)
    {
    case Format::r8_srgb: return VK_FORMAT_R8_SRGB;
    case Format::rg8_srgb: return VK_FORMAT_R8G8_SRGB;
    case Format::rgba8_srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::bgra8_srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::rgba4_unorm: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case Format::r5g5b5a1_unorm: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case Format::r5g6b5_unorm: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case Format::r8_unorm: return VK_FORMAT_R8_UNORM;
    case Format::rg8_unorm: return VK_FORMAT_R8G8_UNORM;
    case Format::rgba8_unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::bgra8_unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::r16_unorm: return VK_FORMAT_R16_UNORM;
    case Format::rg16_unorm: return VK_FORMAT_R16G16_UNORM;
    case Format::rgba16_unorm: return VK_FORMAT_R16G16B16A16_UNORM;
    case Format::r8_uint: return VK_FORMAT_R8_UINT;
    case Format::rg8_uint: return VK_FORMAT_R8G8_UINT;
    case Format::rgba8_uint: return VK_FORMAT_R8G8B8A8_UINT;
    case Format::bgra8_uint: return VK_FORMAT_B8G8R8A8_UINT;
    case Format::r16_uint: return VK_FORMAT_R16_UINT;
    case Format::rg16_uint: return VK_FORMAT_R16G16_UINT;
    case Format::rgba16_uint: return VK_FORMAT_R16G16B16A16_UINT;
    case Format::r32_uint: return VK_FORMAT_R32_UINT;
    case Format::rg32_uint: return VK_FORMAT_R32G32_UINT;
    case Format::rgb32_uint: return VK_FORMAT_R32G32B32_UINT;
    case Format::rgba32_uint: return VK_FORMAT_R32G32B32A32_UINT;
    case Format::r16_float: return VK_FORMAT_R16_SFLOAT;
    case Format::rg16_float: return VK_FORMAT_R16G16_SFLOAT;
    case Format::rgba16_float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::r32_float: return VK_FORMAT_R32_SFLOAT;
    case Format::rg32_float: return VK_FORMAT_R32G32_SFLOAT;
    case Format::rgb32_float: return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::rgba32_float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::rgb10a2_unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case Format::rg11b10_float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case Format::d16_unorm: return VK_FORMAT_D16_UNORM;
    case Format::d24_unorm_s8_uint: return VK_FORMAT_D24_UNORM_S8_UINT;
    case Format::d32_float: return VK_FORMAT_D32_SFLOAT;
    case Format::s8_uint: return VK_FORMAT_S8_UINT;
    case Format::d32_float_s8_uint: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case Format::eac_rg: return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
    case Format::astc_4x4_srgb: return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    case Format::astc_4x4_unorm: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case Format::bc3_srgb: return VK_FORMAT_BC3_SRGB_BLOCK;
    case Format::bc3_unorm: return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::bc5_rg: return VK_FORMAT_BC5_UNORM_BLOCK;
    case Format::bc7_srgb: return VK_FORMAT_BC7_SRGB_BLOCK;
    case Format::bc7_unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
    case Format::undefined: return VK_FORMAT_UNDEFINED;
    }
    return {};
}

uint64 divide_up(uint64 value, uint64 divisor) noexcept
{
    assert(divisor != 0);
    return value / divisor + (value % divisor != 0 ? 1u : 0u);
}

constexpr bool compatible_view_formats(Format image_format, Format view_format) noexcept
{
    if (image_format == view_format) return true;
    const TextureFormatInfo image = get_texture_format_info(image_format);
    const TextureFormatInfo view = get_texture_format_info(view_format);
    if (image.depth || image.stencil || view.depth || view.stencil) return false;
    if (image.block_extent.x == 1 && view.block_extent.x == 1)
        return image.bytes_per_block != 0 && image.bytes_per_block == view.bytes_per_block;
    return (image_format == Format::astc_4x4_unorm && view_format == Format::astc_4x4_srgb) ||
           (image_format == Format::astc_4x4_srgb && view_format == Format::astc_4x4_unorm) ||
           (image_format == Format::bc3_unorm && view_format == Format::bc3_srgb) ||
           (image_format == Format::bc3_srgb && view_format == Format::bc3_unorm) ||
           (image_format == Format::bc7_unorm && view_format == Format::bc7_srgb) ||
           (image_format == Format::bc7_srgb && view_format == Format::bc7_unorm);
}

bool has_depth_aspect(Format format) noexcept
{
    return get_texture_format_info(format).depth;
}

bool has_stencil_aspect(Format format) noexcept
{
    return get_texture_format_info(format).stencil;
}

VkImageAspectFlags image_aspects(Format format) noexcept
{
    VkImageAspectFlags result = 0;
    if (has_depth_aspect(format)) result |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (has_stencil_aspect(format)) result |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (result == 0) result = VK_IMAGE_ASPECT_COLOR_BIT;
    return result;
}

VkImageType to_vk(TextureType type) noexcept
{
    switch (type)
    {
    case TextureType::one_d: return VK_IMAGE_TYPE_1D;
    case TextureType::three_d: return VK_IMAGE_TYPE_3D;
    case TextureType::two_d:
    case TextureType::cube:
    case TextureType::two_d_array:
    case TextureType::cube_array: return VK_IMAGE_TYPE_2D;
    }
    return VK_IMAGE_TYPE_MAX_ENUM;
}

VkImageViewType to_vk_view(TextureType type) noexcept
{
    switch (type)
    {
    case TextureType::one_d: return VK_IMAGE_VIEW_TYPE_1D;
    case TextureType::two_d: return VK_IMAGE_VIEW_TYPE_2D;
    case TextureType::three_d: return VK_IMAGE_VIEW_TYPE_3D;
    case TextureType::cube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TextureType::two_d_array: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TextureType::cube_array: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    }
    return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
}

VkFormatFeatureFlags2 required_format_features(TextureUsage usage)
{
    VkFormatFeatureFlags2 result = 0;
    if (has_flag(usage, TextureUsage::sampled))
        result |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
    if (has_flag(usage, TextureUsage::storage))
        result |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    if (has_flag(usage, TextureUsage::color_attachment))
        result |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
    if (has_flag(usage, TextureUsage::depth_stencil_attachment))
        result |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (has_flag(usage, TextureUsage::transfer_source))
        result |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
    if (has_flag(usage, TextureUsage::transfer_destination))
        result |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
    return result;
}

constexpr uint32 known_texture_usage_bits =
    static_cast<uint32>(TextureUsage::sampled) |
    static_cast<uint32>(TextureUsage::storage) |
    static_cast<uint32>(TextureUsage::color_attachment) |
    static_cast<uint32>(TextureUsage::depth_stencil_attachment) |
    static_cast<uint32>(TextureUsage::transfer_source) |
    static_cast<uint32>(TextureUsage::transfer_destination);

bool has_valid_texture_usage_bits(TextureUsage usage) noexcept
{
    const uint32 bits = static_cast<uint32>(usage);
    return bits != 0 && (bits & ~known_texture_usage_bits) == 0;
}

enum class TextureCompression : uint8
{
    none,
    etc2,
    astc,
    bc,
};

TextureCompression texture_compression(Format format) noexcept
{
    switch (format)
    {
    case Format::bc3_srgb:
    case Format::bc3_unorm:
    case Format::bc5_rg:
    case Format::bc7_srgb:
    case Format::bc7_unorm: return TextureCompression::bc;
    case Format::astc_4x4_srgb:
    case Format::astc_4x4_unorm: return TextureCompression::astc;
    case Format::eac_rg: return TextureCompression::etc2;
    default: return TextureCompression::none;
    }
}

VkFormatFeatureFlags2 optimal_format_features(VkPhysicalDevice physical_device, Format format)
{
    VkFormatProperties3 properties3{
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
    };
    VkFormatProperties2 properties2{
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &properties3,
    };
    vkGetPhysicalDeviceFormatProperties2(physical_device, to_vk(format), &properties2);
    return properties3.optimalTilingFeatures;
}

VkBlendFactor to_vk(BlendFactor factor)
{
    switch (factor)
    {
    case BlendFactor::zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::one: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::source_color: return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::one_minus_source_color: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::destination_color: return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::one_minus_destination_color: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::source_alpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::one_minus_source_alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::destination_alpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::one_minus_destination_alpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::source_alpha_saturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }
    return VK_BLEND_FACTOR_MAX_ENUM;
}

constexpr VkPipelineStageFlags2 to_vk(Stage stages)
{
    VkPipelineStageFlags2 result = 0;
    if (has_flag(stages, Stage::indirect)) result |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    if (has_flag(stages, Stage::index_input)) result |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    if (has_flag(stages, Stage::vertex)) result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    if (has_flag(stages, Stage::mesh)) result |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
    if (has_flag(stages, Stage::depth_stencil_tests)) result |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    if (has_flag(stages, Stage::fragment)) result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (has_flag(stages, Stage::color_output)) result |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (has_flag(stages, Stage::compute)) result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (has_flag(stages, Stage::transfer)) result |= VK_PIPELINE_STAGE_2_COPY_BIT;
    if (has_flag(stages, Stage::host)) result |= VK_PIPELINE_STAGE_2_HOST_BIT;
    if (has_flag(stages, Stage::all_commands)) result |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    return result;
}

static_assert(to_vk(Stage::all_commands) == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
static_assert(to_vk(Stage::all_commands | Stage::host) ==
              (VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT));

VkAccessFlags2 to_vk(Access accesses)
{
    VkAccessFlags2 result = 0;
    if (has_flag(accesses, Access::transfer_read)) result |= VK_ACCESS_2_TRANSFER_READ_BIT;
    if (has_flag(accesses, Access::transfer_write)) result |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (has_flag(accesses, Access::shader_read)) result |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    if (has_flag(accesses, Access::shader_write)) result |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (has_flag(accesses, Access::color_read)) result |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    if (has_flag(accesses, Access::color_write)) result |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (has_flag(accesses, Access::depth_stencil_read)) result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (has_flag(accesses, Access::depth_stencil_write)) result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (has_flag(accesses, Access::indirect_read)) result |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    if (has_flag(accesses, Access::index_read)) result |= VK_ACCESS_2_INDEX_READ_BIT;
    if (has_flag(accesses, Access::host_read)) result |= VK_ACCESS_2_HOST_READ_BIT;
    if (has_flag(accesses, Access::descriptor_read)) result |= VK_ACCESS_2_SAMPLER_HEAP_READ_BIT_EXT | VK_ACCESS_2_RESOURCE_HEAP_READ_BIT_EXT;
    return result;
}

constexpr VkBufferUsageFlags universal_buffer_usage =
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

constexpr VkMemoryPropertyFlags cpu_visible_memory_properties =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

constexpr VkMemoryPropertyFlags forbidden_memory_properties =
    VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
    VK_MEMORY_PROPERTY_PROTECTED_BIT |
    VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
    VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;

bool is_usable_memory_type(const VkPhysicalDeviceMemoryProperties& properties, uint32 index)
{
    const VkMemoryType& type = properties.memoryTypes[index];
    if ((type.propertyFlags & forbidden_memory_properties) != 0)
        return false;
    return (properties.memoryHeaps[type.heapIndex].flags & VK_MEMORY_HEAP_TILE_MEMORY_BIT_QCOM) == 0;
}

constexpr VkAddressCommandFlagsKHR address_flags =
    VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR |
    VK_ADDRESS_COMMAND_STORAGE_BUFFER_USAGE_BIT_KHR;
} // namespace

namespace detail
{

struct DeviceFunctions
{
    PFN_vkWriteSamplerDescriptorsEXT write_sampler_descriptors = nullptr;
    PFN_vkWriteResourceDescriptorsEXT write_resource_descriptors = nullptr;
    PFN_vkCmdBindSamplerHeapEXT cmd_bind_sampler_heap = nullptr;
    PFN_vkCmdBindResourceHeapEXT cmd_bind_texture_heap = nullptr;
    PFN_vkCmdPushDataEXT cmd_push_data = nullptr;
    PFN_vkCmdBindIndexBuffer3KHR cmd_bind_index_buffer = nullptr;
    PFN_vkCmdDrawIndirect2KHR cmd_draw_indirect = nullptr;
    PFN_vkCmdDrawIndexedIndirect2KHR cmd_draw_indexed_indirect = nullptr;
    PFN_vkCmdDispatchIndirect2KHR cmd_dispatch_indirect = nullptr;
    PFN_vkCmdDrawMeshTasksEXT cmd_draw_mesh_tasks = nullptr;
    PFN_vkCmdDrawMeshTasksIndirect2EXT cmd_draw_mesh_tasks_indirect = nullptr;
    PFN_vkCmdCopyMemoryKHR cmd_copy_memory = nullptr;
    PFN_vkCmdCopyMemoryToImageKHR cmd_copy_memory_to_image = nullptr;
    PFN_vkCmdCopyImageToMemoryKHR cmd_copy_image_to_memory = nullptr;
};

struct BackingBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceAddress address = 0;
};

struct PresentContext;

struct DeferredSwapchainImage
{
    uint64 retire_value = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct SwapchainDeleteQueue
{
    void push(uint64 retire_value, VkSwapchainKHR swapchain, VkImageView view) noexcept
    {
        assert(swapchain && view);
        if (count == capacity)
        {
            const size_t old_size = capacity;
            capacity = old_size == 0 ? 1 : old_size * 2;
            entries = static_cast<DeferredSwapchainImage*>(realloc(entries, capacity * sizeof(DeferredSwapchainImage)));
            for (size_t index = 0; index < first; ++index)
            {
                entries[old_size + index] = entries[index];
                entries[index] = {};
            }
        }
        if (count != 0)
        {
            const size_t back = (first + count - 1) % capacity;
            assert(entries[back].retire_value <= retire_value &&
                   "swapchain deletion retire values must be monotonic");
        }
        entries[(first + count) % capacity] = {
            .retire_value = retire_value,
            .swapchain = swapchain,
            .view = view,
        };
        ++count;
    }

    void collect(VkDevice device, uint64 completed_value) noexcept
    {
        while (count != 0 && entries[first].retire_value <= completed_value)
        {
            const DeferredSwapchainImage& entry = entries[first];
            const bool final_image = count == 1 || entries[(first + 1) % capacity].swapchain != entry.swapchain;
            vkDestroyImageView(device, entry.view, nullptr);
            if (final_image)
                vkDestroySwapchainKHR(device, entry.swapchain, nullptr);
            entries[first] = {};
            first = (first + 1) % capacity;
            --count;
        }
        if (count == 0) first = 0;
    }

    DeferredSwapchainImage* entries = nullptr;
    size_t capacity = 0;
    size_t first = 0;
    size_t count = 0;
};

struct RetiredSwapchain
{
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkImageView views[max_swapchain_images]{};
    uint32 view_count = 0;
};

struct TextureInitialization
{
    VkImage image = VK_NULL_HANDLE;
    VkImageAspectFlags aspect_mask = 0;
    uint32 mip_levels = 0;
    uint32 array_layers = 0;
    TextureInitialization* previous = nullptr;
    TextureInitialization* next = nullptr;
    struct TextureInitializationList* owner = nullptr;
};

struct TextureInitializationList
{
    TextureInitialization* first = nullptr;
    TextureInitialization* last = nullptr;
};

void append_texture_initialization(TextureInitializationList& list, TextureInitialization& initialization) noexcept
{
    assert(!initialization.owner && !initialization.previous && !initialization.next);
    initialization.owner = &list;
    initialization.previous = list.last;
    if (list.last)
        list.last->next = &initialization;
    else
        list.first = &initialization;
    list.last = &initialization;
}

void remove_texture_initialization(TextureInitialization& initialization) noexcept
{
    TextureInitializationList* list = initialization.owner;
    assert(list);
    if (initialization.previous)
        initialization.previous->next = initialization.next;
    else
        list->first = initialization.next;
    if (initialization.next)
        initialization.next->previous = initialization.previous;
    else
        list->last = initialization.previous;
    initialization.owner = nullptr;
    initialization.previous = nullptr;
    initialization.next = nullptr;
}

} // namespace detail

struct Swapchain;

struct GpuHeapOwner
{
    Device* state = nullptr;
    detail::BackingBuffer backing;
};

struct TextureHeapOwner
{
    Device* state = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct TimelineSemaphore
{
    Device* state = nullptr;
    VkSemaphore semaphore = VK_NULL_HANDLE;
};

struct CommandBuffer
{
    Device* state = nullptr;
    CommandBuffer* next = nullptr;
    CommandBuffer* previous = nullptr;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    uint64 retire_value = 0;
    Swapchain* swapchain = nullptr;
};

namespace detail
{

struct PresentContext
{
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;
    VkFence presented = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    bool present_pending = false;
};

} // namespace detail

struct Device
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_messenger = nullptr;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    uint32 queue_family = 0;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceProperties physical_properties{};
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heap_properties{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    uint64 max_timeline_value_difference = 0;
    uint64 texture_heap_alignment = 16;
    uint32 texture_memory_type = VK_MAX_MEMORY_TYPES;
    detail::DeviceFunctions fn;
    DeviceCaps caps;
    VkFormatFeatureFlags2 format_features[format_count]{};
    bool texture_compression_etc2 = false;
    detail::TextureInitializationList pending_texture_initializations;
    CommandBuffer* next_command_context = nullptr;
    VkCommandBufferSubmitInfo* command_submit_infos = nullptr;
    size_t command_submit_capacity = 0;
    size_t command_context_count = 0;
    VkSemaphore command_retirement = VK_NULL_HANDLE;
    uint64 command_retirement_value = 0;
    uint64 completed_command_retirement = 0;
    detail::SwapchainDeleteQueue swapchain_delete_queue;
    detail::PresentContext present_contexts[max_swapchain_images]{};
    detail::RetiredSwapchain retired_swapchains[max_swapchain_images]{};
    Swapchain* swapchain = nullptr;
    Swapchain* acquired_swapchain = nullptr;
    uint32 active_command_buffers = 0;
    uint32 present_context_count = 0;
    uint32 next_present_context = 0;

    ~Device();

    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] bool find_memory_type(uint32 bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred, VkDeviceSize minimum_heap_size,
                                        uint32& output, VkMemoryPropertyFlags avoided = 0) const noexcept
    {
        bool has_best = false;
        bool best_is_avoided = false;
        uint32 best = 0;
        uint32 best_score = 0;
        VkDeviceSize best_heap_size = 0;
        for (uint32 i = 0; i < memory_properties.memoryTypeCount; ++i)
        {
            if ((bits & (1u << i)) == 0)
                continue;
            const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[i].propertyFlags;
            if ((flags & required) != required)
                continue;
            if (!is_usable_memory_type(memory_properties, i))
                continue;
            const VkMemoryHeap& heap = memory_properties.memoryHeaps[memory_properties.memoryTypes[i].heapIndex];
            if (heap.size < minimum_heap_size)
            {
                continue;
            }
            const bool is_avoided = (flags & avoided) != 0;
            const uint32 score = static_cast<uint32>(detail::popcount(flags & preferred));
            if (!has_best || (best_is_avoided && !is_avoided) ||
                (best_is_avoided == is_avoided &&
                 (score > best_score || (score == best_score && heap.size > best_heap_size))))
            {
                best = i;
                has_best = true;
                best_is_avoided = is_avoided;
                best_score = score;
                best_heap_size = heap.size;
            }
        }
        if (!has_best)
            return false;
        output = best;
        return true;
    }

    void create_backing_buffer(detail::BackingBuffer& output, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                               VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoided = 0) const noexcept
    {
        output = {};
        detail::BackingBuffer result{};
        const VkBufferCreateInfo buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        require_vk(vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer));

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
        uint32 memory_type = 0;
        const bool has_memory_type = find_memory_type(
            requirements.memoryTypeBits, required, preferred,
            requirements.size, memory_type, avoided);
        assert(has_memory_type);
        if (!has_memory_type)
            abort();

        const VkMemoryAllocateFlagsInfo flags_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        };
        const VkMemoryAllocateInfo allocate_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &flags_info,
            .allocationSize = requirements.size,
            .memoryTypeIndex = memory_type,
        };
        require_vk(vkAllocateMemory(device, &allocate_info, nullptr, &result.memory));
        require_vk(vkBindBufferMemory(device, result.buffer, result.memory, 0));

        if ((required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
        {
            require_vk(vkMapMemory(
                device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped));
        }

        const VkBufferDeviceAddressInfo address_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = result.buffer,
        };
        result.address = vkGetBufferDeviceAddress(device, &address_info);
        output = result;
    }

    [[nodiscard]] GpuHeap allocate_gpu_heap(VkDeviceSize size, MemoryType memory) noexcept;
    [[nodiscard]] GpuHeap allocate_descriptor_heap(VkDeviceSize size, MemoryType memory) noexcept;
    [[nodiscard]] Error create_command_contexts() noexcept;
    [[nodiscard]] Error create_command_context(CommandBuffer& context) noexcept;
    [[nodiscard]] Error grow_command_context_pool() noexcept;
    void destroy_command_context(CommandBuffer& context) noexcept;
    void destroy_command_contexts() noexcept;
    void reset_command_context(CommandBuffer& context) noexcept;
    void reset_retired_command_contexts() noexcept;
    [[nodiscard]] CommandBuffer& acquire_command_context() noexcept;
    [[nodiscard]] uint64 next_command_retirement() noexcept;
    void poll_command_retirement() noexcept;
    void wait_command_retirement(uint64 value) noexcept;
    [[nodiscard]] Error create_present_context(detail::PresentContext& context) noexcept;
    void destroy_present_context(detail::PresentContext& context) noexcept;
    void finish_present_context(detail::PresentContext& context) noexcept;
    void wait_present_context(detail::PresentContext& context) noexcept;
    void poll_present_contexts() noexcept;
    void queue_retired_swapchain(detail::RetiredSwapchain& retired) noexcept;
    void drain_contexts() noexcept;

};

namespace
{

VkMemoryRequirements buffer_memory_requirements(Device& device, VkBufferUsageFlags usage) noexcept
{
    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 1,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VkDeviceBufferMemoryRequirements requirements_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
        .pCreateInfo = &buffer_info,
    };
    VkMemoryRequirements2 requirements{
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    vkGetDeviceBufferMemoryRequirements(device.device, &requirements_info, &requirements);
    return requirements.memoryRequirements;
}

bool supports_gpu_heap_memory(Device& device) noexcept
{
    const VkMemoryRequirements ordinary = buffer_memory_requirements(device, universal_buffer_usage);
    uint32 memory_type = 0;
    if (!device.find_memory_type(ordinary.memoryTypeBits, cpu_visible_memory_properties, 0, ordinary.size, memory_type) ||
        !device.find_memory_type(ordinary.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, ordinary.size, memory_type,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
    {
        return false;
    }
    const VkMemoryRequirements descriptor = buffer_memory_requirements(device, universal_buffer_usage | VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT);
    return device.find_memory_type(descriptor.memoryTypeBits, cpu_visible_memory_properties, 0, descriptor.size, memory_type);
}

bool fits_image_format_properties(const VkImageCreateInfo& image_info, const VkImageFormatProperties& properties) noexcept
{
    return image_info.extent.width <= properties.maxExtent.width &&
           image_info.extent.height <= properties.maxExtent.height &&
           image_info.extent.depth <= properties.maxExtent.depth &&
           image_info.mipLevels <= properties.maxMipLevels &&
           image_info.arrayLayers <= properties.maxArrayLayers;
}

bool supports_image_create_info(Device& device, const VkImageCreateInfo& image_info, VkImageFormatProperties* output = nullptr) noexcept
{
    const VkPhysicalDeviceImageFormatInfo2 format_info{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = image_info.format,
        .type = image_info.imageType,
        .tiling = image_info.tiling,
        .usage = image_info.usage,
        .flags = image_info.flags,
    };
    VkImageFormatProperties2 properties{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    };
    const VkResult result = vkGetPhysicalDeviceImageFormatProperties2(device.physical_device, &format_info, &properties);
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED)
        return false;
    require_vk(result);
    if (output)
        *output = properties.imageFormatProperties;
    return fits_image_format_properties(image_info, properties.imageFormatProperties);
}

VkMemoryRequirements image_memory_requirements(Device& device, const VkImageCreateInfo& image_info) noexcept
{
    const VkDeviceImageMemoryRequirements requirements_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
        .pCreateInfo = &image_info,
    };
    VkMemoryRequirements2 requirements{
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    vkGetDeviceImageMemoryRequirements(device.device, &requirements_info, &requirements);
    return requirements.memoryRequirements;
}

void include_texture_heap_alignment(Device& device, const VkMemoryRequirements& requirements) noexcept
{
    if (requirements.alignment > device.texture_heap_alignment)
        device.texture_heap_alignment = requirements.alignment;
}

bool select_texture_memory_type(Device& device) noexcept
{
    const VkFormatFeatureFlags2 color_features = device.format_features[static_cast<uint32>(Format::rgba8_unorm)];
    if ((color_features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) == 0)
        return false;
    // Probes cover DCC-capable color, broad 3D, and sampled depth layouts.
    // Resource Memory Association makes the color mask common to ordinary optimal-tiled images. Intersect every public depth/stencil format below.
    const uint32 probe_2d_size = device.physical_properties.limits.maxImageDimension2D < 2048
                                       ? device.physical_properties.limits.maxImageDimension2D
                                       : 2048;
    VkImageUsageFlags color_usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((color_features & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) != 0)
        color_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {.width = probe_2d_size, .height = probe_2d_size, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = color_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (!supports_image_create_info(device, image_info))
    {
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!supports_image_create_info(device, image_info))
            return false;
    }
    const VkMemoryRequirements color_requirements = image_memory_requirements(device, image_info);
    uint32 memory_type_bits = color_requirements.memoryTypeBits;
    include_texture_heap_alignment(device, color_requirements);

    constexpr TextureUsage broad_texture_usage = TextureUsage::sampled | TextureUsage::storage | TextureUsage::transfer_destination;
    const VkFormatFeatureFlags2 broad_features = device.format_features[static_cast<uint32>(Format::rgba32_float)];
    const VkFormatFeatureFlags2 broad_required_features = required_format_features(broad_texture_usage);
    if ((broad_features & broad_required_features) == broad_required_features)
    {
        const uint32 probe_3d_size = device.physical_properties.limits.maxImageDimension3D < 2048
                                           ? device.physical_properties.limits.maxImageDimension3D
                                           : 2048;
        image_info.imageType = VK_IMAGE_TYPE_3D;
        image_info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        image_info.extent = {.width = probe_3d_size, .height = probe_3d_size, .depth = probe_3d_size < 4 ? probe_3d_size : 4};
        image_info.mipLevels = (32u - detail::count_leading_zeros(probe_3d_size));
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (supports_image_create_info(device, image_info))
            include_texture_heap_alignment(device, image_memory_requirements(device, image_info));
    }

    constexpr Format depth_stencil_formats[]{
        Format::d16_unorm,
        Format::d24_unorm_s8_uint,
        Format::d32_float,
        Format::s8_uint,
        Format::d32_float_s8_uint,
    };
    const VkFormatFeatureFlags2 storage_features = required_format_features(TextureUsage::storage);
    for (Format format : depth_stencil_formats)
    {
        const VkFormatFeatureFlags2 features = device.format_features[static_cast<uint32>(format)];
        const bool combined = has_depth_aspect(format) && has_stencil_aspect(format);
        VkImageUsageFlags compatibility_usage = 0;
        if ((features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) != 0) compatibility_usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        else if ((features & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) compatibility_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        else if ((features & storage_features) == storage_features) compatibility_usage = VK_IMAGE_USAGE_STORAGE_BIT;
        else if (!combined && (features & VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT) != 0) compatibility_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        else if (!combined && (features & VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT) != 0) compatibility_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (compatibility_usage != 0)
        {
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = to_vk(format);
            image_info.extent = {.width = 1, .height = 1, .depth = 1};
            image_info.mipLevels = 1;
            image_info.usage = compatibility_usage;
            VkImageFormatProperties compatibility_properties{};
            if (supports_image_create_info(device, image_info, &compatibility_properties))
            {
                image_info.extent = {.width = 512, .height = 512, .depth = 1};
                if ((features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) != 0 && (features & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
                    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

                bool supported = image_info.usage == compatibility_usage
                                     ? fits_image_format_properties(image_info, compatibility_properties)
                                     : supports_image_create_info(device, image_info);
                if (!supported && image_info.usage != compatibility_usage)
                {
                    image_info.usage = compatibility_usage;
                    supported = fits_image_format_properties(image_info, compatibility_properties);
                }
                if (!supported)
                {
                    image_info.extent = {.width = 1, .height = 1, .depth = 1};
                    image_info.usage = compatibility_usage;
                }

                const VkMemoryRequirements requirements = image_memory_requirements(device, image_info);
                memory_type_bits &= requirements.memoryTypeBits;
                include_texture_heap_alignment(device, requirements);
            }
        }
    }
    return device.find_memory_type(memory_type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 1, device.texture_memory_type,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
}

void destroy_owned_swapchain(Device& device) noexcept;

} // namespace

Device::~Device()
{
    assert(active_command_buffers == 0 && "device destroyed while a command buffer is active");
    assert(!acquired_swapchain && "device destroyed while a swapchain image is acquired");
    if (device)
    {
        drain_contexts();
        destroy_owned_swapchain(*this);
        swapchain_delete_queue.collect(device, completed_command_retirement);
        assert(swapchain_delete_queue.count == 0);
    }
    destroy_command_contexts();
    free(swapchain_delete_queue.entries);
    for (uint32 index = 0; index < present_context_count; ++index)
    {
        destroy_present_context(present_contexts[index]);
    }
    if (device) vkDestroyDevice(device, nullptr);
    if (instance && surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance && debug_messenger && destroy_debug_messenger) destroy_debug_messenger(instance, debug_messenger, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
}

Error Device::create_command_contexts() noexcept
{
    assert(!next_command_context && !command_retirement && !command_context_count);
    const VkSemaphoreTypeCreateInfo type_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type_info,
    };
    Error error = error_from_vk(vkCreateSemaphore(device, &semaphore_info, nullptr, &command_retirement));
    if (error != Error::none)
        return error;
    for (uint32 index = 0; index < initial_command_context_count; ++index)
    {
        error = grow_command_context_pool();
        if (error != Error::none)
        {
            destroy_command_contexts();
            return error;
        }
    }
    return Error::none;
}

Error Device::create_command_context(CommandBuffer& context) noexcept
{
    assert(!context.next && !context.previous && !context.command_pool &&
           !context.command_buffer && !context.state &&
           context.retire_value == 0);
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = queue_family,
    };
    Error error = error_from_vk(vkCreateCommandPool(device, &pool_info, nullptr, &context.command_pool));
    if (error != Error::none)
        return error;
    const VkCommandBufferAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = context.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    error = error_from_vk(vkAllocateCommandBuffers(device, &allocate_info, &context.command_buffer));
    if (error != Error::none)
        destroy_command_context(context);
    return error;
}

Error Device::grow_command_context_pool() noexcept
{
    CommandBuffer* context = new CommandBuffer;
    const Error error = create_command_context(*context);
    if (error != Error::none)
    {
        delete context;
        return error;
    }
    if (command_context_count == command_submit_capacity)
    {
        command_submit_capacity = command_submit_capacity == 0 ? initial_command_context_count : command_submit_capacity * 2;
        command_submit_infos = static_cast<VkCommandBufferSubmitInfo*>(realloc(command_submit_infos, command_submit_capacity * sizeof(*command_submit_infos)));
    }
    ++command_context_count;
    if (!next_command_context)
    {
        context->next = context;
        context->previous = context;
        next_command_context = context;
    }
    else
    {
        context->next = next_command_context;
        context->previous = next_command_context->previous;
        context->previous->next = context;
        next_command_context->previous = context;
    }
    return Error::none;
}

void Device::destroy_command_context(CommandBuffer& context) noexcept
{
    if (device && context.command_pool) vkDestroyCommandPool(device, context.command_pool, nullptr);
    context = {};
}

void Device::destroy_command_contexts() noexcept
{
    if (next_command_context) next_command_context->previous->next = nullptr;
    while (next_command_context)
    {
        CommandBuffer* context = next_command_context;
        next_command_context = context->next;
        destroy_command_context(*context);
        delete context;
    }
    free(command_submit_infos);
    command_submit_infos = nullptr;
    command_submit_capacity = 0;
    command_context_count = 0;
    if (device && command_retirement) vkDestroySemaphore(device, command_retirement, nullptr);
    command_retirement = VK_NULL_HANDLE;
    command_retirement_value = 0;
    completed_command_retirement = 0;
}

void Device::reset_command_context(CommandBuffer& context) noexcept
{
    assert(context.command_pool && context.command_buffer &&
           !context.state &&
           context.retire_value <= completed_command_retirement);
    assert_vk(vkResetCommandPool(device, context.command_pool, 0));
    context.retire_value = 0;
}

void Device::reset_retired_command_contexts() noexcept
{
    if (!next_command_context)
        return;
    CommandBuffer* context = next_command_context;
    do
    {
        if (context->retire_value != 0 && context->retire_value <= completed_command_retirement)
            reset_command_context(*context);
        context = context->next;
    } while (context != next_command_context);
}

CommandBuffer& Device::acquire_command_context() noexcept
{
    CommandBuffer* context = next_command_context;
    assert(context);
    if (active_command_buffers == 0 && context->retire_value > completed_command_retirement)
        poll_command_retirement();
    if (context->state || context->retire_value > completed_command_retirement)
    {
        require_error(grow_command_context_pool());
        context = next_command_context->previous;
        assert(context != next_command_context &&
               context->next == next_command_context &&
               context->retire_value == 0);
    }
    if (context->retire_value != 0) reset_command_context(*context);
    next_command_context = context->next;
    return *context;
}

namespace
{

uint64 query_timeline_value(const TimelineSemaphore& semaphore) noexcept
{
    uint64 value = 0;
    assert_vk(vkGetSemaphoreCounterValue(semaphore.state->device, semaphore.semaphore, &value));
    return value;
}

} // namespace

void Device::poll_command_retirement() noexcept
{
    if (!command_retirement || completed_command_retirement == command_retirement_value)
        return;
    uint64 completed = 0;
    assert_vk(vkGetSemaphoreCounterValue(device, command_retirement, &completed));
    assert(completed >= completed_command_retirement && completed <= command_retirement_value);
    if (completed == completed_command_retirement)
        return;
    completed_command_retirement = completed;
    reset_retired_command_contexts();
    swapchain_delete_queue.collect(device, completed_command_retirement);
}

void Device::wait_command_retirement(uint64 value) noexcept
{
    assert(value <= command_retirement_value);
    if (value <= completed_command_retirement)
    {
        reset_retired_command_contexts();
        swapchain_delete_queue.collect(device, completed_command_retirement);
        return;
    }
    const VkSemaphoreWaitInfo wait_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &command_retirement,
        .pValues = &value,
    };
    assert_vk(vkWaitSemaphores(device, &wait_info, ~uint64{0}));
    completed_command_retirement = value;
    reset_retired_command_contexts();
    swapchain_delete_queue.collect(device, completed_command_retirement);
}

uint64 Device::next_command_retirement() noexcept
{
    const uint64 next = command_retirement_value + 1;
    if (next - completed_command_retirement > max_timeline_value_difference)
    {
        poll_command_retirement();
        if (next - completed_command_retirement > max_timeline_value_difference)
            wait_command_retirement(next - max_timeline_value_difference);
        assert(next - completed_command_retirement <= max_timeline_value_difference);
    }
    command_retirement_value = next;
    return next;
}

Error Device::create_present_context(detail::PresentContext& context) noexcept
{
    assert(!context.acquired && !context.rendered && !context.presented && !context.swapchain && !context.present_pending);
    const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    Error error = error_from_vk(vkCreateSemaphore(device, &semaphore_info, nullptr, &context.acquired));
    if (error != Error::none)
        return error;
    error = error_from_vk(vkCreateSemaphore(device, &semaphore_info, nullptr, &context.rendered));
    if (error != Error::none)
    {
        destroy_present_context(context);
        return error;
    }
    const VkFenceCreateInfo fence_info{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    error = error_from_vk(vkCreateFence(device, &fence_info, nullptr, &context.presented));
    if (error != Error::none) destroy_present_context(context);
    return error;
}

void Device::destroy_present_context(detail::PresentContext& context) noexcept
{
    assert(!context.present_pending);
    if (device && context.acquired) vkDestroySemaphore(device, context.acquired, nullptr);
    if (device && context.rendered) vkDestroySemaphore(device, context.rendered, nullptr);
    if (device && context.presented) vkDestroyFence(device, context.presented, nullptr);
    context = {};
}

void Device::finish_present_context(detail::PresentContext& context) noexcept
{
    assert(!context.present_pending && context.swapchain);
    const VkSwapchainKHR completed_swapchain = context.swapchain;
    context.swapchain = VK_NULL_HANDLE;
    for (detail::RetiredSwapchain& retired : retired_swapchains)
    {
        if (retired.handle != completed_swapchain)
            continue;
        for (const detail::PresentContext& pending : present_contexts)
        {
            if (pending.present_pending && pending.swapchain == completed_swapchain)
                return;
        }
        queue_retired_swapchain(retired);
        return;
    }
}

void Device::wait_present_context(detail::PresentContext& context) noexcept
{
    if (!context.present_pending)
        return;
    assert_vk(vkWaitForFences(device, 1, &context.presented, VK_TRUE, ~uint64{0}));
    context.present_pending = false;
    finish_present_context(context);
}

void Device::poll_present_contexts() noexcept
{
    for (uint32 index = 0; index < present_context_count; ++index)
    {
        detail::PresentContext& context = present_contexts[index];
        if (!context.present_pending)
            continue;
        const VkResult result = vkGetFenceStatus(device, context.presented);
        if (result == VK_NOT_READY)
            continue;
        assert_vk(result);
        context.present_pending = false;
        finish_present_context(context);
    }
}

void Device::queue_retired_swapchain(detail::RetiredSwapchain& retired) noexcept
{
    assert(retired.handle && retired.view_count != 0);
    assert(active_command_buffers == 0 && "swapchain retirement is not allowed while a command buffer is recording");
    for (uint32 index = 0; index < retired.view_count; ++index)
    {
        const VkImageView view = retired.views[index];
        assert(view);
        swapchain_delete_queue.push(command_retirement_value, retired.handle, view);
    }
    retired = {};
    swapchain_delete_queue.collect(device, completed_command_retirement);
}

void Device::drain_contexts() noexcept
{
    wait_command_retirement(command_retirement_value);
    for (uint32 index = 0; index < present_context_count; ++index)
    {
        wait_present_context(present_contexts[index]);
    }
    for (const detail::RetiredSwapchain& retired : retired_swapchains)
    {
        assert(!retired.handle);
    }
    swapchain_delete_queue.collect(device, completed_command_retirement);
}

GpuHeap Device::allocate_gpu_heap(VkDeviceSize size, MemoryType memory) noexcept
{
    if (memory == MemoryType::texture_descriptor_heap || memory == MemoryType::sampler_descriptor_heap)
        return allocate_descriptor_heap(size, memory);

    VkMemoryPropertyFlags required = 0;
    VkMemoryPropertyFlags preferred = 0;
    VkMemoryPropertyFlags avoided = 0;
    switch (memory)
    {
    case MemoryType::cpu_visible:
        required = cpu_visible_memory_properties;
        break;
    case MemoryType::gpu_only:
        required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        avoided = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        break;
    case MemoryType::readback:
        required = cpu_visible_memory_properties;
        preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
    default:
        assert(false && "create_gpu_heap received an invalid memory type");
        return {};
    }

    GpuHeapOwner* heap = new GpuHeapOwner{.state = this};
    create_backing_buffer(heap->backing, size, universal_buffer_usage, required, preferred, avoided);
    return {
        .range = {
            .cpu = static_cast<byte*>(heap->backing.mapped),
            .gpu = reinterpret_cast<byte*>(static_cast<uintptr>(heap->backing.address)),
            .size = size,
        },
        .owner = heap,
    };
}

GpuHeap Device::allocate_descriptor_heap(VkDeviceSize size, MemoryType memory) noexcept
{
    assert(memory == MemoryType::texture_descriptor_heap || memory == MemoryType::sampler_descriptor_heap);
    const bool texture_heap = memory == MemoryType::texture_descriptor_heap;
    const VkDeviceSize resource_alignment = heap_properties.imageDescriptorAlignment > heap_properties.bufferDescriptorAlignment
                                                ? heap_properties.imageDescriptorAlignment
                                                : heap_properties.bufferDescriptorAlignment;
    const VkDeviceSize reserved_alignment = texture_heap ? resource_alignment : heap_properties.samplerDescriptorAlignment;
    const VkDeviceSize heap_alignment = texture_heap ? heap_properties.resourceHeapAlignment : heap_properties.samplerHeapAlignment;
    const VkDeviceSize reserved_size = texture_heap ? heap_properties.minResourceHeapReservedRange : heap_properties.minSamplerHeapReservedRange;

    const VkDeviceSize reserved_offset = align_up(size, reserved_alignment);
    const VkDeviceSize bind_size = reserved_offset + reserved_size;
    const VkDeviceSize allocation_alignment = heap_alignment > gpu_allocation_alignment ? heap_alignment : gpu_allocation_alignment;
    const VkDeviceSize alignment_padding = allocation_alignment - 1;
    const VkDeviceSize backing_size = bind_size + alignment_padding;

    GpuHeapOwner* heap = new GpuHeapOwner{.state = this};
    create_backing_buffer(heap->backing, backing_size, universal_buffer_usage | VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT, cpu_visible_memory_properties, 0);

    const VkDeviceAddress gpu_address = align_up(heap->backing.address, allocation_alignment);
    const VkDeviceSize allocation_offset = gpu_address - heap->backing.address;
    return {
        .range = {
            .cpu = static_cast<byte*>(heap->backing.mapped) + allocation_offset,
            .gpu = reinterpret_cast<byte*>(static_cast<uintptr>(gpu_address)),
            .size = size,
        },
        .owner = heap,
    };
}

struct Texture
{
    Device* state = nullptr;
    VkImage image = VK_NULL_HANDLE;
    uint32 width = 0;
    uint32 height = 0;
    uint32 depth = 0;
    uint32 layer_count = 0;
    TextureType type = TextureType::two_d;
    Format format = Format::rgba8_unorm;
    detail::TextureInitialization initialization;

    ~Texture()
    {
        if (initialization.owner) detail::remove_texture_initialization(initialization);
        vkDestroyImage(state->device, image, nullptr);
    }
};

struct RenderView
{
    Device* state = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    uint32 width = 0;
    uint32 height = 0;
    bool swapchain_view = false;
};

struct Swapchain
{
    Device* state = nullptr;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkImage images[max_swapchain_images]{};
    RenderView render_views[max_swapchain_images]{};
    bool initialized[max_swapchain_images]{};
    uint32 image_count = 0;
    uint32 image_index = 0;
    uint32 width = 0;
    uint32 height = 0;
    Format format = Format::bgra8_srgb;
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    detail::PresentContext* present_context = nullptr;
    CommandBuffer* transition_commands = nullptr;
    bool acquired = false;
    bool recreate_required = false;

    Swapchain() = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
};

struct PSO
{
    Device* state = nullptr;
    VkPipeline pso = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;

    ~PSO()
    {
        if (state && pso) vkDestroyPipeline(state->device, pso, nullptr);
    }
};

namespace
{

void record_image_barriers(VkCommandBuffer command_buffer, Span<const VkImageMemoryBarrier2> barriers) noexcept
{
    assert((command_buffer && barriers.data && barriers.size != 0 && barriers.size <= UINT_MAX) && "image barrier batch is invalid");
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<uint32>(barriers.size),
        .pImageMemoryBarriers = barriers.data,
    };
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

void make_heap_bind_info(GpuRange heap, VkDeviceSize reserved_alignment, VkDeviceSize reserved_size, VkBindHeapInfoEXT& output) noexcept
{
    const VkDeviceSize reserved_offset = align_up<VkDeviceSize>(heap.size, reserved_alignment);
    output = {
        .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .heapRange = {
            .address = static_cast<VkDeviceAddress>(reinterpret_cast<uintptr>(heap.gpu)),
            .size = reserved_offset + reserved_size,
        },
        .reservedRangeOffset = reserved_offset,
        .reservedRangeSize = reserved_size,
    };
}

Error enumerate_device_extensions(VkPhysicalDevice physical_device, Span<VkExtensionProperties> values, uint32& count) noexcept
{
    count = static_cast<uint32>(values.size);
    const VkResult result = vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, values.data);
    return result == VK_INCOMPLETE ? Error::unsupported : error_from_vk(result);
}

Error enumerate_instance_extensions(Span<VkExtensionProperties> values, uint32& count) noexcept
{
    count = static_cast<uint32>(values.size);
    const VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &count, values.data);
    return result == VK_INCOMPLETE ? Error::unsupported : error_from_vk(result);
}

#if !defined(NDEBUG)
Error enumerate_instance_layers(Span<VkLayerProperties> values, uint32& count) noexcept
{
    count = static_cast<uint32>(values.size);
    const VkResult result = vkEnumerateInstanceLayerProperties(&count, values.data);
    return result == VK_INCOMPLETE ? Error::unsupported : error_from_vk(result);
}
#endif

struct QueriedFeatures
{
    VkPhysicalDeviceFeatures2 core{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features vulkan11{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features vulkan12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features vulkan13{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features vulkan14{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptor_heap{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR address_commands{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR};
    VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untyped_pointers{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR};
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unified_image_layouts{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchain_maintenance1{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR};

    explicit QueriedFeatures(bool presentation, bool include_unified_image_layouts)
    {
        core.pNext = &vulkan11;
        vulkan11.pNext = &vulkan12;
        vulkan12.pNext = &vulkan13;
        vulkan13.pNext = &vulkan14;
        vulkan14.pNext = &descriptor_heap;
        descriptor_heap.pNext = &address_commands;
        address_commands.pNext = &untyped_pointers;
        untyped_pointers.pNext = include_unified_image_layouts ? static_cast<void*>(&unified_image_layouts) : static_cast<void*>(&mesh_shader);
        unified_image_layouts.pNext = &mesh_shader;
        mesh_shader.pNext = presentation ? &swapchain_maintenance1 : nullptr;
    }
};

struct Candidate
{
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    uint32 queue_family = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    bool unified_image_layouts = false;
    bool image_cube_array = false;
    bool texture_compression_bc = false;
    bool texture_compression_astc = false;
    bool texture_compression_etc2 = false;
    bool storage_input_output16 = false;
    bool khr_swapchain_maintenance1 = false;
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heap_properties{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    VkPhysicalDeviceVulkan12Properties vulkan12_properties{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
};

Error inspect_candidate(VkPhysicalDevice physical_device, VkSurfaceKHR surface, bool khr_surface_maintenance1, bool ext_surface_maintenance1,
                        Candidate& output) noexcept
{
    VkExtensionProperties extensions[max_device_extensions]{};
    uint32 extension_count = 0;
    const Error extension_error = enumerate_device_extensions(physical_device, {extensions, max_device_extensions}, extension_count);
    if (extension_error != Error::none)
        return extension_error;
    const bool unified_image_layouts_extension = has_name(
        {extensions, extension_count},
        VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME);
    constexpr const char* required_extensions[]{
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME,
        VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
    };
    for (const char* name : required_extensions)
    {
        if (!has_name({extensions, extension_count}, name))
            return Error::unsupported;
    }
    const bool khr_swapchain_maintenance1 = khr_surface_maintenance1 &&
                                            has_name({extensions, extension_count}, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    const bool ext_swapchain_maintenance1 = ext_surface_maintenance1 &&
                                            has_name({extensions, extension_count}, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    if (surface &&
        (!has_name({extensions, extension_count}, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
         (!khr_swapchain_maintenance1 && !ext_swapchain_maintenance1)))
    {
        return Error::unsupported;
    }

    Candidate result{
        .physical_device = physical_device,
    };
    result.heap_properties.pNext = &result.vulkan12_properties;
    VkPhysicalDeviceProperties2 properties2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &result.heap_properties,
    };
    vkGetPhysicalDeviceProperties2(physical_device, &properties2);
    result.properties = properties2.properties;
    result.heap_properties.pNext = nullptr;
    result.vulkan12_properties.pNext = nullptr;
    if (result.properties.apiVersion < VK_API_VERSION_1_4)
        return Error::unsupported;

    vkGetPhysicalDeviceMemoryProperties(physical_device, &result.memory_properties);
    bool cpu_visible_memory = false;
    for (uint32 index = 0; index < result.memory_properties.memoryTypeCount; ++index)
    {
        if (is_usable_memory_type(result.memory_properties, index) &&
            (result.memory_properties.memoryTypes[index].propertyFlags & cpu_visible_memory_properties) == cpu_visible_memory_properties)
        {
            cpu_visible_memory = true;
            break;
        }
    }
    if (!cpu_visible_memory) return Error::unsupported;

    QueriedFeatures features(surface != VK_NULL_HANDLE, unified_image_layouts_extension);
    vkGetPhysicalDeviceFeatures2(physical_device, &features.core);
    const bool required_features =
        features.core.features.shaderInt16 == VK_TRUE &&
        features.core.features.samplerAnisotropy == VK_TRUE &&
        features.core.features.depthBiasClamp == VK_TRUE &&
        features.core.features.independentBlend == VK_TRUE &&
        features.core.features.fragmentStoresAndAtomics == VK_TRUE &&
        features.core.features.vertexPipelineStoresAndAtomics == VK_TRUE &&
        features.core.features.shaderStorageImageReadWithoutFormat == VK_TRUE &&
        features.core.features.shaderStorageImageWriteWithoutFormat == VK_TRUE &&
        features.core.features.multiDrawIndirect == VK_TRUE &&
        features.core.features.drawIndirectFirstInstance == VK_TRUE &&
        features.vulkan11.storageBuffer16BitAccess == VK_TRUE &&
        features.vulkan11.storagePushConstant16 == VK_TRUE &&
        features.vulkan11.shaderDrawParameters == VK_TRUE &&
        features.vulkan12.shaderFloat16 == VK_TRUE &&
        features.vulkan12.scalarBlockLayout == VK_TRUE &&
        features.vulkan12.bufferDeviceAddress == VK_TRUE &&
        features.vulkan12.timelineSemaphore == VK_TRUE &&
        features.vulkan13.synchronization2 == VK_TRUE &&
        features.vulkan13.dynamicRendering == VK_TRUE &&
        features.vulkan13.maintenance4 == VK_TRUE &&
        features.vulkan14.maintenance5 == VK_TRUE &&
        features.descriptor_heap.descriptorHeap == VK_TRUE &&
        features.address_commands.deviceAddressCommands == VK_TRUE &&
        features.untyped_pointers.shaderUntypedPointers == VK_TRUE &&
        features.mesh_shader.meshShader == VK_TRUE &&
        (features.core.features.textureCompressionBC == VK_TRUE ||
         features.core.features.textureCompressionASTC_LDR == VK_TRUE) &&
        (!surface || features.swapchain_maintenance1.swapchainMaintenance1 == VK_TRUE);
    if (!required_features)
        return Error::unsupported;
    result.unified_image_layouts = unified_image_layouts_extension && features.unified_image_layouts.unifiedImageLayouts == VK_TRUE;
    result.image_cube_array = features.core.features.imageCubeArray == VK_TRUE;
    result.texture_compression_bc = features.core.features.textureCompressionBC == VK_TRUE;
    result.texture_compression_astc = features.core.features.textureCompressionASTC_LDR == VK_TRUE;
    result.texture_compression_etc2 = features.core.features.textureCompressionETC2 == VK_TRUE;
    result.storage_input_output16 = features.vulkan11.storageInputOutput16 == VK_TRUE;
    result.khr_swapchain_maintenance1 = khr_swapchain_maintenance1;

    uint32 available_queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &available_queue_count, nullptr);
    if (available_queue_count > max_queue_families)
        return Error::unsupported;
    VkQueueFamilyProperties queues[max_queue_families]{};
    uint32 queue_count = available_queue_count;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, queues);
    uint32 queue_family = queue_count;
    constexpr VkQueueFlags required_queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (uint32 index = 0; index < queue_count; ++index)
    {
        if (queues[index].queueCount == 0 || (queues[index].queueFlags & required_queue_flags) != required_queue_flags)
            continue;
        VkBool32 presentation_supported = VK_TRUE;
        if (surface)
        {
            const Error presentation_error = error_from_vk(vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, index, surface, &presentation_supported));
            if (presentation_error != Error::none)
                return presentation_error;
        }
        if (presentation_supported == VK_TRUE)
        {
            queue_family = index;
            break;
        }
    }
    if (queue_family == queue_count)
        return Error::unsupported;

    result.queue_family = queue_family;
    output = result;
    return Error::none;
}

Error enumerate_physical_devices(VkInstance instance, Span<VkPhysicalDevice> values, uint32& count) noexcept
{
    count = static_cast<uint32>(values.size);
    const VkResult result = vkEnumeratePhysicalDevices(instance, &count, values.data);
    return result == VK_INCOMPLETE ? Error::unsupported : error_from_vk(result);
}

DeviceInit fail_device_creation(Device* device, Error error) noexcept
{
    delete device;
    return {
        .error = error,
    };
}

Error recreate_swapchain(Swapchain& swapchain) noexcept;

} // namespace

DeviceInit create_device(const DeviceDesc& desc) noexcept
{
    const bool presentation = desc.window != nullptr;
    assert(desc.desired_swapchain_image_count != 0 && desc.desired_swapchain_image_count <= max_swapchain_images &&
           "swapchain image count must fit the wrapper's presentation context array");
#if !defined(_WIN32)
    if (presentation)
        return {.error = Error::unsupported};
#endif

    uint32 loader_version = VK_API_VERSION_1_0;
    Error error = error_from_vk(vkEnumerateInstanceVersion(&loader_version));
    if (error != Error::none)
        return { .error = error };
    if (loader_version < VK_API_VERSION_1_4)
        return { .error = Error::unsupported };

    Device* state = new Device;
    state->present_context_count = presentation ? desc.desired_swapchain_image_count : 0;
    VkExtensionProperties instance_extensions[max_instance_extensions]{};
    uint32 instance_extension_count = 0;
    error = enumerate_instance_extensions({instance_extensions, max_instance_extensions}, instance_extension_count);
    if (error != Error::none)
        return fail_device_creation(state, error);
#if defined(_WIN32)
    const bool khr_surface_maintenance1 = presentation && has_name(
        {instance_extensions, instance_extension_count},
        VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    const bool ext_surface_maintenance1 = presentation && has_name(
        {instance_extensions, instance_extension_count},
        VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    if (presentation &&
        (!has_name({instance_extensions, instance_extension_count},
                   VK_KHR_SURFACE_EXTENSION_NAME) ||
         !has_name({instance_extensions, instance_extension_count},
                   VK_KHR_WIN32_SURFACE_EXTENSION_NAME) ||
         !has_name({instance_extensions, instance_extension_count},
                   VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) ||
         (!khr_surface_maintenance1 && !ext_surface_maintenance1)))
    {
        return fail_device_creation(state, Error::unsupported);
    }
#else
    constexpr bool khr_surface_maintenance1 = false;
    constexpr bool ext_surface_maintenance1 = false;
#endif
#if !defined(NDEBUG)
    VkLayerProperties layers[max_instance_layers]{};
    uint32 layer_count = 0;
    error = enumerate_instance_layers({layers, max_instance_layers}, layer_count);
    if (error != Error::none)
        return fail_device_creation(state, error);
    const bool debug_utils_available = has_name({instance_extensions, instance_extension_count}, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const bool validation_available = has_name({layers, layer_count}, "VK_LAYER_KHRONOS_validation");
#endif

    const char* enabled_instance_extensions[6]{};
    uint32 enabled_instance_extension_count = 0;
    const char* enabled_layers[1]{};
    uint32 enabled_layer_count = 0;
#if !defined(NDEBUG)
    if (debug_utils_available) enabled_instance_extensions[enabled_instance_extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (validation_available) enabled_layers[enabled_layer_count++] = "VK_LAYER_KHRONOS_validation";
#endif
#if defined(_WIN32)
    if (presentation)
    {
        enabled_instance_extensions[enabled_instance_extension_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
        enabled_instance_extensions[enabled_instance_extension_count++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
        enabled_instance_extensions[enabled_instance_extension_count++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
        if (khr_surface_maintenance1)
            enabled_instance_extensions[enabled_instance_extension_count++] = VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
        if (ext_surface_maintenance1)
            enabled_instance_extensions[enabled_instance_extension_count++] = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
    }
#endif

    const VkApplicationInfo app_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "NoGraphicsAPI application",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = "NoGraphicsAPI",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion = VK_API_VERSION_1_4,
    };
    const VkInstanceCreateInfo instance_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = enabled_layer_count,
        .ppEnabledLayerNames = enabled_layer_count ? enabled_layers : nullptr,
        .enabledExtensionCount = enabled_instance_extension_count,
        .ppEnabledExtensionNames = enabled_instance_extension_count ? enabled_instance_extensions : nullptr,
    };
    error = error_from_vk(vkCreateInstance(&instance_info, nullptr, &state->instance));
    if (error != Error::none)
        return fail_device_creation(state, error);

#if !defined(NDEBUG)
    if (debug_utils_available)
    {
        const PFN_vkCreateDebugUtilsMessengerEXT create_debug =
            load_instance_proc<PFN_vkCreateDebugUtilsMessengerEXT>(state->instance, "vkCreateDebugUtilsMessengerEXT");
        state->destroy_debug_messenger = load_instance_proc<PFN_vkDestroyDebugUtilsMessengerEXT>(state->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (!create_debug || !state->destroy_debug_messenger)
            return fail_device_creation(state, Error::driver_error);
        const VkDebugUtilsMessengerCreateInfoEXT debug_info{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_callback,
        };
        error = error_from_vk(create_debug(state->instance, &debug_info, nullptr, &state->debug_messenger));
        if (error != Error::none)
            return fail_device_creation(state, error);
    }
#endif

#if defined(_WIN32)
    if (presentation)
    {
        const VkWin32SurfaceCreateInfoKHR surface_info{
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .hinstance = GetModuleHandleW(nullptr),
            .hwnd = static_cast<HWND>(desc.window),
        };
        error = error_from_vk(vkCreateWin32SurfaceKHR(state->instance, &surface_info, nullptr, &state->surface));
        if (error != Error::none)
            return fail_device_creation(state, error);
    }
#endif

    VkPhysicalDevice physical_devices[max_physical_devices]{};
    uint32 physical_device_count = 0;
    error = enumerate_physical_devices(state->instance, {physical_devices, max_physical_devices}, physical_device_count);
    if (error != Error::none)
        return fail_device_creation(state, error);

    Candidate selected{};
    bool has_selected = false;
    for (uint32 index = 0; index < physical_device_count; ++index)
    {
        const VkPhysicalDevice physical_device = physical_devices[index];
        Candidate candidate{};
        error = inspect_candidate(physical_device, state->surface, khr_surface_maintenance1, ext_surface_maintenance1, candidate);
        if (error == Error::unsupported)
            continue;
        if (error != Error::none)
            return fail_device_creation(state, error);
        if (!has_selected || candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            selected = candidate;
            has_selected = true;
        }
        if (candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            break;
    }
    if (!has_selected)
        return fail_device_creation(state, Error::unsupported);

    state->physical_device = selected.physical_device;
    state->queue_family = selected.queue_family;
    state->physical_properties = selected.properties;
    state->heap_properties = selected.heap_properties;
    state->max_timeline_value_difference = selected.vulkan12_properties.maxTimelineSemaphoreValueDifference;
    state->heap_properties.pNext = nullptr;
    state->memory_properties = selected.memory_properties;
    for (uint32 value = 0; value < format_count; ++value)
    {
        state->format_features[value] = optimal_format_features(
            state->physical_device, static_cast<Format>(value));
    }

    QueriedFeatures enabled_features(presentation, selected.unified_image_layouts);
    state->texture_compression_etc2 = selected.texture_compression_etc2;
    enabled_features.core.features.imageCubeArray = selected.image_cube_array;
    enabled_features.core.features.samplerAnisotropy = VK_TRUE;
    enabled_features.core.features.shaderInt16 = VK_TRUE;
    enabled_features.core.features.depthBiasClamp = VK_TRUE;
    enabled_features.core.features.independentBlend = VK_TRUE;
    enabled_features.core.features.textureCompressionBC = selected.texture_compression_bc;
    enabled_features.core.features.textureCompressionASTC_LDR = selected.texture_compression_astc;
    enabled_features.core.features.textureCompressionETC2 = selected.texture_compression_etc2;
    enabled_features.core.features.fragmentStoresAndAtomics = VK_TRUE;
    enabled_features.core.features.vertexPipelineStoresAndAtomics = VK_TRUE;
    enabled_features.core.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    enabled_features.core.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enabled_features.core.features.multiDrawIndirect = VK_TRUE;
    enabled_features.core.features.drawIndirectFirstInstance = VK_TRUE;
    enabled_features.vulkan11.storageBuffer16BitAccess = VK_TRUE;
    enabled_features.vulkan11.storagePushConstant16 = VK_TRUE;
    enabled_features.vulkan11.storageInputOutput16 = selected.storage_input_output16;
    enabled_features.vulkan11.shaderDrawParameters = VK_TRUE;
    enabled_features.vulkan12.shaderFloat16 = VK_TRUE;
    enabled_features.vulkan12.scalarBlockLayout = VK_TRUE;
    enabled_features.vulkan12.timelineSemaphore = VK_TRUE;
    enabled_features.vulkan12.bufferDeviceAddress = VK_TRUE;
    enabled_features.vulkan13.synchronization2 = VK_TRUE;
    enabled_features.vulkan13.dynamicRendering = VK_TRUE;
    enabled_features.vulkan13.maintenance4 = VK_TRUE;
    enabled_features.vulkan14.maintenance5 = VK_TRUE;
    enabled_features.descriptor_heap.descriptorHeap = VK_TRUE;
    enabled_features.address_commands.deviceAddressCommands = VK_TRUE;
    enabled_features.untyped_pointers.shaderUntypedPointers = VK_TRUE;
    enabled_features.unified_image_layouts.unifiedImageLayouts = selected.unified_image_layouts ? VK_TRUE : VK_FALSE;
    enabled_features.mesh_shader.meshShader = VK_TRUE;
    enabled_features.swapchain_maintenance1.swapchainMaintenance1 = VK_TRUE;

    constexpr float queue_priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = state->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    const char* enabled_device_extensions[7]{};
    uint32 enabled_device_extension_count = 0;
    enabled_device_extensions[enabled_device_extension_count++] = VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME;
    enabled_device_extensions[enabled_device_extension_count++] = VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME;
    enabled_device_extensions[enabled_device_extension_count++] = VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME;
    if (selected.unified_image_layouts)
    {
        enabled_device_extensions[enabled_device_extension_count++] = VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME;
    }
    enabled_device_extensions[enabled_device_extension_count++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
#if defined(_WIN32)
    if (presentation)
    {
        enabled_device_extensions[enabled_device_extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        enabled_device_extensions[enabled_device_extension_count++] = selected.khr_swapchain_maintenance1
            ? VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
            : VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
    }
#endif
    const VkDeviceCreateInfo device_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabled_features.core,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = enabled_device_extension_count,
        .ppEnabledExtensionNames = enabled_device_extensions,
    };
    error = error_from_vk(vkCreateDevice(state->physical_device, &device_info, nullptr, &state->device));
    if (error != Error::none)
        return fail_device_creation(state, error);
    vkGetDeviceQueue(state->device, state->queue_family, 0, &state->queue);
    if (!supports_gpu_heap_memory(*state) || !select_texture_memory_type(*state))
        return fail_device_creation(state, Error::unsupported);

    state->fn.write_sampler_descriptors = load_device_proc<PFN_vkWriteSamplerDescriptorsEXT>(state->device, "vkWriteSamplerDescriptorsEXT");
    state->fn.write_resource_descriptors = load_device_proc<PFN_vkWriteResourceDescriptorsEXT>(state->device, "vkWriteResourceDescriptorsEXT");
    state->fn.cmd_bind_sampler_heap = load_device_proc<PFN_vkCmdBindSamplerHeapEXT>(state->device, "vkCmdBindSamplerHeapEXT");
    state->fn.cmd_bind_texture_heap = load_device_proc<PFN_vkCmdBindResourceHeapEXT>(state->device, "vkCmdBindResourceHeapEXT");
    state->fn.cmd_push_data = load_device_proc<PFN_vkCmdPushDataEXT>(state->device, "vkCmdPushDataEXT");
    state->fn.cmd_bind_index_buffer = load_device_proc<PFN_vkCmdBindIndexBuffer3KHR>(state->device, "vkCmdBindIndexBuffer3KHR");
    state->fn.cmd_draw_indirect = load_device_proc<PFN_vkCmdDrawIndirect2KHR>(state->device, "vkCmdDrawIndirect2KHR");
    state->fn.cmd_draw_indexed_indirect = load_device_proc<PFN_vkCmdDrawIndexedIndirect2KHR>(state->device, "vkCmdDrawIndexedIndirect2KHR");
    state->fn.cmd_dispatch_indirect = load_device_proc<PFN_vkCmdDispatchIndirect2KHR>(state->device, "vkCmdDispatchIndirect2KHR");
    state->fn.cmd_draw_mesh_tasks = load_device_proc<PFN_vkCmdDrawMeshTasksEXT>(state->device, "vkCmdDrawMeshTasksEXT");
    state->fn.cmd_draw_mesh_tasks_indirect = load_device_proc<PFN_vkCmdDrawMeshTasksIndirect2EXT>(state->device, "vkCmdDrawMeshTasksIndirect2EXT");
    state->fn.cmd_copy_memory = load_device_proc<PFN_vkCmdCopyMemoryKHR>(state->device, "vkCmdCopyMemoryKHR");
    state->fn.cmd_copy_memory_to_image = load_device_proc<PFN_vkCmdCopyMemoryToImageKHR>(state->device, "vkCmdCopyMemoryToImageKHR");
    state->fn.cmd_copy_image_to_memory = load_device_proc<PFN_vkCmdCopyImageToMemoryKHR>(state->device, "vkCmdCopyImageToMemoryKHR");
    if (!state->fn.write_sampler_descriptors || !state->fn.write_resource_descriptors ||
        !state->fn.cmd_bind_sampler_heap || !state->fn.cmd_bind_texture_heap ||
        !state->fn.cmd_push_data || !state->fn.cmd_bind_index_buffer ||
        !state->fn.cmd_draw_indirect || !state->fn.cmd_draw_indexed_indirect ||
        !state->fn.cmd_dispatch_indirect || !state->fn.cmd_draw_mesh_tasks ||
        !state->fn.cmd_draw_mesh_tasks_indirect || !state->fn.cmd_copy_memory ||
        !state->fn.cmd_copy_memory_to_image || !state->fn.cmd_copy_image_to_memory)
    {
        return fail_device_creation(state, Error::driver_error);
    }

    error = state->create_command_contexts();
    if (error != Error::none)
        return fail_device_creation(state, error);
    for (uint32 index = 0; index < state->present_context_count; ++index)
    {
        error = state->create_present_context(state->present_contexts[index]);
        if (error != Error::none)
            return fail_device_creation(state, error);
    }

    state->caps = {
        .device_name = state->physical_properties.deviceName,
        .max_push_data_size = state->heap_properties.maxPushDataSize,
        .texture_heap_alignment = state->texture_heap_alignment,
        .texture_descriptor_size = state->heap_properties.imageDescriptorSize,
        .sampler_descriptor_size = state->heap_properties.samplerDescriptorSize,
        .texture_compression_bc = selected.texture_compression_bc,
        .texture_compression_astc = selected.texture_compression_astc,
        .storage_input_output16 = selected.storage_input_output16,
    };
    if (presentation)
    {
        state->swapchain = new Swapchain;
        state->swapchain->state = state;
        state->swapchain->format = desc.swapchain_format;
        error = recreate_swapchain(*state->swapchain);
        if (error != Error::none)
            return fail_device_creation(state, error);
    }
    return {
        .device = state,
    };
}

void destroy_device(Device* device) noexcept
{
    delete device;
}

TimelineSemaphore* create_timeline_semaphore(Device* device, uint64 initial_value) noexcept
{
    assert(device && "create_timeline_semaphore called with a null device");

    const VkSemaphoreTypeCreateInfo timeline_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = initial_value,
    };
    const VkSemaphoreCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timeline_info,
    };
    TimelineSemaphore* result = new TimelineSemaphore{
        .state = device,
    };
    require_vk(vkCreateSemaphore(device->device, &create_info, nullptr, &result->semaphore));
    return result;
}

void destroy_timeline_semaphore(TimelineSemaphore* semaphore) noexcept
{
    if (!semaphore) return;
    vkDestroySemaphore(semaphore->state->device, semaphore->semaphore, nullptr);
    delete semaphore;
}

uint64 timeline_completed_value(const TimelineSemaphore* semaphore) noexcept
{
    assert((semaphore && semaphore->state && semaphore->semaphore) && "timeline_completed_value received an invalid semaphore");
    return query_timeline_value(*semaphore);
}

void wait_timeline(TimelinePoint point) noexcept
{
    TimelineSemaphore* semaphore = point.semaphore;
    assert((semaphore && semaphore->state && semaphore->semaphore) && "wait_timeline requires a live timeline semaphore");

    const VkSemaphore handle = semaphore->semaphore;
    const VkSemaphoreWaitInfo wait_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &handle,
        .pValues = &point.value,
    };
    assert_vk(vkWaitSemaphores(
        semaphore->state->device,
        &wait_info,
        ~uint64{0}));
    semaphore->state->poll_command_retirement();
}

GpuHeap create_gpu_heap(Device* device, uint64 byte_count, MemoryType memory) noexcept
{
    assert(device);
    return device->allocate_gpu_heap(byte_count, memory);
}

void destroy_gpu_heap(const GpuHeap& heap) noexcept
{
    if (!heap.owner) return;
    assert(heap.owner->state);
    const VkDevice device = heap.owner->state->device;
    const detail::BackingBuffer& backing = heap.owner->backing;
    if (backing.mapped) vkUnmapMemory(device, backing.memory);
    vkDestroyBuffer(device, backing.buffer, nullptr);
    vkFreeMemory(device, backing.memory, nullptr);
    delete heap.owner;
}

namespace
{

void retire_swapchain_handle(Swapchain& swapchain) noexcept
{
    Device* device = swapchain.state;
    if (!swapchain.handle)
    {
        assert(swapchain.image_count == 0);
        swapchain.width = 0;
        swapchain.height = 0;
        return;
    }

    device->poll_present_contexts();
    detail::RetiredSwapchain retired{
        .handle = swapchain.handle,
    };
    for (uint32 index = 0; index < swapchain.image_count; ++index)
    {
        RenderView& render_view = swapchain.render_views[index];
        retired.views[retired.view_count++] = render_view.view;
        render_view = {};
        swapchain.images[index] = VK_NULL_HANDLE;
        swapchain.initialized[index] = false;
    }
    swapchain.image_count = 0;
    swapchain.handle = VK_NULL_HANDLE;
    swapchain.width = 0;
    swapchain.height = 0;

    bool present_pending = false;
    for (uint32 index = 0; index < device->present_context_count; ++index)
    {
        const detail::PresentContext& context = device->present_contexts[index];
        if (context.present_pending && context.swapchain == retired.handle)
        {
            present_pending = true;
            break;
        }
    }
    if (!present_pending)
    {
        device->queue_retired_swapchain(retired);
        return;
    }
    for (detail::RetiredSwapchain& slot : device->retired_swapchains)
    {
        if (!slot.handle)
        {
            slot = retired;
            return;
        }
    }
    for (uint32 index = 0; index < device->present_context_count; ++index)
    {
        detail::PresentContext& context = device->present_contexts[index];
        if (context.present_pending && context.swapchain == retired.handle)
            device->wait_present_context(context);
    }
    device->queue_retired_swapchain(retired);
}

void destroy_owned_swapchain(Device& device) noexcept
{
    if (!device.swapchain)
        return;
    Swapchain* swapchain = device.swapchain;
    assert((swapchain->state == &device && !swapchain->acquired &&
                       device.acquired_swapchain != swapchain &&
                       device.active_command_buffers == 0) && "device-owned swapchain destroyed while its image or command buffer is active");
    retire_swapchain_handle(*swapchain);
    swapchain->state = nullptr;
    delete swapchain;
    device.swapchain = nullptr;
}

VkCompositeAlphaFlagBitsKHR choose_composite_alpha(VkCompositeAlphaFlagsKHR supported) noexcept
{
    constexpr VkCompositeAlphaFlagBitsKHR choices[]{
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (VkCompositeAlphaFlagBitsKHR choice : choices)
    {
        if ((supported & choice) != 0)
            return choice;
    }
    assert(false && "surface exposes no composite alpha mode");
    abort();
}

[[nodiscard]] bool swapchain_surface_configuration_changed(const Swapchain& swapchain) noexcept
{
    VkSurfaceCapabilitiesKHR capabilities{};
    const Error error = error_from_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(swapchain.state->physical_device, swapchain.state->surface, &capabilities));
    require_error(error);

    const VkExtent2D extent = capabilities.currentExtent;
    const uint32 variable_extent = UINT_MAX;
    return extent.width == variable_extent ||
           extent.height == variable_extent ||
           extent.width != swapchain.width ||
           extent.height != swapchain.height ||
           capabilities.currentTransform != swapchain.transform ||
           choose_composite_alpha(capabilities.supportedCompositeAlpha) != swapchain.composite_alpha;
}

Error recreate_swapchain(Swapchain& swapchain) noexcept
{
    Device& device = *swapchain.state;
    assert(!swapchain.acquired && !device.acquired_swapchain && device.active_command_buffers == 0);
    const VkSurfacePresentModeKHR present_mode_info{
        .sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR,
        .presentMode = swapchain_present_mode,
    };
    const VkPhysicalDeviceSurfaceInfo2KHR surface_info{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
        .pNext = &present_mode_info,
        .surface = device.surface,
    };
    VkSurfaceCapabilities2KHR capabilities_info{
        .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
    };
    Error error = error_from_vk(vkGetPhysicalDeviceSurfaceCapabilities2KHR(device.physical_device, &surface_info, &capabilities_info));
    if (error != Error::none)
        return error;
    const VkSurfaceCapabilitiesKHR& capabilities = capabilities_info.surfaceCapabilities;

    const VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT_MAX)
        return Error::unsupported;
    if (extent.width == 0 || extent.height == 0)
    {
        swapchain.width = 0;
        swapchain.height = 0;
        swapchain.recreate_required = true;
        return Error::none;
    }
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
    {
        return Error::unsupported;
    }

    VkSurfaceFormatKHR formats[max_surface_formats]{};
    uint32 surface_format_count = 0;
    error = error_from_vk(vkGetPhysicalDeviceSurfaceFormatsKHR(device.physical_device, device.surface, &surface_format_count, nullptr));
    if (error != Error::none)
        return error;
    if (surface_format_count == 0 || surface_format_count > max_surface_formats)
        return Error::unsupported;
    error = error_from_vk(vkGetPhysicalDeviceSurfaceFormatsKHR(device.physical_device, device.surface, &surface_format_count, formats));
    if (error != Error::none)
        return error;

    const VkFormat requested_format = to_vk(swapchain.format);
    bool format_supported = false;
    for (uint32 index = 0; index < surface_format_count; ++index)
    {
        if ((formats[index].format == requested_format ||
             formats[index].format == VK_FORMAT_UNDEFINED) &&
            formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            format_supported = true;
            break;
        }
    }
    if (!format_supported)
        return Error::unsupported;

    uint32 requested_image_count = device.present_context_count;
    if (requested_image_count < capabilities.minImageCount) requested_image_count = capabilities.minImageCount;
    if (capabilities.maxImageCount != 0 && requested_image_count > capabilities.maxImageCount) requested_image_count = capabilities.maxImageCount;
    if (requested_image_count == 0 || requested_image_count > max_swapchain_images) return Error::unsupported;

    const VkCompositeAlphaFlagBitsKHR composite_alpha = choose_composite_alpha(capabilities.supportedCompositeAlpha);
    const VkSwapchainKHR old_handle = swapchain.handle;
    const VkSwapchainPresentModesCreateInfoKHR present_modes_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR,
        .presentModeCount = 1,
        .pPresentModes = &swapchain_present_mode,
    };
    const VkSwapchainCreateInfoKHR create_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = &present_modes_info,
        .surface = device.surface,
        .minImageCount = requested_image_count,
        .imageFormat = requested_format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = composite_alpha,
        .presentMode = swapchain_present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = old_handle,
    };
    VkSwapchainKHR new_handle = VK_NULL_HANDLE;
    error = error_from_vk(vkCreateSwapchainKHR(device.device, &create_info, nullptr, &new_handle));
    if (error != Error::none)
    {
        retire_swapchain_handle(swapchain);
        return error;
    }

    VkImage images[max_swapchain_images]{};
    uint32 image_count = 0;
    VkResult result = vkGetSwapchainImagesKHR(device.device, new_handle, &image_count, nullptr);
    if (result != VK_SUCCESS || image_count == 0 || image_count > max_swapchain_images)
    {
        vkDestroySwapchainKHR(device.device, new_handle, nullptr);
        retire_swapchain_handle(swapchain);
        return result == VK_SUCCESS ? Error::unsupported : error_from_vk(result);
    }
    result = vkGetSwapchainImagesKHR(device.device, new_handle, &image_count, images);
    if (result != VK_SUCCESS)
    {
        vkDestroySwapchainKHR(device.device, new_handle, nullptr);
        retire_swapchain_handle(swapchain);
        return error_from_vk(result);
    }

    VkImageView views[max_swapchain_images]{};
    for (uint32 index = 0; index < image_count; ++index)
    {
        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[index],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = requested_format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        result = vkCreateImageView(device.device, &view_info, nullptr, &views[index]);
        if (result != VK_SUCCESS)
        {
            for (uint32 created = 0; created < index; ++created)
            {
                vkDestroyImageView(device.device, views[created], nullptr);
            }
            vkDestroySwapchainKHR(device.device, new_handle, nullptr);
            retire_swapchain_handle(swapchain);
            return error_from_vk(result);
        }
    }

    retire_swapchain_handle(swapchain);
    swapchain.handle = new_handle;
    swapchain.image_count = image_count;
    swapchain.width = extent.width;
    swapchain.height = extent.height;
    swapchain.transform = capabilities.currentTransform;
    swapchain.composite_alpha = composite_alpha;
    swapchain.recreate_required = false;
    for (uint32 index = 0; index < image_count; ++index)
    {
        swapchain.images[index] = images[index];
        swapchain.render_views[index] = {
            .state = &device,
            .view = views[index],
            .width = extent.width,
            .height = extent.height,
            .swapchain_view = true,
        };
    }
    return Error::none;
}

} // namespace

uint32x2 get_drawable_extent(Device* device) noexcept
{
    assert(device && "get_drawable_extent called with a null device");
    assert(device->active_command_buffers == 0 &&
           !device->acquired_swapchain &&
           "get_drawable_extent must be called outside command recording and presentation");
    if (!device->swapchain)
        return {};

    VkSurfaceCapabilitiesKHR capabilities{};
    const Error error = error_from_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical_device, device->surface, &capabilities));
    require_error(error);
    const VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT_MAX || extent.height == UINT_MAX)
    {
        require_error(Error::unsupported);
    }

    Swapchain& swapchain = *device->swapchain;
    if (swapchain.handle && (swapchain.width != extent.width || swapchain.height != extent.height))
    {
        swapchain.recreate_required = true;
    }
    return {
        .x = extent.width,
        .y = extent.height,
    };
}

SwapchainFrame acquire(Device* device) noexcept
{
    assert((device && device->swapchain &&
                       device->swapchain->state == device &&
                       !device->swapchain->acquired &&
                       !device->acquired_swapchain &&
                       device->active_command_buffers == 0) && "acquire received a device without an available swapchain");

    Swapchain* swapchain = device->swapchain;
    detail::PresentContext* present_context = nullptr;

    for (;;)
    {
        if (!swapchain->handle || swapchain->recreate_required)
        {
            const Error error = recreate_swapchain(*swapchain);
            require_error(error);
            const bool drawable = swapchain->handle && swapchain->width != 0 && swapchain->height != 0;
            if (!drawable)
                return {};
        }

        if (!present_context)
        {
            present_context = &device->present_contexts[device->next_present_context];
            device->wait_present_context(*present_context);
            assert(!present_context->present_pending && !present_context->swapchain);
        }

        uint32 image_index = 0;
        const VkResult result = vkAcquireNextImageKHR(
            device->device,
            swapchain->handle,
            ~uint64{0},
            present_context->acquired,
            VK_NULL_HANDLE,
            &image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            swapchain->recreate_required = true;
            continue;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            abort_vk_failure(result);
        assert((image_index < swapchain->image_count) && "swapchain returned an invalid image index");

        swapchain->image_index = image_index;
        swapchain->present_context = present_context;
        swapchain->acquired = true;
        swapchain->recreate_required = result == VK_SUBOPTIMAL_KHR && swapchain_surface_configuration_changed(*swapchain);
        device->acquired_swapchain = swapchain;
        return {
            .render_view = &swapchain->render_views[image_index],
            .extent = {.x = swapchain->width, .y = swapchain->height},
        };
    }
}

namespace
{

struct PreparedTexture
{
    VkFormat view_formats[format_count]{};
    VkImageFormatListCreateInfo format_list{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
    };
    VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    };
};

void prepare_texture(Device& device, const TextureDesc& desc, PreparedTexture& output) noexcept
{
    output = {};
    output.format_list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
    output.image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;

    VkImageUsageFlags usage = 0;
    if (has_flag(desc.usage, TextureUsage::sampled)) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (has_flag(desc.usage, TextureUsage::storage)) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (has_flag(desc.usage, TextureUsage::color_attachment)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (has_flag(desc.usage, TextureUsage::depth_stencil_attachment)) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (has_flag(desc.usage, TextureUsage::transfer_source)) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (has_flag(desc.usage, TextureUsage::transfer_destination)) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    const VkImageType image_type = to_vk(desc.type);
    const VkFormat format = to_vk(desc.format);
    const VkFormatFeatureFlags2 sampled_features = required_format_features(TextureUsage::sampled);
    const VkFormatFeatureFlags2 storage_features = required_format_features(TextureUsage::storage);
    uint32 view_format_count = 1;
    output.view_formats[0] = format;
    for (uint32 value = 0; desc.mutable_format && value < format_count; ++value)
    {
        const Format view_format = static_cast<Format>(value);
        if (view_format == desc.format || !compatible_view_formats(desc.format, view_format))
            continue;
        const VkFormatFeatureFlags2 view_features = device.format_features[value];
        const bool sampled = has_flag(desc.usage, TextureUsage::sampled) && (view_features & sampled_features) == sampled_features;
        const bool storage = has_flag(desc.usage, TextureUsage::storage) && (view_features & storage_features) == storage_features;
        if (!sampled && !storage)
            continue;
        assert(view_format_count < format_count);
        output.view_formats[view_format_count++] = to_vk(view_format);
    }

    VkImageCreateFlags image_flags = 0;
    if (desc.type == TextureType::cube || desc.type == TextureType::cube_array) image_flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (view_format_count > 1) image_flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

    output.format_list.viewFormatCount = view_format_count;
    output.format_list.pViewFormats = output.view_formats;
    output.image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = view_format_count > 1 ? &output.format_list : nullptr,
        .flags = image_flags,
        .imageType = image_type,
        .format = format,
        .extent = {
            .width = desc.extent.x,
            .height = desc.extent.y,
            .depth = desc.extent.z,
        },
        .mipLevels = desc.mip_levels,
        .arrayLayers = desc.layer_count,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
}

} // namespace

TextureHeap create_texture_heap(Device* device, uint64 byte_count) noexcept
{
    assert(device && "create_texture_heap called with a null device");
    TextureHeapOwner* owner = new TextureHeapOwner{
        .state = device,
    };
    const VkMemoryAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = byte_count,
        .memoryTypeIndex = device->texture_memory_type,
    };
    require_vk(vkAllocateMemory(device->device, &allocate_info, nullptr, &owner->memory));
    return {
        .size = byte_count,
        .owner = owner,
    };
}

void destroy_texture_heap(const TextureHeap& heap) noexcept
{
    if (!heap.owner) return;
    vkFreeMemory(heap.owner->state->device, heap.owner->memory, nullptr);
    delete heap.owner;
}

SizeAlign get_texture_size_align(Device* device, const TextureDesc& desc) noexcept
{
    assert(device && "get_texture_size_align called with a null device");
    PreparedTexture texture{};
    prepare_texture(*device, desc, texture);
    const VkMemoryRequirements requirements = image_memory_requirements(*device, texture.image_info);
    return {
        .size = requirements.size,
        .align = requirements.alignment,
    };
}

Texture* create_texture(Device* device, const TextureDesc& desc, const TextureHeap& heap, uint64 offset) noexcept
{
    const TextureHeapOwner* owner = heap.owner;
    assert(device && owner && owner->state == device && owner->memory && "create_texture requires a texture heap from the same device");
    assert(device->active_command_buffers == 0 && "create_texture is not allowed while a command buffer is recording");
    PreparedTexture texture{};
    prepare_texture(*device, desc, texture);

    Texture* result = new Texture{
        .state = device,
        .width = desc.extent.x,
        .height = desc.extent.y,
        .depth = desc.extent.z,
        .layer_count = desc.layer_count,
        .type = desc.type,
        .format = desc.format,
    };
    require_vk(vkCreateImage(device->device, &texture.image_info, nullptr, &result->image));
    require_vk(vkBindImageMemory(device->device, result->image, owner->memory, offset));

    result->initialization = {
        .image = result->image,
        .aspect_mask = image_aspects(desc.format),
        .mip_levels = desc.mip_levels,
        .array_layers = desc.layer_count,
    };
    detail::append_texture_initialization(device->pending_texture_initializations, result->initialization);
    return result;
}

void destroy_texture(Texture* texture) noexcept
{
    delete texture;
}

RenderView* create_render_view(Texture* texture, const RenderViewDesc& desc) noexcept
{
    assert(texture && texture->state);

    uint32 width = texture->width >> desc.mip_level;
    uint32 height = texture->height >> desc.mip_level;
    if (width == 0) width = 1;
    if (height == 0) height = 1;
    RenderView* result = new RenderView{
        .state = texture->state,
        .width = width,
        .height = height,
    };
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = to_vk(texture->format),
        .subresourceRange = {
            .aspectMask = image_aspects(texture->format),
            .baseMipLevel = desc.mip_level,
            .levelCount = 1,
            .baseArrayLayer = desc.slice,
            .layerCount = 1,
        },
    };
    require_vk(vkCreateImageView(texture->state->device, &view_info, nullptr, &result->view));
    return result;
}

void destroy_render_view(RenderView* render_view) noexcept
{
    if (!render_view) return;
    assert(!render_view->swapchain_view && "swapchain render views are owned by their swapchain");
    vkDestroyImageView(render_view->state->device, render_view->view, nullptr);
    delete render_view;
}

void write_texture_descriptor(Device* device, void* cpu_destination, const Texture* texture, TextureDescriptorType type,
                              const TextureDescriptorDesc& desc) noexcept
{
    assert(device && texture);

    const VkImageUsageFlags descriptor_usage = static_cast<VkImageUsageFlags>(
        type == TextureDescriptorType::sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT);

    VkImageAspectFlags descriptor_aspect = 0;
    switch (desc.aspect)
    {
    case TextureAspect::automatic:
        descriptor_aspect = has_depth_aspect(texture->format) ? VK_IMAGE_ASPECT_DEPTH_BIT :
                            has_stencil_aspect(texture->format) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        break;
    case TextureAspect::color: descriptor_aspect = VK_IMAGE_ASPECT_COLOR_BIT; break;
    case TextureAspect::depth: descriptor_aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
    case TextureAspect::stencil: descriptor_aspect = VK_IMAGE_ASPECT_STENCIL_BIT; break;
    }
    const VkImageViewUsageCreateInfo view_usage{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = descriptor_usage,
    };
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &view_usage,
        .image = texture->image,
        .viewType = to_vk_view(texture->type),
        .format = to_vk(desc.format == Format::undefined ? texture->format : desc.format),
        .subresourceRange = {
            .aspectMask = descriptor_aspect,
            .baseMipLevel = desc.base_mip,
            .levelCount = desc.mip_count == 0 ? VK_REMAINING_MIP_LEVELS : desc.mip_count,
            .baseArrayLayer = desc.base_layer,
            .layerCount = desc.layer_count == 0 ? VK_REMAINING_ARRAY_LAYERS : desc.layer_count,
        },
    };
    const VkImageDescriptorInfoEXT image_descriptor{
        .sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
        .pView = &view_info,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkResourceDescriptorInfoEXT descriptor_info{
        .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
        .type = type == TextureDescriptorType::sampled
            ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .data = {.pImage = &image_descriptor},
    };
    const VkHostAddressRangeEXT destination{
        .address = cpu_destination,
        .size = static_cast<size_t>(device->heap_properties.imageDescriptorSize),
    };
    assert_vk(device->fn.write_resource_descriptors(device->device, 1, &descriptor_info, &destination));
}

void write_sampler_descriptor(Device* device, void* cpu_destination, const SamplerDesc& desc) noexcept
{
    assert(device);
    const VkSamplerCreateInfo sampler_info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = static_cast<VkFilter>(desc.mag_filter),
        .minFilter = static_cast<VkFilter>(desc.min_filter),
        .mipmapMode = static_cast<VkSamplerMipmapMode>(desc.mip_filter),
        .addressModeU = static_cast<VkSamplerAddressMode>(desc.address_u),
        .addressModeV = static_cast<VkSamplerAddressMode>(desc.address_v),
        .addressModeW = static_cast<VkSamplerAddressMode>(desc.address_w),
        .anisotropyEnable = desc.anisotropic ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = desc.anisotropic ? 4.0f : 1.0f,
        .compareEnable = desc.compare_enabled ? VK_TRUE : VK_FALSE,
        .compareOp = static_cast<VkCompareOp>(desc.compare),
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    const VkHostAddressRangeEXT destination{
        .address = cpu_destination,
        .size = static_cast<size_t>(device->heap_properties.samplerDescriptorSize),
    };
    assert_vk(device->fn.write_sampler_descriptors(device->device, 1, &sampler_info, &destination));
}

namespace
{

PSO* create_raster_pso(Device* device, Span<const uint32> first_stage_spirv, Span<const uint32> fragment_spirv, Span<const ColorTargetDesc> color_targets,
                       Format depth_format, Format stencil_format, const RasterizationState& rasterization_state, bool mesh) noexcept
{
    assert(device && "PSO creation called with a null device");
    assert((color_targets.size == 0 || color_targets.data) && color_targets.size <= max_color_attachments &&
           "color targets must fit the wrapper's attachment array");

    const VkShaderModuleCreateInfo first_stage_module_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = first_stage_spirv.size * sizeof(uint32),
        .pCode = first_stage_spirv.data,
    };
    const VkShaderModuleCreateInfo fragment_module_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fragment_spirv.size * sizeof(uint32),
        .pCode = fragment_spirv.data,
    };
    const VkPipelineShaderStageCreateInfo stages[]{
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &first_stage_module_info,
            .stage = mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT,
            .pName = mesh ? "meshMain" : "vertexMain",
        },
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &fragment_module_info,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pName = "fragmentMain",
        },
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo input_assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = static_cast<VkCullModeFlags>(rasterization_state.cull == CullMode::none ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT),
        .frontFace = rasterization_state.cull == CullMode::counter_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = rasterization_state.depth_bias_constant != 0.0f ||
                           rasterization_state.depth_bias_clamp != 0.0f ||
                           rasterization_state.depth_bias_slope != 0.0f,
        .depthBiasConstantFactor = rasterization_state.depth_bias_constant,
        .depthBiasClamp = rasterization_state.depth_bias_clamp,
        .depthBiasSlopeFactor = rasterization_state.depth_bias_slope,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineDepthStencilStateCreateInfo depth_stencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };
    VkPipelineColorBlendAttachmentState color_attachments[max_color_attachments]{};
    for (size_t index = 0; index < color_targets.size; ++index)
    {
        const ColorTargetDesc& target = color_targets.data[index];
        color_attachments[index] = {
            .blendEnable = target.blend.enabled,
            .srcColorBlendFactor = to_vk(target.blend.color.source),
            .dstColorBlendFactor = to_vk(target.blend.color.destination),
            .colorBlendOp = static_cast<VkBlendOp>(target.blend.color.operation),
            .srcAlphaBlendFactor = to_vk(target.blend.alpha.source),
            .dstAlphaBlendFactor = to_vk(target.blend.alpha.destination),
            .alphaBlendOp = static_cast<VkBlendOp>(target.blend.alpha.operation),
            .colorWriteMask = target.write_mask,
        };
    }
    const VkPipelineColorBlendStateCreateInfo color_blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = static_cast<uint32>(color_targets.size),
        .pAttachments = color_targets.size ? color_attachments : nullptr,
    };
    constexpr VkDynamicState dynamic_states[]{
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
    const VkPipelineDynamicStateCreateInfo dynamic_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<uint32>(sizeof(dynamic_states) / sizeof(dynamic_states[0])),
        .pDynamicStates = dynamic_states,
    };
    VkFormat color_formats[max_color_attachments]{};
    for (size_t index = 0; index < color_targets.size; ++index)
    {
        color_formats[index] = to_vk(color_targets.data[index].format);
    }
    const VkPipelineRenderingCreateInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = static_cast<uint32>(color_targets.size),
        .pColorAttachmentFormats = color_targets.size ? color_formats : nullptr,
        .depthAttachmentFormat = to_vk(depth_format),
        .stencilAttachmentFormat = to_vk(stencil_format),
    };
    const VkPipelineCreateFlags2CreateInfo flags_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .pNext = &rendering_info,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
    };
    const VkGraphicsPipelineCreateInfo pso_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &flags_info,
        .stageCount = static_cast<uint32>(sizeof(stages) / sizeof(stages[0])),
        .pStages = stages,
        .pVertexInputState = mesh ? nullptr : &vertex_input,
        .pInputAssemblyState = mesh ? nullptr : &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic_state,
        .basePipelineIndex = -1,
    };
    PSO* result = new PSO{
        .state = device,
        .bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS,
    };
    require_vk(vkCreateGraphicsPipelines(device->device, VK_NULL_HANDLE, 1, &pso_info, nullptr, &result->pso));
    return result;
}

} // namespace

PSO* create_graphics_pso(Device* device, const GraphicsPSODesc& desc) noexcept
{
    return create_raster_pso(device, desc.vertex_spirv, desc.fragment_spirv, desc.color_targets, desc.depth_format,
                             desc.stencil_format, desc.rasterization, false);
}

PSO* create_mesh_pso(Device* device, const MeshPSODesc& desc) noexcept
{
    return create_raster_pso(device, desc.mesh_spirv, desc.fragment_spirv, desc.color_targets, desc.depth_format,
                             desc.stencil_format, desc.rasterization, true);
}

PSO* create_compute_pso(Device* device, Span<const uint32> compute_spirv) noexcept
{
    assert(device && "create_compute_pso called with a null device");

    const VkShaderModuleCreateInfo module_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = compute_spirv.size * sizeof(uint32),
        .pCode = compute_spirv.data,
    };
    const VkPipelineShaderStageCreateInfo stage{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = &module_info,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .pName = "computeMain",
    };
    const VkPipelineCreateFlags2CreateInfo flags_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
    };
    const VkComputePipelineCreateInfo pso_info{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = &flags_info,
        .stage = stage,
        .basePipelineIndex = -1,
    };
    PSO* result = new PSO{
        .state = device,
        .bind_point = VK_PIPELINE_BIND_POINT_COMPUTE,
    };
    require_vk(vkCreateComputePipelines(device->device, VK_NULL_HANDLE, 1, &pso_info, nullptr, &result->pso));
    return result;
}

void destroy_pso(PSO* pso) noexcept
{
    delete pso;
}

CommandBuffer* begin_commands(Device* device) noexcept
{
    assert(device && "begin_commands called with a null device");
    CommandBuffer* result = &device->acquire_command_context();
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    assert_vk(vkBeginCommandBuffer(result->command_buffer, &begin_info));
    result->state = device;
    if (device->pending_texture_initializations.first)
    {
        VkImageMemoryBarrier2 barriers[image_barrier_batch_size]{};
        uint32 barrier_count = 0;
        while (device->pending_texture_initializations.first)
        {
            detail::TextureInitialization* initialization = device->pending_texture_initializations.first;
            detail::remove_texture_initialization(*initialization);
            barriers[barrier_count++] = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = initialization->image,
                .subresourceRange = {
                    .aspectMask = initialization->aspect_mask,
                    .levelCount = initialization->mip_levels,
                    .layerCount = initialization->array_layers,
                },
            };
            if (barrier_count == image_barrier_batch_size)
            {
                record_image_barriers(result->command_buffer, {barriers, barrier_count});
                barrier_count = 0;
            }
        }
        if (barrier_count != 0) record_image_barriers(result->command_buffer, {barriers, barrier_count});
    }
    if (device->acquired_swapchain && !device->acquired_swapchain->transition_commands)
    {
        Swapchain* swapchain = device->acquired_swapchain;
        assert((swapchain->state == device && swapchain->acquired &&
                           swapchain->present_context &&
                           swapchain->image_index < swapchain->image_count) && "the acquired swapchain state is invalid");
        const VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .oldLayout = swapchain->initialized[swapchain->image_index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain->images[swapchain->image_index],
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        record_image_barriers(result->command_buffer, {&barrier, 1});
        result->swapchain = swapchain;
        swapchain->transition_commands = result;
    }
    ++device->active_command_buffers;
    return result;
}

namespace
{

void submit_commands(Span<CommandBuffer* const> commands, Device* device, TimelinePoint completion, VkSemaphore wait_semaphore,
                     VkSemaphore signal_semaphore) noexcept
{
    TimelineSemaphore* completion_semaphore = completion.semaphore;
    assert((completion_semaphore &&
                                  completion_semaphore->state == device &&
                                  completion_semaphore->semaphore) && "submission completion requires a live timeline semaphore owned by the device");
    assert(commands.data && commands.size != 0 && commands.size <= UINT_MAX);
    assert(device->active_command_buffers == commands.size && "submit must consume every begun command buffer");
    assert(!device->pending_texture_initializations.first && !device->pending_texture_initializations.last && "pending texture transitions were not recorded");
    assert(device->command_context_count >= commands.size);
    for (size_t index = 0; index < commands.size; ++index)
    {
        CommandBuffer* current = commands.data[index];
        assert((current && current->state == device) && "command buffer batch contains an invalid handle");
        assert_vk(vkEndCommandBuffer(current->command_buffer));
        current->state = nullptr;
        assert(device->active_command_buffers != 0);
        --device->active_command_buffers;
        device->command_submit_infos[index] = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = current->command_buffer,
            .deviceMask = 1,
        };
    }
    assert(device->active_command_buffers == 0);
    const uint64 retirement = device->next_command_retirement();
    const VkSemaphoreSubmitInfo wait_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = wait_semaphore,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkSemaphoreSubmitInfo work_signal_infos[2]{
      {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = completion_semaphore->semaphore,
        .value = completion.value,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      },
      {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = signal_semaphore,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      },
    };
    const VkSemaphoreSubmitInfo retirement_signal_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = device->command_retirement,
        .value = retirement,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkSubmitInfo2 submit_infos[2]{
      {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = wait_semaphore ? 1u : 0u,
        .pWaitSemaphoreInfos = wait_semaphore ? &wait_info : nullptr,
        .commandBufferInfoCount = static_cast<uint32>(commands.size),
        .pCommandBufferInfos = device->command_submit_infos,
        .signalSemaphoreInfoCount = signal_semaphore ? 2u : 1u,
        .pSignalSemaphoreInfos = work_signal_infos,
      },
      {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &retirement_signal_info,
      },
    };
    assert_vk(vkQueueSubmit2(device->queue, 2, submit_infos, VK_NULL_HANDLE));

    for (size_t index = 0; index < commands.size; ++index)
    {
        CommandBuffer* current = commands.data[index];
        current->retire_value = retirement;
        current->swapchain = nullptr;
    }
}

} // namespace

void submit(Span<CommandBuffer* const> commands, TimelinePoint completion) noexcept
{
    assert(commands.data && commands.size != 0);
    CommandBuffer* first = commands.data[0];
    assert(first && first->state);
    Device* device = first->state;
    assert(!device->acquired_swapchain && "an acquired swapchain frame must be submitted with submit_and_present");
#if !defined(NDEBUG)
    for (size_t index = 0; index < commands.size; ++index)
    {
        assert(!commands.data[index]->swapchain && "swapchain command buffers must be submitted with submit_and_present");
    }
#endif
    submit_commands(commands, device, completion, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

void submit_and_present(Device* device, Span<CommandBuffer* const> commands, TimelinePoint completion) noexcept
{
    const bool has_swapchain = device && device->swapchain && device->swapchain->state == device;
    assert(has_swapchain && "submit_and_present received a device without a swapchain");
    Swapchain* swapchain = device->swapchain;
    assert(commands.data && commands.size != 0);
    CommandBuffer* first_commands = commands.data[0];
    CommandBuffer* last_commands = commands.data[commands.size - 1];
    assert(first_commands && first_commands->state &&
           last_commands && last_commands->state == first_commands->state);
    Device* command_device = first_commands->state;
    assert((swapchain->acquired &&
                       command_device == device &&
                       first_commands->swapchain == swapchain &&
                       swapchain->transition_commands == first_commands &&
                       swapchain->present_context) && "submit_and_present received an invalid swapchain command buffer batch");
#if !defined(NDEBUG)
    for (size_t index = 1; index < commands.size; ++index)
    {
        assert(!commands.data[index]->swapchain && "only the first command buffer may own the swapchain transition");
    }
#endif

    Device& owner = *device;
    detail::PresentContext& present_context = *swapchain->present_context;
    const VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapchain->images[swapchain->image_index],
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    record_image_barriers(last_commands->command_buffer, {&barrier, 1});
    assert(!present_context.present_pending && !present_context.swapchain);
    assert_vk(vkResetFences(owner.device, 1, &present_context.presented));
    swapchain->transition_commands = nullptr;
    submit_commands(commands, command_device, completion, present_context.acquired, present_context.rendered);
    swapchain->initialized[swapchain->image_index] = true;

    const VkSwapchainPresentFenceInfoKHR fence_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
        .swapchainCount = 1,
        .pFences = &present_context.presented,
    };
    const VkPresentInfoKHR present_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = &fence_info,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_context.rendered,
        .swapchainCount = 1,
        .pSwapchains = &swapchain->handle,
        .pImageIndices = &swapchain->image_index,
    };
    const VkResult result = vkQueuePresentKHR(owner.queue, &present_info);
    present_context.present_pending = true;
    present_context.swapchain = swapchain->handle;
    swapchain->present_context = nullptr;
    swapchain->acquired = false;
    owner.acquired_swapchain = nullptr;
    owner.next_present_context = (owner.next_present_context + 1) % owner.present_context_count;
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
        swapchain->recreate_required = true;
    else if (result == VK_SUBOPTIMAL_KHR)
        swapchain->recreate_required = swapchain->recreate_required || swapchain_surface_configuration_changed(*swapchain);
    else if (result != VK_SUCCESS)
        abort_vk_failure(result);
}

void wait_idle(Device* device) noexcept
{
    assert(device && "wait_idle called with a null device");
    assert(device->active_command_buffers == 0 && "wait_idle is not allowed while a command buffer is recording");
    assert(!device->acquired_swapchain && "wait_idle is not allowed while a swapchain image is acquired");
    device->drain_contexts();
    device->next_present_context = 0;
}

const DeviceCaps& get_device_caps(const Device* device) noexcept
{
    assert(device && "get_device_caps called with a null device");
    return device->caps;
}

bool supports_texture_format(const Device* device, Format format, TextureUsage usage) noexcept
{
    assert(device && static_cast<uint32>(format) < format_count && has_valid_texture_usage_bits(usage));

    const TextureFormatInfo info = get_texture_format_info(format);
    if ((info.depth || info.stencil) && has_flag(usage, TextureUsage::color_attachment))
    {
        return false;
    }
    if (!info.depth && !info.stencil && has_flag(usage, TextureUsage::depth_stencil_attachment))
    {
        return false;
    }
    if (info.depth && info.stencil && (has_flag(usage, TextureUsage::transfer_source) || has_flag(usage, TextureUsage::transfer_destination)))
    {
        return false;
    }
    switch (texture_compression(format))
    {
    case TextureCompression::none: break;
    case TextureCompression::etc2:
        if (!device->texture_compression_etc2)
            return false;
        break;
    case TextureCompression::astc:
        if (!device->caps.texture_compression_astc)
            return false;
        break;
    case TextureCompression::bc:
        if (!device->caps.texture_compression_bc)
            return false;
        break;
    }

    const VkFormatFeatureFlags2 available = device->format_features[static_cast<uint32>(format)];
    const VkFormatFeatureFlags2 required = required_format_features(usage);
    return (available & required) == required;
}

void bind_pso(CommandBuffer* commands, const PSO* pso) noexcept
{
    assert(commands && pso);
    vkCmdBindPipeline(commands->command_buffer, pso->bind_point, pso->pso);
}

VkDeviceMemoryImageCopyKHR make_texture_copy_region(const Texture& texture, const TextureCopyDesc& copy, GpuRange memory) noexcept
{
    uint32 mip_width = texture.width >> copy.mip_level;
    uint32 mip_height = texture.height >> copy.mip_level;
    uint32 mip_depth = texture.depth >> copy.mip_level;
    if (mip_width == 0) mip_width = 1;
    if (mip_height == 0) mip_height = 1;
    if (mip_depth == 0) mip_depth = 1;

    const uint32 width = copy.extent.x == 0 ? mip_width - copy.offset.x : copy.extent.x;
    const TextureFormatInfo format_info = get_texture_format_info(texture.format);
    const uint64 row_pitch = copy.row_pitch_bytes == 0
                                 ? divide_up(width, format_info.block_extent.x) * format_info.bytes_per_block
                                 : copy.row_pitch_bytes;
    assert((copy.slice_pitch_bytes == 0 || row_pitch != 0) && "slice pitch conversion requires a non-zero row pitch");
    return {
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR,
        .addressRange = {
            .address = static_cast<VkDeviceAddress>(reinterpret_cast<uintptr>(memory.gpu)),
            .size = memory.size,
        },
        .addressFlags = address_flags,
        .addressRowLength = static_cast<uint32>(copy.row_pitch_bytes / format_info.bytes_per_block * format_info.block_extent.x),
        .addressImageHeight = copy.slice_pitch_bytes == 0 ? 0u : static_cast<uint32>(copy.slice_pitch_bytes / row_pitch * format_info.block_extent.y),
        .imageSubresource = {
            .aspectMask = image_aspects(texture.format),
            .mipLevel = copy.mip_level,
            .baseArrayLayer = copy.base_slice,
            .layerCount = copy.slice_count == 0 ? texture.layer_count - copy.base_slice : copy.slice_count,
        },
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageOffset = {
            .x = static_cast<int32>(copy.offset.x),
            .y = static_cast<int32>(copy.offset.y),
            .z = static_cast<int32>(copy.offset.z),
        },
        .imageExtent = {
            .width = width,
            .height = copy.extent.y == 0 ? mip_height - copy.offset.y : copy.extent.y,
            .depth = copy.extent.z == 0 ? mip_depth - copy.offset.z : copy.extent.z,
        },
    };
}

void emit_root_data(CommandBuffer* commands, ByteSpan root) noexcept
{
    if (root.size == 0)
        return;
    assert(commands->state);
    const VkPushDataInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
        .data = {
            .address = root.data,
            .size = root.size,
        },
    };
    commands->state->fn.cmd_push_data(commands->command_buffer, &info);
}

void set_texture_descriptor_heap(CommandBuffer* commands, GpuRange heap) noexcept
{
    assert(commands && commands->state);
    const VkPhysicalDeviceDescriptorHeapPropertiesEXT& properties = commands->state->heap_properties;
    VkBindHeapInfoEXT bind_info{};
    make_heap_bind_info(
        heap,
        properties.imageDescriptorAlignment > properties.bufferDescriptorAlignment
            ? properties.imageDescriptorAlignment
            : properties.bufferDescriptorAlignment,
        properties.minResourceHeapReservedRange,
        bind_info);
    commands->state->fn.cmd_bind_texture_heap(commands->command_buffer, &bind_info);
}

void set_sampler_descriptor_heap(CommandBuffer* commands, GpuRange heap) noexcept
{
    assert(commands && commands->state);
    const VkPhysicalDeviceDescriptorHeapPropertiesEXT& properties = commands->state->heap_properties;
    VkBindHeapInfoEXT bind_info{};
    make_heap_bind_info(heap, properties.samplerDescriptorAlignment, properties.minSamplerHeapReservedRange, bind_info);
    commands->state->fn.cmd_bind_sampler_heap(commands->command_buffer, &bind_info);
}

void set_viewport(CommandBuffer* commands, const Viewport& viewport) noexcept
{
    assert(commands);
    const VkViewport vk_viewport{
        .x = viewport.x,
        .y = viewport.y,
        .width = viewport.width,
        .height = viewport.height,
        .minDepth = viewport.min_depth,
        .maxDepth = viewport.max_depth,
    };
    vkCmdSetViewportWithCount(commands->command_buffer, 1, &vk_viewport);
}

void set_scissor(CommandBuffer* commands, const Scissor& scissor) noexcept
{
    assert(commands);
    const VkRect2D vk_scissor{
        .offset = {.x = scissor.x, .y = scissor.y},
        .extent = {.width = scissor.width, .height = scissor.height},
    };
    vkCmdSetScissorWithCount(commands->command_buffer, 1, &vk_scissor);
}

void set_depth_stencil(CommandBuffer* commands, const DepthStencilState& state) noexcept
{
    assert(commands);

    vkCmdSetDepthTestEnable(commands->command_buffer, state.depth_test);
    if (state.depth_test)
    {
        vkCmdSetDepthWriteEnable(commands->command_buffer, state.depth_write);
        vkCmdSetDepthCompareOp(commands->command_buffer, static_cast<VkCompareOp>(state.depth_compare));
    }
    vkCmdSetStencilTestEnable(commands->command_buffer, state.stencil_test);
    if (!state.stencil_test)
        return;

    vkCmdSetStencilOp(commands->command_buffer, VK_STENCIL_FACE_FRONT_BIT,
                     static_cast<VkStencilOp>(state.front.fail), static_cast<VkStencilOp>(state.front.pass),
                     static_cast<VkStencilOp>(state.front.depth_fail), static_cast<VkCompareOp>(state.front.compare));
    vkCmdSetStencilOp(commands->command_buffer, VK_STENCIL_FACE_BACK_BIT,
                     static_cast<VkStencilOp>(state.back.fail), static_cast<VkStencilOp>(state.back.pass),
                     static_cast<VkStencilOp>(state.back.depth_fail), static_cast<VkCompareOp>(state.back.compare));
    vkCmdSetStencilCompareMask(commands->command_buffer, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencil_read_mask);
    vkCmdSetStencilWriteMask(commands->command_buffer, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencil_write_mask);
    vkCmdSetStencilReference(commands->command_buffer, VK_STENCIL_FACE_FRONT_BIT, state.front.reference);
    vkCmdSetStencilReference(commands->command_buffer, VK_STENCIL_FACE_BACK_BIT, state.back.reference);
}

void begin_render_pass(CommandBuffer* commands, const RenderingDesc& desc) noexcept
{
    assert(commands && (desc.colors.size == 0 || desc.colors.data) && desc.colors.size <= max_color_attachments);
    const RenderView* area_view = desc.colors.size ? desc.colors.data[0].render_view
                                                  : (desc.depth.render_view ? desc.depth.render_view : desc.stencil.render_view);
    assert(area_view && "begin_render_pass requires an attachment to determine the render area");

    VkRenderingAttachmentInfo color_attachments[max_color_attachments]{};
    for (size_t index = 0; index < desc.colors.size; ++index)
    {
        const ColorAttachment& attachment = desc.colors.data[index];
        assert(attachment.render_view);
        color_attachments[index] = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = attachment.render_view->view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = static_cast<VkAttachmentLoadOp>(attachment.load),
            .storeOp = static_cast<VkAttachmentStoreOp>(attachment.store),
            .clearValue = {
                .color = {
                    .float32 = {attachment.clear.x, attachment.clear.y, attachment.clear.z, attachment.clear.w},
                },
            },
        };
    }

    const VkRenderingAttachmentInfo depth_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = desc.depth.render_view ? desc.depth.render_view->view : VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = static_cast<VkAttachmentLoadOp>(desc.depth.load),
        .storeOp = static_cast<VkAttachmentStoreOp>(desc.depth.store),
        .clearValue = {
            .depthStencil = {
                .depth = desc.depth.clear,
            },
        },
    };
    const VkRenderingAttachmentInfo stencil_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = desc.stencil.render_view ? desc.stencil.render_view->view : VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = static_cast<VkAttachmentLoadOp>(desc.stencil.load),
        .storeOp = static_cast<VkAttachmentStoreOp>(desc.stencil.store),
        .clearValue = {
            .depthStencil = {
                .stencil = desc.stencil.clear,
            },
        },
    };
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {
            .extent = {.width = area_view->width, .height = area_view->height},
        },
        .layerCount = 1,
        .colorAttachmentCount = static_cast<uint32>(desc.colors.size),
        .pColorAttachments = color_attachments,
        .pDepthAttachment = desc.depth.render_view ? &depth_attachment : nullptr,
        .pStencilAttachment = desc.stencil.render_view ? &stencil_attachment : nullptr,
    };
    vkCmdBeginRendering(commands->command_buffer, &rendering_info);

    set_viewport(commands, {.width = static_cast<float>(area_view->width), .height = static_cast<float>(area_view->height)});
    set_scissor(commands, {.width = area_view->width, .height = area_view->height});
    set_depth_stencil(commands, {});
}

void end_render_pass(CommandBuffer* commands) noexcept
{
    assert(commands);
    vkCmdEndRendering(commands->command_buffer);
}

void draw(CommandBuffer* commands, ByteSpan root, uint32 vertex_count, uint32 instance_count, uint32 first_vertex, uint32 first_instance) noexcept
{
    assert(commands);
    emit_root_data(commands, root);
    vkCmdDraw(commands->command_buffer, vertex_count, instance_count, first_vertex, first_instance);
}

void draw_indexed(CommandBuffer* commands, ByteSpan root, GpuRange indices, IndexType type, uint32 index_count, uint32 instance_count, uint32 first_index,
                  int32 vertex_offset, uint32 first_instance) noexcept
{
    assert(commands && commands->state);
    emit_root_data(commands, root);
    const VkBindIndexBuffer3InfoKHR bind_info{
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .addressRange = {
            .address = static_cast<VkDeviceAddress>(reinterpret_cast<uintptr>(indices.gpu)),
            .size = indices.size,
        },
        .addressFlags = address_flags,
        .indexType = static_cast<VkIndexType>(type),
    };
    commands->state->fn.cmd_bind_index_buffer(commands->command_buffer, &bind_info);
    vkCmdDrawIndexed(commands->command_buffer, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void draw_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments, uint32 draw_count, uint32 stride) noexcept
{
    assert(commands && commands->state);
    emit_root_data(commands, root);
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange = {
            .address = static_cast<VkDeviceAddress>(reinterpret_cast<uintptr>(arguments.gpu)),
            .size = arguments.size,
            .stride = stride == 0 ? sizeof(VkDrawIndirectCommand) : stride,
        },
        .addressFlags = address_flags,
        .drawCount = draw_count,
    };
    commands->state->fn.cmd_draw_indirect(commands->command_buffer, &info);
}

void draw_indexed_indirect(CommandBuffer* commands, ByteSpan root, GpuRange indices, IndexType type, GpuRange arguments, uint32 draw_count,
                           uint32 stride) noexcept
{
    assert(commands && commands->state);
    emit_root_data(commands, root);
    const VkBindIndexBuffer3InfoKHR bind_info{
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .addressRange = {
            .address = static_cast<VkDeviceAddress>(reinterpret_cast<uintptr>(indices.gpu)),
            .size = indices.size,
        },
        .addressFlags = address_flags,
        .indexType = static_cast<VkIndexType>(type),
    };
    commands->state->fn.cmd_bind_index_buffer(commands->command_buffer, &bind_info);
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange = {
            .address = static_cast<VkDeviceAddress>(reinterpret_cast<uintptr>(arguments.gpu)),
            .size = arguments.size,
            .stride = stride == 0 ? sizeof(VkDrawIndexedIndirectCommand) : stride,
        },
        .addressFlags = address_flags,
        .drawCount = draw_count,
    };
    commands->state->fn.cmd_draw_indexed_indirect(commands->command_buffer, &info);
}

void dispatch(CommandBuffer* commands, ByteSpan root, uint32x3 group_count) noexcept
{
    assert(commands);
    emit_root_data(commands, root);
    vkCmdDispatch(commands->command_buffer, group_count.x, group_count.y, group_count.z);
}

void dispatch_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments) noexcept
{
    assert(commands && commands->state);
    emit_root_data(commands, root);
    const VkDispatchIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DISPATCH_INDIRECT_2_INFO_KHR,
        .addressRange = {
            .address = static_cast<VkDeviceAddress>(reinterpret_cast<uintptr>(arguments.gpu)),
            .size = arguments.size,
        },
        .addressFlags = address_flags,
    };
    commands->state->fn.cmd_dispatch_indirect(commands->command_buffer, &info);
}

void draw_meshlets(CommandBuffer* commands, ByteSpan root, uint32x3 group_count) noexcept
{
    assert(commands && commands->state);
    emit_root_data(commands, root);
    commands->state->fn.cmd_draw_mesh_tasks(commands->command_buffer, group_count.x, group_count.y, group_count.z);
}

void draw_meshlets_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments, uint32 draw_count, uint32 stride) noexcept
{
    assert(commands && commands->state);
    emit_root_data(commands, root);
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange = {
            .address = static_cast<VkDeviceAddress>(reinterpret_cast<uintptr>(arguments.gpu)),
            .size = arguments.size,
            .stride = stride == 0 ? sizeof(VkDrawMeshTasksIndirectCommandEXT) : stride,
        },
        .addressFlags = address_flags,
        .drawCount = draw_count,
    };
    commands->state->fn.cmd_draw_mesh_tasks_indirect(commands->command_buffer, &info);
}

void copy_memory(CommandBuffer* commands, GpuRange source, GpuRange destination) noexcept
{
    assert(commands && commands->state);
    const VkDeviceMemoryCopyKHR region{
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_COPY_KHR,
        .srcRange = {
            .address = static_cast<VkDeviceAddress>(reinterpret_cast<uintptr>(source.gpu)),
            .size = source.size,
        },
        .srcFlags = address_flags,
        .dstRange = {
            .address = static_cast<VkDeviceAddress>(reinterpret_cast<uintptr>(destination.gpu)),
            .size = destination.size,
        },
        .dstFlags = address_flags,
    };
    const VkCopyDeviceMemoryInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_INFO_KHR,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.cmd_copy_memory(commands->command_buffer, &info);
}

void copy_memory_to_texture(CommandBuffer* commands, GpuRange source, Texture* destination, const TextureCopyDesc& copy) noexcept
{
    assert(commands && commands->state && destination);
    const VkDeviceMemoryImageCopyKHR region = make_texture_copy_region(*destination, copy, source);
    const VkCopyDeviceMemoryImageInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .image = destination->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.cmd_copy_memory_to_image(commands->command_buffer, &info);
}

void copy_texture_to_memory(CommandBuffer* commands, Texture* source, GpuRange destination, const TextureCopyDesc& copy) noexcept
{
    assert(commands && commands->state && source);
    const VkDeviceMemoryImageCopyKHR region = make_texture_copy_region(*source, copy, destination);
    const VkCopyDeviceMemoryImageInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .image = source->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.cmd_copy_image_to_memory(commands->command_buffer, &info);
}

void barrier(CommandBuffer* commands, Stage before, Access before_access, Stage after, Access after_access) noexcept
{
    assert(commands);
    const VkMemoryBarrier2 memory_barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = to_vk(before),
        .srcAccessMask = to_vk(before_access),
        .dstStageMask = to_vk(after),
        .dstAccessMask = to_vk(after_access),
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &memory_barrier,
    };
    vkCmdPipelineBarrier2(commands->command_buffer, &dependency);
}

} // namespace gpu
