#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <offsetAllocator.hpp>

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

#include <bit>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

// Keep assert expressions type-checked in release without evaluating them.
#if defined(NDEBUG)
#undef assert
#define assert(expression) ((void)sizeof(static_cast<bool>(expression)))
#endif

namespace gpu
{
using std::uintptr_t;

namespace
{

constexpr uint32_t max_device_extensions = 512;
constexpr uint32_t max_instance_extensions = 256;
constexpr uint32_t max_instance_layers = 64;
constexpr uint32_t max_physical_devices = 32;
constexpr uint32_t max_queue_families = 64;
constexpr uint32_t max_color_attachments = 8;
constexpr uint32_t image_barrier_batch_size = 64;
constexpr uint32_t initial_command_context_count = 2;
constexpr uint32_t max_swapchain_images = 8;
constexpr VkPresentModeKHR swapchain_present_mode = VK_PRESENT_MODE_FIFO_KHR;
constexpr uint32_t max_heap_suballocations = 128u * 1024u;
constexpr uint32_t gpu_allocation_alignment = 16;
#if !defined(NDEBUG)
constexpr uint32_t live_allocation_word_count =
    (max_heap_suballocations + 63u) / 64u;
#endif
constexpr uint32_t max_surface_formats = 64;
constexpr uint32_t format_count =
    static_cast<uint32_t>(Format::undefined);
static_assert(sizeof(OffsetAllocator::NodeIndex) == sizeof(uint32_t));
static_assert(
    sizeof(decltype(OffsetAllocator::Allocation{}.offset)) ==
    sizeof(uint32_t));
static_assert(max_heap_suballocations <
              OffsetAllocator::Allocation::NO_SPACE);

constexpr uint32_t allocation_unit_count(VkDeviceSize size) noexcept
{
    return static_cast<uint32_t>((size - 1) / gpu_allocation_alignment + 1);
}

[[nodiscard]] Error error_from_vk(VkResult result) noexcept
{
    switch (result)
    {
    case VK_SUCCESS: return Error::none;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_TOO_MANY_OBJECTS: std::abort();
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
    std::abort();
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
        std::abort();
    }
}

template<typename T>
T load_instance_proc(VkInstance instance, const char* name) noexcept
{
    const auto proc = vkGetInstanceProcAddr(instance, name);
    return reinterpret_cast<T>(proc);
}

template<typename T>
T load_device_proc(VkDevice device, const char* name) noexcept
{
    const auto proc = vkGetDeviceProcAddr(device, name);
    return reinterpret_cast<T>(proc);
}

template<typename T>
T align_up(T value, T alignment)
{
    assert(alignment != 0);
    assert(value <= std::numeric_limits<T>::max() - (alignment - 1));
    return ((value + alignment - 1) / alignment) * alignment;
}

template<typename T>
constexpr bool has_flag(T value, T flag)
{
    using U = std::underlying_type_t<T>;
    return (static_cast<U>(value) & static_cast<U>(flag)) != 0;
}

bool has_name(Span<const VkExtensionProperties> values, const char* name)
{
    for (size_t index = 0; index < values.size; ++index)
    {
        if (std::strcmp(values.data[index].extensionName, name) == 0)
            return true;
    }
    return false;
}

#if !defined(NDEBUG)
bool has_name(Span<const VkLayerProperties> values, const char* name)
{
    for (size_t index = 0; index < values.size; ++index)
    {
        if (std::strcmp(values.data[index].layerName, name) == 0)
            return true;
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && callback_data &&
        callback_data->pMessage)
    {
        // Keep the library callback dependency-free. Applications can still install
        // their own messenger; this one makes validation failures debugger-visible.
        std::fputs("NoGraphicsAPI validation: ", stderr);
        std::fputs(callback_data->pMessage, stderr);
        std::fputc('\n', stderr);
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
    case Format::undefined: break;
    }
    assert(false && "unknown NoGraphicsAPI format");
    std::abort();
}

uint64_t divide_up(uint64_t value, uint64_t divisor) noexcept
{
    assert(divisor != 0);
    return value / divisor + (value % divisor != 0 ? 1u : 0u);
}

uint64_t
base_level_byte_size(Format format, uint32_t width, uint32_t height, uint32_t depth)
{
    const TextureFormatInfo info = get_texture_format_info(format);
    assert(info.bytes_per_block != 0 && info.block_width != 0 && info.block_height != 0);
    const uint64_t width_in_blocks = divide_up(width, info.block_width);
    const uint64_t height_in_blocks = divide_up(height, info.block_height);
    uint64_t size = info.bytes_per_block;
    bool fits =
        width_in_blocks == 0 || size <= std::numeric_limits<uint64_t>::max() / width_in_blocks;
    assert(fits && "texture byte size overflow");
    size *= width_in_blocks;
    fits = height_in_blocks == 0 ||
           size <= std::numeric_limits<uint64_t>::max() / height_in_blocks;
    assert(fits && "texture byte size overflow");
    size *= height_in_blocks;
    fits = depth == 0 || size <= std::numeric_limits<uint64_t>::max() / depth;
    assert(fits && "texture byte size overflow");
    size *= depth;
    return size;
}

enum class FormatCompatibility : uint8_t
{
    none,
    bits_8,
    bits_16,
    bits_32,
    bits_64,
    bits_96,
    bits_128,
    bc3,
    bc5,
    bc7,
    eac_rg,
    astc_4x4,
};

constexpr FormatCompatibility format_compatibility(Format format) noexcept
{
    switch (format)
    {
    case Format::r8_srgb:
    case Format::r8_unorm:
    case Format::r8_uint: return FormatCompatibility::bits_8;
    case Format::rg8_srgb:
    case Format::rgba4_unorm:
    case Format::r5g5b5a1_unorm:
    case Format::r5g6b5_unorm:
    case Format::rg8_unorm:
    case Format::r16_unorm:
    case Format::rg8_uint:
    case Format::r16_uint:
    case Format::r16_float: return FormatCompatibility::bits_16;
    case Format::rgba8_srgb:
    case Format::bgra8_srgb:
    case Format::rgba8_unorm:
    case Format::bgra8_unorm:
    case Format::rg16_unorm:
    case Format::rgba8_uint:
    case Format::bgra8_uint:
    case Format::rg16_uint:
    case Format::r32_uint:
    case Format::rg16_float:
    case Format::r32_float:
    case Format::rgb10a2_unorm:
    case Format::rg11b10_float: return FormatCompatibility::bits_32;
    case Format::rgba16_unorm:
    case Format::rgba16_uint:
    case Format::rg32_uint:
    case Format::rgba16_float:
    case Format::rg32_float: return FormatCompatibility::bits_64;
    case Format::rgb32_uint:
    case Format::rgb32_float: return FormatCompatibility::bits_96;
    case Format::rgba32_uint:
    case Format::rgba32_float: return FormatCompatibility::bits_128;
    case Format::bc3_srgb:
    case Format::bc3_unorm: return FormatCompatibility::bc3;
    case Format::bc5_rg: return FormatCompatibility::bc5;
    case Format::bc7_srgb:
    case Format::bc7_unorm: return FormatCompatibility::bc7;
    case Format::eac_rg: return FormatCompatibility::eac_rg;
    case Format::astc_4x4_srgb:
    case Format::astc_4x4_unorm: return FormatCompatibility::astc_4x4;
    case Format::d16_unorm:
    case Format::d24_unorm_s8_uint:
    case Format::d32_float:
    case Format::s8_uint:
    case Format::d32_float_s8_uint:
    case Format::undefined: return FormatCompatibility::none;
    }
    return FormatCompatibility::none;
}

bool is_concrete_format(Format format) noexcept
{
    return get_texture_format_info(format).bytes_per_block != 0;
}

constexpr bool compatible_view_formats(Format image_format, Format view_format) noexcept
{
    if (image_format == view_format)
        return true;
    const FormatCompatibility image_class = format_compatibility(image_format);
    return image_class != FormatCompatibility::none &&
           image_class == format_compatibility(view_format);
}

static_assert(compatible_view_formats(Format::rgba8_unorm, Format::bgra8_srgb));
static_assert(compatible_view_formats(Format::rgba8_unorm, Format::rg16_float));
static_assert(compatible_view_formats(Format::r8_unorm, Format::r8_uint));
static_assert(compatible_view_formats(Format::astc_4x4_unorm, Format::astc_4x4_srgb));
static_assert(compatible_view_formats(Format::bc7_unorm, Format::bc7_srgb));
static_assert(!compatible_view_formats(Format::bc3_unorm, Format::bc7_unorm));
static_assert(!compatible_view_formats(Format::d32_float, Format::r32_float));

bool has_depth_aspect(Format format) noexcept
{
    return get_texture_format_info(format).depth;
}

bool has_stencil_aspect(Format format) noexcept
{
    return get_texture_format_info(format).stencil;
}

bool is_color_format(Format format) noexcept
{
    return is_concrete_format(format) && !has_depth_aspect(format) && !has_stencil_aspect(format);
}

VkImageAspectFlags image_aspects(Format format) noexcept
{
    VkImageAspectFlags result = 0;
    if (has_depth_aspect(format))
        result |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (has_stencil_aspect(format))
        result |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (result == 0)
        result = VK_IMAGE_ASPECT_COLOR_BIT;
    return result;
}

uint64_t format_bit(Format format) noexcept
{
    assert(format != Format::undefined && "undefined has no texture format bit");
    static_assert(format_count <= 64);
    return uint64_t{1} << static_cast<uint32_t>(format);
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
    assert(false && "unknown NoGraphicsAPI texture type");
    std::abort();
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
    assert(false && "unknown NoGraphicsAPI texture type");
    std::abort();
}

uint32_t physical_layer_count(const TextureDesc& desc) noexcept
{
    if (desc.type != TextureType::cube && desc.type != TextureType::cube_array)
        return desc.layer_count;
    const bool fits = desc.layer_count <=
                      std::numeric_limits<uint32_t>::max() / 6u;
    assert(fits && "cube texture layer count overflow");
    return desc.layer_count * 6u;
}

uint64_t image_resource_byte_size(const TextureDesc& desc,
                                       uint32_t physical_layers)
{
    uint64_t total = 0;
    auto width = desc.width;
    auto height = desc.height;
    auto depth = desc.depth;
    for (uint32_t mip = 0; mip < desc.mip_levels; ++mip)
    {
        uint64_t level_size = base_level_byte_size(desc.format, width, height, depth);
        bool fits = physical_layers == 0 ||
                    level_size <= std::numeric_limits<uint64_t>::max() /
                                      physical_layers;
        assert(fits && "texture array byte size overflow");
        level_size *= physical_layers;
        fits = total <= std::numeric_limits<uint64_t>::max() - level_size;
        assert(fits && "texture mip-chain byte size overflow");
        total += level_size;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
        depth = depth > 1 ? depth / 2 : 1;
    }
    return total;
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

constexpr uint32_t known_texture_usage_bits =
    static_cast<uint32_t>(TextureUsage::sampled) |
    static_cast<uint32_t>(TextureUsage::storage) |
    static_cast<uint32_t>(TextureUsage::color_attachment) |
    static_cast<uint32_t>(TextureUsage::depth_stencil_attachment) |
    static_cast<uint32_t>(TextureUsage::transfer_source) |
    static_cast<uint32_t>(TextureUsage::transfer_destination);

bool has_valid_texture_usage_bits(TextureUsage usage) noexcept
{
    const uint32_t bits = static_cast<uint32_t>(usage);
    return bits != 0 && (bits & ~known_texture_usage_bits) == 0;
}

enum class TextureCompression : uint8_t
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

VkFilter to_vk(Filter filter)
{
    switch (filter)
    {
    case Filter::nearest: return VK_FILTER_NEAREST;
    case Filter::linear: return VK_FILTER_LINEAR;
    }
    assert(false && "unknown texture filter");
    std::abort();
}

VkSamplerAddressMode to_vk(AddressMode mode)
{
    switch (mode)
    {
    case AddressMode::repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::mirrored_repeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::clamp_to_edge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
    assert(false && "unknown sampler address mode");
    std::abort();
}

VkCullModeFlags to_vk(CullMode cull)
{
    switch (cull)
    {
    case CullMode::none: return VK_CULL_MODE_NONE;
    case CullMode::clockwise: return VK_CULL_MODE_BACK_BIT;
    case CullMode::counter_clockwise: return VK_CULL_MODE_BACK_BIT;
    }
    assert(false && "unknown cull mode");
    std::abort();
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
    assert(false && "unknown blend factor");
    std::abort();
}

VkBlendOp to_vk(BlendOp operation)
{
    switch (operation)
    {
    case BlendOp::add: return VK_BLEND_OP_ADD;
    case BlendOp::subtract: return VK_BLEND_OP_SUBTRACT;
    case BlendOp::reverse_subtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::minimum: return VK_BLEND_OP_MIN;
    case BlendOp::maximum: return VK_BLEND_OP_MAX;
    }
    assert(false && "unknown blend operation");
    std::abort();
}

VkAttachmentLoadOp to_vk(LoadOp op)
{
    switch (op)
    {
    case LoadOp::load: return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::discard: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    assert(false && "unknown attachment load operation");
    std::abort();
}

VkAttachmentStoreOp to_vk(StoreOp op)
{
    switch (op)
    {
    case StoreOp::store: return VK_ATTACHMENT_STORE_OP_STORE;
    case StoreOp::discard: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    assert(false && "unknown attachment store operation");
    std::abort();
}

VkCompareOp to_vk(CompareOp op)
{
    switch (op)
    {
    case CompareOp::never: return VK_COMPARE_OP_NEVER;
    case CompareOp::less: return VK_COMPARE_OP_LESS;
    case CompareOp::equal: return VK_COMPARE_OP_EQUAL;
    case CompareOp::less_equal: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::greater: return VK_COMPARE_OP_GREATER;
    case CompareOp::not_equal: return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::greater_equal: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::always: return VK_COMPARE_OP_ALWAYS;
    }
    assert(false && "unknown depth/stencil comparison operation");
    std::abort();
}

VkStencilOp to_vk(StencilOp op)
{
    switch (op)
    {
    case StencilOp::keep: return VK_STENCIL_OP_KEEP;
    case StencilOp::zero: return VK_STENCIL_OP_ZERO;
    case StencilOp::replace: return VK_STENCIL_OP_REPLACE;
    case StencilOp::increment_clamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case StencilOp::decrement_clamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case StencilOp::invert: return VK_STENCIL_OP_INVERT;
    case StencilOp::increment_wrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case StencilOp::decrement_wrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    assert(false && "unknown stencil operation");
    std::abort();
}

constexpr VkPipelineStageFlags2 to_vk(Stage stages)
{
    constexpr auto known_bits = static_cast<uint64_t>(Stage::indirect) |
                                static_cast<uint64_t>(Stage::index_input) |
                                static_cast<uint64_t>(Stage::vertex) |
                                static_cast<uint64_t>(Stage::mesh) |
                                static_cast<uint64_t>(Stage::depth_stencil_tests) |
                                static_cast<uint64_t>(Stage::fragment) |
                                static_cast<uint64_t>(Stage::color_output) |
                                static_cast<uint64_t>(Stage::compute) |
                                static_cast<uint64_t>(Stage::transfer) |
                                static_cast<uint64_t>(Stage::host) |
                                static_cast<uint64_t>(Stage::all_commands);
    const auto bits = static_cast<uint64_t>(stages);
    const bool valid_bits = (bits & ~known_bits) == 0;
    assert(valid_bits && "pipeline stage mask contains unknown bits");

    VkPipelineStageFlags2 result = 0;
    if (has_flag(stages, Stage::indirect)) result |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    if (has_flag(stages, Stage::index_input)) result |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    if (has_flag(stages, Stage::vertex)) result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    if (has_flag(stages, Stage::mesh)) result |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
    if (has_flag(stages, Stage::depth_stencil_tests))
        result |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    if (has_flag(stages, Stage::fragment)) result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (has_flag(stages, Stage::color_output))
        result |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
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
    constexpr auto known_bits = static_cast<uint64_t>(Access::transfer_read) |
                                static_cast<uint64_t>(Access::transfer_write) |
                                static_cast<uint64_t>(Access::shader_read) |
                                static_cast<uint64_t>(Access::shader_write) |
                                static_cast<uint64_t>(Access::color_read) |
                                static_cast<uint64_t>(Access::color_write) |
                                static_cast<uint64_t>(Access::depth_stencil_read) |
                                static_cast<uint64_t>(Access::depth_stencil_write) |
                                static_cast<uint64_t>(Access::indirect_read) |
                                static_cast<uint64_t>(Access::index_read) |
                                static_cast<uint64_t>(Access::host_read) |
                                static_cast<uint64_t>(Access::descriptor_read);
    const bool valid_bits = (static_cast<uint64_t>(accesses) & ~known_bits) == 0;
    assert(valid_bits && "access mask contains unknown bits");
    VkAccessFlags2 result = 0;
    if (has_flag(accesses, Access::transfer_read)) result |= VK_ACCESS_2_TRANSFER_READ_BIT;
    if (has_flag(accesses, Access::transfer_write)) result |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (has_flag(accesses, Access::shader_read))
        result |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    if (has_flag(accesses, Access::shader_write)) result |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (has_flag(accesses, Access::color_read))
        result |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    if (has_flag(accesses, Access::color_write))
        result |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (has_flag(accesses, Access::depth_stencil_read))
        result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (has_flag(accesses, Access::depth_stencil_write))
        result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (has_flag(accesses, Access::indirect_read)) result |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    if (has_flag(accesses, Access::index_read)) result |= VK_ACCESS_2_INDEX_READ_BIT;
    if (has_flag(accesses, Access::host_read)) result |= VK_ACCESS_2_HOST_READ_BIT;
    if (has_flag(accesses, Access::descriptor_read))
        result |= VK_ACCESS_2_SAMPLER_HEAP_READ_BIT_EXT |
                  VK_ACCESS_2_RESOURCE_HEAP_READ_BIT_EXT;
    return result;
}

void validate_stage_access(Stage stages, Access accesses)
{
    if (accesses == Access::none)
        return;
    assert(stages != Stage::none &&
           "a non-zero access mask requires a non-zero pipeline stage mask");

    const auto all_commands = has_flag(stages, Stage::all_commands);
    const auto has_shader_stage = all_commands ||
                                  has_flag(stages, Stage::vertex) ||
                                  has_flag(stages, Stage::mesh) ||
                                  has_flag(stages, Stage::fragment) ||
                                  has_flag(stages, Stage::compute);
    const auto transfer_stage = all_commands || has_flag(stages, Stage::transfer);
    const bool compatible =
        (!has_flag(accesses, Access::transfer_read) || transfer_stage) &&
        (!has_flag(accesses, Access::transfer_write) || transfer_stage) &&
        (!has_flag(accesses, Access::shader_read) || has_shader_stage) &&
        (!has_flag(accesses, Access::shader_write) || has_shader_stage) &&
        (!has_flag(accesses, Access::descriptor_read) || has_shader_stage) &&
        (!has_flag(accesses, Access::color_read) || all_commands || has_flag(stages, Stage::color_output)) &&
        (!has_flag(accesses, Access::color_write) || all_commands || has_flag(stages, Stage::color_output)) &&
        (!has_flag(accesses, Access::depth_stencil_read) ||
         all_commands || has_flag(stages, Stage::depth_stencil_tests)) &&
        (!has_flag(accesses, Access::depth_stencil_write) ||
         all_commands || has_flag(stages, Stage::depth_stencil_tests)) &&
        (!has_flag(accesses, Access::indirect_read) || all_commands || has_flag(stages, Stage::indirect)) &&
        (!has_flag(accesses, Access::index_read) || all_commands || has_flag(stages, Stage::index_input)) &&
        (!has_flag(accesses, Access::host_read) || has_flag(stages, Stage::host));
    assert(compatible && "access is incompatible with the stage mask");
}

constexpr VkBufferUsageFlags universal_buffer_usage =
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

size_t memory_pool_index(MemoryType memory)
{
    switch (memory)
    {
    case MemoryType::cpu_visible: return 0;
    case MemoryType::gpu_only: return 1;
    case MemoryType::readback: return 2;
    default:
        assert(false && "invalid GPU memory type");
        std::abort();
    }
}

constexpr VkMemoryPropertyFlags cpu_visible_memory_properties =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

constexpr VkMemoryPropertyFlags forbidden_memory_properties =
    VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
    VK_MEMORY_PROPERTY_PROTECTED_BIT |
    VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
    VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;

bool is_usable_memory_type(const VkPhysicalDeviceMemoryProperties& properties,
                           uint32_t index)
{
    if (index >= properties.memoryTypeCount)
        return false;
    const auto& type = properties.memoryTypes[index];
    if ((type.propertyFlags & forbidden_memory_properties) != 0 ||
        type.heapIndex >= properties.memoryHeapCount)
    {
        return false;
    }
    return (properties.memoryHeaps[type.heapIndex].flags &
            VK_MEMORY_HEAP_TILE_MEMORY_BIT_QCOM) == 0;
}

bool has_cpu_visible_device_memory(const VkPhysicalDeviceMemoryProperties& properties, uint32_t heap_memory_block_size)
{
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
    {
        if (!is_usable_memory_type(properties, i))
            continue;
        const auto& type = properties.memoryTypes[i];
        if ((type.propertyFlags & cpu_visible_memory_properties) !=
            cpu_visible_memory_properties)
        {
            continue;
        }
        if (type.heapIndex < properties.memoryHeapCount &&
            (properties.memoryHeaps[type.heapIndex].flags &
             VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 &&
            properties.memoryHeaps[type.heapIndex].size >= heap_memory_block_size)
        {
            return true;
        }
    }
    return false;
}

bool has_gpu_only_device_memory(const VkPhysicalDeviceMemoryProperties& properties, uint32_t heap_memory_block_size)
{
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
    {
        if (!is_usable_memory_type(properties, i))
            continue;
        const auto& type = properties.memoryTypes[i];
        const auto flags = type.propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 &&
            type.heapIndex < properties.memoryHeapCount &&
            (properties.memoryHeaps[type.heapIndex].flags &
             VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 &&
            properties.memoryHeaps[type.heapIndex].size >= heap_memory_block_size)
        {
            return true;
        }
    }
    return false;
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
    VkDeviceSize size = 0;
};

struct AllocationHeap;
struct DescriptorAllocation;
struct ImageHeap;
struct CommandContext;
struct PresentContext;

struct FixedFunction8
{
    using Invoke = void (*)(const void*, Device&, uint64_t) noexcept;

    template<typename Callback>
    void set(const Callback& callback) noexcept
    {
        static_assert(sizeof(Callback) <= sizeof(storage));
        static_assert(alignof(Callback) <= alignof(uint64_t));
        static_assert(std::is_trivially_copyable_v<Callback>);
        static_assert(std::is_trivially_destructible_v<Callback>);
        static_assert(std::is_nothrow_invocable_v<const Callback&, Device&,
                                                   uint64_t>);
        ::new (static_cast<void*>(storage)) Callback(callback);
        invoke = [](const void* data, Device& device,
                    uint64_t payload) noexcept {
            (*static_cast<const Callback*>(data))(device, payload);
        };
    }

    void operator()(Device& device, uint64_t payload) const noexcept
    {
        assert(invoke);
        invoke(storage, device, payload);
    }

    void clear() noexcept
    {
        invoke = nullptr;
    }

    alignas(uint64_t) byte storage[sizeof(uint64_t)]{};
    Invoke invoke = nullptr;
};

static_assert(sizeof(FixedFunction8::storage) == 8);

struct DeferredDeletion
{
    uint64_t retire_value = 0;
    uint64_t payload = 0;
    FixedFunction8 callback;
};

struct DeletionQueue
{
    template<typename Callback>
    void push(uint64_t retire_value, uint64_t payload,
              const Callback& callback) noexcept
    {
        if (count == entries.size())
        {
            const size_t old_size = entries.size();
            entries.resize(old_size == 0 ? 1 : old_size * 2);
            for (size_t index = 0; index < first; ++index)
            {
                entries[old_size + index] = entries[index];
                entries[index] = {};
            }
        }
        if (count != 0)
        {
            const size_t back = (first + count - 1) % entries.size();
            assert(entries[back].retire_value <= retire_value &&
                   "deferred deletion retire values must be monotonic");
        }
        DeferredDeletion& entry = entries[(first + count) % entries.size()];
        entry.retire_value = retire_value;
        entry.payload = payload;
        entry.callback.set(callback);
        ++count;
    }

    void collect(Device& device, uint64_t completed_value) noexcept
    {
        while (count != 0 &&
               entries[first].retire_value <= completed_value)
        {
            DeferredDeletion& entry = entries[first];
            entry.callback(device, entry.payload);
            entry.callback.clear();
            entry.retire_value = 0;
            entry.payload = 0;
            first = (first + 1) % entries.size();
            --count;
        }
        if (count == 0)
            first = 0;
    }

    std::vector<DeferredDeletion> entries;
    size_t first = 0;
    size_t count = 0;
};

struct RetiredSwapchain
{
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkImageView views[max_swapchain_images]{};
    uint32_t view_count = 0;
    uint32_t pending_presents = 0;
};

enum class GpuAllocationOwnerKind : uint8_t
{
    allocation_heap,
    descriptor_allocation,
};

struct TextureInitialization
{
    VkImage image = VK_NULL_HANDLE;
    VkImageAspectFlags aspect_mask = 0;
    uint32_t mip_levels = 0;
    uint32_t array_layers = 0;
    bool initialized = false;
    TextureInitialization* previous = nullptr;
    TextureInitialization* next = nullptr;
    struct TextureInitializationList* owner = nullptr;
};

struct TextureInitializationList
{
    TextureInitialization* first = nullptr;
    TextureInitialization* last = nullptr;
};

void append_texture_initialization(TextureInitializationList& list,
                                   TextureInitialization& initialization) noexcept
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

void transfer_texture_initializations(TextureInitializationList& destination,
                                      TextureInitializationList& source) noexcept
{
    assert(!destination.first && !destination.last);
    destination = source;
    source = {};
    for (TextureInitialization* current = destination.first; current; current = current->next)
        current->owner = &destination;
}

} // namespace detail

struct Swapchain;

struct GpuAllocationOwner
{
    Device* state = nullptr;
    void* object = nullptr;
    detail::GpuAllocationOwnerKind kind =
        detail::GpuAllocationOwnerKind::allocation_heap;
};

struct TimelineSemaphore
{
    Device* state = nullptr;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t last_signal = 0;
};

struct CommandBuffer
{
    Device* state = nullptr;
    detail::CommandContext* context = nullptr;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    bool recording = false;
    bool rendering = false;
#if !defined(NDEBUG)
    Format rendering_color_formats[max_color_attachments]{};
    uint32_t rendering_color_count = 0;
    Format rendering_depth_format = Format::undefined;
    Format rendering_stencil_format = Format::undefined;
    bool rendering_has_depth = false;
    bool rendering_has_stencil = false;
    bool graphics_compatibility_dirty = true;
    bool graphics_compatible = false;
    const PSO* bound_graphics = nullptr;
    const PSO* bound_compute = nullptr;
#endif
    Swapchain* swapchain = nullptr;
    detail::TextureInitializationList pending_texture_initializations;
};

namespace detail
{

struct CommandContext
{
    CommandContext* next = nullptr;
    CommandContext* previous = nullptr;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    CommandBuffer commands{};
    uint64_t retire_value = 0;
    bool active = false;
};

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
    uint32_t queue_family = 0;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceProperties physical_properties{};
    VkPhysicalDeviceVulkan13Properties vulkan13_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heap_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    uint64_t max_timeline_value_difference = 0;
    uint32_t heap_memory_block_size = 0;
    detail::DeviceFunctions fn;
    DeviceCaps caps;
    VkFormatFeatureFlags2 format_features[format_count]{};
    bool image_cube_array = false;
    bool texture_compression_etc2 = false;
    detail::AllocationHeap* allocation_heaps[3]{};
    detail::AllocationHeap* active_allocation_heaps[3]{};
    detail::DescriptorAllocation* descriptor_allocations = nullptr;
    detail::ImageHeap* image_heaps = nullptr;
    detail::ImageHeap* active_image_heaps[VK_MAX_MEMORY_TYPES]{};
    detail::TextureInitializationList pending_texture_initializations;
    detail::CommandContext* next_command_context = nullptr;
    std::vector<VkCommandBufferSubmitInfo> command_submit_infos;
    VkSemaphore command_retirement = VK_NULL_HANDLE;
    uint64_t command_retirement_value = 0;
    uint64_t completed_command_retirement = 0;
    detail::DeletionQueue deletion_queue;
    detail::PresentContext present_contexts[max_swapchain_images]{};
    detail::RetiredSwapchain retired_swapchains[max_swapchain_images]{};
    Swapchain* swapchain = nullptr;
    Swapchain* acquired_swapchain = nullptr;
    uint32_t live_timeline_semaphores = 0;
    uint32_t active_command_buffers = 0;
    uint32_t present_context_count = 0;
    uint32_t next_present_context = 0;

    ~Device();

    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    void destroy_backing(detail::BackingBuffer& backing) const noexcept
    {
        if (!device)
            return;
        if (backing.mapped)
            vkUnmapMemory(device, backing.memory);
        if (backing.buffer)
            vkDestroyBuffer(device, backing.buffer, nullptr);
        if (backing.memory)
            free_memory(backing.memory);
        backing = {};
    }

    void allocate_memory(const VkMemoryAllocateInfo& info,
                         VkDeviceMemory& output) const noexcept
    {
        assert(info.memoryTypeIndex < memory_properties.memoryTypeCount);
        const auto heap_index = memory_properties.memoryTypes[info.memoryTypeIndex].heapIndex;
        assert(heap_index < memory_properties.memoryHeapCount);
        assert(info.allocationSize <= memory_properties.memoryHeaps[heap_index].size);
        require_vk(vkAllocateMemory(device, &info, nullptr, &output));
    }

    void free_memory(VkDeviceMemory memory) const noexcept
    {
        if (!memory)
            return;
        vkFreeMemory(device, memory, nullptr);
    }

    [[nodiscard]] bool find_memory_type(uint32_t bits,
                                        VkMemoryPropertyFlags required,
                                        VkMemoryPropertyFlags preferred,
                                        VkDeviceSize minimum_heap_size,
                                        uint32_t& output,
                                        VkMemoryPropertyFlags forbidden = 0) const noexcept
    {
        bool has_best = false;
        uint32_t best = 0;
        uint32_t best_score = 0;
        VkDeviceSize best_heap_size = 0;
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        {
            if ((bits & (1u << i)) == 0)
                continue;
            const auto flags = memory_properties.memoryTypes[i].propertyFlags;
            if ((flags & required) != required || (flags & forbidden) != 0)
                continue;
            if (!is_usable_memory_type(memory_properties, i))
                continue;
            const auto& heap = memory_properties.memoryHeaps[
                memory_properties.memoryTypes[i].heapIndex];
            if (heap.size < minimum_heap_size ||
                ((required & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
                 (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0))
            {
                continue;
            }
            const auto score = static_cast<uint32_t>(std::popcount(flags & preferred));
            if (!has_best || score > best_score ||
                (score == best_score && heap.size > best_heap_size))
            {
                best = i;
                has_best = true;
                best_score = score;
                best_heap_size = heap.size;
            }
        }
        if (!has_best)
            return false;
        output = best;
        return true;
    }

    void create_backing_buffer(
        detail::BackingBuffer& output,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred,
        VkMemoryPropertyFlags forbidden = 0) const noexcept
    {
        output = {};
        assert(size != 0);
        assert(size <= vulkan13_properties.maxBufferSize);
        detail::BackingBuffer result{
            .size = size,
        };
        const VkBufferCreateInfo buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        require_vk(vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer));

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
        uint32_t memory_type = 0;
        const bool has_memory_type = find_memory_type(
            requirements.memoryTypeBits, required, preferred,
            requirements.size, memory_type, forbidden);
        assert(has_memory_type);

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
        allocate_memory(allocate_info, result.memory);
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
        const bool valid_address = result.address != 0 && result.address % gpu_allocation_alignment == 0 &&
                                   (!result.mapped || reinterpret_cast<uintptr_t>(result.mapped) % gpu_allocation_alignment == 0);
        assert(valid_address && "GPU allocation backing has an invalid address or alignment");
        if (!valid_address)
            std::abort();
        output = result;
    }

    [[nodiscard]] GpuAllocation<> allocate_gpu(VkDeviceSize size, MemoryType memory) noexcept;
    [[nodiscard]] detail::AllocationHeap* create_allocation_heap(
        MemoryType memory) noexcept;
    [[nodiscard]] bool try_allocate_gpu(detail::AllocationHeap& heap,
                                        VkDeviceSize size,
                                        uint32_t unit_count,
                                        GpuAllocation<>& output) noexcept;
    [[nodiscard]] GpuAllocation<> allocate_descriptor_heap(VkDeviceSize size, MemoryType memory) noexcept;
    void release_allocation(const GpuAllocation<>& allocation) noexcept;
    [[nodiscard]] Error create_command_contexts() noexcept;
    [[nodiscard]] Error create_command_context(detail::CommandContext& context) noexcept;
    [[nodiscard]] Error grow_command_context_pool() noexcept;
    void destroy_command_context(detail::CommandContext& context) noexcept;
    void destroy_command_contexts() noexcept;
    void reset_command_context(detail::CommandContext& context) noexcept;
    [[nodiscard]] detail::CommandContext& acquire_command_context() noexcept;
    [[nodiscard]] uint64_t next_command_retirement() noexcept;
    void poll_command_retirement() noexcept;
    void wait_command_retirement(uint64_t value) noexcept;
    template<typename Callback>
    void defer_delete(const Callback& callback,
                      uint64_t payload = 0) noexcept
    {
        assert(active_command_buffers == 0 &&
               "resource destruction is not allowed while a command buffer is recording");
        poll_command_retirement();
        deletion_queue.push(command_retirement_value, payload, callback);
        deletion_queue.collect(*this, completed_command_retirement);
    }
    [[nodiscard]] Error create_present_context(detail::PresentContext& context) noexcept;
    void destroy_present_context(detail::PresentContext& context) noexcept;
    void finish_present_context(detail::PresentContext& context) noexcept;
    void wait_present_context(detail::PresentContext& context) noexcept;
    void poll_present_contexts() noexcept;
    void queue_retired_swapchain(detail::RetiredSwapchain& retired) noexcept;
    void drain_contexts() noexcept;

};

namespace detail
{

struct ImageHeap
{
    ImageHeap(Device* owner, uint32_t type)
        : state(owner),
          memory_type(type),
          offsets(owner->heap_memory_block_size / gpu_allocation_alignment, max_heap_suballocations)
    {}

    ~ImageHeap()
    {
        if (state && memory)
            state->free_memory(memory);
    }

    Device* state = nullptr;
    ImageHeap* next = nullptr;
    uint32_t memory_type = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    OffsetAllocator::Allocator offsets;
};

struct AllocationHeap
{
    AllocationHeap(Device* device, MemoryType type)
        : allocation_owner{
              .state = device,
              .object = this,
              .kind = GpuAllocationOwnerKind::allocation_heap,
          },
          state(device),
          memory(type),
          offsets(device->heap_memory_block_size / gpu_allocation_alignment, max_heap_suballocations)
    {}

    ~AllocationHeap()
    {
        if (state)
            state->destroy_backing(backing);
    }

    GpuAllocationOwner allocation_owner;
    Device* state = nullptr;
    AllocationHeap* next = nullptr;
    MemoryType memory;
    BackingBuffer backing;
    OffsetAllocator::Allocator offsets;
#if !defined(NDEBUG)
    uint64_t live_allocations[live_allocation_word_count]{};
#endif
};

struct DescriptorAllocation
{
    explicit DescriptorAllocation(Device* device)
        : allocation_owner{
              .state = device,
              .object = this,
              .kind = GpuAllocationOwnerKind::descriptor_allocation,
          },
          state(device)
    {}

    ~DescriptorAllocation()
    {
        if (state)
            state->destroy_backing(backing);
    }

    GpuAllocationOwner allocation_owner;
    Device* state = nullptr;
    DescriptorAllocation* previous = nullptr;
    DescriptorAllocation* next = nullptr;
    BackingBuffer backing;
    GpuAllocation<> value{};
};

} // namespace detail

namespace
{

void destroy_owned_swapchain(Device& device) noexcept;

}

Device::~Device()
{
    assert(live_timeline_semaphores == 0 &&
           "timeline semaphores must be destroyed before their device");
    assert(active_command_buffers == 0 &&
           "device destroyed while a command buffer is active");
    assert(!acquired_swapchain &&
           "device destroyed while a swapchain image is acquired");
    if (device)
    {
        drain_contexts();
        destroy_owned_swapchain(*this);
        deletion_queue.collect(*this, completed_command_retirement);
        assert(deletion_queue.count == 0);
    }
    destroy_command_contexts();
    for (uint32_t index = 0; index < present_context_count; ++index)
        destroy_present_context(present_contexts[index]);
    for (detail::AllocationHeap*& pool : allocation_heaps)
    {
        while (pool)
        {
            detail::AllocationHeap* heap = pool;
            pool = heap->next;
            delete heap;
        }
    }
    while (descriptor_allocations)
    {
        detail::DescriptorAllocation* allocation = descriptor_allocations;
        descriptor_allocations = allocation->next;
        delete allocation;
    }
    while (image_heaps)
    {
        detail::ImageHeap* heap = image_heaps;
        image_heaps = heap->next;
        delete heap;
    }
    if (device)
        vkDestroyDevice(device, nullptr);
    if (instance && surface)
        vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance && debug_messenger && destroy_debug_messenger)
        destroy_debug_messenger(instance, debug_messenger, nullptr);
    if (instance)
        vkDestroyInstance(instance, nullptr);
}

Error Device::create_command_contexts() noexcept
{
    assert(!next_command_context && !command_retirement &&
           command_submit_infos.empty());
    const VkSemaphoreTypeCreateInfo type_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type_info,
    };
    auto error = error_from_vk(vkCreateSemaphore(
        device, &semaphore_info, nullptr, &command_retirement));
    if (error != Error::none)
        return error;
    for (uint32_t index = 0;
         index < initial_command_context_count;
         ++index)
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

Error Device::create_command_context(detail::CommandContext& context) noexcept
{
    assert(!context.next && !context.previous && !context.command_pool &&
           !context.command_buffer && !context.commands.state &&
           context.retire_value == 0 && !context.active);
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = queue_family,
    };
    auto error = error_from_vk(vkCreateCommandPool(
        device, &pool_info, nullptr, &context.command_pool));
    if (error != Error::none)
        return error;
    const VkCommandBufferAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = context.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    error = error_from_vk(vkAllocateCommandBuffers(
        device, &allocate_info, &context.command_buffer));
    if (error != Error::none)
        destroy_command_context(context);
    return error;
}

Error Device::grow_command_context_pool() noexcept
{
    auto* context = new detail::CommandContext;
    const Error error = create_command_context(*context);
    if (error != Error::none)
    {
        delete context;
        return error;
    }
    command_submit_infos.push_back({});
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

void Device::destroy_command_context(detail::CommandContext& context) noexcept
{
    if (device && context.command_pool)
        vkDestroyCommandPool(device, context.command_pool, nullptr);
    context = {};
}

void Device::destroy_command_contexts() noexcept
{
    if (next_command_context)
        next_command_context->previous->next = nullptr;
    while (next_command_context)
    {
        detail::CommandContext* context = next_command_context;
        next_command_context = context->next;
        destroy_command_context(*context);
        delete context;
    }
    command_submit_infos.clear();
    if (device && command_retirement)
        vkDestroySemaphore(device, command_retirement, nullptr);
    command_retirement = VK_NULL_HANDLE;
    command_retirement_value = 0;
    completed_command_retirement = 0;
}

void Device::reset_command_context(detail::CommandContext& context) noexcept
{
    assert(context.command_pool && context.command_buffer && !context.active &&
           !context.commands.state &&
           context.retire_value <= completed_command_retirement);
    require_vk(vkResetCommandPool(device, context.command_pool, 0));
    context.retire_value = 0;
}

detail::CommandContext& Device::acquire_command_context() noexcept
{
    detail::CommandContext* context = next_command_context;
    assert(context);
    if (active_command_buffers == 0)
        poll_command_retirement();
    if (context->active ||
        context->retire_value > completed_command_retirement)
    {
        require_error(grow_command_context_pool());
        context = next_command_context->previous;
        assert(context != next_command_context &&
               context->next == next_command_context &&
               !context->active && context->retire_value == 0);
    }
    reset_command_context(*context);
    next_command_context = context->next;
    return *context;
}

namespace
{

uint64_t query_timeline_value(const TimelineSemaphore& semaphore) noexcept
{
    uint64_t value = 0;
    assert_vk(vkGetSemaphoreCounterValue(
        semaphore.state->device, semaphore.semaphore, &value));
    return value;
}

} // namespace

void Device::poll_command_retirement() noexcept
{
    if (command_retirement &&
        completed_command_retirement != command_retirement_value)
    {
        uint64_t completed = 0;
        require_vk(vkGetSemaphoreCounterValue(
            device, command_retirement, &completed));
        assert(completed >= completed_command_retirement &&
               completed <= command_retirement_value);
        completed_command_retirement = completed;
    }
    deletion_queue.collect(*this, completed_command_retirement);
}

void Device::wait_command_retirement(uint64_t value) noexcept
{
    assert(value <= command_retirement_value);
    if (value <= completed_command_retirement)
    {
        deletion_queue.collect(*this, completed_command_retirement);
        return;
    }
    const VkSemaphoreWaitInfo wait_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &command_retirement,
        .pValues = &value,
    };
    require_vk(vkWaitSemaphores(
        device, &wait_info, std::numeric_limits<uint64_t>::max()));
    completed_command_retirement = value;
    deletion_queue.collect(*this, completed_command_retirement);
}

uint64_t Device::next_command_retirement() noexcept
{
    assert(command_retirement_value !=
           std::numeric_limits<uint64_t>::max());
    const uint64_t next = command_retirement_value + 1;
    if (max_timeline_value_difference !=
            std::numeric_limits<uint64_t>::max() &&
        next - completed_command_retirement >
            max_timeline_value_difference)
    {
        poll_command_retirement();
        const bool within_timeline_limit =
            next - completed_command_retirement <=
            max_timeline_value_difference;
        assert(within_timeline_limit &&
               "private retirement timeline capacity exhausted");
        if (!within_timeline_limit)
        {
            std::abort();
        }
    }
    command_retirement_value = next;
    return next;
}

Error Device::create_present_context(detail::PresentContext& context) noexcept
{
    assert(!context.acquired && !context.rendered && !context.presented &&
           !context.swapchain && !context.present_pending);
    const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    auto error = error_from_vk(
        vkCreateSemaphore(device, &semaphore_info, nullptr, &context.acquired));
    if (error != Error::none)
        return error;
    error = error_from_vk(
        vkCreateSemaphore(device, &semaphore_info, nullptr, &context.rendered));
    if (error != Error::none)
    {
        destroy_present_context(context);
        return error;
    }
    const VkFenceCreateInfo fence_info{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    error = error_from_vk(
        vkCreateFence(device, &fence_info, nullptr, &context.presented));
    if (error != Error::none)
        destroy_present_context(context);
    return error;
}

void Device::destroy_present_context(detail::PresentContext& context) noexcept
{
    assert(!context.present_pending);
    if (device && context.acquired)
        vkDestroySemaphore(device, context.acquired, nullptr);
    if (device && context.rendered)
        vkDestroySemaphore(device, context.rendered, nullptr);
    if (device && context.presented)
        vkDestroyFence(device, context.presented, nullptr);
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
        assert(retired.pending_presents != 0);
        --retired.pending_presents;
        if (retired.pending_presents == 0)
            queue_retired_swapchain(retired);
        return;
    }
}

void Device::wait_present_context(detail::PresentContext& context) noexcept
{
    if (!context.present_pending)
        return;
    assert_vk(vkWaitForFences(
        device, 1, &context.presented, VK_TRUE,
        std::numeric_limits<uint64_t>::max()));
    context.present_pending = false;
    finish_present_context(context);
}

void Device::poll_present_contexts() noexcept
{
    for (uint32_t index = 0; index < present_context_count; ++index)
    {
        detail::PresentContext& context = present_contexts[index];
        if (!context.present_pending)
            continue;
        const VkResult result = vkGetFenceStatus(device, context.presented);
        if (result == VK_NOT_READY)
            continue;
        require_vk(result);
        context.present_pending = false;
        finish_present_context(context);
    }
}

void Device::queue_retired_swapchain(
    detail::RetiredSwapchain& retired) noexcept
{
    assert(retired.handle && retired.pending_presents == 0);
    for (uint32_t index = 0; index < retired.view_count; ++index)
    {
        const VkImageView view = retired.views[index];
        assert(view);
        defer_delete([view](Device& owner, uint64_t) noexcept {
            vkDestroyImageView(owner.device, view, nullptr);
        });
    }
    const VkSwapchainKHR handle = retired.handle;
    defer_delete([handle](Device& owner, uint64_t) noexcept {
        vkDestroySwapchainKHR(owner.device, handle, nullptr);
    });
    retired = {};
}

void Device::drain_contexts() noexcept
{
    wait_command_retirement(command_retirement_value);
    for (uint32_t index = 0; index < present_context_count; ++index)
        wait_present_context(present_contexts[index]);
    for (const detail::RetiredSwapchain& retired : retired_swapchains)
        assert(!retired.handle);
    deletion_queue.collect(*this, completed_command_retirement);
}

detail::AllocationHeap* Device::create_allocation_heap(MemoryType memory) noexcept
{
    VkMemoryPropertyFlags required = 0;
    VkMemoryPropertyFlags preferred = 0;
    VkMemoryPropertyFlags forbidden = 0;
    switch (memory)
    {
    case MemoryType::cpu_visible:
        required = cpu_visible_memory_properties;
        break;
    case MemoryType::gpu_only:
        required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        forbidden = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        break;
    case MemoryType::readback:
        required = cpu_visible_memory_properties;
        preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
    default:
        assert(false && "gpu_malloc received an invalid memory type");
        std::abort();
    }

    detail::AllocationHeap* heap = new detail::AllocationHeap(this, memory);
    create_backing_buffer(
        heap->backing,
        heap_memory_block_size,
        universal_buffer_usage,
        required,
        preferred,
        forbidden);
    return heap;
}

bool Device::try_allocate_gpu(detail::AllocationHeap& heap,
                              VkDeviceSize size,
                              uint32_t unit_count,
                              GpuAllocation<>& output) noexcept
{
    const OffsetAllocator::Allocation token = heap.offsets.allocate(unit_count);
    if (token.offset == OffsetAllocator::Allocation::NO_SPACE)
        return false;
    const VkDeviceSize offset = static_cast<VkDeviceSize>(token.offset) * gpu_allocation_alignment;
    assert(token.metadata < max_heap_suballocations &&
           offset < heap.backing.size &&
           "OffsetAllocator returned invalid allocation metadata");
    assert(offset <= heap.backing.size &&
           size <= heap.backing.size - offset &&
           "OffsetAllocator returned an invalid allocation range");

    byte* cpu_pointer = nullptr;
    if (heap.backing.mapped)
    {
        const uintptr_t host_address =
            reinterpret_cast<uintptr_t>(heap.backing.mapped) + offset;
        assert(host_address % gpu_allocation_alignment == 0 && "mapped GPU allocation is not 16-byte aligned");
        cpu_pointer = reinterpret_cast<byte*>(host_address);
    }
    const VkDeviceAddress gpu_address = heap.backing.address + offset;
    assert(gpu_address % gpu_allocation_alignment == 0 && "GPU allocation is not 16-byte aligned");

    const uint64_t raw_token =
        (static_cast<uint64_t>(token.metadata) << 32) |
        static_cast<uint64_t>(token.offset);
    assert(raw_token != std::numeric_limits<uint64_t>::max());
#if !defined(NDEBUG)
    const uint32_t live_word = token.metadata / 64u;
    const uint64_t live_bit = uint64_t{1} << (token.metadata % 64u);
    assert((heap.live_allocations[live_word] & live_bit) == 0 &&
           "OffsetAllocator returned metadata for a live allocation");
    heap.live_allocations[live_word] |= live_bit;
#endif
    output = {
        .cpu = cpu_pointer,
        .gpu = reinterpret_cast<byte*>(static_cast<uintptr_t>(gpu_address)),
        .size = size,
        .allocation_owner = &heap.allocation_owner,
        .allocation_token = raw_token + 1,
    };
    return true;
}

GpuAllocation<> Device::allocate_gpu(VkDeviceSize size, MemoryType memory) noexcept
{
    if (size > heap_memory_block_size)
    {
        assert(false && "gpu_malloc request exceeds the configured heap memory block size");
        return {};
    }

    const uint32_t unit_count = allocation_unit_count(size);
    const auto pool_index = memory_pool_index(memory);
    detail::AllocationHeap*& pool = allocation_heaps[pool_index];
    detail::AllocationHeap*& active = active_allocation_heaps[pool_index];
    assert((!active || (active->state == this && active->memory == memory)) && "active GPU allocation heap is invalid");
    if (active)
    {
        GpuAllocation<> allocation{};
        if (try_allocate_gpu(*active, size, unit_count, allocation))
        {
            return allocation;
        }
    }
    for (detail::AllocationHeap* heap = pool; heap; heap = heap->next)
    {
        if (heap == active)
            continue;
        GpuAllocation<> allocation{};
        if (try_allocate_gpu(*heap, size, unit_count, allocation))
        {
            active = heap;
            return allocation;
        }
    }

    detail::AllocationHeap* heap = create_allocation_heap(memory);
    GpuAllocation<> allocation{};
    if (!try_allocate_gpu(*heap, size, unit_count, allocation))
    {
        delete heap;
        return {};
    }
    heap->next = pool;
    pool = heap;
    active = heap;
    return allocation;
}

GpuAllocation<> Device::allocate_descriptor_heap(VkDeviceSize size, MemoryType memory) noexcept
{
    assert(memory == MemoryType::texture_heap || memory == MemoryType::sampler_heap);
    const bool texture_heap = memory == MemoryType::texture_heap;
    const auto resource_alignment = heap_properties.imageDescriptorAlignment > heap_properties.bufferDescriptorAlignment
                                        ? heap_properties.imageDescriptorAlignment
                                        : heap_properties.bufferDescriptorAlignment;
    const auto reserved_alignment = texture_heap ? resource_alignment : heap_properties.samplerDescriptorAlignment;
    const auto heap_alignment =
        texture_heap ? heap_properties.resourceHeapAlignment : heap_properties.samplerHeapAlignment;
    const auto reserved_size =
        texture_heap ? heap_properties.minResourceHeapReservedRange : heap_properties.minSamplerHeapReservedRange;
    const auto maximum_size = texture_heap ? heap_properties.maxResourceHeapSize : heap_properties.maxSamplerHeapSize;

    assert(size <= maximum_size);
    assert(size <= std::numeric_limits<VkDeviceSize>::max() - (reserved_alignment - 1));

    const auto reserved_offset = align_up(size, reserved_alignment);
    assert(reserved_offset <= maximum_size);
    assert(reserved_size <= maximum_size - reserved_offset);
    const auto bind_size = reserved_offset + reserved_size;
    const auto allocation_alignment = heap_alignment > gpu_allocation_alignment ? heap_alignment : gpu_allocation_alignment;
    const auto alignment_padding = allocation_alignment - 1;
    assert(bind_size <= vulkan13_properties.maxBufferSize);
    assert(alignment_padding <= vulkan13_properties.maxBufferSize - bind_size);
    const auto backing_size = bind_size + alignment_padding;

    detail::DescriptorAllocation* allocation = new detail::DescriptorAllocation(this);
    create_backing_buffer(allocation->backing, backing_size,
                          universal_buffer_usage | VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT,
                          cpu_visible_memory_properties, 0);

    const auto gpu_address = align_up(allocation->backing.address, allocation_alignment);
    const auto allocation_offset = gpu_address - allocation->backing.address;
    const auto cpu_address = reinterpret_cast<uintptr_t>(allocation->backing.mapped) + allocation_offset;
    assert(allocation->backing.mapped);
    assert(gpu_address % heap_alignment == 0);
    assert(gpu_address % gpu_allocation_alignment == 0);
    assert(cpu_address % gpu_allocation_alignment == 0);
    assert(allocation_offset <= allocation->backing.size);
    assert(bind_size <= allocation->backing.size - allocation_offset);

    allocation->value = {
        .cpu = reinterpret_cast<byte*>(cpu_address),
        .gpu = reinterpret_cast<byte*>(static_cast<uintptr_t>(gpu_address)),
        .size = size,
        .allocation_owner = &allocation->allocation_owner,
    };
    const auto result = allocation->value;
    allocation->next = descriptor_allocations;
    if (allocation->next)
        allocation->next->previous = allocation;
    descriptor_allocations = allocation;
    return result;
}

void Device::release_allocation(const GpuAllocation<>& allocation) noexcept
{
    if (!allocation.gpu)
    {
        assert(!allocation.cpu && allocation.size == 0 &&
               !allocation.allocation_owner && allocation.allocation_token == 0 &&
               "gpu_free received an invalid empty allocation");
        return;
    }

    const GpuAllocationOwner* owner = allocation.allocation_owner;
    assert(owner && owner->state == this && owner->object &&
           "gpu_free requires a live allocation returned by gpu_malloc");
    assert(active_command_buffers == 0 &&
           "gpu_free is not allowed while a command buffer is recording");

    if (owner->kind == detail::GpuAllocationOwnerKind::descriptor_allocation)
    {
        auto* record = static_cast<detail::DescriptorAllocation*>(
            owner->object);
#if !defined(NDEBUG)
        assert(&record->allocation_owner == owner && record->state == this &&
               record->value.cpu == allocation.cpu &&
               record->value.gpu == allocation.gpu &&
               record->value.size == allocation.size &&
               record->value.allocation_owner == owner &&
               record->value.allocation_token == 0 &&
               allocation.allocation_token == 0 &&
               "gpu_free allocation fields do not match the original gpu_malloc result");
#endif
        if (record->previous)
            record->previous->next = record->next;
        else
            descriptor_allocations = record->next;
        if (record->next)
            record->next->previous = record->previous;
        record->previous = nullptr;
        record->next = nullptr;
        record->allocation_owner = {};
        record->value = {};
        defer_delete(
            [record](Device&, uint64_t) noexcept {
                delete record;
            });
        return;
    }

    assert(owner->kind == detail::GpuAllocationOwnerKind::allocation_heap &&
           allocation.allocation_token != 0 &&
           "gpu_free received invalid allocation ownership metadata");
    auto* heap = static_cast<detail::AllocationHeap*>(
        owner->object);
    assert(&heap->allocation_owner == owner && heap->state == this &&
           (heap->memory == MemoryType::cpu_visible ||
            heap->memory == MemoryType::gpu_only ||
            heap->memory == MemoryType::readback) &&
           "gpu_free allocation owner does not belong to this device");

    const uint64_t raw_token = allocation.allocation_token - 1;
    const OffsetAllocator::Allocation offset_allocation{
        .offset = static_cast<uint32_t>(raw_token),
        .metadata = static_cast<OffsetAllocator::NodeIndex>(raw_token >> 32),
    };
#if !defined(NDEBUG)
    const VkDeviceSize offset = static_cast<VkDeviceSize>(offset_allocation.offset) * gpu_allocation_alignment;
    assert(offset_allocation.offset != OffsetAllocator::Allocation::NO_SPACE &&
           offset < heap->backing.size &&
           offset_allocation.metadata < max_heap_suballocations &&
           "gpu_free received an invalid allocation token");
    const uint32_t live_word = offset_allocation.metadata / 64u;
    const uint64_t live_bit = uint64_t{1} <<
                                   (offset_allocation.metadata % 64u);
    assert((heap->live_allocations[live_word] & live_bit) != 0 &&
           "gpu_free received an allocation that is not live");

    const uint32_t unit_count = heap->offsets.allocationSize(offset_allocation);
    const bool valid_size = allocation.size != 0 && allocation.size <= heap->backing.size && allocation_unit_count(allocation.size) == unit_count;
    const VkDeviceAddress expected_address = heap->backing.address + offset;
    const bool valid_address = valid_size && offset <= heap->backing.size && allocation.size <= heap->backing.size - offset &&
                               expected_address == static_cast<VkDeviceAddress>(reinterpret_cast<uintptr_t>(allocation.gpu));
    byte* expected_cpu = nullptr;
    if (valid_address && heap->backing.mapped)
    {
        expected_cpu = reinterpret_cast<byte*>(
            reinterpret_cast<uintptr_t>(heap->backing.mapped) +
            static_cast<uintptr_t>(
                expected_address - heap->backing.address));
    }
    assert(valid_address && expected_cpu == allocation.cpu &&
           "gpu_free allocation fields do not match the original gpu_malloc result");
#endif

#if !defined(NDEBUG)
    heap->live_allocations[live_word] &= ~live_bit;
#endif
    defer_delete(
        [heap](Device& owner_device, uint64_t token) noexcept {
            const OffsetAllocator::Allocation retired{
                .offset = static_cast<uint32_t>(token),
                .metadata =
                    static_cast<OffsetAllocator::NodeIndex>(token >> 32),
            };
            heap->offsets.free(retired);
            owner_device.active_allocation_heaps[
                memory_pool_index(heap->memory)] = heap;
        },
        raw_token);
}

struct Texture
{
    Device* state = nullptr;
    VkImage image = VK_NULL_HANDLE;
    detail::ImageHeap* heap = nullptr;
    OffsetAllocator::Allocation heap_allocation{};
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint32_t mip_levels = 0;
    uint32_t layer_count = 0;
    uint32_t array_layers = 0;
    uint64_t sampled_view_format_mask = 0;
    uint64_t storage_view_format_mask = 0;
    TextureType type = TextureType::two_d;
    Format format = Format::rgba8_unorm;
    TextureUsage usage = TextureUsage::none;
    bool owns_image = true;
    bool swapchain_image = false;
    bool owns_heap_allocation = false;
    uint32_t live_render_views = 0;
    detail::TextureInitialization initialization;

    ~Texture()
    {
        if (!state)
            return;
        assert(live_render_views == 0 &&
               "render views must be destroyed before their texture");
        if (initialization.owner)
            detail::remove_texture_initialization(initialization);
        if (image && owns_image)
            vkDestroyImage(state->device, image, nullptr);
        if (owns_heap_allocation)
        {
            assert(heap);
            const bool valid_heap = heap && heap->state == state &&
                                    heap->memory_type < VK_MAX_MEMORY_TYPES;
            assert(valid_heap && "texture image heap ownership is invalid");
            heap->offsets.free(heap_allocation);
            state->active_image_heaps[heap->memory_type] = heap;
        }
    }
};

struct RenderView
{
    Device* state = nullptr;
    Texture* texture = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    bool swapchain_view = false;
};

struct Swapchain
{
    Device* state = nullptr;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    Texture textures[max_swapchain_images]{};
    RenderView render_views[max_swapchain_images]{};
    bool initialized[max_swapchain_images]{};
    uint32_t image_count = 0;
    uint32_t image_index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::bgra8_srgb;
    VkSurfaceTransformFlagBitsKHR transform =
        VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkCompositeAlphaFlagBitsKHR composite_alpha =
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    detail::PresentContext* present_context = nullptr;
    CommandBuffer* transition_commands = nullptr;
    bool acquired = false;
    bool recreate_required = false;
    bool extent_queried = false;

    Swapchain() = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
};

struct PSO
{
    enum class Kind : uint8_t
    {
        vertex_graphics,
        mesh_graphics,
        compute,
    };

    Device* state = nullptr;
    VkPipeline pso = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
#if !defined(NDEBUG)
    Format color_formats[max_color_attachments]{};
    uint32_t color_count = 0;
    Format depth_format = Format::undefined;
    Format stencil_format = Format::undefined;
    Kind kind = Kind::vertex_graphics;
#endif

    ~PSO()
    {
        if (state && pso)
            vkDestroyPipeline(state->device, pso, nullptr);
    }
};

struct AddressRange
{
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
};

namespace
{

void record_image_barriers(VkCommandBuffer command_buffer,
                           Span<const VkImageMemoryBarrier2> barriers) noexcept
{
    const bool valid = command_buffer && barriers.data && barriers.size != 0 &&
                       barriers.size <= std::numeric_limits<uint32_t>::max();
    assert(valid && "image barrier batch is invalid");
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size),
        .pImageMemoryBarriers = barriers.data,
    };
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

void make_heap_bind_info(GpuRange heap,
                         VkDeviceSize heap_alignment,
                         VkDeviceSize reserved_alignment,
                         VkDeviceSize reserved_size,
                         VkDeviceSize maximum_size,
                         VkBindHeapInfoEXT& output) noexcept
{
    assert(heap.gpu && heap.size != 0 &&
           "descriptor heap range must be non-empty");
    assert(heap.size <= std::numeric_limits<VkDeviceSize>::max() -
                            (reserved_alignment - 1));
    const auto address = static_cast<VkDeviceAddress>(
        reinterpret_cast<uintptr_t>(heap.gpu));
    const auto reserved_offset = align_up<VkDeviceSize>(heap.size, reserved_alignment);
    assert(reserved_offset <= maximum_size &&
           reserved_size <= maximum_size - reserved_offset &&
           "descriptor heap size exceeds the implementation limit");
    const auto total_size = reserved_offset + reserved_size;
    assert(address % heap_alignment == 0 &&
           address <= std::numeric_limits<VkDeviceAddress>::max() - total_size &&
           "descriptor heap address or size is invalid");
    output = {
        .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .heapRange = {
            .address = address,
            .size = total_size,
        },
        .reservedRangeOffset = reserved_offset,
        .reservedRangeSize = reserved_size,
    };
}

Error enumerate_device_extensions(VkPhysicalDevice physical_device,
                                  Span<VkExtensionProperties> values,
                                  uint32_t& count) noexcept
{
    for (;;)
    {
        uint32_t available = 0;
        auto error = error_from_vk(
            vkEnumerateDeviceExtensionProperties(
                physical_device, nullptr, &available, nullptr));
        if (error != Error::none)
            return error;
        if (available > values.size)
            return Error::unsupported;
        count = available;
        const auto result = vkEnumerateDeviceExtensionProperties(
            physical_device, nullptr, &count, values.data);
        if (result == VK_INCOMPLETE)
            continue;
        error = error_from_vk(result);
        if (error != Error::none)
            return error;
        return Error::none;
    }
}

Error enumerate_instance_extensions(Span<VkExtensionProperties> values,
                                    uint32_t& count) noexcept
{
    for (;;)
    {
        uint32_t available = 0;
        auto error = error_from_vk(
            vkEnumerateInstanceExtensionProperties(nullptr, &available, nullptr));
        if (error != Error::none)
            return error;
        if (available > values.size)
            return Error::unsupported;
        count = available;
        const auto result =
            vkEnumerateInstanceExtensionProperties(nullptr, &count, values.data);
        if (result == VK_INCOMPLETE)
            continue;
        error = error_from_vk(result);
        if (error != Error::none)
            return error;
        return Error::none;
    }
}

#if !defined(NDEBUG)
Error enumerate_instance_layers(Span<VkLayerProperties> values,
                                uint32_t& count) noexcept
{
    for (;;)
    {
        uint32_t available = 0;
        auto error = error_from_vk(
            vkEnumerateInstanceLayerProperties(&available, nullptr));
        if (error != Error::none)
            return error;
        if (available > values.size)
            return Error::unsupported;
        count = available;
        const auto result = vkEnumerateInstanceLayerProperties(&count, values.data);
        if (result == VK_INCOMPLETE)
            continue;
        error = error_from_vk(result);
        if (error != Error::none)
            return error;
        return Error::none;
    }
}
#endif

struct QueriedFeatures
{
    VkPhysicalDeviceFeatures2 core{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features vulkan11{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features vulkan12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features vulkan13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features vulkan14{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptor_heap{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR address_commands{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR};
    VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untyped_pointers{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR};
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unified_image_layouts{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapchain_maintenance1{
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};

    explicit QueriedFeatures(bool presentation, bool include_unified_image_layouts)
    {
        core.pNext = &vulkan11;
        vulkan11.pNext = &vulkan12;
        vulkan12.pNext = &vulkan13;
        vulkan13.pNext = &vulkan14;
        vulkan14.pNext = &descriptor_heap;
        descriptor_heap.pNext = &address_commands;
        address_commands.pNext = &untyped_pointers;
        untyped_pointers.pNext = include_unified_image_layouts
            ? static_cast<void*>(&unified_image_layouts)
            : static_cast<void*>(&mesh_shader);
        unified_image_layouts.pNext = &mesh_shader;
        mesh_shader.pNext =
            presentation ? &swapchain_maintenance1 : nullptr;
    }
};

struct Candidate
{
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkPhysicalDeviceProperties properties{};
    bool unified_image_layouts = false;
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heap_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    VkPhysicalDeviceVulkan11Properties vulkan11_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties vulkan12_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties vulkan13_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
};

Error inspect_candidate(VkPhysicalDevice physical_device,
                        VkSurfaceKHR surface,
                        uint32_t heap_memory_block_size,
                        Candidate& output) noexcept
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);

    VkExtensionProperties extensions[max_device_extensions]{};
    uint32_t extension_count = 0;
    const auto extension_error = enumerate_device_extensions(
        physical_device, {extensions, max_device_extensions}, extension_count);
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
    if (surface &&
        (!has_name({extensions, extension_count},
                   VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
         !has_name({extensions, extension_count},
                   VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)))
    {
        return Error::unsupported;
    }
    if (properties.apiVersion < VK_API_VERSION_1_4)
        return Error::unsupported;

    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    if (!has_cpu_visible_device_memory(memory_properties, heap_memory_block_size))
        return Error::unsupported;
    if (!has_gpu_only_device_memory(memory_properties, heap_memory_block_size))
        return Error::unsupported;

    QueriedFeatures features(
        surface != VK_NULL_HANDLE, unified_image_layouts_extension);
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
        features.vulkan14.maintenance5 == VK_TRUE &&
        features.descriptor_heap.descriptorHeap == VK_TRUE &&
        features.address_commands.deviceAddressCommands == VK_TRUE &&
        features.untyped_pointers.shaderUntypedPointers == VK_TRUE &&
        features.mesh_shader.meshShader == VK_TRUE &&
        (features.core.features.textureCompressionBC == VK_TRUE ||
         features.core.features.textureCompressionASTC_LDR == VK_TRUE) &&
        (!surface ||
         features.swapchain_maintenance1.swapchainMaintenance1 == VK_TRUE);
    if (!required_features)
        return Error::unsupported;

    uint32_t available_queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &available_queue_count, nullptr);
    if (available_queue_count > max_queue_families)
        return Error::unsupported;
    VkQueueFamilyProperties queues[max_queue_families]{};
    uint32_t queue_count = available_queue_count;
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &queue_count, queues);
    uint32_t queue_family = queue_count;
    constexpr auto required_queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (uint32_t index = 0; index < queue_count; ++index)
    {
        VkBool32 presentation_supported = VK_TRUE;
        if (surface)
        {
            const Error presentation_error = error_from_vk(
                vkGetPhysicalDeviceSurfaceSupportKHR(
                    physical_device, index, surface,
                    &presentation_supported));
            if (presentation_error != Error::none)
                return presentation_error;
        }
        if (queues[index].queueCount != 0 &&
            (queues[index].queueFlags & required_queue_flags) ==
                required_queue_flags &&
            presentation_supported == VK_TRUE)
        {
            queue_family = index;
            break;
        }
    }
    if (queue_family == queue_count)
        return Error::unsupported;

    Candidate result{
        .physical_device = physical_device,
        .queue_family = queue_family,
        .properties = properties,
        .unified_image_layouts =
            unified_image_layouts_extension &&
            features.unified_image_layouts.unifiedImageLayouts == VK_TRUE,
    };

    result.heap_properties.pNext = &result.vulkan11_properties;
    result.vulkan11_properties.pNext = &result.vulkan12_properties;
    result.vulkan12_properties.pNext = &result.vulkan13_properties;
    result.vulkan13_properties.pNext = &result.mesh_properties;
    VkPhysicalDeviceProperties2 properties2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &result.heap_properties,
    };
    vkGetPhysicalDeviceProperties2(physical_device, &properties2);
    result.heap_properties.pNext = nullptr;
    result.vulkan11_properties.pNext = nullptr;
    result.vulkan12_properties.pNext = nullptr;
    result.vulkan13_properties.pNext = nullptr;
    const auto& heap = result.heap_properties;
    const auto resource_descriptor_alignment =
        heap.imageDescriptorAlignment > heap.bufferDescriptorAlignment
            ? heap.imageDescriptorAlignment
            : heap.bufferDescriptorAlignment;
    const bool valid_heap_properties =
        resource_descriptor_alignment != 0 &&
        (resource_descriptor_alignment & (resource_descriptor_alignment - 1)) == 0 &&
        heap.samplerDescriptorAlignment != 0 &&
        (heap.samplerDescriptorAlignment & (heap.samplerDescriptorAlignment - 1)) == 0 &&
        heap.resourceHeapAlignment != 0 &&
        (heap.resourceHeapAlignment & (heap.resourceHeapAlignment - 1)) == 0 &&
        heap.samplerHeapAlignment != 0 &&
        (heap.samplerHeapAlignment & (heap.samplerHeapAlignment - 1)) == 0 &&
        heap.imageDescriptorSize != 0 && heap.bufferDescriptorSize != 0 &&
        heap.samplerDescriptorSize != 0 &&
        heap.minResourceHeapReservedRange <= heap.maxResourceHeapSize &&
        heap.minSamplerHeapReservedRange <= heap.maxSamplerHeapSize;
    if (result.vulkan11_properties.maxMemoryAllocationSize < heap_memory_block_size ||
        result.vulkan13_properties.maxBufferSize < heap_memory_block_size ||
        !valid_heap_properties)
        return Error::unsupported;
    output = result;
    return Error::none;
}

Error enumerate_physical_devices(VkInstance instance,
                                 Span<VkPhysicalDevice> values,
                                 uint32_t& count) noexcept
{
    for (;;)
    {
        uint32_t available = 0;
        Error error = error_from_vk(
            vkEnumeratePhysicalDevices(instance, &available, nullptr));
        if (error != Error::none)
            return error;
        if (available > values.size)
            return Error::unsupported;
        count = available;
        const VkResult result = vkEnumeratePhysicalDevices(
            instance, &count, values.data);
        if (result == VK_INCOMPLETE)
            continue;
        error = error_from_vk(result);
        if (error != Error::none)
            return error;
        return Error::none;
    }
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
    const bool valid_desc = desc.heap_memory_block_size != 0 && desc.heap_memory_block_size % gpu_allocation_alignment == 0 &&
                            desc.desired_swapchain_image_count != 0 &&
                            desc.desired_swapchain_image_count <= max_swapchain_images &&
                            (presentation ? is_color_format(desc.swapchain_format) : desc.swapchain_format == Format::undefined);
    assert(valid_desc && "create_device requires a non-zero 16-byte-multiple heap memory block size, a swapchain image count in [1, 8], and a color "
                         "swapchain format exactly when a window is supplied");
    if (!valid_desc)
        return {.error = Error::unsupported};
#if !defined(_WIN32)
    if (presentation)
        return {.error = Error::unsupported};
#endif

    uint32_t loader_version = VK_API_VERSION_1_0;
    auto error = error_from_vk(vkEnumerateInstanceVersion(&loader_version));
    if (error != Error::none)
        return {
            .error = error,
        };
    if (loader_version < VK_API_VERSION_1_4)
        return {
            .error = Error::unsupported,
        };

    auto* state = new Device;
    state->heap_memory_block_size = desc.heap_memory_block_size;
    state->present_context_count = presentation ? desc.desired_swapchain_image_count : 0;
    VkExtensionProperties instance_extensions[max_instance_extensions]{};
    uint32_t instance_extension_count = 0;
    error = enumerate_instance_extensions(
        {instance_extensions, max_instance_extensions}, instance_extension_count);
    if (error != Error::none)
        return fail_device_creation(state, error);
#if defined(_WIN32)
    if (presentation &&
        (!has_name({instance_extensions, instance_extension_count},
                   VK_KHR_SURFACE_EXTENSION_NAME) ||
         !has_name({instance_extensions, instance_extension_count},
                   VK_KHR_WIN32_SURFACE_EXTENSION_NAME) ||
         !has_name({instance_extensions, instance_extension_count},
                   VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) ||
         !has_name({instance_extensions, instance_extension_count},
                   VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME)))
    {
        return fail_device_creation(state, Error::unsupported);
    }
#endif
#if !defined(NDEBUG)
    VkLayerProperties layers[max_instance_layers]{};
    uint32_t layer_count = 0;
    error = enumerate_instance_layers(
        {layers, max_instance_layers}, layer_count);
    if (error != Error::none)
        return fail_device_creation(state, error);
    const bool debug_utils_available =
        has_name({instance_extensions, instance_extension_count},
                 VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const bool validation_available =
        has_name({layers, layer_count}, "VK_LAYER_KHRONOS_validation");
#endif

    const char* enabled_instance_extensions[5]{};
    uint32_t enabled_instance_extension_count = 0;
    const char* enabled_layers[1]{};
    uint32_t enabled_layer_count = 0;
#if !defined(NDEBUG)
    if (debug_utils_available)
        enabled_instance_extensions[enabled_instance_extension_count++] =
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (validation_available)
        enabled_layers[enabled_layer_count++] = "VK_LAYER_KHRONOS_validation";
#endif
#if defined(_WIN32)
    if (presentation)
    {
        enabled_instance_extensions[enabled_instance_extension_count++] =
            VK_KHR_SURFACE_EXTENSION_NAME;
        enabled_instance_extensions[enabled_instance_extension_count++] =
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
        enabled_instance_extensions[enabled_instance_extension_count++] =
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
        enabled_instance_extensions[enabled_instance_extension_count++] =
            VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
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
        .ppEnabledExtensionNames = enabled_instance_extension_count
            ? enabled_instance_extensions
            : nullptr,
    };
    error = error_from_vk(vkCreateInstance(&instance_info, nullptr, &state->instance));
    if (error != Error::none)
        return fail_device_creation(state, error);

#if !defined(NDEBUG)
    if (debug_utils_available)
    {
        const auto create_debug = load_instance_proc<PFN_vkCreateDebugUtilsMessengerEXT>(
            state->instance, "vkCreateDebugUtilsMessengerEXT");
        state->destroy_debug_messenger =
            load_instance_proc<PFN_vkDestroyDebugUtilsMessengerEXT>(
                state->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (!create_debug || !state->destroy_debug_messenger)
            return fail_device_creation(state, Error::driver_error);
        const VkDebugUtilsMessengerCreateInfoEXT debug_info{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_callback,
        };
        error = error_from_vk(
            create_debug(state->instance, &debug_info, nullptr, &state->debug_messenger));
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
        error = error_from_vk(vkCreateWin32SurfaceKHR(
            state->instance, &surface_info, nullptr, &state->surface));
        if (error != Error::none)
            return fail_device_creation(state, error);
    }
#endif

    VkPhysicalDevice physical_devices[max_physical_devices]{};
    uint32_t physical_device_count = 0;
    error = enumerate_physical_devices(
        state->instance,
        {physical_devices, max_physical_devices},
        physical_device_count);
    if (error != Error::none)
        return fail_device_creation(state, error);

    Candidate selected{};
    bool has_selected = false;
    for (uint32_t index = 0; index < physical_device_count; ++index)
    {
        const VkPhysicalDevice physical_device = physical_devices[index];
        Candidate candidate{};
        error = inspect_candidate(physical_device, state->surface, state->heap_memory_block_size, candidate);
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
    state->vulkan13_properties = selected.vulkan13_properties;
    state->mesh_properties = selected.mesh_properties;
    state->max_timeline_value_difference =
        selected.vulkan12_properties.maxTimelineSemaphoreValueDifference;
    state->heap_properties.pNext = nullptr;
    state->vulkan13_properties.pNext = nullptr;
    state->mesh_properties.pNext = nullptr;
    vkGetPhysicalDeviceMemoryProperties(state->physical_device, &state->memory_properties);
    for (uint32_t value = 0; value < format_count; ++value)
    {
        state->format_features[value] = optimal_format_features(
            state->physical_device, static_cast<Format>(value));
    }

    QueriedFeatures enabled_features(
        presentation, selected.unified_image_layouts);
    vkGetPhysicalDeviceFeatures2(state->physical_device, &enabled_features.core);
    const VkBool32 image_cube_array =
        enabled_features.core.features.imageCubeArray;
    const VkBool32 sampler_anisotropy =
        enabled_features.core.features.samplerAnisotropy;
    const VkBool32 texture_compression_bc = enabled_features.core.features.textureCompressionBC;
    const VkBool32 texture_compression_astc =
        enabled_features.core.features.textureCompressionASTC_LDR;
    const VkBool32 texture_compression_etc2 = enabled_features.core.features.textureCompressionETC2;
    const VkBool32 storage_input_output16 =
        enabled_features.vulkan11.storageInputOutput16;
    state->image_cube_array = image_cube_array == VK_TRUE;
    state->texture_compression_etc2 = texture_compression_etc2 == VK_TRUE;
    enabled_features.core.features = {};
    enabled_features.core.features.imageCubeArray = image_cube_array;
    enabled_features.core.features.samplerAnisotropy = sampler_anisotropy;
    enabled_features.core.features.shaderInt16 = VK_TRUE;
    enabled_features.core.features.depthBiasClamp = VK_TRUE;
    enabled_features.core.features.independentBlend = VK_TRUE;
    enabled_features.core.features.textureCompressionBC = texture_compression_bc;
    enabled_features.core.features.textureCompressionASTC_LDR = texture_compression_astc;
    enabled_features.core.features.textureCompressionETC2 = texture_compression_etc2;
    enabled_features.core.features.fragmentStoresAndAtomics = VK_TRUE;
    enabled_features.core.features.vertexPipelineStoresAndAtomics = VK_TRUE;
    enabled_features.core.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    enabled_features.core.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enabled_features.core.features.multiDrawIndirect = VK_TRUE;
    enabled_features.core.features.drawIndirectFirstInstance = VK_TRUE;
    enabled_features.vulkan11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &enabled_features.vulkan12,
        .storageBuffer16BitAccess = VK_TRUE,
        .storagePushConstant16 = VK_TRUE,
        .storageInputOutput16 = storage_input_output16,
        .shaderDrawParameters = VK_TRUE,
    };
    enabled_features.vulkan12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &enabled_features.vulkan13,
        .shaderFloat16 = VK_TRUE,
        .scalarBlockLayout = VK_TRUE,
        .timelineSemaphore = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
    };
    enabled_features.vulkan13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &enabled_features.vulkan14,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    enabled_features.vulkan14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
        .pNext = &enabled_features.descriptor_heap,
        .maintenance5 = VK_TRUE,
    };
    enabled_features.descriptor_heap = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
        .pNext = &enabled_features.address_commands,
        .descriptorHeap = VK_TRUE,
    };
    enabled_features.address_commands = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR,
        .pNext = &enabled_features.untyped_pointers,
        .deviceAddressCommands = VK_TRUE,
    };
    enabled_features.untyped_pointers = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR,
        .pNext = selected.unified_image_layouts
            ? static_cast<void*>(&enabled_features.unified_image_layouts)
            : static_cast<void*>(&enabled_features.mesh_shader),
        .shaderUntypedPointers = VK_TRUE,
    };
    enabled_features.unified_image_layouts = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR,
        .pNext = &enabled_features.mesh_shader,
        .unifiedImageLayouts = selected.unified_image_layouts ? VK_TRUE : VK_FALSE,
    };
    enabled_features.mesh_shader = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
        .pNext = presentation
            ? static_cast<void*>(&enabled_features.swapchain_maintenance1)
            : nullptr,
        .meshShader = VK_TRUE,
    };
    enabled_features.swapchain_maintenance1 = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
        .swapchainMaintenance1 = VK_TRUE,
    };

    constexpr float queue_priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = state->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    const char* enabled_device_extensions[7]{};
    uint32_t enabled_device_extension_count = 0;
    enabled_device_extensions[enabled_device_extension_count++] =
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME;
    enabled_device_extensions[enabled_device_extension_count++] =
        VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME;
    enabled_device_extensions[enabled_device_extension_count++] =
        VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME;
    if (selected.unified_image_layouts)
    {
        enabled_device_extensions[enabled_device_extension_count++] =
            VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME;
    }
    enabled_device_extensions[enabled_device_extension_count++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
#if defined(_WIN32)
    if (presentation)
    {
        enabled_device_extensions[enabled_device_extension_count++] =
            VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        enabled_device_extensions[enabled_device_extension_count++] =
            VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
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
    error = error_from_vk(
        vkCreateDevice(state->physical_device, &device_info, nullptr, &state->device));
    if (error != Error::none)
        return fail_device_creation(state, error);
    vkGetDeviceQueue(state->device, state->queue_family, 0, &state->queue);

    state->fn.write_sampler_descriptors =
        load_device_proc<PFN_vkWriteSamplerDescriptorsEXT>(
            state->device, "vkWriteSamplerDescriptorsEXT");
    state->fn.write_resource_descriptors =
        load_device_proc<PFN_vkWriteResourceDescriptorsEXT>(
            state->device, "vkWriteResourceDescriptorsEXT");
    state->fn.cmd_bind_sampler_heap = load_device_proc<PFN_vkCmdBindSamplerHeapEXT>(
        state->device, "vkCmdBindSamplerHeapEXT");
    state->fn.cmd_bind_texture_heap = load_device_proc<PFN_vkCmdBindResourceHeapEXT>(
        state->device, "vkCmdBindResourceHeapEXT");
    state->fn.cmd_push_data = load_device_proc<PFN_vkCmdPushDataEXT>(
        state->device, "vkCmdPushDataEXT");
    state->fn.cmd_bind_index_buffer = load_device_proc<PFN_vkCmdBindIndexBuffer3KHR>(
        state->device, "vkCmdBindIndexBuffer3KHR");
    state->fn.cmd_draw_indirect = load_device_proc<PFN_vkCmdDrawIndirect2KHR>(
        state->device, "vkCmdDrawIndirect2KHR");
    state->fn.cmd_draw_indexed_indirect =
        load_device_proc<PFN_vkCmdDrawIndexedIndirect2KHR>(
            state->device, "vkCmdDrawIndexedIndirect2KHR");
    state->fn.cmd_dispatch_indirect = load_device_proc<PFN_vkCmdDispatchIndirect2KHR>(
        state->device, "vkCmdDispatchIndirect2KHR");
    state->fn.cmd_draw_mesh_tasks = load_device_proc<PFN_vkCmdDrawMeshTasksEXT>(
        state->device, "vkCmdDrawMeshTasksEXT");
    state->fn.cmd_draw_mesh_tasks_indirect =
        load_device_proc<PFN_vkCmdDrawMeshTasksIndirect2EXT>(
            state->device, "vkCmdDrawMeshTasksIndirect2EXT");
    state->fn.cmd_copy_memory = load_device_proc<PFN_vkCmdCopyMemoryKHR>(
        state->device, "vkCmdCopyMemoryKHR");
    state->fn.cmd_copy_memory_to_image = load_device_proc<PFN_vkCmdCopyMemoryToImageKHR>(
        state->device, "vkCmdCopyMemoryToImageKHR");
    state->fn.cmd_copy_image_to_memory = load_device_proc<PFN_vkCmdCopyImageToMemoryKHR>(
        state->device, "vkCmdCopyImageToMemoryKHR");
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
    if (presentation)
    {
        for (uint32_t index = 0; index < state->present_context_count; ++index)
        {
            error = state->create_present_context(state->present_contexts[index]);
            if (error != Error::none)
                return fail_device_creation(state, error);
        }
    }

    state->caps = {
        .device_name = state->physical_properties.deviceName,
        .max_push_data_size = state->heap_properties.maxPushDataSize,
        .texture_descriptor_size = state->heap_properties.imageDescriptorSize,
        .texture_descriptor_stride = align_up(
            state->heap_properties.imageDescriptorSize,
            state->heap_properties.imageDescriptorAlignment),
        .sampler_descriptor_size = state->heap_properties.samplerDescriptorSize,
        .sampler_descriptor_stride = align_up(
            state->heap_properties.samplerDescriptorSize,
            state->heap_properties.samplerDescriptorAlignment),
        .texture_compression_bc = texture_compression_bc == VK_TRUE,
        .texture_compression_astc = texture_compression_astc == VK_TRUE,
        .storage_input_output16 = storage_input_output16 == VK_TRUE,
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

TimelineSemaphore* create_timeline_semaphore(Device* device,
                                             uint64_t initial_value) noexcept
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
    auto* result = new TimelineSemaphore{
        .state = device,
        .last_signal = initial_value,
    };
    require_vk(vkCreateSemaphore(
        device->device, &create_info, nullptr, &result->semaphore));
    ++device->live_timeline_semaphores;
    return result;
}

void destroy_timeline_semaphore(TimelineSemaphore* semaphore) noexcept
{
    if (!semaphore)
        return;
    Device* device = semaphore->state;
    const bool valid = device && semaphore->semaphore &&
                       device->live_timeline_semaphores != 0;
    assert(valid && "destroy_timeline_semaphore received an invalid semaphore");
    assert(device->active_command_buffers == 0 &&
           "timeline semaphore destruction is not allowed while a command buffer is recording");

    const VkSemaphore handle = semaphore->semaphore;
    semaphore->semaphore = VK_NULL_HANDLE;
    semaphore->state = nullptr;
    --device->live_timeline_semaphores;
    delete semaphore;
    device->defer_delete(
        [handle](Device& owner, uint64_t) noexcept {
            vkDestroySemaphore(owner.device, handle, nullptr);
        });
}

uint64_t timeline_completed_value(const TimelineSemaphore* semaphore) noexcept
{
    const bool valid = semaphore && semaphore->state && semaphore->semaphore;
    assert(valid && "timeline_completed_value received an invalid semaphore");
    return query_timeline_value(*semaphore);
}

void wait_timeline(TimelinePoint point) noexcept
{
    TimelineSemaphore* semaphore = point.semaphore;
    const bool valid = semaphore && semaphore->state && semaphore->semaphore &&
                       point.value <= semaphore->last_signal;
    assert(valid && "wait_timeline received an invalid or unsignaled point");

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
        std::numeric_limits<uint64_t>::max()));
    semaphore->state->poll_command_retirement();
}

GpuAllocation<> gpu_malloc(Device* device, uint64_t byte_count, MemoryType memory) noexcept
{
    assert(device && "gpu_malloc called with a null device");
    assert(byte_count != 0 && "gpu_malloc byte count must be non-zero");
    device->poll_command_retirement();
    switch (memory)
    {
    case MemoryType::cpu_visible:
    case MemoryType::gpu_only:
    case MemoryType::readback: return device->allocate_gpu(byte_count, memory);
    case MemoryType::texture_heap:
    case MemoryType::sampler_heap: return device->allocate_descriptor_heap(byte_count, memory);
    default: assert(false && "gpu_malloc received an invalid memory type"); std::abort();
    }
}

void gpu_free(const GpuAllocation<>& allocation) noexcept
{
    if (!allocation.gpu && !allocation.cpu && allocation.size == 0 &&
        !allocation.allocation_owner && allocation.allocation_token == 0)
        return;
    assert(allocation.allocation_owner &&
           "gpu_free requires a live allocation returned by gpu_malloc");
    allocation.allocation_owner->state->release_allocation(allocation);
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
    for (uint32_t index = 0; index < swapchain.image_count; ++index)
    {
        Texture& texture = swapchain.textures[index];
        RenderView& render_view = swapchain.render_views[index];
        const bool valid = render_view.state == device &&
                           render_view.texture == &texture &&
                           render_view.view &&
                           render_view.swapchain_view &&
                           texture.state == device &&
                           texture.swapchain_image &&
                           texture.live_render_views == 1;
        assert(valid && "swapchain image-view ownership is invalid");
        retired.views[retired.view_count++] = render_view.view;
        --texture.live_render_views;
        render_view = {};
        texture = {};
        swapchain.initialized[index] = false;
    }
    swapchain.image_count = 0;
    swapchain.handle = VK_NULL_HANDLE;
    swapchain.width = 0;
    swapchain.height = 0;

    for (uint32_t index = 0; index < device->present_context_count; ++index)
    {
        const detail::PresentContext& context = device->present_contexts[index];
        if (context.present_pending && context.swapchain == retired.handle)
            ++retired.pending_presents;
    }
    if (retired.pending_presents == 0)
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
    assert(false && "retired swapchain generation storage exhausted");
    std::abort();
}

void destroy_owned_swapchain(Device& device) noexcept
{
    if (!device.swapchain)
        return;
    Swapchain* swapchain = device.swapchain;
    const bool valid = swapchain->state == &device && !swapchain->acquired &&
                       device.acquired_swapchain != swapchain &&
                       device.active_command_buffers == 0;
    assert(valid &&
           "device-owned swapchain destroyed while its image or command buffer is active");
    retire_swapchain_handle(*swapchain);
    swapchain->state = nullptr;
    delete swapchain;
    device.swapchain = nullptr;
}

VkCompositeAlphaFlagBitsKHR choose_composite_alpha(
    VkCompositeAlphaFlagsKHR supported) noexcept
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
    std::abort();
}

[[nodiscard]] bool swapchain_surface_configuration_changed(
    const Swapchain& swapchain) noexcept
{
    VkSurfaceCapabilitiesKHR capabilities{};
    const Error error = error_from_vk(
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            swapchain.state->physical_device,
            swapchain.state->surface,
            &capabilities));
    if (error != Error::none)
        require_error(error);

    const VkExtent2D extent = capabilities.currentExtent;
    const uint32_t variable_extent =
        std::numeric_limits<uint32_t>::max();
    return extent.width == variable_extent ||
           extent.height == variable_extent ||
           extent.width != swapchain.width ||
           extent.height != swapchain.height ||
           capabilities.currentTransform != swapchain.transform ||
           choose_composite_alpha(capabilities.supportedCompositeAlpha) !=
               swapchain.composite_alpha;
}

Error recreate_swapchain(Swapchain& swapchain) noexcept
{
    Device& device = *swapchain.state;
    assert(!swapchain.acquired && !device.acquired_swapchain &&
           device.active_command_buffers == 0);
    const VkSurfacePresentModeEXT present_mode_info{
        .sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT,
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
    Error error = error_from_vk(vkGetPhysicalDeviceSurfaceCapabilities2KHR(
        device.physical_device, &surface_info, &capabilities_info));
    if (error != Error::none)
        return error;
    const VkSurfaceCapabilitiesKHR& capabilities =
        capabilities_info.surfaceCapabilities;

    const VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == std::numeric_limits<uint32_t>::max())
        return Error::unsupported;
    if (extent.width == 0 || extent.height == 0)
    {
        swapchain.width = 0;
        swapchain.height = 0;
        swapchain.recreate_required = true;
        return Error::none;
    }
    if ((capabilities.supportedUsageFlags &
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
    {
        return Error::unsupported;
    }

    VkSurfaceFormatKHR formats[max_surface_formats]{};
    uint32_t surface_format_count = 0;
    error = error_from_vk(vkGetPhysicalDeviceSurfaceFormatsKHR(
        device.physical_device, device.surface,
        &surface_format_count, nullptr));
    if (error != Error::none)
        return error;
    if (surface_format_count == 0 ||
        surface_format_count > max_surface_formats)
        return Error::unsupported;
    error = error_from_vk(vkGetPhysicalDeviceSurfaceFormatsKHR(
        device.physical_device, device.surface,
        &surface_format_count, formats));
    if (error != Error::none)
        return error;

    const VkFormat requested_format = to_vk(swapchain.format);
    bool format_supported = false;
    for (uint32_t index = 0; index < surface_format_count; ++index)
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

    uint32_t requested_image_count = device.present_context_count;
    if (requested_image_count < capabilities.minImageCount)
        requested_image_count = capabilities.minImageCount;
    if (capabilities.maxImageCount != 0 && requested_image_count > capabilities.maxImageCount)
        requested_image_count = capabilities.maxImageCount;
    if (requested_image_count == 0 || requested_image_count > max_swapchain_images)
        return Error::unsupported;

    const VkCompositeAlphaFlagBitsKHR composite_alpha =
        choose_composite_alpha(capabilities.supportedCompositeAlpha);
    const VkSwapchainKHR old_handle = swapchain.handle;
    const VkSwapchainPresentModesCreateInfoEXT present_modes_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT,
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
    error = error_from_vk(vkCreateSwapchainKHR(
        device.device, &create_info, nullptr, &new_handle));
    if (error != Error::none)
    {
        retire_swapchain_handle(swapchain);
        return error;
    }

    VkImage images[max_swapchain_images]{};
    uint32_t image_count = 0;
    VkResult result = vkGetSwapchainImagesKHR(
        device.device, new_handle, &image_count, nullptr);
    if (result != VK_SUCCESS || image_count == 0 ||
        image_count > max_swapchain_images)
    {
        vkDestroySwapchainKHR(device.device, new_handle, nullptr);
        retire_swapchain_handle(swapchain);
        return result == VK_SUCCESS ? Error::unsupported : error_from_vk(result);
    }
    result = vkGetSwapchainImagesKHR(
        device.device, new_handle, &image_count, images);
    if (result != VK_SUCCESS)
    {
        vkDestroySwapchainKHR(device.device, new_handle, nullptr);
        retire_swapchain_handle(swapchain);
        return error_from_vk(result);
    }

    VkImageView views[max_swapchain_images]{};
    for (uint32_t index = 0; index < image_count; ++index)
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
        result = vkCreateImageView(
            device.device, &view_info, nullptr, &views[index]);
        if (result != VK_SUCCESS)
        {
            for (uint32_t created = 0; created < index; ++created)
                vkDestroyImageView(device.device, views[created], nullptr);
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
    for (uint32_t index = 0; index < image_count; ++index)
    {
        Texture& texture = swapchain.textures[index];
        texture.state = &device;
        texture.image = images[index];
        texture.width = extent.width;
        texture.height = extent.height;
        texture.depth = 1;
        texture.mip_levels = 1;
        texture.layer_count = 1;
        texture.array_layers = 1;
        texture.type = TextureType::two_d;
        texture.format = swapchain.format;
        texture.usage = TextureUsage::color_attachment;
        texture.owns_image = false;
        texture.swapchain_image = true;
        swapchain.render_views[index] = {
            .state = &device,
            .texture = &texture,
            .view = views[index],
            .width = extent.width,
            .height = extent.height,
            .swapchain_view = true,
        };
        ++texture.live_render_views;
    }
    return Error::none;
}

bool try_suballocate_image(Device& device,
                           Texture& texture,
                           detail::ImageHeap& heap,
                           uint32_t memory_type,
                           const VkMemoryRequirements& requirements,
                           uint32_t unit_count,
                           VkDeviceSize padded_unit_count) noexcept
{
    if (heap.memory_type != memory_type)
        return false;
    for (uint32_t attempt = 0; attempt < 2; ++attempt)
    {
        if (attempt != 0)
        {
            if (padded_unit_count == unit_count || padded_unit_count > device.heap_memory_block_size / gpu_allocation_alignment)
                break;
            unit_count = static_cast<uint32_t>(padded_unit_count);
        }
        const OffsetAllocator::Allocation token = heap.offsets.allocate(unit_count);
        if (token.offset == OffsetAllocator::Allocation::NO_SPACE)
            return false;
        const VkDeviceSize raw_offset = static_cast<VkDeviceSize>(token.offset) * gpu_allocation_alignment;
        const VkDeviceSize allocated_size = static_cast<VkDeviceSize>(heap.offsets.allocationSize(token)) * gpu_allocation_alignment;
        const VkDeviceSize memory_offset = align_up(raw_offset, requirements.alignment);
        const bool valid_range = memory_offset >= raw_offset && memory_offset - raw_offset <= allocated_size &&
                                 requirements.size <= allocated_size - (memory_offset - raw_offset) && memory_offset <= device.heap_memory_block_size &&
                                 requirements.size <= device.heap_memory_block_size - memory_offset;
        if (valid_range)
        {
            texture.heap = &heap;
            texture.heap_allocation = token;
            texture.owns_heap_allocation = true;
            require_vk(vkBindImageMemory(device.device, texture.image, heap.memory, memory_offset));
            return true;
        }
        heap.offsets.free(token);
    }
    return false;
}

} // namespace

DrawableExtent get_drawable_extent(Device* device) noexcept
{
    assert(device && "get_drawable_extent called with a null device");
    assert(device->active_command_buffers == 0 &&
           !device->acquired_swapchain &&
           "get_drawable_extent must be called outside command recording and presentation");
    device->poll_command_retirement();
    device->poll_present_contexts();
    if (!device->swapchain)
        return {};

    VkSurfaceCapabilitiesKHR capabilities{};
    const Error error = error_from_vk(
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            device->physical_device, device->surface, &capabilities));
    if (error != Error::none)
        require_error(error);
    const VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == std::numeric_limits<uint32_t>::max() ||
        extent.height == std::numeric_limits<uint32_t>::max())
    {
        require_error(Error::unsupported);
    }

    Swapchain& swapchain = *device->swapchain;
    swapchain.extent_queried = true;
    if (swapchain.handle &&
        (swapchain.width != extent.width ||
         swapchain.height != extent.height))
    {
        swapchain.recreate_required = true;
    }
    return {
        .width = extent.width,
        .height = extent.height,
    };
}

SwapchainFrame acquire(Device* device) noexcept
{
    const bool valid = device && device->swapchain &&
                       device->swapchain->state == device &&
                       !device->swapchain->acquired &&
                       !device->acquired_swapchain &&
                       device->active_command_buffers == 0;
    assert(valid && "acquire received a device without an available swapchain");

    Swapchain* swapchain = device->swapchain;
    const bool extent_queried = swapchain->extent_queried;
    swapchain->extent_queried = false;
    if (!extent_queried)
        device->poll_command_retirement();
    device->poll_present_contexts();
    detail::PresentContext* present_context = nullptr;

    for (;;)
    {
        if (!swapchain->handle || swapchain->recreate_required)
        {
            const Error error = recreate_swapchain(*swapchain);
            if (error != Error::none)
                require_error(error);
            const bool drawable = swapchain->handle &&
                                  swapchain->width != 0 &&
                                  swapchain->height != 0;
            assert(drawable && "acquire requires a non-zero drawable extent");
            if (!drawable)
                std::abort();
        }

        if (!present_context)
        {
            present_context = &device->present_contexts[device->next_present_context];
            device->wait_present_context(*present_context);
            assert(!present_context->present_pending && !present_context->swapchain);
        }

        uint32_t image_index = 0;
        const VkResult result = vkAcquireNextImageKHR(
            device->device,
            swapchain->handle,
            std::numeric_limits<uint64_t>::max(),
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
        const bool valid_index = image_index < swapchain->image_count;
        assert(valid_index && "swapchain returned an invalid image index");

        swapchain->image_index = image_index;
        swapchain->present_context = present_context;
        swapchain->acquired = true;
        swapchain->recreate_required =
            result == VK_SUBOPTIMAL_KHR &&
            swapchain_surface_configuration_changed(*swapchain);
        device->acquired_swapchain = swapchain;
        return {
            .render_view = &swapchain->render_views[image_index],
            .width = swapchain->width,
            .height = swapchain->height,
        };
    }
}

Texture* create_texture(Device* device, const TextureDesc& desc) noexcept
{
    assert(device && "create_texture called with a null device");
    const bool commands_recording = device->active_command_buffers != 0;
    assert(!commands_recording &&
           "create_texture is not allowed while a command buffer is recording");
    device->poll_command_retirement();
    const bool valid_format = is_concrete_format(desc.format);
    const bool valid_dimensions = desc.width != 0 && desc.height != 0 &&
                                  desc.depth != 0 && desc.layer_count != 0;
    bool valid_type = true;
    bool valid_shape = valid_dimensions;
    switch (desc.type)
    {
    case TextureType::one_d:
        valid_shape = valid_shape && desc.height == 1 && desc.depth == 1 &&
                      desc.layer_count == 1;
        break;
    case TextureType::two_d:
        valid_shape = valid_shape && desc.depth == 1 && desc.layer_count == 1;
        break;
    case TextureType::three_d:
        valid_shape = valid_shape && desc.layer_count == 1;
        break;
    case TextureType::cube:
        valid_shape = valid_shape && desc.depth == 1 &&
                      desc.width == desc.height && desc.layer_count == 1;
        break;
    case TextureType::two_d_array:
        valid_shape = valid_shape && desc.depth == 1;
        break;
    case TextureType::cube_array:
        valid_shape = valid_shape && desc.depth == 1 &&
                      desc.width == desc.height;
        break;
    default:
        valid_type = false;
        valid_shape = false;
        break;
    }
    assert(valid_type && "texture type is invalid");
    assert(valid_format && "texture format must not be undefined");
    assert(valid_dimensions &&
           "texture dimensions and layer count must be non-zero");
    assert(valid_shape && "texture dimensions or layer count do not match its type");
    assert(desc.mip_levels != 0 && "texture mip count must be non-zero");
    uint32_t maximum_dimension = desc.width > desc.height ? desc.width : desc.height;
    if (desc.depth > maximum_dimension)
        maximum_dimension = desc.depth;
    const auto maximum_mip_levels = static_cast<uint32_t>(
        std::bit_width(maximum_dimension));
    assert(desc.mip_levels <= maximum_mip_levels &&
           "texture mip count exceeds the maximum for its dimensions");
    const bool supported_texture_type = desc.type != TextureType::cube_array ||
                                        device->image_cube_array;
    assert(supported_texture_type &&
           "texture type requires an unsupported optional core feature");
    const bool depth_stencil_format = has_depth_aspect(desc.format) ||
                                      has_stencil_aspect(desc.format);
    assert((!depth_stencil_format ||
            !has_flag(desc.usage, TextureUsage::color_attachment)) &&
           "a depth/stencil format cannot be used as a color attachment");
    assert((depth_stencil_format ||
            !has_flag(desc.usage, TextureUsage::depth_stencil_attachment)) &&
           "a color format cannot be used as a depth/stencil attachment");
    const TextureFormatInfo texture_format_info = get_texture_format_info(desc.format);
    const bool combined_transfer = texture_format_info.depth && texture_format_info.stencil &&
                                   (has_flag(desc.usage, TextureUsage::transfer_source) ||
                                    has_flag(desc.usage, TextureUsage::transfer_destination));
    assert(!combined_transfer &&
           "combined depth/stencil transfers require an aspect selector");
    const bool attachment = has_flag(desc.usage, TextureUsage::color_attachment) ||
                            has_flag(desc.usage,
                                     TextureUsage::depth_stencil_attachment);
    const bool attachment_shape = !attachment ||
                                  (desc.type != TextureType::one_d &&
                                   desc.type != TextureType::three_d);
    assert(attachment_shape &&
           "render attachments must have a 2D, array, or cube texture type");
    assert(desc.usage != TextureUsage::none && "texture must have at least one usage");
    const bool valid_usage_bits = has_valid_texture_usage_bits(desc.usage);
    assert(valid_usage_bits && "texture usage mask contains unknown bits");
    const auto& limits = device->physical_properties.limits;
    assert(!attachment ||
           (desc.width <= limits.maxFramebufferWidth &&
            desc.height <= limits.maxFramebufferHeight));
    if (has_flag(desc.usage, TextureUsage::color_attachment) ||
        has_flag(desc.usage, TextureUsage::depth_stencil_attachment))
    {
        const auto viewport_width = static_cast<float>(desc.width);
        const auto viewport_height = static_cast<float>(desc.height);
        assert(desc.width <= limits.maxViewportDimensions[0] &&
               desc.height <= limits.maxViewportDimensions[1] &&
               limits.viewportBoundsRange[0] <= 0.0f &&
               viewport_width <= limits.viewportBoundsRange[1] &&
               viewport_height <= limits.viewportBoundsRange[1]);
    }

    VkImageUsageFlags usage = 0;
    if (has_flag(desc.usage, TextureUsage::sampled)) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (has_flag(desc.usage, TextureUsage::storage)) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (has_flag(desc.usage, TextureUsage::color_attachment))
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (has_flag(desc.usage, TextureUsage::depth_stencil_attachment))
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (has_flag(desc.usage, TextureUsage::transfer_source))
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (has_flag(desc.usage, TextureUsage::transfer_destination))
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    assert(usage != 0);

    const uint32_t array_layers = physical_layer_count(desc);
    const auto image_type = to_vk(desc.type);
    const auto format = to_vk(desc.format);
    assert(supports_texture_format(device, desc.format, desc.usage));
    const VkFormatFeatureFlags2 sampled_features = required_format_features(
        TextureUsage::sampled);
    const VkFormatFeatureFlags2 storage_features = required_format_features(
        TextureUsage::storage);
    VkFormat view_formats[format_count]{};
    uint32_t view_format_count = 1;
    view_formats[0] = format;
    const uint64_t base_format_bit = format_bit(desc.format);
    uint64_t sampled_view_format_mask =
        has_flag(desc.usage, TextureUsage::sampled) ? base_format_bit : 0;
    uint64_t storage_view_format_mask =
        has_flag(desc.usage, TextureUsage::storage) ? base_format_bit : 0;
    for (uint32_t value = 0; desc.mutable_format && value < format_count; ++value)
    {
        const Format view_format = static_cast<Format>(value);
        if (view_format == desc.format ||
            !compatible_view_formats(desc.format, view_format))
            continue;
        const VkFormatFeatureFlags2 view_features = device->format_features[value];
        const bool sampled = has_flag(desc.usage, TextureUsage::sampled) &&
                             (view_features & sampled_features) == sampled_features;
        const bool storage = has_flag(desc.usage, TextureUsage::storage) &&
                             (view_features & storage_features) == storage_features;
        if (!sampled && !storage)
            continue;
        assert(view_format_count < format_count);
        view_formats[view_format_count++] = to_vk(view_format);
        const uint64_t bit = format_bit(view_format);
        if (sampled)
            sampled_view_format_mask |= bit;
        if (storage)
            storage_view_format_mask |= bit;
    }

    VkImageCreateFlags image_flags = 0;
    if (desc.type == TextureType::cube || desc.type == TextureType::cube_array)
        image_flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (view_format_count > 1)
        image_flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

    const VkImageFormatListCreateInfo format_list{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = view_format_count,
        .pViewFormats = view_formats,
    };
    const VkPhysicalDeviceImageFormatInfo2 format_info{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = view_format_count > 1 ? &format_list : nullptr,
        .format = format,
        .type = image_type,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .flags = image_flags,
    };
    VkImageFormatProperties2 image_properties2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    };
    const auto format_result = vkGetPhysicalDeviceImageFormatProperties2(
        device->physical_device, &format_info, &image_properties2);
    require_vk(format_result);
    const auto& image_properties = image_properties2.imageFormatProperties;
    assert(desc.width <= image_properties.maxExtent.width &&
           desc.height <= image_properties.maxExtent.height &&
           desc.depth <= image_properties.maxExtent.depth &&
           desc.mip_levels <= image_properties.maxMipLevels &&
           array_layers <= image_properties.maxArrayLayers &&
           image_resource_byte_size(desc, array_layers) <=
               image_properties.maxResourceSize);

    auto* result = new Texture{
        .state = device,
        .width = desc.width,
        .height = desc.height,
        .depth = desc.depth,
        .mip_levels = desc.mip_levels,
        .layer_count = desc.layer_count,
        .array_layers = array_layers,
        .sampled_view_format_mask = sampled_view_format_mask,
        .storage_view_format_mask = storage_view_format_mask,
        .type = desc.type,
        .format = desc.format,
        .usage = desc.usage,
    };

    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = view_format_count > 1 ? &format_list : nullptr,
        .flags = image_flags,
        .imageType = image_type,
        .format = format,
        .extent = {
            .width = desc.width,
            .height = desc.height,
            .depth = desc.depth,
        },
        .mipLevels = desc.mip_levels,
        .arrayLayers = array_layers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    require_vk(vkCreateImage(device->device, &image_info, nullptr, &result->image));

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device->device, result->image, &requirements);
    assert(requirements.alignment != 0 && (requirements.alignment & (requirements.alignment - 1)) == 0);
    const bool allocation_fits = requirements.size != 0 && requirements.size <= device->heap_memory_block_size;
    assert(allocation_fits && "texture allocation cannot be suballocated from the configured heap memory block");
    if (!allocation_fits)
    {
        delete result;
        return nullptr;
    }
    uint32_t memory_type = 0;
    const bool has_memory_type = device->find_memory_type(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        0,
        device->heap_memory_block_size,
        memory_type,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    assert(has_memory_type);
    if (!has_memory_type)
    {
        delete result;
        return nullptr;
    }

    const uint32_t unit_count = allocation_unit_count(requirements.size);
    const VkDeviceSize padded_unit_count = unit_count + (requirements.alignment - 1) / gpu_allocation_alignment;
    assert(memory_type < VK_MAX_MEMORY_TYPES);
    detail::ImageHeap*& active = device->active_image_heaps[memory_type];
    const bool valid_active = !active || (active->state == device && active->memory_type == memory_type);
    assert(valid_active && "active image heap is invalid");
    bool allocated = false;
    if (active)
    {
        allocated = try_suballocate_image(*device, *result, *active, memory_type, requirements, unit_count, padded_unit_count);
    }
    if (!allocated)
    {
        for (detail::ImageHeap* heap = device->image_heaps; heap; heap = heap->next)
        {
            if (heap == active)
                continue;
            if (try_suballocate_image(*device, *result, *heap, memory_type, requirements, unit_count, padded_unit_count))
            {
                allocated = true;
                active = heap;
                break;
            }
        }
    }
    if (!allocated)
    {
        detail::ImageHeap* heap = new detail::ImageHeap(device, memory_type);
        const VkMemoryAllocateInfo allocate_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = device->heap_memory_block_size,
            .memoryTypeIndex = memory_type,
        };
        device->allocate_memory(allocate_info, heap->memory);
        allocated = try_suballocate_image(*device, *result, *heap, memory_type, requirements, unit_count, padded_unit_count);
        assert(allocated && "a new image heap could not satisfy its first allocation");
        if (!allocated)
        {
            delete heap;
            delete result;
            return nullptr;
        }
        heap->next = device->image_heaps;
        device->image_heaps = heap;
        active = heap;
    }

    const VkImageAspectFlags aspect_mask = image_aspects(desc.format);

    result->initialization = {
        .image = result->image,
        .aspect_mask = aspect_mask,
        .mip_levels = desc.mip_levels,
        .array_layers = array_layers,
    };
    detail::append_texture_initialization(
        device->pending_texture_initializations, result->initialization);
    return result;
}

RenderView* create_render_view(
    Texture* texture, const RenderViewDesc& desc) noexcept
{
    const bool valid_texture = texture && texture->state;
    const bool commands_recording = valid_texture &&
                                     texture->state->active_command_buffers != 0;
    assert(!commands_recording &&
           "create_render_view is not allowed while a command buffer is recording");
    const bool valid_usage = valid_texture &&
        (has_flag(texture->usage, TextureUsage::color_attachment) ||
         has_flag(texture->usage, TextureUsage::depth_stencil_attachment));
    const bool valid_type = valid_texture &&
                            texture->type != TextureType::one_d &&
                            texture->type != TextureType::three_d;
    const bool valid_subresource = valid_texture &&
                                   desc.mip_level < texture->mip_levels &&
                                   desc.slice < texture->array_layers;
    assert(valid_usage &&
           "render view texture must support attachment use");
    assert(valid_type &&
           "render views require a 2D, array, or cube texture");
    assert(valid_subresource && "render view subresource is invalid");

    uint32_t width = texture->width >> desc.mip_level;
    uint32_t height = texture->height >> desc.mip_level;
    if (width == 0)
        width = 1;
    if (height == 0)
        height = 1;
    auto* result = new RenderView{
        .state = texture->state,
        .texture = texture,
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
    require_vk(vkCreateImageView(
        texture->state->device, &view_info, nullptr, &result->view));
    ++texture->live_render_views;
    return result;
}

void write_texture_descriptor(Device* device,
                              void* cpu_destination,
                              const Texture* texture,
                              TextureDescriptorType type,
                              const TextureDescriptorDesc& desc) noexcept
{
    const bool valid = device && cpu_destination && texture &&
                       texture->state == device;
    assert(valid &&
           "write_texture_descriptor requires a valid device, destination, and texture from this device");

    const bool valid_type = type == TextureDescriptorType::sampled ||
                            type == TextureDescriptorType::storage;
    const auto required_usage = type == TextureDescriptorType::sampled
        ? TextureUsage::sampled
        : TextureUsage::storage;
    const bool valid_usage = valid_type &&
                             has_flag(texture->usage, required_usage);
    const bool valid_base_mip = desc.base_mip < texture->mip_levels;
    const auto mip_count = valid_base_mip
        ? (desc.mip_count == 0
               ? texture->mip_levels - desc.base_mip
               : desc.mip_count)
        : 0;
    const bool valid_mips = valid_base_mip && mip_count != 0 &&
                            mip_count <= texture->mip_levels - desc.base_mip;
    const bool valid_base_layer = desc.base_layer < texture->layer_count;
    const auto layer_count = valid_base_layer
        ? (desc.layer_count == 0
               ? texture->layer_count - desc.base_layer
               : desc.layer_count)
        : 0;
    const bool valid_layers = valid_base_layer && layer_count != 0 &&
                              layer_count <= texture->layer_count - desc.base_layer;
    const Format view_format = desc.format == Format::undefined
        ? texture->format
        : desc.format;
    const bool valid_view_format =
        is_concrete_format(view_format) &&
        compatible_view_formats(texture->format, view_format);
    const uint64_t view_format_bit = valid_view_format ? format_bit(view_format) : 0;
    const uint64_t descriptor_view_format_mask = type == TextureDescriptorType::sampled
                                                          ? texture->sampled_view_format_mask
                                                          : texture->storage_view_format_mask;
    const bool valid_descriptor_format = valid_view_format &&
                                         (descriptor_view_format_mask &
                                          view_format_bit) != 0;
    assert(valid_usage && "texture was not created for this descriptor type");
    assert(valid_mips && "texture descriptor mip range is invalid");
    assert(valid_layers && "texture descriptor layer range is invalid");
    assert(valid_view_format &&
           "texture descriptor format is incompatible with this texture");
    assert(valid_descriptor_format &&
           "texture descriptor format does not support this descriptor type");

    uint32_t base_array_layer = desc.base_layer;
    uint32_t array_layer_count = layer_count;
    if (texture->type == TextureType::cube ||
        texture->type == TextureType::cube_array)
    {
        base_array_layer *= 6u;
        array_layer_count *= 6u;
    }
    const auto descriptor_usage = static_cast<VkImageUsageFlags>(
        type == TextureDescriptorType::sampled
            ? VK_IMAGE_USAGE_SAMPLED_BIT
            : VK_IMAGE_USAGE_STORAGE_BIT);

    VkImageAspectFlags descriptor_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (has_depth_aspect(texture->format))
        descriptor_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    else if (has_stencil_aspect(texture->format))
        descriptor_aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
    const VkImageViewUsageCreateInfo view_usage{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = descriptor_usage,
    };
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &view_usage,
        .image = texture->image,
        .viewType = to_vk_view(texture->type),
        .format = to_vk(view_format),
        .subresourceRange = {
            .aspectMask = descriptor_aspect,
            .baseMipLevel = desc.base_mip,
            .levelCount = mip_count,
            .baseArrayLayer = base_array_layer,
            .layerCount = array_layer_count,
        },
    };
    const VkImageDescriptorInfoEXT image_descriptor{
        .sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
        .pView = &view_info,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkResourceDescriptorDataEXT data{
        .pImage = &image_descriptor,
    };
    const VkResourceDescriptorInfoEXT descriptor_info{
        .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
        .type = type == TextureDescriptorType::sampled
            ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .data = data,
    };
    const VkHostAddressRangeEXT destination{
        .address = cpu_destination,
        .size = static_cast<size_t>(device->heap_properties.imageDescriptorSize),
    };
    assert_vk(device->fn.write_resource_descriptors(
        device->device, 1, &descriptor_info, &destination));
}

void write_sampler_descriptor(Device* device,
                              void* cpu_destination,
                              const SamplerDesc& desc) noexcept
{
    const bool valid = device && cpu_destination;
    assert(valid && "write_sampler_descriptor requires a valid device and destination");
    const VkSamplerCreateInfo sampler_info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = to_vk(desc.mag_filter),
        .minFilter = to_vk(desc.min_filter),
        .mipmapMode = desc.mip_filter == Filter::nearest
            ? VK_SAMPLER_MIPMAP_MODE_NEAREST
            : VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = to_vk(desc.address_u),
        .addressModeV = to_vk(desc.address_v),
        .addressModeW = to_vk(desc.address_w),
        .anisotropyEnable = desc.anisotropic ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = desc.anisotropic ? 4.0f : 1.0f,
        .compareEnable = desc.compare_enabled ? VK_TRUE : VK_FALSE,
        .compareOp = to_vk(desc.compare),
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    const VkHostAddressRangeEXT destination{
        .address = cpu_destination,
        .size = static_cast<size_t>(device->heap_properties.samplerDescriptorSize),
    };
    assert_vk(device->fn.write_sampler_descriptors(
        device->device, 1, &sampler_info, &destination));
}

namespace
{

VkStencilOpState to_vk(const StencilFaceState& state,
                       uint8_t compare_mask,
                       uint8_t write_mask) noexcept
{
    return {
        .failOp = to_vk(state.fail),
        .passOp = to_vk(state.pass),
        .depthFailOp = to_vk(state.depth_fail),
        .compareOp = to_vk(state.compare),
        .compareMask = compare_mask,
        .writeMask = write_mask,
        .reference = state.reference,
    };
}

PSO* create_raster_pso(Device* device,
                       Span<const uint32_t> first_stage_spirv,
                       Span<const uint32_t> fragment_spirv,
                       Span<const ColorTargetDesc> color_targets,
                       Format depth_format,
                       Format stencil_format,
                       const RasterizationState& rasterization_state,
                       const DepthStencilState& depth_stencil_state,
                       PSO::Kind kind) noexcept
{
    assert(device && "PSO creation called with a null device");
    const bool valid_color_count =
        (color_targets.size == 0 || color_targets.data) &&
        color_targets.size <= max_color_attachments &&
        (!device || color_targets.size <=
                        device->physical_properties.limits.maxColorAttachments);
    assert(valid_color_count && "PSO has too many color targets");
    const bool depth_enabled = depth_format != Format::undefined;
    const bool stencil_enabled = stencil_format != Format::undefined;
    const bool valid_depth_format = !depth_enabled || has_depth_aspect(depth_format);
    const bool valid_stencil_format = !stencil_enabled ||
                                      has_stencil_aspect(stencil_format);
    const bool matching_depth_stencil = !depth_enabled || !stencil_enabled ||
                                        depth_format == stencil_format;
    assert(valid_depth_format && "PSO depth format has no depth aspect");
    assert(valid_stencil_format && "PSO stencil format has no stencil aspect");
    assert(matching_depth_stencil &&
           "PSO depth and stencil formats must match when both are present");
    const bool valid_depth_write =
        !depth_stencil_state.depth_write || depth_stencil_state.depth_test;
    const bool depth_state_has_attachment =
        (!depth_stencil_state.depth_test && !depth_stencil_state.depth_write) ||
        depth_enabled;
    const bool stencil_state_has_attachment = !depth_stencil_state.stencil_test || stencil_enabled;
    assert(valid_depth_write && "depth writes require depth testing");
    assert(depth_state_has_attachment && "enabled depth state requires a PSO depth format");
    assert(stencil_state_has_attachment &&
           "enabled stencil state requires a PSO stencil format");

    for (size_t index = 0; index < color_targets.size; ++index)
    {
        const ColorTargetDesc& target = color_targets.data[index];
        const bool valid_target = is_color_format(target.format) &&
                                  (target.write_mask & ~0xfu) == 0;
        assert(valid_target &&
               "PSO color target needs a color format and a four-bit write mask");
        const VkFormatFeatureFlags2 format_features =
            valid_target ? device->format_features[static_cast<uint32_t>(target.format)] : 0;
        const bool supports_attachment =
            (format_features & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) != 0;
        const bool supports_blending =
            !target.blend.enabled ||
            (format_features & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT) != 0;
        assert(supports_attachment && "PSO color target format cannot be an attachment");
        assert(supports_blending && "PSO color target format does not support blending");
    }
    assert(!depth_enabled ||
           (device->format_features[static_cast<uint32_t>(depth_format)] &
            VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) != 0);
    assert(!stencil_enabled ||
           (device->format_features[static_cast<uint32_t>(stencil_format)] &
            VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) != 0);

    const bool valid_first_stage_spirv = first_stage_spirv.data &&
        first_stage_spirv.size != 0 &&
        first_stage_spirv.size <= std::numeric_limits<size_t>::max() /
                                      sizeof(uint32_t);
    const bool valid_fragment_spirv = fragment_spirv.data &&
        fragment_spirv.size != 0 &&
        fragment_spirv.size <= std::numeric_limits<size_t>::max() /
                                   sizeof(uint32_t);
    assert(valid_first_stage_spirv &&
           "first-stage SPIR-V shader bytecode is empty or too large");
    assert(valid_fragment_spirv &&
           "fragment SPIR-V shader bytecode is empty or too large");

    const VkShaderModuleCreateInfo first_stage_module_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = first_stage_spirv.size * sizeof(uint32_t),
        .pCode = first_stage_spirv.data,
    };
    const VkShaderModuleCreateInfo fragment_module_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fragment_spirv.size * sizeof(uint32_t),
        .pCode = fragment_spirv.data,
    };
    const bool mesh = kind == PSO::Kind::mesh_graphics;

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
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = to_vk(rasterization_state.cull),
        .frontFace = rasterization_state.cull == CullMode::counter_clockwise
                         ? VK_FRONT_FACE_CLOCKWISE
                         : VK_FRONT_FACE_COUNTER_CLOCKWISE,
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
        .depthTestEnable = depth_stencil_state.depth_test,
        .depthWriteEnable = depth_stencil_state.depth_write,
        .depthCompareOp = to_vk(depth_stencil_state.depth_compare),
        .stencilTestEnable = depth_stencil_state.stencil_test,
        .front = to_vk(depth_stencil_state.front,
                       depth_stencil_state.stencil_read_mask,
                       depth_stencil_state.stencil_write_mask),
        .back = to_vk(depth_stencil_state.back,
                      depth_stencil_state.stencil_read_mask,
                      depth_stencil_state.stencil_write_mask),
    };
    VkPipelineColorBlendAttachmentState color_attachments[max_color_attachments]{};
    for (size_t index = 0; index < color_targets.size; ++index)
    {
        const ColorTargetDesc& target = color_targets.data[index];
        color_attachments[index] = {
            .blendEnable = target.blend.enabled,
            .srcColorBlendFactor = to_vk(target.blend.color.source),
            .dstColorBlendFactor = to_vk(target.blend.color.destination),
            .colorBlendOp = to_vk(target.blend.color.operation),
            .srcAlphaBlendFactor = to_vk(target.blend.alpha.source),
            .dstAlphaBlendFactor = to_vk(target.blend.alpha.destination),
            .alphaBlendOp = to_vk(target.blend.alpha.operation),
            .colorWriteMask = target.write_mask,
        };
    }
    const VkPipelineColorBlendStateCreateInfo color_blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = static_cast<uint32_t>(color_targets.size),
        .pAttachments = color_targets.size ? color_attachments : nullptr,
    };
    constexpr VkDynamicState dynamic_states[]{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    const VkPipelineDynamicStateCreateInfo dynamic_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<uint32_t>(
            sizeof(dynamic_states) / sizeof(dynamic_states[0])),
        .pDynamicStates = dynamic_states,
    };
    VkFormat color_formats[max_color_attachments]{};
    for (size_t index = 0; index < color_targets.size; ++index)
        color_formats[index] = to_vk(color_targets.data[index].format);
    const auto vk_depth_format = depth_enabled ? to_vk(depth_format)
                                                : VK_FORMAT_UNDEFINED;
    const auto vk_stencil_format = stencil_enabled ? to_vk(stencil_format)
                                                    : VK_FORMAT_UNDEFINED;
    const VkPipelineRenderingCreateInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = static_cast<uint32_t>(color_targets.size),
        .pColorAttachmentFormats = color_targets.size ? color_formats : nullptr,
        .depthAttachmentFormat = vk_depth_format,
        .stencilAttachmentFormat = vk_stencil_format,
    };
    const VkPipelineCreateFlags2CreateInfo flags_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .pNext = &rendering_info,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
    };
    const VkGraphicsPipelineCreateInfo pso_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &flags_info,
        .stageCount = static_cast<uint32_t>(sizeof(stages) / sizeof(stages[0])),
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
    auto* result = new PSO{
        .state = device,
        .bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS,
    };
#if !defined(NDEBUG)
    result->color_count = static_cast<uint32_t>(color_targets.size);
    result->depth_format = depth_format;
    result->stencil_format = stencil_format;
    result->kind = kind;
    for (size_t index = 0; index < color_targets.size; ++index)
        result->color_formats[index] = color_targets.data[index].format;
#endif
    const auto pso_result = vkCreateGraphicsPipelines(
        device->device, VK_NULL_HANDLE, 1, &pso_info, nullptr, &result->pso);
    require_vk(pso_result);
    return result;
}

} // namespace

PSO* create_graphics_pso(Device* device, const GraphicsPSODesc& desc) noexcept
{
    return create_raster_pso(device,
                             desc.vertex_spirv,
                             desc.fragment_spirv,
                             desc.color_targets,
                             desc.depth_format,
                             desc.stencil_format,
                             desc.rasterization,
                             desc.depth_stencil,
                             PSO::Kind::vertex_graphics);
}

PSO* create_mesh_pso(Device* device, const MeshPSODesc& desc) noexcept
{
    return create_raster_pso(device,
                             desc.mesh_spirv,
                             desc.fragment_spirv,
                             desc.color_targets,
                             desc.depth_format,
                             desc.stencil_format,
                             desc.rasterization,
                             desc.depth_stencil,
                             PSO::Kind::mesh_graphics);
}

PSO* create_compute_pso(Device* device, Span<const uint32_t> compute_spirv) noexcept
{
    assert(device && "create_compute_pso called with a null device");

    const bool valid_spirv = compute_spirv.data && compute_spirv.size != 0 &&
        compute_spirv.size <= std::numeric_limits<size_t>::max() /
                                  sizeof(uint32_t);
    assert(valid_spirv &&
           "compute SPIR-V shader bytecode is empty or too large");
    const VkShaderModuleCreateInfo module_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = compute_spirv.size * sizeof(uint32_t),
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
    auto* result = new PSO{
        .state = device,
        .bind_point = VK_PIPELINE_BIND_POINT_COMPUTE,
    };
#if !defined(NDEBUG)
    result->kind = PSO::Kind::compute;
#endif
    const auto pso_result = vkCreateComputePipelines(
        device->device, VK_NULL_HANDLE, 1, &pso_info, nullptr, &result->pso);
    require_vk(pso_result);
    return result;
}

CommandBuffer* begin_commands(Device* device) noexcept
{
    assert(device && "begin_commands called with a null device");
    detail::CommandContext& context = device->acquire_command_context();
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    require_vk(vkBeginCommandBuffer(context.command_buffer, &begin_info));

    CommandBuffer* result = &context.commands;
    assert(!result->state && !result->context && !result->recording);
    *result = {
        .state = device,
        .context = &context,
        .command_buffer = context.command_buffer,
        .recording = true,
    };
    detail::transfer_texture_initializations(
        result->pending_texture_initializations,
        device->pending_texture_initializations);
    if (result->pending_texture_initializations.first)
    {
        VkImageMemoryBarrier2 barriers[image_barrier_batch_size]{};
        uint32_t barrier_count = 0;
        for (detail::TextureInitialization* initialization =
                 result->pending_texture_initializations.first;
             initialization;
             initialization = initialization->next)
        {
            assert(initialization && !initialization->initialized);
            assert(initialization->owner == &result->pending_texture_initializations);
            barriers[barrier_count++] = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT |
                                 VK_ACCESS_2_MEMORY_WRITE_BIT,
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
                record_image_barriers(
                    result->command_buffer, {barriers, barrier_count});
                barrier_count = 0;
            }
        }
        if (barrier_count != 0)
            record_image_barriers(
                result->command_buffer, {barriers, barrier_count});
    }
    if (device->acquired_swapchain &&
        !device->acquired_swapchain->transition_commands)
    {
        Swapchain* swapchain = device->acquired_swapchain;
        const bool valid = swapchain->state == device && swapchain->acquired &&
                           swapchain->present_context &&
                           swapchain->image_index < swapchain->image_count;
        assert(valid && "the acquired swapchain state is invalid");
        Texture& texture = swapchain->textures[swapchain->image_index];
        const VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT |
                             VK_ACCESS_2_MEMORY_WRITE_BIT,
            .oldLayout = swapchain->initialized[swapchain->image_index]
                ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture.image,
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
    context.active = true;
    ++device->active_command_buffers;
    return result;
}

namespace
{

void complete_texture_initializations(CommandBuffer& commands) noexcept
{
    detail::TextureInitialization* initialization =
        commands.pending_texture_initializations.first;
    while (initialization)
    {
        detail::TextureInitialization* next = initialization->next;
        const bool valid = !initialization->initialized &&
                           initialization->owner ==
                               &commands.pending_texture_initializations;
        assert(valid && "submitted texture initialization state is invalid");
        initialization->initialized = true;
        initialization->owner = nullptr;
        initialization->previous = nullptr;
        initialization->next = nullptr;
        initialization = next;
    }
    commands.pending_texture_initializations = {};
}

void submit_commands(Span<CommandBuffer* const> commands,
                     Device* device,
                     TimelinePoint completion,
                     VkSemaphore wait_semaphore,
                     VkSemaphore signal_semaphore) noexcept
{
    TimelineSemaphore* completion_semaphore = completion.semaphore;
    const bool valid_completion = completion_semaphore &&
                                  completion_semaphore->state == device &&
                                  completion_semaphore->semaphore &&
                                  completion.value > completion_semaphore->last_signal;
    assert(valid_completion &&
           "submission completion must monotonically advance a timeline owned by the device");
    assert(commands.data && commands.size != 0 &&
           commands.size <= std::numeric_limits<uint32_t>::max());
    assert(device->active_command_buffers == commands.size &&
           "submit must consume every begun command buffer");
    assert(!device->pending_texture_initializations.first &&
           !device->pending_texture_initializations.last &&
           "pending texture transitions were not recorded");
#if !defined(NDEBUG)
    if (device->max_timeline_value_difference !=
        std::numeric_limits<uint64_t>::max())
    {
        const uint64_t completed_value =
            query_timeline_value(*completion_semaphore);
        const bool valid_completed_value =
            completed_value <= completion_semaphore->last_signal;
        const bool valid_difference = valid_completed_value &&
            completion.value - completed_value <=
                device->max_timeline_value_difference;
        assert(valid_difference &&
               "submission timeline value exceeds the device's maximum value difference");
    }
#endif

    assert(device->command_submit_infos.size() >= commands.size);
    for (size_t index = 0; index < commands.size; ++index)
    {
        CommandBuffer* current = commands.data[index];
        const bool live = current && current->state == device &&
                          current->context && current->context->active &&
                          &current->context->commands == current &&
                          current->command_buffer ==
                              current->context->command_buffer &&
                          current->recording && !current->rendering;
        assert(live && "command buffer batch contains an invalid handle");
        assert((index == 0 ||
                !current->pending_texture_initializations.first) &&
               "texture initialization commands must be submitted first");
        require_vk(vkEndCommandBuffer(current->command_buffer));
        current->recording = false;
        current->context->active = false;
        assert(device->active_command_buffers != 0);
        --device->active_command_buffers;
        device->command_submit_infos[index] = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = current->command_buffer,
            .deviceMask = 1,
        };
    }
    assert(device->active_command_buffers == 0);
    const uint64_t retirement = device->next_command_retirement();
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
        .commandBufferInfoCount = static_cast<uint32_t>(commands.size),
        .pCommandBufferInfos = device->command_submit_infos.data(),
        .signalSemaphoreInfoCount = signal_semaphore ? 2u : 1u,
        .pSignalSemaphoreInfos = work_signal_infos,
      },
      {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &retirement_signal_info,
      },
    };
    require_vk(vkQueueSubmit2(
        device->queue, 2, submit_infos, VK_NULL_HANDLE));

    for (size_t index = 0; index < commands.size; ++index)
    {
        CommandBuffer* current = commands.data[index];
        detail::CommandContext* context = current->context;
        complete_texture_initializations(*current);
        context->retire_value = retirement;
        *current = {};
    }
    completion_semaphore->last_signal = completion.value;
}

} // namespace

void submit(Span<CommandBuffer* const> commands,
            TimelinePoint completion) noexcept
{
    assert(commands.data && commands.size != 0);
    CommandBuffer* first = commands.data[0];
    assert(first && first->state);
    Device* device = first->state;
    assert(!device->acquired_swapchain &&
           "an acquired swapchain frame must be submitted with submit_and_present");
#if !defined(NDEBUG)
    for (size_t index = 0; index < commands.size; ++index)
        assert(!commands.data[index]->swapchain &&
               "swapchain command buffers must be submitted with submit_and_present");
#endif
    submit_commands(
        commands, device, completion, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

void submit_and_present(Device* device,
                        Span<CommandBuffer* const> commands,
                        TimelinePoint completion) noexcept
{
    const bool has_swapchain =
        device && device->swapchain &&
        device->swapchain->state == device;
    assert(has_swapchain &&
           "submit_and_present received a device without a swapchain");
    Swapchain* swapchain = device->swapchain;
    assert(commands.data && commands.size != 0);
    CommandBuffer* first_commands = commands.data[0];
    CommandBuffer* last_commands = commands.data[commands.size - 1];
    assert(first_commands && first_commands->state &&
           last_commands && last_commands->state == first_commands->state &&
           last_commands->recording && !last_commands->rendering);
    Device* command_device = first_commands->state;
    const bool valid = swapchain->acquired &&
                       command_device == device &&
                       first_commands->swapchain == swapchain &&
                       swapchain->transition_commands == first_commands &&
                       swapchain->present_context;
    assert(valid &&
           "submit_and_present received an invalid swapchain command buffer batch");
#if !defined(NDEBUG)
    for (size_t index = 1; index < commands.size; ++index)
        assert(!commands.data[index]->swapchain &&
               "only the first command buffer may own the swapchain transition");
#endif

    Device& owner = *device;
    detail::PresentContext& present_context = *swapchain->present_context;
    Texture& texture = swapchain->textures[swapchain->image_index];
    const VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture.image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    record_image_barriers(last_commands->command_buffer, {&barrier, 1});
    assert(!present_context.present_pending &&
           !present_context.swapchain);
    require_vk(vkResetFences(
        owner.device, 1, &present_context.presented));
    swapchain->transition_commands = nullptr;
    submit_commands(
        commands, command_device, completion, present_context.acquired,
        present_context.rendered);
    swapchain->initialized[swapchain->image_index] = true;

    const VkSwapchainPresentFenceInfoEXT fence_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
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
        swapchain->recreate_required =
            swapchain->recreate_required ||
            swapchain_surface_configuration_changed(*swapchain);
    else if (result != VK_SUCCESS)
        abort_vk_failure(result);
}

void wait_idle(Device* device) noexcept
{
    assert(device && "wait_idle called with a null device");
    assert(device->active_command_buffers == 0 &&
           "wait_idle is not allowed while a command buffer is recording");
    assert(!device->acquired_swapchain &&
           "wait_idle is not allowed while a swapchain image is acquired");
    device->drain_contexts();
    detail::CommandContext* context = device->next_command_context;
    do
    {
        device->reset_command_context(*context);
        context = context->next;
    } while (context != device->next_command_context);
    device->next_present_context = 0;
}

const DeviceCaps& get_device_caps(const Device* device) noexcept
{
    assert(device && "get_device_caps called with a null device");
    return device->caps;
}

bool supports_texture_format(const Device* device, Format format, TextureUsage usage) noexcept
{
    const bool valid = device && is_concrete_format(format) && has_valid_texture_usage_bits(usage);
    assert(valid &&
           "supports_texture_format requires a device, concrete format, and non-empty usage");

    const TextureFormatInfo info = get_texture_format_info(format);
    if ((info.depth || info.stencil) && has_flag(usage, TextureUsage::color_attachment))
    {
        return false;
    }
    if (!info.depth && !info.stencil && has_flag(usage, TextureUsage::depth_stencil_attachment))
    {
        return false;
    }
    if (info.depth && info.stencil &&
        (has_flag(usage, TextureUsage::transfer_source) ||
         has_flag(usage, TextureUsage::transfer_destination)))
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

    const VkFormatFeatureFlags2 available =
        device->format_features[static_cast<uint32_t>(format)];
    const VkFormatFeatureFlags2 required = required_format_features(usage);
    return (available & required) == required;
}

void destroy_device(Device* device) noexcept
{
    delete device;
}

void destroy_texture(Texture* texture) noexcept
{
    if (!texture)
        return;
    const bool valid = texture->state && !texture->swapchain_image &&
                       texture->live_render_views == 0;
    assert(valid &&
           "destroy_texture requires a non-swapchain texture with no live render views");
    Device* device = texture->state;
    assert(device->active_command_buffers == 0 &&
           "texture destruction is not allowed while a command buffer is recording");
    if (texture->initialization.owner)
        detail::remove_texture_initialization(texture->initialization);

    const VkImage image =
        texture->owns_image ? texture->image : VK_NULL_HANDLE;
    detail::ImageHeap* heap =
        texture->owns_heap_allocation ? texture->heap : nullptr;
    const uint64_t heap_token =
        static_cast<uint64_t>(texture->heap_allocation.offset) |
        (static_cast<uint64_t>(texture->heap_allocation.metadata) << 32);
    texture->state = nullptr;
    delete texture;

    if (image)
    {
        device->defer_delete(
            [image](Device& owner, uint64_t) noexcept {
                vkDestroyImage(owner.device, image, nullptr);
            });
    }
    if (heap)
    {
        device->defer_delete(
            [heap](Device& owner, uint64_t token) noexcept {
                const OffsetAllocator::Allocation allocation{
                    .offset = static_cast<uint32_t>(token),
                    .metadata =
                        static_cast<OffsetAllocator::NodeIndex>(token >> 32),
                };
                heap->offsets.free(allocation);
                owner.active_image_heaps[heap->memory_type] = heap;
            },
            heap_token);
    }
}

void destroy_render_view(RenderView* render_view) noexcept
{
    if (!render_view)
        return;
    assert(!render_view->swapchain_view &&
           "swapchain render views are owned by their swapchain");
    Device* device = render_view->state;
    const bool valid = device && render_view->texture &&
                       render_view->texture->state == device &&
                       render_view->view &&
                       render_view->texture->live_render_views != 0;
    assert(valid && "render view state is invalid");
    assert(device->active_command_buffers == 0 &&
           "render view destruction is not allowed while a command buffer is recording");
    const VkImageView view = render_view->view;
    --render_view->texture->live_render_views;
    *render_view = {};
    delete render_view;
    device->defer_delete(
        [view](Device& owner, uint64_t) noexcept {
            vkDestroyImageView(owner.device, view, nullptr);
        });
}

void destroy_pso(PSO* pso) noexcept
{
    if (!pso)
        return;
    Device* device = pso->state;
    const bool valid = device && pso->pso;
    assert(valid && "destroy_pso received an invalid PSO");
    assert(device->active_command_buffers == 0 &&
           "PSO destruction is not allowed while a command buffer is recording");
    const VkPipeline handle = pso->pso;
    pso->state = nullptr;
    pso->pso = VK_NULL_HANDLE;
    delete pso;
    device->defer_delete(
        [handle](Device& owner, uint64_t) noexcept {
            vkDestroyPipeline(owner.device, handle, nullptr);
        });
}

void bind_pso(CommandBuffer* commands, const PSO* pso) noexcept
{
    const bool valid = commands && commands->recording && pso;
    assert(valid && "bind_pso received an empty command buffer or PSO");
    const bool same_device = pso->state == commands->state;
    assert(same_device && "PSO belongs to a different device");
    vkCmdBindPipeline(
        commands->command_buffer, pso->bind_point, pso->pso);
#if !defined(NDEBUG)
    if (pso->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS)
    {
        commands->bound_graphics = pso;
        commands->graphics_compatibility_dirty = true;
    }
    else
        commands->bound_compute = pso;
#endif
}

AddressRange validate_range(CommandBuffer* commands, GpuRange range) noexcept
{
    const bool valid = commands && commands->recording;
    assert(valid && "address commands require a recording command buffer");
    const bool nonempty = range.gpu && range.size != 0;
    assert(nonempty && "GPU range must have a non-null pointer and non-zero size");
    const auto address = static_cast<VkDeviceAddress>(
        reinterpret_cast<uintptr_t>(range.gpu));
    const bool address_fits =
        address <= std::numeric_limits<VkDeviceAddress>::max() - range.size;
    assert(address_fits && "GPU range address overflow");
    return {
        .address = address,
        .size = range.size,
    };
}

void validate_texture(CommandBuffer* commands, Texture* texture) noexcept
{
    const bool valid = texture && texture->state == commands->state;
    assert(valid && "texture is empty or belongs to a different device");
}

VkDeviceMemoryImageCopyKHR make_texture_copy_region(
    const Texture& texture,
    const TextureCopyDesc& copy,
    const AddressRange& memory) noexcept
{
    const bool valid_mip = copy.mip_level < texture.mip_levels;
    assert(valid_mip && "texture copy mip level is invalid");

    uint32_t mip_width = texture.width >> copy.mip_level;
    uint32_t mip_height = texture.height >> copy.mip_level;
    uint32_t mip_depth = texture.depth >> copy.mip_level;
    if (mip_width == 0) mip_width = 1;
    if (mip_height == 0) mip_height = 1;
    if (mip_depth == 0) mip_depth = 1;

    const bool valid_offsets = copy.x < mip_width && copy.y < mip_height &&
                               copy.z < mip_depth;
    const uint32_t width = valid_offsets
        ? (copy.width == 0 ? mip_width - copy.x : copy.width)
        : 0;
    const uint32_t height = valid_offsets
        ? (copy.height == 0 ? mip_height - copy.y : copy.height)
        : 0;
    const uint32_t depth = valid_offsets
        ? (copy.depth == 0 ? mip_depth - copy.z : copy.depth)
        : 0;
    const bool valid_extent = valid_offsets && width != 0 && height != 0 &&
                              depth != 0 && width <= mip_width - copy.x &&
                              height <= mip_height - copy.y &&
                              depth <= mip_depth - copy.z;
    const bool valid_slice_base = copy.base_slice < texture.array_layers;
    const uint32_t slice_count = valid_slice_base
        ? (copy.slice_count == 0
               ? texture.array_layers - copy.base_slice
               : copy.slice_count)
        : 0;
    const bool valid_slices = valid_slice_base && slice_count != 0 &&
                              slice_count <= texture.array_layers - copy.base_slice;
    const bool valid_3d_slices = texture.type != TextureType::three_d ||
                                 (copy.base_slice == 0 && slice_count == 1);
    assert(valid_extent && "texture copy offset or extent is outside the mip level");
    assert(valid_slices && "texture copy array-slice range is invalid");
    assert(valid_3d_slices && "3D texture copies use exactly one array slice");

    const TextureFormatInfo format_info = get_texture_format_info(texture.format);
    const bool valid_block_offset =
        copy.x % format_info.block_width == 0 && copy.y % format_info.block_height == 0;
    const bool valid_block_extent =
        (width % format_info.block_width == 0 || copy.x + width == mip_width) &&
        (height % format_info.block_height == 0 || copy.y + height == mip_height);
    assert(valid_block_offset && "texture copy offsets must be aligned to the texel-block extent");
    assert(valid_block_extent &&
           "texture copy extents must be block aligned or reach the mip edge");

    const uint64_t width_in_blocks = divide_up(width, format_info.block_width);
    const uint64_t height_in_blocks = divide_up(height, format_info.block_height);
    const bool packed_row_fits =
        width_in_blocks <= std::numeric_limits<uint64_t>::max() / format_info.bytes_per_block;
    const uint64_t packed_row_size =
        packed_row_fits ? width_in_blocks * format_info.bytes_per_block : 0;
    const uint64_t row_pitch = copy.row_pitch_bytes == 0
        ? packed_row_size
        : copy.row_pitch_bytes;
    const uint64_t row_blocks =
        format_info.bytes_per_block != 0 ? row_pitch / format_info.bytes_per_block : 0;
    const bool row_texel_count_fits =
        row_blocks <= std::numeric_limits<uint32_t>::max() / format_info.block_width;
    const bool valid_row_pitch =
        packed_row_fits && row_pitch >= packed_row_size &&
        row_pitch % format_info.bytes_per_block == 0 &&
        row_pitch <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) &&
        row_texel_count_fits;
    const bool packed_slice_fits =
        height_in_blocks == 0 ||
        row_pitch <= std::numeric_limits<uint64_t>::max() / height_in_blocks;
    const uint64_t packed_slice_size = packed_slice_fits ? row_pitch * height_in_blocks : 0;
    const uint64_t slice_pitch = copy.slice_pitch_bytes == 0
        ? packed_slice_size
        : copy.slice_pitch_bytes;
    const uint64_t slice_block_rows = row_pitch != 0 ? slice_pitch / row_pitch : 0;
    const bool image_height_fits =
        slice_block_rows <= std::numeric_limits<uint32_t>::max() / format_info.block_height;
    const bool valid_slice_pitch = packed_slice_fits && slice_pitch >= packed_slice_size &&
                                   slice_pitch % row_pitch == 0 && image_height_fits;
    assert(valid_row_pitch &&
           "texture copy row pitch must fit, contain a row, and be block aligned");
    assert(valid_slice_pitch && "texture copy slice pitch must fit and contain whole block rows");

    const uint64_t total_slices =
        static_cast<uint64_t>(slice_count) * depth;
    bool required_size_fits = total_slices != 0 &&
                              total_slices - 1 <=
                                  std::numeric_limits<uint64_t>::max() /
                                      slice_pitch;
    uint64_t required_size = required_size_fits
        ? (total_slices - 1) * slice_pitch
        : 0;
    const uint64_t preceding_rows = height_in_blocks - 1u;
    required_size_fits = required_size_fits &&
                         preceding_rows <=
                             std::numeric_limits<uint64_t>::max() / row_pitch;
    const uint64_t row_bytes = required_size_fits
        ? preceding_rows * row_pitch
        : 0;
    required_size_fits = required_size_fits &&
                         required_size <=
                             std::numeric_limits<uint64_t>::max() - row_bytes;
    if (required_size_fits)
        required_size += row_bytes;
    required_size_fits = required_size_fits &&
                         required_size <=
                             std::numeric_limits<uint64_t>::max() - packed_row_size;
    if (required_size_fits)
        required_size += packed_row_size;
    const bool memory_fits = required_size_fits && required_size <= memory.size;
    const VkDeviceAddress address = memory_fits ? memory.address : 0;
    const uint64_t address_alignment =
        format_info.depth || format_info.stencil ? 4u : format_info.bytes_per_block;
    const bool aligned = memory_fits && address % address_alignment == 0;
    assert(memory_fits && "texture copy memory range is too small");
    assert(aligned && "texture copy memory address is not format aligned");

    const bool copyable_format = !format_info.depth || !format_info.stencil;
    assert(copyable_format &&
           "combined depth/stencil copies require an aspect selector");
    const VkImageAspectFlags copy_aspect = image_aspects(texture.format);
    return {
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR,
        .addressRange =
            {
                .address = address,
                .size = required_size,
            },
        .addressFlags = address_flags,
        .addressRowLength = copy.row_pitch_bytes == 0
                                ? 0u
                                : static_cast<uint32_t>(row_blocks * format_info.block_width),
        .addressImageHeight =
            copy.slice_pitch_bytes == 0
                ? 0u
                : static_cast<uint32_t>(slice_block_rows * format_info.block_height),
        .imageSubresource =
            {
                .aspectMask = copy_aspect,
                .mipLevel = copy.mip_level,
                .baseArrayLayer = copy.base_slice,
                .layerCount = slice_count,
            },
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageOffset =
            {
                .x = static_cast<int32_t>(copy.x),
                .y = static_cast<int32_t>(copy.y),
                .z = static_cast<int32_t>(copy.z),
            },
        .imageExtent =
            {
                .width = width,
                .height = height,
                .depth = depth,
            },
    };
}

void require_graphics_pso(CommandBuffer* commands, PSO::Kind kind) noexcept
{
#if !defined(NDEBUG)
    assert(commands->bound_graphics && "draw requires a bound graphics PSO");
    const PSO& pso = *commands->bound_graphics;
    if (commands->graphics_compatibility_dirty)
    {
        bool compatible =
            pso.color_count == commands->rendering_color_count &&
            (pso.depth_format != Format::undefined) ==
                commands->rendering_has_depth &&
            (pso.stencil_format != Format::undefined) ==
                commands->rendering_has_stencil &&
            (!commands->rendering_has_depth ||
             pso.depth_format == commands->rendering_depth_format) &&
            (!commands->rendering_has_stencil ||
             pso.stencil_format == commands->rendering_stencil_format);
        for (uint32_t index = 0;
             compatible && index < pso.color_count;
             ++index)
        {
            compatible = pso.color_formats[index] ==
                         commands->rendering_color_formats[index];
        }
        commands->graphics_compatible = compatible;
        commands->graphics_compatibility_dirty = false;
    }
    const bool kind_matches = pso.kind == kind;
    assert(kind_matches && "draw type does not match the bound graphics PSO");
    assert(commands->graphics_compatible &&
           "graphics PSO formats do not match the active attachments");
#else
    (void)commands;
    (void)kind;
#endif
}

void require_compute_pso(CommandBuffer* commands) noexcept
{
#if !defined(NDEBUG)
    assert(commands->bound_compute && "dispatch requires a bound compute PSO");
#else
    (void)commands;
#endif
}

void emit_root_data(CommandBuffer* commands, ByteSpan root) noexcept
{
    const bool has_data = root.data != nullptr;
    const bool valid = has_data == (root.size != 0) &&
                       (!has_data || ((root.size & 3u) == 0 &&
                                     root.size <= commands->state->heap_properties.maxPushDataSize));
    assert(valid && "draw/dispatch root data must be null with zero size or non-null, "
                    "four-byte sized, and fit "
                    "VkPhysicalDeviceDescriptorHeapPropertiesEXT::maxPushDataSize");
    if (!has_data)
        return;
    const VkPushDataInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
        .data =
            {
                .address = root.data,
                .size = root.size,
            },
    };
    commands->state->fn.cmd_push_data(commands->command_buffer, &info);
}

void set_texture_heap(CommandBuffer* commands, GpuRange heap) noexcept
{
    const bool valid = commands && commands->recording;
    assert(valid && "set_texture_heap requires a recording command buffer");
    const auto& properties = commands->state->heap_properties;
    VkBindHeapInfoEXT bind_info{};
    make_heap_bind_info(
        heap,
        properties.resourceHeapAlignment,
        properties.imageDescriptorAlignment > properties.bufferDescriptorAlignment
            ? properties.imageDescriptorAlignment
            : properties.bufferDescriptorAlignment,
        properties.minResourceHeapReservedRange,
        properties.maxResourceHeapSize,
        bind_info);
    commands->state->fn.cmd_bind_texture_heap(commands->command_buffer, &bind_info);
}

void set_sampler_heap(CommandBuffer* commands, GpuRange heap) noexcept
{
    const bool valid = commands && commands->recording;
    assert(valid && "set_sampler_heap requires a recording command buffer");
    const auto& properties = commands->state->heap_properties;
    VkBindHeapInfoEXT bind_info{};
    make_heap_bind_info(heap,
                        properties.samplerHeapAlignment,
                        properties.samplerDescriptorAlignment,
                        properties.minSamplerHeapReservedRange,
                        properties.maxSamplerHeapSize,
                        bind_info);
    commands->state->fn.cmd_bind_sampler_heap(commands->command_buffer, &bind_info);
}

void begin_render_pass(CommandBuffer* commands, const RenderingDesc& desc) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering &&
                       (desc.colors.size == 0 || desc.colors.data) &&
                       desc.colors.size <= max_color_attachments &&
                       (!commands || desc.colors.size <=
                           commands->state->physical_properties.limits.maxColorAttachments);
    assert(valid &&
           "begin_render_pass requires an idle recording command buffer and a supported color count");

    VkRenderingAttachmentInfo color_attachments[max_color_attachments]{};
    uint32_t width = 0;
    uint32_t height = 0;
    for (size_t index = 0; index < desc.colors.size; ++index)
    {
        const ColorAttachment& attachment = desc.colors.data[index];
        RenderView* render_view = attachment.render_view;
        Texture* texture = render_view ? render_view->texture : nullptr;
        const bool valid_color = render_view &&
            render_view->state == commands->state &&
            render_view->view &&
            texture && texture->state == commands->state &&
            has_flag(texture->usage, TextureUsage::color_attachment) &&
            is_color_format(texture->format);
        assert(valid_color &&
               "every color render target must belong to the device and support color attachments");
        if (width == 0)
        {
            width = render_view->width;
            height = render_view->height;
        }
        const bool matching = render_view->width == width &&
                              render_view->height == height;
        assert(matching && "rendering attachment dimensions differ");
        validate_texture(commands, texture);
        color_attachments[index] = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = render_view->view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = to_vk(attachment.load),
            .storeOp = to_vk(attachment.store),
            .clearValue = {
                .color = {
                    .float32 = {
                        attachment.clear.x,
                        attachment.clear.y,
                        attachment.clear.z,
                        attachment.clear.w,
                    },
                },
            },
        };
    }

    RenderView* depth_view = desc.depth.render_view;
    RenderView* stencil_view = desc.stencil.render_view;
    Texture* depth_texture = depth_view ? depth_view->texture : nullptr;
    Texture* stencil_texture = stencil_view ? stencil_view->texture : nullptr;
    const bool has_attachment = desc.colors.size != 0 || depth_view || stencil_view;
    assert(has_attachment && "begin_render_pass requires at least one attachment");
    if (depth_view)
    {
        const bool valid_depth = depth_view->state == commands->state &&
            depth_view->view &&
            depth_texture && depth_texture->state == commands->state &&
            has_flag(depth_texture->usage, TextureUsage::depth_stencil_attachment) &&
            has_depth_aspect(depth_texture->format);
        assert(valid_depth &&
                "depth render target must belong to the device and have a depth aspect");
        if (width == 0)
        {
            width = depth_view->width;
            height = depth_view->height;
        }
        const bool matching = depth_view->width == width &&
                              depth_view->height == height;
        const bool valid_clear = desc.depth.load != LoadOp::clear ||
            (desc.depth.clear >= 0.0f && desc.depth.clear <= 1.0f);
        assert(matching && "depth attachment dimensions differ");
        assert(valid_clear && "clear depth must be in the [0, 1] range");
        validate_texture(commands, depth_texture);
    }
    if (stencil_view)
    {
        const bool valid_stencil = stencil_view->state == commands->state &&
            stencil_view->view &&
            stencil_texture && stencil_texture->state == commands->state &&
            has_flag(stencil_texture->usage, TextureUsage::depth_stencil_attachment) &&
            has_stencil_aspect(stencil_texture->format);
        assert(valid_stencil &&
                "stencil render target must belong to the device and have a stencil aspect");
        if (width == 0)
        {
            width = stencil_view->width;
            height = stencil_view->height;
        }
        const bool matching = stencil_view->width == width &&
                              stencil_view->height == height;
        const bool same_depth_stencil_view = !depth_view ||
                                             depth_view == stencil_view;
        assert(matching && "stencil attachment dimensions differ");
        assert(same_depth_stencil_view &&
                "depth and stencil attachments must reference the same render view");
        validate_texture(commands, stencil_texture);
    }

    VkImageView depth_stencil_view = VK_NULL_HANDLE;
    if (depth_view)
        depth_stencil_view = depth_view->view;
    else if (stencil_view)
        depth_stencil_view = stencil_view->view;

    const VkRenderingAttachmentInfo depth_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = depth_view ? depth_stencil_view : VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = to_vk(desc.depth.load),
        .storeOp = to_vk(desc.depth.store),
        .clearValue = {
            .depthStencil = {
                .depth = desc.depth.clear,
            },
        },
    };
    const VkRenderingAttachmentInfo stencil_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = stencil_view ? depth_stencil_view : VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = to_vk(desc.stencil.load),
        .storeOp = to_vk(desc.stencil.store),
        .clearValue = {
            .depthStencil = {
                .depth = 1.0f,
                .stencil = desc.stencil.clear,
            },
        },
    };
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {
            .extent = {.width = width, .height = height},
        },
        .layerCount = 1,
        .colorAttachmentCount = static_cast<uint32_t>(desc.colors.size),
        .pColorAttachments = desc.colors.size ? color_attachments : nullptr,
        .pDepthAttachment = depth_view ? &depth_attachment : nullptr,
        .pStencilAttachment = stencil_view ? &stencil_attachment : nullptr,
    };
    vkCmdBeginRendering(commands->command_buffer, &rendering_info);

    const VkViewport viewport{
        .width = static_cast<float>(width),
        .height = static_cast<float>(height),
        .maxDepth = 1.0f,
    };
    const VkRect2D scissor{
        .extent = {.width = width, .height = height},
    };
    vkCmdSetViewport(commands->command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(commands->command_buffer, 0, 1, &scissor);
    commands->rendering = true;
#if !defined(NDEBUG)
    commands->rendering_color_count = static_cast<uint32_t>(desc.colors.size);
    for (size_t index = 0; index < desc.colors.size; ++index)
    {
        commands->rendering_color_formats[index] =
            desc.colors.data[index].render_view->texture->format;
    }
    commands->rendering_has_depth = depth_view != nullptr;
    commands->rendering_has_stencil = stencil_view != nullptr;
    commands->rendering_depth_format =
        depth_texture ? depth_texture->format : Format::undefined;
    commands->rendering_stencil_format =
        stencil_texture ? stencil_texture->format : Format::undefined;
    commands->graphics_compatibility_dirty = true;
#endif
}

void end_render_pass(CommandBuffer* commands) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "end_render_pass requires an active rendering scope");
    vkCmdEndRendering(commands->command_buffer);
    commands->rendering = false;
#if !defined(NDEBUG)
    commands->rendering_color_count = 0;
    commands->rendering_has_depth = false;
    commands->rendering_has_stencil = false;
    commands->graphics_compatibility_dirty = true;
#endif
}

void draw(CommandBuffer* commands, ByteSpan root, uint32_t vertex_count,
          uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw requires an active rendering scope");
    require_graphics_pso(commands, PSO::Kind::vertex_graphics);
    emit_root_data(commands, root);
    vkCmdDraw(commands->command_buffer, vertex_count, instance_count, first_vertex, first_instance);
}

void draw_indexed(CommandBuffer* commands, ByteSpan root, GpuRange indices, IndexType type,
                  uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                  int32_t vertex_offset, uint32_t first_instance) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw_indexed requires an active rendering scope");
    const bool valid_type = type == IndexType::uint16 || type == IndexType::uint32;
    assert(valid_type && "unknown index type");
    require_graphics_pso(commands, PSO::Kind::vertex_graphics);
    const auto range = validate_range(commands, indices);
    const auto alignment = type == IndexType::uint16 ? 2u : 4u;
    assert(range.address % alignment == 0 && "index address range is empty or misaligned");
    const auto index_size = static_cast<uint64_t>(alignment);
    const bool range_fits =
        first_index <= range.size / index_size &&
        index_count <= (range.size - static_cast<uint64_t>(first_index) * index_size) / index_size;
    assert(range_fits && "indexed draw exceeds the bound index address range");

    const VkBindIndexBuffer3InfoKHR bind_info{
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .addressRange =
            {
                .address = range.address,
                .size = range.size,
            },
        .addressFlags = address_flags,
        .indexType = type == IndexType::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32,
    };
    commands->state->fn.cmd_bind_index_buffer(commands->command_buffer, &bind_info);
    emit_root_data(commands, root);
    vkCmdDrawIndexed(commands->command_buffer, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void draw_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments,
                   uint32_t draw_count, uint32_t stride) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw_indirect requires an active rendering scope");
    require_graphics_pso(commands, PSO::Kind::vertex_graphics);
    if (stride == 0)
        stride = sizeof(VkDrawIndirectCommand);
    const auto required_size =
        draw_count == 0 ? 0ull : static_cast<uint64_t>(draw_count - 1) * stride + sizeof(VkDrawIndirectCommand);
    const bool valid_arguments = draw_count != 0 && stride >= sizeof(VkDrawIndirectCommand) && (stride & 3u) == 0 &&
                                 draw_count <= commands->state->physical_properties.limits.maxDrawIndirectCount;
    assert(valid_arguments && "indirect draw range, count, or stride is invalid");
    const auto range = validate_range(commands, arguments);
    const bool range_fits = (range.address & 3u) == 0 && range.size >= required_size && stride <= range.size;
    assert(range_fits && "indirect draw range, count, or stride is invalid");
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange =
            {
                .address = range.address,
                .size = range.size,
                .stride = stride,
            },
        .addressFlags = address_flags,
        .drawCount = draw_count,
    };
    emit_root_data(commands, root);
    commands->state->fn.cmd_draw_indirect(commands->command_buffer, &info);
}

void draw_indexed_indirect(CommandBuffer* commands, ByteSpan root, GpuRange indices, IndexType type,
                           GpuRange arguments, uint32_t draw_count,
                           uint32_t stride) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw_indexed_indirect requires an active rendering scope");
    const bool valid_type = type == IndexType::uint16 || type == IndexType::uint32;
    assert(valid_type && "unknown index type");
    require_graphics_pso(commands, PSO::Kind::vertex_graphics);
    const auto index_range = validate_range(commands, indices);
    const auto index_alignment = type == IndexType::uint16 ? 2u : 4u;
    const bool valid_index_range = index_range.address % index_alignment == 0;
    assert(valid_index_range && "index address range is empty or misaligned");
    if (stride == 0)
        stride = sizeof(VkDrawIndexedIndirectCommand);
    const auto required_size =
        draw_count == 0 ? 0ull
                        : static_cast<uint64_t>(draw_count - 1) * stride + sizeof(VkDrawIndexedIndirectCommand);
    const bool valid_arguments = draw_count != 0 && stride >= sizeof(VkDrawIndexedIndirectCommand) &&
                                 (stride & 3u) == 0 &&
                                 draw_count <= commands->state->physical_properties.limits.maxDrawIndirectCount;
    assert(valid_arguments && "indexed indirect draw range, count, or stride is invalid");
    const auto argument_range = validate_range(commands, arguments);
    const bool range_fits =
        (argument_range.address & 3u) == 0 && argument_range.size >= required_size && stride <= argument_range.size;
    assert(range_fits && "indexed indirect draw range, count, or stride is invalid");

    const VkBindIndexBuffer3InfoKHR bind_info{
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .addressRange =
            {
                .address = index_range.address,
                .size = index_range.size,
            },
        .addressFlags = address_flags,
        .indexType = type == IndexType::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32,
    };
    commands->state->fn.cmd_bind_index_buffer(commands->command_buffer, &bind_info);
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange =
            {
                .address = argument_range.address,
                .size = argument_range.size,
                .stride = stride,
            },
        .addressFlags = address_flags,
        .drawCount = draw_count,
    };
    emit_root_data(commands, root);
    commands->state->fn.cmd_draw_indexed_indirect(commands->command_buffer, &info);
}

void dispatch(CommandBuffer* commands, ByteSpan root, uint32_t x, uint32_t y,
              uint32_t z) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering;
    assert(valid && "dispatch requires a recording command buffer outside rendering");
    require_compute_pso(commands);
    const auto& limits = commands->state->physical_properties.limits.maxComputeWorkGroupCount;
    const bool count_fits = x <= limits[0] && y <= limits[1] && z <= limits[2];
    assert(count_fits && "dispatch group count exceeds VkPhysicalDeviceLimits::maxComputeWorkGroupCount");
    emit_root_data(commands, root);
    vkCmdDispatch(commands->command_buffer, x, y, z);
}

void dispatch_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering;
    assert(valid && "dispatch_indirect requires a recording command buffer outside rendering");
    require_compute_pso(commands);
    const auto range = validate_range(commands, arguments);
    const bool range_fits = (range.address & 3u) == 0 && range.size >= sizeof(VkDispatchIndirectCommand);
    assert(range_fits && "indirect dispatch range is too small or misaligned");
    const VkDispatchIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DISPATCH_INDIRECT_2_INFO_KHR,
        .addressRange =
            {
                .address = range.address,
                .size = range.size,
            },
        .addressFlags = address_flags,
    };
    emit_root_data(commands, root);
    commands->state->fn.cmd_dispatch_indirect(commands->command_buffer, &info);
}

void draw_meshlets(CommandBuffer* commands, ByteSpan root, uint32_t x, uint32_t y,
                   uint32_t z) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw_meshlets requires an active rendering scope");
    require_graphics_pso(commands, PSO::Kind::mesh_graphics);
    const auto& properties = commands->state->mesh_properties;
    const bool axis_counts_fit = x <= properties.maxMeshWorkGroupCount[0] && y <= properties.maxMeshWorkGroupCount[1] &&
                                 z <= properties.maxMeshWorkGroupCount[2];
    const uint64_t xy = static_cast<uint64_t>(x) * y;
    const bool product_fits = z == 0 || xy <= std::numeric_limits<uint64_t>::max() / z;
    const uint64_t total = product_fits ? xy * z : 0;
    const bool total_count_fits = product_fits && total <= properties.maxMeshWorkGroupTotalCount;
    assert(axis_counts_fit && total_count_fits && "meshlet group count exceeds the device mesh-shader limits");
    emit_root_data(commands, root);
    commands->state->fn.cmd_draw_mesh_tasks(commands->command_buffer, x, y, z);
}

void draw_meshlets_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments,
                            uint32_t draw_count, uint32_t stride) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw_meshlets_indirect requires an active rendering scope");
    require_graphics_pso(commands, PSO::Kind::mesh_graphics);
    if (stride == 0)
        stride = sizeof(VkDrawMeshTasksIndirectCommandEXT);
    const bool valid_arguments = draw_count != 0 && stride >= sizeof(VkDrawMeshTasksIndirectCommandEXT) &&
                                 (stride & 3u) == 0 &&
                                 draw_count <= commands->state->physical_properties.limits.maxDrawIndirectCount;
    assert(valid_arguments && "mesh indirect draw count or stride is invalid");
    const AddressRange range = validate_range(commands, arguments);
    const uint64_t required_size =
        static_cast<uint64_t>(draw_count - 1u) * stride + sizeof(VkDrawMeshTasksIndirectCommandEXT);
    const bool range_fits = (range.address & 3u) == 0 && required_size <= range.size && stride <= range.size;
    assert(range_fits && "mesh indirect draw range is too small or misaligned");
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange =
            {
                .address = range.address,
                .size = range.size,
                .stride = stride,
            },
        .addressFlags = address_flags,
        .drawCount = draw_count,
    };
    emit_root_data(commands, root);
    commands->state->fn.cmd_draw_mesh_tasks_indirect(commands->command_buffer, &info);
}

void copy_memory(CommandBuffer* commands, GpuRange source, GpuRange destination) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering;
    assert(valid && "copy_memory requires a recording command buffer outside rendering");
    const auto source_range = validate_range(commands, source);
    const auto destination_range = validate_range(commands, destination);
    const bool destination_fits = destination_range.size >= source_range.size;
    assert(destination_fits &&
           "copy_memory destination range is smaller than its source range");
    const bool overlaps =
        source_range.address < destination_range.address + source_range.size &&
        destination_range.address < source_range.address + source_range.size;
    assert(!overlaps && "copy_memory source and destination ranges overlap");

    const VkDeviceMemoryCopyKHR region{
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_COPY_KHR,
        .srcRange = {
            .address = source_range.address,
            .size = source_range.size,
        },
        .srcFlags = address_flags,
        .dstRange = {
            .address = destination_range.address,
            .size = destination_range.size,
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

void copy_memory_to_texture(CommandBuffer* commands,
                            GpuRange source,
                            Texture* destination,
                            const TextureCopyDesc& copy) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering && destination;
    assert(valid &&
           "copy_memory_to_texture received an empty object or active rendering scope");
    const bool valid_texture = destination->state == commands->state &&
                               has_flag(destination->usage,
                                        TextureUsage::transfer_destination);
    assert(valid_texture &&
           "texture must belong to the device and support transfer destination use");
    validate_texture(commands, destination);
    const auto source_range = validate_range(commands, source);
    const VkDeviceMemoryImageCopyKHR region =
        make_texture_copy_region(*destination, copy, source_range);
    const VkCopyDeviceMemoryImageInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .image = destination->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.cmd_copy_memory_to_image(commands->command_buffer, &info);
}

void copy_texture_to_memory(CommandBuffer* commands,
                            Texture* source,
                            GpuRange destination,
                            const TextureCopyDesc& copy) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering && source;
    assert(valid &&
           "copy_texture_to_memory received an empty object or active rendering scope");
    const bool valid_texture = source->state == commands->state &&
                               has_flag(source->usage,
                                        TextureUsage::transfer_source);
    assert(valid_texture &&
           "texture must belong to the device and support transfer source use");
    validate_texture(commands, source);
    const auto destination_range = validate_range(commands, destination);
    const VkDeviceMemoryImageCopyKHR region =
        make_texture_copy_region(*source, copy, destination_range);
    const VkCopyDeviceMemoryImageInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .image = source->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.cmd_copy_image_to_memory(commands->command_buffer, &info);
}

void barrier(CommandBuffer* commands,
             Stage before,
             Access before_access,
             Stage after,
             Access after_access) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering;
    assert(valid && "barrier requires a recording command buffer outside rendering");
    const bool valid_host_scopes =
        !has_flag(before, Stage::host) &&
        (!has_flag(after, Stage::host) || has_flag(after_access, Access::host_read));
    assert(valid_host_scopes &&
           "the host stage is destination-only and requires host-read access");
    validate_stage_access(before, before_access);
    validate_stage_access(after, after_access);
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
