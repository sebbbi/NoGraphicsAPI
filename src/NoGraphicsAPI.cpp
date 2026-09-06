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

#include <bit>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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
constexpr uint32_t gpu_allocation_alignment = 16;
constexpr uint32_t max_surface_formats = 64;
constexpr uint32_t format_count = static_cast<uint32_t>(Format::undefined);

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
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && callback_data && callback_data->pMessage)
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
    return {};
}

uint64_t divide_up(uint64_t value, uint64_t divisor) noexcept
{
    assert(divisor != 0);
    return value / divisor + (value % divisor != 0 ? 1u : 0u);
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
    return image_class != FormatCompatibility::none && image_class == format_compatibility(view_format);
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
    assert(false && "unknown NoGraphicsAPI texture type");
    return {};
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
    return {};
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
    return {};
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
    return {};
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
    return {};
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
    return {};
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
    return {};
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
    return {};
}

VkAttachmentStoreOp to_vk(StoreOp op)
{
    switch (op)
    {
    case StoreOp::store: return VK_ATTACHMENT_STORE_OP_STORE;
    case StoreOp::discard: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    assert(false && "unknown attachment store operation");
    return {};
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
    return {};
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
    return {};
}

constexpr VkPipelineStageFlags2 to_vk(Stage stages)
{
    constexpr uint64_t known_bits = static_cast<uint64_t>(Stage::indirect) |
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
    const uint64_t bits = static_cast<uint64_t>(stages);
    const bool valid_bits = (bits & ~known_bits) == 0;
    assert(valid_bits && "pipeline stage mask contains unknown bits");

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
    constexpr uint64_t known_bits = static_cast<uint64_t>(Access::transfer_read) |
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

bool is_usable_memory_type(const VkPhysicalDeviceMemoryProperties& properties,
                           uint32_t index)
{
    if (index >= properties.memoryTypeCount)
        return false;
    const VkMemoryType& type = properties.memoryTypes[index];
    if ((type.propertyFlags & forbidden_memory_properties) != 0 || type.heapIndex >= properties.memoryHeapCount)
        return false;
    return (properties.memoryHeaps[type.heapIndex].flags & VK_MEMORY_HEAP_TILE_MEMORY_BIT_QCOM) == 0;
}

bool has_cpu_visible_device_memory(const VkPhysicalDeviceMemoryProperties& properties)
{
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
    {
        if (!is_usable_memory_type(properties, i))
            continue;
        const VkMemoryType& type = properties.memoryTypes[i];
        if ((type.propertyFlags & cpu_visible_memory_properties) != cpu_visible_memory_properties)
            continue;
        if (type.heapIndex < properties.memoryHeapCount && (properties.memoryHeaps[type.heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            return true;
    }
    return false;
}

bool has_device_local_memory(const VkPhysicalDeviceMemoryProperties& properties)
{
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
    {
        if (!is_usable_memory_type(properties, i))
            continue;
        const VkMemoryType& type = properties.memoryTypes[i];
        const VkMemoryPropertyFlags flags = type.propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 && type.heapIndex < properties.memoryHeapCount &&
            (properties.memoryHeaps[type.heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            return true;
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

struct GpuHeapRecord;
struct CommandContext;
struct PresentContext;

struct DeferredSwapchainImage
{
    uint64_t retire_value = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct SwapchainDeleteQueue
{
    void push(uint64_t retire_value, VkSwapchainKHR swapchain, VkImageView view) noexcept
    {
        assert(swapchain && view);
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
                   "swapchain deletion retire values must be monotonic");
        }
        entries[(first + count) % entries.size()] = {
            .retire_value = retire_value,
            .swapchain = swapchain,
            .view = view,
        };
        ++count;
    }

    void collect(VkDevice device, uint64_t completed_value) noexcept
    {
        while (count != 0 && entries[first].retire_value <= completed_value)
        {
            const DeferredSwapchainImage& entry = entries[first];
            const bool final_image = count == 1 || entries[(first + 1) % entries.size()].swapchain != entry.swapchain;
            vkDestroyImageView(device, entry.view, nullptr);
            if (final_image)
                vkDestroySwapchainKHR(device, entry.swapchain, nullptr);
            entries[first] = {};
            first = (first + 1) % entries.size();
            --count;
        }
        if (count == 0) first = 0;
    }

    std::vector<DeferredSwapchainImage> entries;
    size_t first = 0;
    size_t count = 0;
};

struct RetiredSwapchain
{
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkImageView views[max_swapchain_images]{};
    uint32_t view_count = 0;
};

struct TextureInitialization
{
    VkImage image = VK_NULL_HANDLE;
    VkImageAspectFlags aspect_mask = 0;
    uint32_t mip_levels = 0;
    uint32_t array_layers = 0;
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

void transfer_texture_initializations(TextureInitializationList& destination, TextureInitializationList& source) noexcept
{
    assert(!destination.first && !destination.last);
    destination = source;
    source = {};
    for (TextureInitialization* current = destination.first; current; current = current->next)
    {
        current->owner = &destination;
    }
}

} // namespace detail

struct Swapchain;

struct GpuHeapOwner
{
    Device* state = nullptr;
    void* object = nullptr;
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
    detail::CommandContext* context = nullptr;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    bool recording = false;
    bool rendering = false;
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
    VkPhysicalDeviceVulkan13Properties vulkan13_properties{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heap_properties{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_properties{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    uint64_t max_timeline_value_difference = 0;
    uint64_t max_memory_allocation_size = 0;
    uint64_t texture_heap_alignment = 16;
    uint32_t texture_memory_type = VK_MAX_MEMORY_TYPES;
    detail::DeviceFunctions fn;
    DeviceCaps caps;
    VkFormatFeatureFlags2 format_features[format_count]{};
    bool texture_compression_etc2 = false;
    detail::TextureInitializationList pending_texture_initializations;
    detail::CommandContext* next_command_context = nullptr;
    std::vector<VkCommandBufferSubmitInfo> command_submit_infos;
    VkSemaphore command_retirement = VK_NULL_HANDLE;
    uint64_t command_retirement_value = 0;
    uint64_t completed_command_retirement = 0;
    detail::SwapchainDeleteQueue swapchain_delete_queue;
    detail::PresentContext present_contexts[max_swapchain_images]{};
    detail::RetiredSwapchain retired_swapchains[max_swapchain_images]{};
    Swapchain* swapchain = nullptr;
    Swapchain* acquired_swapchain = nullptr;
    uint32_t active_command_buffers = 0;
    uint32_t present_context_count = 0;
    uint32_t next_present_context = 0;

    ~Device();

    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    void destroy_backing(detail::BackingBuffer& backing) const noexcept
    {
        if (!device) return;
        if (backing.mapped) vkUnmapMemory(device, backing.memory);
        if (backing.buffer) vkDestroyBuffer(device, backing.buffer, nullptr);
        if (backing.memory) free_memory(backing.memory);
        backing = {};
    }

    void allocate_memory(const VkMemoryAllocateInfo& info, VkDeviceMemory& output) const noexcept
    {
        assert(info.memoryTypeIndex < memory_properties.memoryTypeCount);
        assert(info.allocationSize <= max_memory_allocation_size);
        const uint32_t heap_index = memory_properties.memoryTypes[info.memoryTypeIndex].heapIndex;
        assert(heap_index < memory_properties.memoryHeapCount);
        assert(info.allocationSize <= memory_properties.memoryHeaps[heap_index].size);
        require_vk(vkAllocateMemory(device, &info, nullptr, &output));
    }

    void free_memory(VkDeviceMemory memory) const noexcept
    {
        if (memory) vkFreeMemory(device, memory, nullptr);
    }

    [[nodiscard]] bool find_memory_type(uint32_t bits,
                                        VkMemoryPropertyFlags required,
                                        VkMemoryPropertyFlags preferred,
                                        VkDeviceSize minimum_heap_size,
                                        uint32_t& output,
                                        VkMemoryPropertyFlags avoided = 0) const noexcept
    {
        bool has_best = false;
        bool best_is_avoided = false;
        uint32_t best = 0;
        uint32_t best_score = 0;
        VkDeviceSize best_heap_size = 0;
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        {
            if ((bits & (1u << i)) == 0)
                continue;
            const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[i].propertyFlags;
            if ((flags & required) != required)
                continue;
            if (!is_usable_memory_type(memory_properties, i))
                continue;
            const VkMemoryHeap& heap = memory_properties.memoryHeaps[memory_properties.memoryTypes[i].heapIndex];
            if (heap.size < minimum_heap_size ||
                ((required & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 && (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0))
            {
                continue;
            }
            const bool is_avoided = (flags & avoided) != 0;
            const uint32_t score = static_cast<uint32_t>(std::popcount(flags & preferred));
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

    void create_backing_buffer(
        detail::BackingBuffer& output,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred,
        VkMemoryPropertyFlags avoided = 0) const noexcept
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
            requirements.size, memory_type, avoided);
        assert(has_memory_type);
        if (!has_memory_type)
            std::abort();

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

    [[nodiscard]] GpuHeap allocate_gpu_heap(VkDeviceSize size, MemoryType memory) noexcept;
    [[nodiscard]] GpuHeap allocate_descriptor_heap(VkDeviceSize size, MemoryType memory) noexcept;
    void release_gpu_heap(const GpuHeap& heap) noexcept;
    [[nodiscard]] Error create_command_contexts() noexcept;
    [[nodiscard]] Error create_command_context(detail::CommandContext& context) noexcept;
    [[nodiscard]] Error grow_command_context_pool() noexcept;
    void destroy_command_context(detail::CommandContext& context) noexcept;
    void destroy_command_contexts() noexcept;
    void reset_command_context(detail::CommandContext& context) noexcept;
    void reset_retired_command_contexts() noexcept;
    [[nodiscard]] detail::CommandContext& acquire_command_context() noexcept;
    [[nodiscard]] uint64_t next_command_retirement() noexcept;
    void poll_command_retirement() noexcept;
    void wait_command_retirement(uint64_t value) noexcept;
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
    uint32_t memory_type = 0;
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
    const bool valid_alignment = requirements.alignment != 0 && (requirements.alignment & (requirements.alignment - 1)) == 0;
    assert(valid_alignment);
    if (!valid_alignment)
        std::abort();
    if (requirements.alignment > device.texture_heap_alignment)
        device.texture_heap_alignment = requirements.alignment;
}

bool select_texture_memory_type(Device& device) noexcept
{
    const VkFormatFeatureFlags2 color_features = device.format_features[static_cast<uint32_t>(Format::rgba8_unorm)];
    if ((color_features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) == 0)
        return false;
    // Probes cover DCC-capable color, broad 3D, and sampled depth layouts.
    // Resource Memory Association makes the color mask common to ordinary optimal-tiled images. Intersect every public depth/stencil format below.
    const uint32_t probe_2d_size = device.physical_properties.limits.maxImageDimension2D < 2048
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
    uint32_t memory_type_bits = color_requirements.memoryTypeBits;
    include_texture_heap_alignment(device, color_requirements);

    constexpr TextureUsage broad_texture_usage = TextureUsage::sampled | TextureUsage::storage | TextureUsage::transfer_destination;
    const VkFormatFeatureFlags2 broad_features = device.format_features[static_cast<uint32_t>(Format::rgba32_float)];
    const VkFormatFeatureFlags2 broad_required_features = required_format_features(broad_texture_usage);
    if ((broad_features & broad_required_features) == broad_required_features)
    {
        const uint32_t probe_3d_size = device.physical_properties.limits.maxImageDimension3D < 2048
                                           ? device.physical_properties.limits.maxImageDimension3D
                                           : 2048;
        image_info.imageType = VK_IMAGE_TYPE_3D;
        image_info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        image_info.extent = {.width = probe_3d_size, .height = probe_3d_size, .depth = probe_3d_size < 4 ? probe_3d_size : 4};
        image_info.mipLevels = std::bit_width(probe_3d_size);
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
        const VkFormatFeatureFlags2 features = device.format_features[static_cast<uint32_t>(format)];
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

} // namespace

namespace detail
{

struct GpuHeapRecord
{
    explicit GpuHeapRecord(Device* device)
        : owner{
              .state = device,
              .object = this,
          },
          state(device)
    {}

    ~GpuHeapRecord()
    {
        if (state)
            state->destroy_backing(backing);
    }

    GpuHeapOwner owner;
    Device* state = nullptr;
    BackingBuffer backing;
};

} // namespace detail

namespace
{

void destroy_owned_swapchain(Device& device) noexcept;

}

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
    for (uint32_t index = 0; index < present_context_count; ++index)
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
    assert(!next_command_context && !command_retirement && command_submit_infos.empty());
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
    detail::CommandContext* context = new detail::CommandContext;
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
    if (device && context.command_pool) vkDestroyCommandPool(device, context.command_pool, nullptr);
    context = {};
}

void Device::destroy_command_contexts() noexcept
{
    if (next_command_context) next_command_context->previous->next = nullptr;
    while (next_command_context)
    {
        detail::CommandContext* context = next_command_context;
        next_command_context = context->next;
        destroy_command_context(*context);
        delete context;
    }
    command_submit_infos.clear();
    if (device && command_retirement) vkDestroySemaphore(device, command_retirement, nullptr);
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

void Device::reset_retired_command_contexts() noexcept
{
    if (!next_command_context)
        return;
    detail::CommandContext* context = next_command_context;
    do
    {
        if (context->retire_value != 0 && context->retire_value <= completed_command_retirement)
            reset_command_context(*context);
        context = context->next;
    } while (context != next_command_context);
}

detail::CommandContext& Device::acquire_command_context() noexcept
{
    detail::CommandContext* context = next_command_context;
    assert(context);
    if (active_command_buffers == 0 && context->retire_value > completed_command_retirement)
        poll_command_retirement();
    if (context->active || context->retire_value > completed_command_retirement)
    {
        require_error(grow_command_context_pool());
        context = next_command_context->previous;
        assert(context != next_command_context &&
               context->next == next_command_context &&
               !context->active && context->retire_value == 0);
    }
    if (context->retire_value != 0) reset_command_context(*context);
    next_command_context = context->next;
    return *context;
}

namespace
{

uint64_t query_timeline_value(const TimelineSemaphore& semaphore) noexcept
{
    uint64_t value = 0;
    assert_vk(vkGetSemaphoreCounterValue(semaphore.state->device, semaphore.semaphore, &value));
    return value;
}

} // namespace

void Device::poll_command_retirement() noexcept
{
    if (!command_retirement || completed_command_retirement == command_retirement_value)
        return;
    uint64_t completed = 0;
    require_vk(vkGetSemaphoreCounterValue(device, command_retirement, &completed));
    assert(completed >= completed_command_retirement && completed <= command_retirement_value);
    if (completed == completed_command_retirement)
        return;
    completed_command_retirement = completed;
    reset_retired_command_contexts();
    swapchain_delete_queue.collect(device, completed_command_retirement);
}

void Device::wait_command_retirement(uint64_t value) noexcept
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
    require_vk(vkWaitSemaphores(device, &wait_info, UINT64_MAX));
    completed_command_retirement = value;
    reset_retired_command_contexts();
    swapchain_delete_queue.collect(device, completed_command_retirement);
}

uint64_t Device::next_command_retirement() noexcept
{
    const uint64_t next = command_retirement_value + 1;
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
    assert_vk(vkWaitForFences(device, 1, &context.presented, VK_TRUE, UINT64_MAX));
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
    assert(retired.handle && retired.view_count != 0);
    assert(active_command_buffers == 0 && "swapchain retirement is not allowed while a command buffer is recording");
    for (uint32_t index = 0; index < retired.view_count; ++index)
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
    for (uint32_t index = 0; index < present_context_count; ++index)
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

    detail::GpuHeapRecord* heap = new detail::GpuHeapRecord(this);
    create_backing_buffer(heap->backing, size, universal_buffer_usage, required, preferred, avoided);
    return {
        .range = {
            .cpu = static_cast<byte*>(heap->backing.mapped),
            .gpu = reinterpret_cast<byte*>(static_cast<uintptr_t>(heap->backing.address)),
            .size = size,
        },
        .owner = &heap->owner,
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
    const VkDeviceSize maximum_size = texture_heap ? heap_properties.maxResourceHeapSize : heap_properties.maxSamplerHeapSize;

    assert(reserved_alignment != 0);
    assert(reserved_size <= maximum_size && size <= maximum_size - reserved_size && "descriptor heap size exceeds the device limit");
    assert(size <= std::numeric_limits<VkDeviceSize>::max() - (reserved_alignment - 1));
    const VkDeviceSize reserved_offset = align_up(size, reserved_alignment);
    assert(reserved_offset <= maximum_size - reserved_size && "descriptor heap size exceeds the device limit after alignment");
    const VkDeviceSize bind_size = reserved_offset + reserved_size;
    const VkDeviceSize allocation_alignment = heap_alignment > gpu_allocation_alignment ? heap_alignment : gpu_allocation_alignment;
    const VkDeviceSize alignment_padding = allocation_alignment - 1;
    assert(bind_size <= vulkan13_properties.maxBufferSize && "descriptor heap backing exceeds maxBufferSize");
    assert(alignment_padding <= vulkan13_properties.maxBufferSize - bind_size && "descriptor heap backing alignment overflows maxBufferSize");
    const VkDeviceSize backing_size = bind_size + alignment_padding;

    detail::GpuHeapRecord* heap = new detail::GpuHeapRecord(this);
    create_backing_buffer(heap->backing, backing_size, universal_buffer_usage | VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT, cpu_visible_memory_properties, 0);

    const bool valid_base_address = heap->backing.address <= std::numeric_limits<VkDeviceAddress>::max() - alignment_padding;
    assert(valid_base_address && "descriptor heap GPU address cannot be aligned without overflow");
    if (!valid_base_address)
        std::abort();
    const VkDeviceAddress gpu_address = align_up(heap->backing.address, allocation_alignment);
    const VkDeviceSize allocation_offset = gpu_address - heap->backing.address;
    const bool valid_range = heap->backing.mapped && gpu_address % heap_alignment == 0 && gpu_address % gpu_allocation_alignment == 0 &&
                             allocation_offset <= heap->backing.size && bind_size <= heap->backing.size - allocation_offset;
    assert(valid_range && "descriptor heap backing range is invalid");
    if (!valid_range)
        std::abort();
    byte* cpu_address = static_cast<byte*>(heap->backing.mapped) + allocation_offset;
    const bool valid_cpu_address = reinterpret_cast<uintptr_t>(cpu_address) % gpu_allocation_alignment == 0;
    assert(valid_cpu_address && "descriptor heap CPU address is misaligned");
    if (!valid_cpu_address)
        std::abort();

    return {
        .range = {
            .cpu = cpu_address,
            .gpu = reinterpret_cast<byte*>(static_cast<uintptr_t>(gpu_address)),
            .size = size,
        },
        .owner = &heap->owner,
    };
}

void Device::release_gpu_heap(const GpuHeap& heap) noexcept
{
    if (!heap.range.gpu)
    {
        assert(!heap.range.cpu && heap.range.size == 0 && !heap.owner && "destroy_gpu_heap received an invalid empty heap");
        return;
    }

    const GpuHeapOwner* owner = heap.owner;
    assert(owner && owner->state == this && owner->object && "destroy_gpu_heap requires a live heap returned by create_gpu_heap");
    detail::GpuHeapRecord* record = static_cast<detail::GpuHeapRecord*>(owner->object);
    assert(&record->owner == owner && record->state == this && "destroy_gpu_heap owner does not match its device");
    record->owner = {};
    delete record;
}

struct Texture
{
    Device* state = nullptr;
    VkImage image = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint32_t mip_levels = 0;
    uint32_t layer_count = 0;
    TextureType type = TextureType::two_d;
    Format format = Format::rgba8_unorm;
    bool owns_image = true;
    bool swapchain_image = false;
    detail::TextureInitialization initialization;

    ~Texture()
    {
        if (!state)
            return;
        if (initialization.owner) detail::remove_texture_initialization(initialization);
        if (image && owns_image) vkDestroyImage(state->device, image, nullptr);
    }
};

struct RenderView
{
    Device* state = nullptr;
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

struct AddressRange
{
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
};

namespace
{

void record_image_barriers(VkCommandBuffer command_buffer, Span<const VkImageMemoryBarrier2> barriers) noexcept
{
    const bool valid = command_buffer && barriers.data && barriers.size != 0 && barriers.size <= std::numeric_limits<uint32_t>::max();
    assert(valid && "image barrier batch is invalid");
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size),
        .pImageMemoryBarriers = barriers.data,
    };
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

void make_heap_bind_info(GpuRange heap,
                         VkDeviceSize reserved_alignment,
                         VkDeviceSize reserved_size,
                         VkBindHeapInfoEXT& output) noexcept
{
    assert(heap.gpu && heap.size != 0);
    const VkDeviceSize reserved_offset = align_up<VkDeviceSize>(heap.size, reserved_alignment);
    output = {
        .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .heapRange = {
            .address = static_cast<VkDeviceAddress>(reinterpret_cast<uintptr_t>(heap.gpu)),
            .size = reserved_offset + reserved_size,
        },
        .reservedRangeOffset = reserved_offset,
        .reservedRangeSize = reserved_size,
    };
}

Error enumerate_device_extensions(VkPhysicalDevice physical_device, Span<VkExtensionProperties> values, uint32_t& count) noexcept
{
    for (;;)
    {
        uint32_t available = 0;
        Error error = error_from_vk(vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &available, nullptr));
        if (error != Error::none)
            return error;
        if (available > values.size)
            return Error::unsupported;
        count = available;
        const VkResult result = vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, values.data);
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
        Error error = error_from_vk(vkEnumerateInstanceExtensionProperties(nullptr, &available, nullptr));
        if (error != Error::none)
            return error;
        if (available > values.size)
            return Error::unsupported;
        count = available;
        const VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &count, values.data);
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
        Error error = error_from_vk(vkEnumerateInstanceLayerProperties(&available, nullptr));
        if (error != Error::none)
            return error;
        if (available > values.size)
            return Error::unsupported;
        count = available;
        const VkResult result = vkEnumerateInstanceLayerProperties(&count, values.data);
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
    uint32_t queue_family = 0;
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
    VkPhysicalDeviceVulkan11Properties vulkan11_properties{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties vulkan12_properties{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties vulkan13_properties{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_properties{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
};

Error inspect_candidate(VkPhysicalDevice physical_device,
                        VkSurfaceKHR surface,
                        bool khr_surface_maintenance1,
                        bool ext_surface_maintenance1,
                        Candidate& output) noexcept
{
    VkExtensionProperties extensions[max_device_extensions]{};
    uint32_t extension_count = 0;
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
    result.heap_properties.pNext = &result.vulkan11_properties;
    result.vulkan11_properties.pNext = &result.vulkan12_properties;
    result.vulkan12_properties.pNext = &result.vulkan13_properties;
    result.vulkan13_properties.pNext = &result.mesh_properties;
    VkPhysicalDeviceProperties2 properties2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &result.heap_properties,
    };
    vkGetPhysicalDeviceProperties2(physical_device, &properties2);
    result.properties = properties2.properties;
    result.heap_properties.pNext = nullptr;
    result.vulkan11_properties.pNext = nullptr;
    result.vulkan12_properties.pNext = nullptr;
    result.vulkan13_properties.pNext = nullptr;
    if (result.properties.apiVersion < VK_API_VERSION_1_4)
        return Error::unsupported;

    vkGetPhysicalDeviceMemoryProperties(physical_device, &result.memory_properties);
    if (!has_cpu_visible_device_memory(result.memory_properties))
        return Error::unsupported;
    if (!has_device_local_memory(result.memory_properties))
        return Error::unsupported;

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

    uint32_t available_queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &available_queue_count, nullptr);
    if (available_queue_count > max_queue_families)
        return Error::unsupported;
    VkQueueFamilyProperties queues[max_queue_families]{};
    uint32_t queue_count = available_queue_count;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, queues);
    uint32_t queue_family = queue_count;
    constexpr VkQueueFlags required_queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (uint32_t index = 0; index < queue_count; ++index)
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
    const VkPhysicalDeviceDescriptorHeapPropertiesEXT& heap = result.heap_properties;
    const VkDeviceSize resource_descriptor_alignment = heap.imageDescriptorAlignment > heap.bufferDescriptorAlignment
                                                       ? heap.imageDescriptorAlignment
                                                       : heap.bufferDescriptorAlignment;
    const bool valid_heap_properties =
        resource_descriptor_alignment != 0 && (resource_descriptor_alignment & (resource_descriptor_alignment - 1)) == 0 &&
        heap.samplerDescriptorAlignment != 0 && (heap.samplerDescriptorAlignment & (heap.samplerDescriptorAlignment - 1)) == 0 &&
        heap.resourceHeapAlignment != 0 && (heap.resourceHeapAlignment & (heap.resourceHeapAlignment - 1)) == 0 &&
        heap.samplerHeapAlignment != 0 && (heap.samplerHeapAlignment & (heap.samplerHeapAlignment - 1)) == 0 &&
        heap.imageDescriptorSize != 0 && heap.bufferDescriptorSize != 0 &&
        heap.samplerDescriptorSize != 0 &&
        heap.minResourceHeapReservedRange <= heap.maxResourceHeapSize &&
        heap.minSamplerHeapReservedRange <= heap.maxSamplerHeapSize;
    if (!valid_heap_properties)
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
        Error error = error_from_vk(vkEnumeratePhysicalDevices(instance, &available, nullptr));
        if (error != Error::none)
            return error;
        if (available > values.size)
            return Error::unsupported;
        count = available;
        const VkResult result = vkEnumeratePhysicalDevices(instance, &count, values.data);
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
    const bool valid_desc = desc.desired_swapchain_image_count != 0 &&
                            desc.desired_swapchain_image_count <= max_swapchain_images &&
                            (presentation ? is_color_format(desc.swapchain_format) : desc.swapchain_format == Format::undefined);
    assert(valid_desc && "create_device requires a swapchain image count in [1, 8] and a color swapchain format exactly when a window is supplied");
    if (!valid_desc)
        return {.error = Error::unsupported};
#if !defined(_WIN32)
    if (presentation)
        return {.error = Error::unsupported};
#endif

    uint32_t loader_version = VK_API_VERSION_1_0;
    Error error = error_from_vk(vkEnumerateInstanceVersion(&loader_version));
    if (error != Error::none)
        return { .error = error };
    if (loader_version < VK_API_VERSION_1_4)
        return { .error = Error::unsupported };

    Device* state = new Device;
    state->present_context_count = presentation ? desc.desired_swapchain_image_count : 0;
    VkExtensionProperties instance_extensions[max_instance_extensions]{};
    uint32_t instance_extension_count = 0;
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
    uint32_t layer_count = 0;
    error = enumerate_instance_layers({layers, max_instance_layers}, layer_count);
    if (error != Error::none)
        return fail_device_creation(state, error);
    const bool debug_utils_available = has_name({instance_extensions, instance_extension_count}, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const bool validation_available = has_name({layers, layer_count}, "VK_LAYER_KHRONOS_validation");
#endif

    const char* enabled_instance_extensions[6]{};
    uint32_t enabled_instance_extension_count = 0;
    const char* enabled_layers[1]{};
    uint32_t enabled_layer_count = 0;
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
    uint32_t physical_device_count = 0;
    error = enumerate_physical_devices(state->instance, {physical_devices, max_physical_devices}, physical_device_count);
    if (error != Error::none)
        return fail_device_creation(state, error);

    Candidate selected{};
    bool has_selected = false;
    for (uint32_t index = 0; index < physical_device_count; ++index)
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
    state->vulkan13_properties = selected.vulkan13_properties;
    state->mesh_properties = selected.mesh_properties;
    state->max_timeline_value_difference = selected.vulkan12_properties.maxTimelineSemaphoreValueDifference;
    state->max_memory_allocation_size = selected.vulkan11_properties.maxMemoryAllocationSize;
    state->heap_properties.pNext = nullptr;
    state->vulkan13_properties.pNext = nullptr;
    state->mesh_properties.pNext = nullptr;
    state->memory_properties = selected.memory_properties;
    for (uint32_t value = 0; value < format_count; ++value)
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
    enabled_features.vulkan11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &enabled_features.vulkan12,
        .storageBuffer16BitAccess = VK_TRUE,
        .storagePushConstant16 = VK_TRUE,
        .storageInputOutput16 = selected.storage_input_output16,
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
        .maintenance4 = VK_TRUE,
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
        .pNext = presentation ? static_cast<void*>(&enabled_features.swapchain_maintenance1) : nullptr,
        .meshShader = VK_TRUE,
    };
    enabled_features.swapchain_maintenance1 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
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
    state->fn.cmd_draw_mesh_tasks_indirect =load_device_proc<PFN_vkCmdDrawMeshTasksIndirect2EXT>(state->device, "vkCmdDrawMeshTasksIndirect2EXT");
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
    TimelineSemaphore* result = new TimelineSemaphore{
        .state = device,
    };
    require_vk(vkCreateSemaphore(device->device, &create_info, nullptr, &result->semaphore));
    return result;
}

void destroy_timeline_semaphore(TimelineSemaphore* semaphore) noexcept
{
    if (!semaphore)
        return;
    Device* device = semaphore->state;
    const bool valid = device && semaphore->semaphore;
    assert(valid && "destroy_timeline_semaphore received an invalid semaphore");

    const VkSemaphore handle = semaphore->semaphore;
    semaphore->semaphore = VK_NULL_HANDLE;
    semaphore->state = nullptr;
    delete semaphore;
    vkDestroySemaphore(device->device, handle, nullptr);
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
    const bool valid = semaphore && semaphore->state && semaphore->semaphore;
    assert(valid && "wait_timeline requires a live timeline semaphore");

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
        UINT64_MAX));
    semaphore->state->poll_command_retirement();
}

GpuHeap create_gpu_heap(Device* device, uint64_t byte_count, MemoryType memory) noexcept
{
    assert(device && "create_gpu_heap called with a null device");
    assert(byte_count != 0 && "create_gpu_heap byte count must be non-zero");
    switch (memory)
    {
    case MemoryType::cpu_visible:
    case MemoryType::gpu_only:
    case MemoryType::readback:
    case MemoryType::texture_descriptor_heap:
    case MemoryType::sampler_descriptor_heap: return device->allocate_gpu_heap(byte_count, memory);
    default: assert(false && "create_gpu_heap received an invalid memory type"); return {};
    }
}

void destroy_gpu_heap(const GpuHeap& heap) noexcept
{
    if (!heap.range.gpu && !heap.range.cpu && heap.range.size == 0 && !heap.owner)
        return;
    assert(heap.owner && heap.owner->state && "destroy_gpu_heap requires a live heap returned by create_gpu_heap");
    heap.owner->state->release_gpu_heap(heap);
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
                           render_view.view &&
                           render_view.swapchain_view &&
                           texture.state == device &&
                           texture.swapchain_image;
        assert(valid && "swapchain image-view ownership is invalid");
        retired.views[retired.view_count++] = render_view.view;
        render_view = {};
        texture = {};
        swapchain.initialized[index] = false;
    }
    swapchain.image_count = 0;
    swapchain.handle = VK_NULL_HANDLE;
    swapchain.width = 0;
    swapchain.height = 0;

    bool present_pending = false;
    for (uint32_t index = 0; index < device->present_context_count; ++index)
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
    for (uint32_t index = 0; index < device->present_context_count; ++index)
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
    const bool valid = swapchain->state == &device && !swapchain->acquired &&
                       device.acquired_swapchain != swapchain &&
                       device.active_command_buffers == 0;
    assert(valid && "device-owned swapchain destroyed while its image or command buffer is active");
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
    const Error error = error_from_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(swapchain.state->physical_device, swapchain.state->surface, &capabilities));
    require_error(error);

    const VkExtent2D extent = capabilities.currentExtent;
    const uint32_t variable_extent = std::numeric_limits<uint32_t>::max();
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
    if (extent.width == std::numeric_limits<uint32_t>::max())
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
    uint32_t surface_format_count = 0;
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
    uint32_t image_count = 0;
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
        result = vkCreateImageView(device.device, &view_info, nullptr, &views[index]);
        if (result != VK_SUCCESS)
        {
            for (uint32_t created = 0; created < index; ++created)
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
        texture.type = TextureType::two_d;
        texture.format = swapchain.format;
        texture.owns_image = false;
        texture.swapchain_image = true;
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
    if (extent.width == std::numeric_limits<uint32_t>::max() || extent.height == std::numeric_limits<uint32_t>::max())
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
    const bool valid = device && device->swapchain &&
                       device->swapchain->state == device &&
                       !device->swapchain->acquired &&
                       !device->acquired_swapchain &&
                       device->active_command_buffers == 0;
    assert(valid && "acquire received a device without an available swapchain");

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

        uint32_t image_index = 0;
        const VkResult result = vkAcquireNextImageKHR(
            device->device,
            swapchain->handle,
            UINT64_MAX,
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
    assert(is_concrete_format(desc.format) && "texture format must not be undefined");
    assert(has_valid_texture_usage_bits(desc.usage) && "texture usage mask contains unknown bits");

    VkImageUsageFlags usage = 0;
    if (has_flag(desc.usage, TextureUsage::sampled)) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (has_flag(desc.usage, TextureUsage::storage)) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (has_flag(desc.usage, TextureUsage::color_attachment)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (has_flag(desc.usage, TextureUsage::depth_stencil_attachment)) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (has_flag(desc.usage, TextureUsage::transfer_source)) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (has_flag(desc.usage, TextureUsage::transfer_destination)) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    assert(usage != 0);

    const VkImageType image_type = to_vk(desc.type);
    const VkFormat format = to_vk(desc.format);
    const VkFormatFeatureFlags2 sampled_features = required_format_features(TextureUsage::sampled);
    const VkFormatFeatureFlags2 storage_features = required_format_features(TextureUsage::storage);
    uint32_t view_format_count = 1;
    output.view_formats[0] = format;
    for (uint32_t value = 0; desc.mutable_format && value < format_count; ++value)
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

VkMemoryRequirements texture_memory_requirements(Device& device, const PreparedTexture& texture) noexcept
{
    const VkMemoryRequirements requirements = image_memory_requirements(device, texture.image_info);
    const bool valid_requirements = requirements.size != 0 && requirements.alignment != 0 && (requirements.alignment & (requirements.alignment - 1)) == 0;
    assert(valid_requirements);
    if (!valid_requirements)
        std::abort();
    assert(device.caps.texture_heap_alignment != 0 && requirements.alignment <= device.caps.texture_heap_alignment &&
           device.caps.texture_heap_alignment % requirements.alignment == 0 &&
           "texture alignment exceeds the device texture heap alignment");
    assert(device.texture_memory_type < device.memory_properties.memoryTypeCount &&
           (requirements.memoryTypeBits & (1u << device.texture_memory_type)) != 0 &&
           "texture is incompatible with the device texture heap type");
    return requirements;
}

} // namespace

TextureHeap create_texture_heap(Device* device, uint64_t byte_count) noexcept
{
    assert(device && "create_texture_heap called with a null device");
    assert(byte_count != 0 && "create_texture_heap byte count must be non-zero");
    assert(device->texture_memory_type < device->memory_properties.memoryTypeCount);
    const uint32_t memory_heap = device->memory_properties.memoryTypes[device->texture_memory_type].heapIndex;
    assert(memory_heap < device->memory_properties.memoryHeapCount &&
           byte_count <= device->memory_properties.memoryHeaps[memory_heap].size &&
           byte_count <= device->max_memory_allocation_size);
    TextureHeapOwner* owner = new TextureHeapOwner{
        .state = device,
    };
    const VkMemoryAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = byte_count,
        .memoryTypeIndex = device->texture_memory_type,
    };
    device->allocate_memory(allocate_info, owner->memory);
    return {
        .size = byte_count,
        .owner = owner,
    };
}

void destroy_texture_heap(const TextureHeap& heap) noexcept
{
    if (!heap.owner)
    {
        assert(heap.size == 0 && "destroy_texture_heap received an invalid empty heap");
        return;
    }
    const TextureHeapOwner* owner = heap.owner;
    Device* device = owner->state;
    assert(device && owner->memory && heap.size != 0 && "destroy_texture_heap requires a live heap returned by create_texture_heap");
    const VkDeviceMemory memory = owner->memory;
    delete owner;
    device->free_memory(memory);
}

SizeAlign get_texture_size_align(Device* device, const TextureDesc& desc) noexcept
{
    assert(device && "get_texture_size_align called with a null device");
    PreparedTexture texture{};
    prepare_texture(*device, desc, texture);
    const VkMemoryRequirements requirements = texture_memory_requirements(*device, texture);
    return {
        .size = requirements.size,
        .align = requirements.alignment,
    };
}

Texture* create_texture(Device* device, const TextureDesc& desc, const TextureHeap& heap, uint64_t offset) noexcept
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
        .mip_levels = desc.mip_levels,
        .layer_count = desc.layer_count,
        .type = desc.type,
        .format = desc.format,
    };
    require_vk(vkCreateImage(device->device, &texture.image_info, nullptr, &result->image));
    require_vk(vkBindImageMemory(device->device, result->image, owner->memory, offset));

    const VkImageAspectFlags aspect_mask = image_aspects(desc.format);

    result->initialization = {
        .image = result->image,
        .aspect_mask = aspect_mask,
        .mip_levels = desc.mip_levels,
        .array_layers = desc.layer_count,
    };
    detail::append_texture_initialization(device->pending_texture_initializations, result->initialization);
    return result;
}

RenderView* create_render_view(Texture* texture, const RenderViewDesc& desc) noexcept
{
    const bool valid_texture = texture && texture->state;
    const bool valid_subresource = valid_texture && desc.mip_level < texture->mip_levels && desc.slice < texture->layer_count;
    assert(valid_subresource && "render view subresource is invalid");

    uint32_t width = texture->width >> desc.mip_level;
    uint32_t height = texture->height >> desc.mip_level;
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

void write_texture_descriptor(Device* device,
                              void* cpu_destination,
                              const Texture* texture,
                              TextureDescriptorType type,
                              const TextureDescriptorDesc& desc) noexcept
{
    const bool valid = device && cpu_destination && texture && texture->state == device;
    assert(valid && "write_texture_descriptor requires a valid device, destination, and texture from this device");

    const bool valid_type = type == TextureDescriptorType::sampled ||
                            type == TextureDescriptorType::storage;
    assert(valid_type && "texture descriptor type is invalid");
    const uint32_t mip_count = desc.mip_count == 0 ? texture->mip_levels - desc.base_mip : desc.mip_count;
    const uint32_t layer_count = desc.layer_count == 0 ? texture->layer_count - desc.base_layer : desc.layer_count;
    const Format view_format = desc.format == Format::undefined ? texture->format : desc.format;

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
    assert(descriptor_aspect != 0 && "texture descriptor aspect is invalid");
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
            .baseArrayLayer = desc.base_layer,
            .layerCount = layer_count,
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
    assert_vk(device->fn.write_resource_descriptors(device->device, 1, &descriptor_info, &destination));
}

void write_sampler_descriptor(Device* device, void* cpu_destination, const SamplerDesc& desc) noexcept
{
    const bool valid = device && cpu_destination;
    assert(valid && "write_sampler_descriptor requires a valid device and destination");
    const VkSamplerCreateInfo sampler_info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = to_vk(desc.mag_filter),
        .minFilter = to_vk(desc.min_filter),
        .mipmapMode = desc.mip_filter == Filter::nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR,
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
    assert_vk(device->fn.write_sampler_descriptors(device->device, 1, &sampler_info, &destination));
}

namespace
{

VkStencilOpState to_vk(const StencilFaceState& state, uint8_t compare_mask, uint8_t write_mask) noexcept
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
                       bool mesh) noexcept
{
    assert(device && "PSO creation called with a null device");
    const bool valid_color_count = (color_targets.size == 0 || color_targets.data) && color_targets.size <= max_color_attachments &&
        (!device || color_targets.size <= device->physical_properties.limits.maxColorAttachments);
    assert(valid_color_count && "PSO has too many color targets");
    const bool depth_enabled = depth_format != Format::undefined;
    const bool stencil_enabled = stencil_format != Format::undefined;
    const bool valid_depth_format = !depth_enabled || has_depth_aspect(depth_format);
    const bool valid_stencil_format = !stencil_enabled || has_stencil_aspect(stencil_format);
    const bool matching_depth_stencil = !depth_enabled || !stencil_enabled || depth_format == stencil_format;
    assert(valid_depth_format && "PSO depth format has no depth aspect");
    assert(valid_stencil_format && "PSO stencil format has no stencil aspect");
    assert(matching_depth_stencil && "PSO depth and stencil formats must match when both are present");
    const bool valid_depth_write = !depth_stencil_state.depth_write || depth_stencil_state.depth_test;
    const bool depth_state_has_attachment = (!depth_stencil_state.depth_test && !depth_stencil_state.depth_write) || depth_enabled;
    const bool stencil_state_has_attachment = !depth_stencil_state.stencil_test || stencil_enabled;
    assert(valid_depth_write && "depth writes require depth testing");
    assert(depth_state_has_attachment && "enabled depth state requires a PSO depth format");
    assert(stencil_state_has_attachment && "enabled stencil state requires a PSO stencil format");

    for (size_t index = 0; index < color_targets.size; ++index)
    {
        const ColorTargetDesc& target = color_targets.data[index];
        const bool valid_target = is_color_format(target.format) && (target.write_mask & ~0xfu) == 0;
        assert(valid_target && "PSO color target needs a color format and a four-bit write mask");
        const VkFormatFeatureFlags2 format_features = valid_target ? device->format_features[static_cast<uint32_t>(target.format)] : 0;
        const bool supports_attachment = (format_features & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) != 0;
        const bool supports_blending = !target.blend.enabled || (format_features & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT) != 0;
        assert(supports_attachment && "PSO color target format cannot be an attachment");
        assert(supports_blending && "PSO color target format does not support blending");
    }
    assert(!depth_enabled || (device->format_features[static_cast<uint32_t>(depth_format)] & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) != 0);
    assert(!stencil_enabled || (device->format_features[static_cast<uint32_t>(stencil_format)] & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) != 0);

    assert(first_stage_spirv.data && first_stage_spirv.size != 0 && "first-stage SPIR-V shader bytecode is empty");
    assert(fragment_spirv.data && fragment_spirv.size != 0 && "fragment SPIR-V shader bytecode is empty");

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
        .depthTestEnable = depth_stencil_state.depth_test,
        .depthWriteEnable = depth_stencil_state.depth_write,
        .depthCompareOp = to_vk(depth_stencil_state.depth_compare),
        .stencilTestEnable = depth_stencil_state.stencil_test,
        .front = to_vk(depth_stencil_state.front, depth_stencil_state.stencil_read_mask, depth_stencil_state.stencil_write_mask),
        .back = to_vk(depth_stencil_state.back, depth_stencil_state.stencil_read_mask, depth_stencil_state.stencil_write_mask),
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
        .dynamicStateCount = static_cast<uint32_t>(sizeof(dynamic_states) / sizeof(dynamic_states[0])),
        .pDynamicStates = dynamic_states,
    };
    VkFormat color_formats[max_color_attachments]{};
    for (size_t index = 0; index < color_targets.size; ++index)
    {
        color_formats[index] = to_vk(color_targets.data[index].format);
    }
    const VkFormat vk_depth_format = depth_enabled ? to_vk(depth_format) : VK_FORMAT_UNDEFINED;
    const VkFormat vk_stencil_format = stencil_enabled ? to_vk(stencil_format) : VK_FORMAT_UNDEFINED;
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
    PSO* result = new PSO{
        .state = device,
        .bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS,
    };
    const VkResult pso_result = vkCreateGraphicsPipelines(device->device, VK_NULL_HANDLE, 1, &pso_info, nullptr, &result->pso);
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
                             false);
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
                             true);
}

PSO* create_compute_pso(Device* device, Span<const uint32_t> compute_spirv) noexcept
{
    assert(device && "create_compute_pso called with a null device");

    assert(compute_spirv.data && compute_spirv.size != 0 && "compute SPIR-V shader bytecode is empty");
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
    PSO* result = new PSO{
        .state = device,
        .bind_point = VK_PIPELINE_BIND_POINT_COMPUTE,
    };
    const VkResult pso_result = vkCreateComputePipelines(device->device, VK_NULL_HANDLE, 1, &pso_info, nullptr, &result->pso);
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
    detail::transfer_texture_initializations(result->pending_texture_initializations, device->pending_texture_initializations);
    if (result->pending_texture_initializations.first)
    {
        VkImageMemoryBarrier2 barriers[image_barrier_batch_size]{};
        uint32_t barrier_count = 0;
        for (detail::TextureInitialization* initialization = result->pending_texture_initializations.first;
             initialization;
             initialization = initialization->next)
        {
            assert(initialization);
            assert(initialization->owner == &result->pending_texture_initializations);
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
        const bool valid = swapchain->state == device && swapchain->acquired &&
                           swapchain->present_context &&
                           swapchain->image_index < swapchain->image_count;
        assert(valid && "the acquired swapchain state is invalid");
        Texture& texture = swapchain->textures[swapchain->image_index];
        const VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .oldLayout = swapchain->initialized[swapchain->image_index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
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
    detail::TextureInitialization* initialization = commands.pending_texture_initializations.first;
    while (initialization)
    {
        detail::TextureInitialization* next = initialization->next;
        const bool valid = initialization->owner == &commands.pending_texture_initializations;
        assert(valid && "submitted texture initialization state is invalid");
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
                                  completion_semaphore->semaphore;
    assert(valid_completion && "submission completion requires a live timeline semaphore owned by the device");
    assert(commands.data && commands.size != 0 && commands.size <= std::numeric_limits<uint32_t>::max());
    assert(device->active_command_buffers == commands.size && "submit must consume every begun command buffer");
    assert(!device->pending_texture_initializations.first && !device->pending_texture_initializations.last && "pending texture transitions were not recorded");
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
        assert((index == 0 || !current->pending_texture_initializations.first) && "texture initialization commands must be submitted first");
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
    require_vk(vkQueueSubmit2( device->queue, 2, submit_infos, VK_NULL_HANDLE));

    for (size_t index = 0; index < commands.size; ++index)
    {
        CommandBuffer* current = commands.data[index];
        detail::CommandContext* context = current->context;
        complete_texture_initializations(*current);
        context->retire_value = retirement;
        *current = {};
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
           last_commands && last_commands->state == first_commands->state &&
           last_commands->recording && !last_commands->rendering);
    Device* command_device = first_commands->state;
    const bool valid = swapchain->acquired &&
                       command_device == device &&
                       first_commands->swapchain == swapchain &&
                       swapchain->transition_commands == first_commands &&
                       swapchain->present_context;
    assert(valid && "submit_and_present received an invalid swapchain command buffer batch");
#if !defined(NDEBUG)
    for (size_t index = 1; index < commands.size; ++index)
    {
        assert(!commands.data[index]->swapchain && "only the first command buffer may own the swapchain transition");
    }
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
    assert(!present_context.present_pending && !present_context.swapchain);
    require_vk(vkResetFences(owner.device, 1, &present_context.presented));
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
    const bool valid = device && is_concrete_format(format) && has_valid_texture_usage_bits(usage);
    assert(valid && "supports_texture_format requires a device, concrete format, and non-empty usage");

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

    const VkFormatFeatureFlags2 available = device->format_features[static_cast<uint32_t>(format)];
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
    const bool valid = texture->state && !texture->swapchain_image;
    assert(valid && "destroy_texture requires a non-swapchain texture");
    Device* device = texture->state;
    if (texture->initialization.owner) detail::remove_texture_initialization(texture->initialization);

    const VkImage image = texture->owns_image ? texture->image : VK_NULL_HANDLE;
    texture->state = nullptr;
    delete texture;

    if (image)
        vkDestroyImage(device->device, image, nullptr);
}

void destroy_render_view(RenderView* render_view) noexcept
{
    if (!render_view)
        return;
    assert(!render_view->swapchain_view && "swapchain render views are owned by their swapchain");
    Device* device = render_view->state;
    const bool valid = device && render_view->view;
    assert(valid && "render view state is invalid");
    const VkImageView view = render_view->view;
    *render_view = {};
    delete render_view;
    vkDestroyImageView(device->device, view, nullptr);
}

void destroy_pso(PSO* pso) noexcept
{
    if (!pso)
        return;
    Device* device = pso->state;
    const bool valid = device && pso->pso;
    assert(valid && "destroy_pso received an invalid PSO");
    const VkPipeline handle = pso->pso;
    pso->state = nullptr;
    pso->pso = VK_NULL_HANDLE;
    delete pso;
    vkDestroyPipeline(device->device, handle, nullptr);
}

void bind_pso(CommandBuffer* commands, const PSO* pso) noexcept
{
    const bool valid = commands && commands->recording && pso;
    assert(valid && "bind_pso received an empty command buffer or PSO");
    const bool same_device = pso->state == commands->state;
    assert(same_device && "PSO belongs to a different device");
    vkCmdBindPipeline(commands->command_buffer, pso->bind_point, pso->pso);
}

AddressRange validate_range(CommandBuffer* commands, GpuRange range) noexcept
{
    const bool valid = commands && commands->recording;
    assert(valid && "address commands require a recording command buffer");
    const bool nonempty = range.gpu && range.size != 0;
    assert(nonempty && "GPU range must have a non-null pointer and non-zero size");
    return {
        .address = static_cast<VkDeviceAddress>(reinterpret_cast<uintptr_t>(range.gpu)),
        .size = range.size,
    };
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

    const bool valid_offsets = copy.offset.x < mip_width && copy.offset.y < mip_height && copy.offset.z < mip_depth;
    const uint32_t width = valid_offsets ? (copy.extent.x == 0 ? mip_width - copy.offset.x : copy.extent.x) : 0;
    const uint32_t height = valid_offsets ? (copy.extent.y == 0 ? mip_height - copy.offset.y : copy.extent.y) : 0;
    const uint32_t depth = valid_offsets ? (copy.extent.z == 0 ? mip_depth - copy.offset.z : copy.extent.z) : 0;
    const bool valid_extent = valid_offsets && width != 0 && height != 0 &&
                              depth != 0 && width <= mip_width - copy.offset.x &&
                              height <= mip_height - copy.offset.y &&
                              depth <= mip_depth - copy.offset.z;
    const bool valid_slice_base = copy.base_slice < texture.layer_count;
    const uint32_t slice_count = valid_slice_base ? (copy.slice_count == 0 ? texture.layer_count - copy.base_slice : copy.slice_count) : 0;
    const bool valid_slices = valid_slice_base && slice_count != 0 && slice_count <= texture.layer_count - copy.base_slice;
    const bool valid_3d_slices = texture.type != TextureType::three_d || (copy.base_slice == 0 && slice_count == 1);
    assert(valid_extent && "texture copy offset or extent is outside the mip level");
    assert(valid_slices && "texture copy array-slice range is invalid");
    assert(valid_3d_slices && "3D texture copies use exactly one array slice");

    const TextureFormatInfo format_info = get_texture_format_info(texture.format);
    const bool valid_block_offset = copy.offset.x % format_info.block_extent.x == 0 && copy.offset.y % format_info.block_extent.y == 0;
    const bool valid_block_extent =
        (width % format_info.block_extent.x == 0 || copy.offset.x + width == mip_width) &&
        (height % format_info.block_extent.y == 0 || copy.offset.y + height == mip_height);
    assert(valid_block_offset && "texture copy offsets must be aligned to the texel-block extent");
    assert(valid_block_extent && "texture copy extents must be block aligned or reach the mip edge");

    const uint64_t width_in_blocks = divide_up(width, format_info.block_extent.x);
    const uint64_t height_in_blocks = divide_up(height, format_info.block_extent.y);
    const uint64_t packed_row_size = width_in_blocks * format_info.bytes_per_block;
    const uint64_t row_pitch = copy.row_pitch_bytes == 0 ? packed_row_size : copy.row_pitch_bytes;
    assert(row_pitch >= packed_row_size && row_pitch % format_info.bytes_per_block == 0 &&
           row_pitch <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) &&
           "texture copy row pitch must contain a row, be block aligned, and fit Vulkan's 31-bit byte limit");
    const uint64_t packed_slice_size = row_pitch * height_in_blocks;
    const uint64_t slice_pitch = copy.slice_pitch_bytes == 0 ? packed_slice_size : copy.slice_pitch_bytes;
    assert(slice_pitch >= packed_slice_size && slice_pitch % row_pitch == 0 && "texture copy slice pitch must contain whole block rows");

    const uint64_t required_size = (static_cast<uint64_t>(slice_count) * depth - 1) * slice_pitch + (height_in_blocks - 1) * row_pitch + packed_row_size;
    const uint64_t address_alignment = format_info.depth || format_info.stencil ? 4u : format_info.bytes_per_block;
    assert(required_size <= memory.size && "texture copy memory range is too small");
    assert(memory.address % address_alignment == 0 && "texture copy memory address is not format aligned");

    const bool copyable_format = !format_info.depth || !format_info.stencil;
    assert(copyable_format && "combined depth/stencil copies require an aspect selector");
    const VkImageAspectFlags copy_aspect = image_aspects(texture.format);
    return {
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR,
        .addressRange = {
            .address = memory.address,
            .size = required_size,
        },
        .addressFlags = address_flags,
        .addressRowLength = copy.row_pitch_bytes == 0 ? 0u : static_cast<uint32_t>(row_pitch / format_info.bytes_per_block * format_info.block_extent.x),
        .addressImageHeight =
            copy.slice_pitch_bytes == 0 ? 0u : static_cast<uint32_t>(slice_pitch / row_pitch * format_info.block_extent.y),
        .imageSubresource = {
            .aspectMask = copy_aspect,
            .mipLevel = copy.mip_level,
            .baseArrayLayer = copy.base_slice,
            .layerCount = slice_count,
        },
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageOffset = {
            .x = static_cast<int32_t>(copy.offset.x),
            .y = static_cast<int32_t>(copy.offset.y),
            .z = static_cast<int32_t>(copy.offset.z),
        },
        .imageExtent = {
            .width = width,
            .height = height,
            .depth = depth,
        },
    };
}

void emit_root_data(CommandBuffer* commands, ByteSpan root) noexcept
{
    const bool has_data = root.data != nullptr;
    const bool valid = has_data == (root.size != 0) && (!has_data || ((root.size & 3u) == 0 && root.size <= commands->state->heap_properties.maxPushDataSize));
    assert(valid && "draw/dispatch root data must be null with zero size or non-null, four-byte sized, and fit "
                    "VkPhysicalDeviceDescriptorHeapPropertiesEXT::maxPushDataSize");
    if (!has_data)
        return;
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
    const bool valid = commands && commands->recording;
    assert(valid && "set_texture_descriptor_heap requires a recording command buffer");
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
    const bool valid = commands && commands->recording;
    assert(valid && "set_sampler_descriptor_heap requires a recording command buffer");
    const VkPhysicalDeviceDescriptorHeapPropertiesEXT& properties = commands->state->heap_properties;
    VkBindHeapInfoEXT bind_info{};
    make_heap_bind_info(heap,
                        properties.samplerDescriptorAlignment,
                        properties.minSamplerHeapReservedRange,
                        bind_info);
    commands->state->fn.cmd_bind_sampler_heap(commands->command_buffer, &bind_info);
}

void begin_render_pass(CommandBuffer* commands, const RenderingDesc& desc) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering &&
                       (desc.colors.size == 0 || desc.colors.data) && desc.colors.size <= max_color_attachments &&
                       (!commands || desc.colors.size <= commands->state->physical_properties.limits.maxColorAttachments);
    assert(valid && "begin_render_pass requires an idle recording command buffer and a supported color count");

    VkRenderingAttachmentInfo color_attachments[max_color_attachments]{};
    uint32_t width = 0;
    uint32_t height = 0;
    for (size_t index = 0; index < desc.colors.size; ++index)
    {
        const ColorAttachment& attachment = desc.colors.data[index];
        RenderView* render_view = attachment.render_view;
        const bool valid_color = render_view && render_view->state == commands->state && render_view->view;
        assert(valid_color && "every color render target must be a live view from the device");
        if (width == 0)
        {
            width = render_view->width;
            height = render_view->height;
        }
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
    const bool has_attachment = desc.colors.size != 0 || depth_view || stencil_view;
    assert(has_attachment && "begin_render_pass requires at least one attachment");
    if (depth_view)
    {
        const bool valid_depth = depth_view->state == commands->state && depth_view->view;
        assert(valid_depth && "depth render target must be a live view from the device");
        if (width == 0)
        {
            width = depth_view->width;
            height = depth_view->height;
        }
    }
    if (stencil_view)
    {
        const bool valid_stencil = stencil_view->state == commands->state && stencil_view->view;
        assert(valid_stencil && "stencil render target must be a live view from the device");
        if (width == 0)
        {
            width = stencil_view->width;
            height = stencil_view->height;
        }
    }

    const VkRenderingAttachmentInfo depth_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = depth_view ? depth_view->view : VK_NULL_HANDLE,
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
        .imageView = stencil_view ? stencil_view->view : VK_NULL_HANDLE,
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
}

void end_render_pass(CommandBuffer* commands) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "end_render_pass requires an active rendering scope");
    vkCmdEndRendering(commands->command_buffer);
    commands->rendering = false;
}

void draw(CommandBuffer* commands, ByteSpan root, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw requires an active rendering scope");
    emit_root_data(commands, root);
    vkCmdDraw(commands->command_buffer, vertex_count, instance_count, first_vertex, first_instance);
}

void draw_indexed(CommandBuffer* commands, ByteSpan root, GpuRange indices, IndexType type, uint32_t index_count,
                  uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw_indexed requires an active rendering scope");
    const bool valid_type = type == IndexType::uint16 || type == IndexType::uint32;
    assert(valid_type && "unknown index type");
    const AddressRange range = validate_range(commands, indices);
    const uint32_t alignment = type == IndexType::uint16 ? 2u : 4u;
    assert(range.address % alignment == 0 && "index address range is empty or misaligned");
    const uint64_t index_size = static_cast<uint64_t>(alignment);
    const bool range_fits = first_index <= range.size / index_size &&
                            index_count <= (range.size - static_cast<uint64_t>(first_index) * index_size) / index_size;
    assert(range_fits && "indexed draw exceeds the bound index address range");

    const VkBindIndexBuffer3InfoKHR bind_info{
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .addressRange = {
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

void draw_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments, uint32_t draw_count, uint32_t stride) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw_indirect requires an active rendering scope");
    if (stride == 0) stride = sizeof(VkDrawIndirectCommand);
    const uint64_t required_size = draw_count == 0 ? 0ull : static_cast<uint64_t>(draw_count - 1) * stride + sizeof(VkDrawIndirectCommand);
    const bool valid_arguments = draw_count != 0 && stride >= sizeof(VkDrawIndirectCommand) && (stride & 3u) == 0 &&
                                 draw_count <= commands->state->physical_properties.limits.maxDrawIndirectCount;
    assert(valid_arguments && "indirect draw range, count, or stride is invalid");
    const AddressRange range = validate_range(commands, arguments);
    const bool range_fits = (range.address & 3u) == 0 && range.size >= required_size && stride <= range.size;
    assert(range_fits && "indirect draw range, count, or stride is invalid");
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange = {
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
                           GpuRange arguments, uint32_t draw_count, uint32_t stride) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw_indexed_indirect requires an active rendering scope");
    const bool valid_type = type == IndexType::uint16 || type == IndexType::uint32;
    assert(valid_type && "unknown index type");
    const AddressRange index_range = validate_range(commands, indices);
    const uint32_t index_alignment = type == IndexType::uint16 ? 2u : 4u;
    const bool valid_index_range = index_range.address % index_alignment == 0;
    assert(valid_index_range && "index address range is empty or misaligned");
    if (stride == 0) stride = sizeof(VkDrawIndexedIndirectCommand);
    const uint64_t required_size = draw_count == 0 ? 0ull : static_cast<uint64_t>(draw_count - 1) * stride + sizeof(VkDrawIndexedIndirectCommand);
    const bool valid_arguments = draw_count != 0 && stride >= sizeof(VkDrawIndexedIndirectCommand) && (stride & 3u) == 0 &&
                                 draw_count <= commands->state->physical_properties.limits.maxDrawIndirectCount;
    assert(valid_arguments && "indexed indirect draw range, count, or stride is invalid");
    const AddressRange argument_range = validate_range(commands, arguments);
    const bool range_fits = (argument_range.address & 3u) == 0 && argument_range.size >= required_size && stride <= argument_range.size;
    assert(range_fits && "indexed indirect draw range, count, or stride is invalid");

    const VkBindIndexBuffer3InfoKHR bind_info{
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .addressRange = {
            .address = index_range.address,
            .size = index_range.size,
        },
        .addressFlags = address_flags,
        .indexType = type == IndexType::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32,
    };
    commands->state->fn.cmd_bind_index_buffer(commands->command_buffer, &bind_info);
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange = {
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

void dispatch(CommandBuffer* commands, ByteSpan root, uint32x3 group_count) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering;
    assert(valid && "dispatch requires a recording command buffer outside rendering");
    const uint32_t* limits = commands->state->physical_properties.limits.maxComputeWorkGroupCount;
    const bool count_fits = group_count.x <= limits[0] && group_count.y <= limits[1] && group_count.z <= limits[2];
    assert(count_fits && "dispatch group count exceeds VkPhysicalDeviceLimits::maxComputeWorkGroupCount");
    emit_root_data(commands, root);
    vkCmdDispatch(commands->command_buffer, group_count.x, group_count.y, group_count.z);
}

void dispatch_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering;
    assert(valid && "dispatch_indirect requires a recording command buffer outside rendering");
    const AddressRange range = validate_range(commands, arguments);
    const bool range_fits = (range.address & 3u) == 0 && range.size >= sizeof(VkDispatchIndirectCommand);
    assert(range_fits && "indirect dispatch range is too small or misaligned");
    const VkDispatchIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DISPATCH_INDIRECT_2_INFO_KHR,
        .addressRange = {
            .address = range.address,
            .size = range.size,
        },
        .addressFlags = address_flags,
    };
    emit_root_data(commands, root);
    commands->state->fn.cmd_dispatch_indirect(commands->command_buffer, &info);
}

void draw_meshlets(CommandBuffer* commands, ByteSpan root, uint32x3 group_count) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw_meshlets requires an active rendering scope");
    const VkPhysicalDeviceMeshShaderPropertiesEXT& properties = commands->state->mesh_properties;
    const bool axis_counts_fit = group_count.x <= properties.maxMeshWorkGroupCount[0] && group_count.y <= properties.maxMeshWorkGroupCount[1] &&
                                 group_count.z <= properties.maxMeshWorkGroupCount[2];
    const bool total_count_fits = static_cast<uint64_t>(group_count.x) * group_count.y * group_count.z <= properties.maxMeshWorkGroupTotalCount;
    assert(axis_counts_fit && total_count_fits && "meshlet group count exceeds the device mesh-shader limits");
    emit_root_data(commands, root);
    commands->state->fn.cmd_draw_mesh_tasks(commands->command_buffer, group_count.x, group_count.y, group_count.z);
}

void draw_meshlets_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments, uint32_t draw_count, uint32_t stride) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw_meshlets_indirect requires an active rendering scope");
    if (stride == 0) stride = sizeof(VkDrawMeshTasksIndirectCommandEXT);
    const bool valid_arguments = draw_count != 0 && stride >= sizeof(VkDrawMeshTasksIndirectCommandEXT) && (stride & 3u) == 0 &&
                                 draw_count <= commands->state->physical_properties.limits.maxDrawIndirectCount;
    assert(valid_arguments && "mesh indirect draw count or stride is invalid");
    const AddressRange range = validate_range(commands, arguments);
    const uint64_t required_size = static_cast<uint64_t>(draw_count - 1u) * stride + sizeof(VkDrawMeshTasksIndirectCommandEXT);
    const bool range_fits = (range.address & 3u) == 0 && required_size <= range.size && stride <= range.size;
    assert(range_fits && "mesh indirect draw range is too small or misaligned");
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange = {
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
    const AddressRange source_range = validate_range(commands, source);
    const AddressRange destination_range = validate_range(commands, destination);
    const bool destination_fits = destination_range.size >= source_range.size;
    assert(destination_fits && "copy_memory destination range is smaller than its source range");
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

void copy_memory_to_texture(CommandBuffer* commands, GpuRange source, Texture* destination, const TextureCopyDesc& copy) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering && destination;
    assert(valid && "copy_memory_to_texture received an empty object or active rendering scope");
    const bool valid_texture = destination->state == commands->state && destination->image;
    assert(valid_texture && "destination texture must be a live texture from the device");
    const AddressRange source_range = validate_range(commands, source);
    const VkDeviceMemoryImageCopyKHR region = make_texture_copy_region(*destination, copy, source_range);
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
    const bool valid = commands && commands->recording && !commands->rendering && source;
    assert(valid && "copy_texture_to_memory received an empty object or active rendering scope");
    const bool valid_texture = source->state == commands->state && source->image;
    assert(valid_texture && "source texture must be a live texture from the device");
    const AddressRange destination_range = validate_range(commands, destination);
    const VkDeviceMemoryImageCopyKHR region = make_texture_copy_region(*source, copy, destination_range);
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
    const bool valid = commands && commands->recording && !commands->rendering;
    assert(valid && "barrier requires a recording command buffer outside rendering");
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
