#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <type_traits>

#if defined(__MINGW32__)
#error "NoGraphicsAPI does not support MinGW"
#endif
#if defined(_M_ARM64EC) || (!defined(_M_X64) && !defined(__x86_64__))
#error "NoGraphicsAPI requires an x86-64 target"
#endif

static_assert(sizeof(void*) == 8, "NoGraphicsAPI requires a 64-bit pointer ABI");

namespace gpu
{

using std::byte;
using std::int32_t;
using std::size_t;
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

struct uint32x2
{
    uint32_t x = 0;
    uint32_t y = 0;
};

struct uint32x3
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
};

struct Device;
struct Texture;
struct RenderView;
struct PSO;
struct CommandBuffer;
struct TimelineSemaphore;
struct GpuHeapOwner;
struct TextureHeapOwner;

template<typename T>
struct Span
{
    T* data = nullptr;
    size_t size = 0;

    constexpr Span() noexcept = default;

    constexpr Span(T* values, size_t count) noexcept : data(values), size(count) {}

    template<size_t Count>
    constexpr Span(T (&values)[Count]) noexcept : data(values), size(Count) {}

    template<typename Container>
    constexpr Span(Container&& values) noexcept
        requires(
            std::is_same_v<std::remove_cv_t<std::remove_pointer_t<decltype(values.data())>>, std::remove_cv_t<T>> &&
            std::is_convertible_v<decltype(values.data()), T*> &&
            std::is_convertible_v<decltype(values.size()), size_t>)
        : Span(values.data(), values.size()) {}

    // Temporary container and initializer-list storage is valid only through
    // the containing full expression. Functions must not retain the span.
    constexpr Span(std::initializer_list<std::remove_const_t<T>> values) noexcept
        requires std::is_const_v<T>
        : Span(values.begin(), values.size()) {}
};

struct ByteSpan
{
    const byte* data = nullptr;
    size_t size = 0;

    constexpr ByteSpan() noexcept = default;

    ByteSpan(const void* bytes, size_t byte_size) noexcept
        : data(static_cast<const byte*>(bytes)), size(byte_size) {}

    // A span converted from a value remains valid only while that value is
    // alive. Functions must not retain the span.
    template<typename T>
        requires(std::is_class_v<T> && !std::is_volatile_v<T> && std::is_trivially_copyable_v<T> && (sizeof(T) & 3u) == 0)
    ByteSpan(const T& value) noexcept
        : data(reinterpret_cast<const byte*>(std::addressof(value))), size(sizeof(T)) {}
};

enum class Error : uint8_t
{
    none,
    unsupported,
    device_lost,
    driver_error,
};

enum class MemoryType : uint8_t
{
    cpu_visible,
    gpu_only,
    readback,
    texture_descriptor_heap,
    sampler_descriptor_heap,
};

struct SizeAlign
{
    uint64_t size = 0;
    uint64_t align = 0;
};

struct GpuRange
{
    void* gpu = nullptr;
    uint64_t size = 0;
};

template<typename T>
struct GpuCpuRange
{
    T* cpu = nullptr;
    T* gpu = nullptr;
    uint64_t size = 0; // Bytes, independent of T.
};

struct GpuHeap
{
    // Copies alias the same allocation. Pass one unchanged copy to destroy_gpu_heap exactly once.
    GpuCpuRange<byte> range{};
    const GpuHeapOwner* owner = nullptr;
};

struct TextureHeap
{
    // Copies alias the same allocation. Pass one unchanged copy to destroy_texture_heap exactly once.
    uint64_t size = 0;
    const TextureHeapOwner* owner = nullptr;
};

struct TimelinePoint
{
    TimelineSemaphore* semaphore = nullptr;
    uint64_t value = 0;
};

enum class Format : uint8_t
{
    r8_srgb,
    rg8_srgb,
    rgba8_srgb,
    bgra8_srgb,

    rgba4_unorm,
    r5g5b5a1_unorm,
    r5g6b5_unorm,

    r8_unorm,
    rg8_unorm,
    rgba8_unorm,
    bgra8_unorm,
    r16_unorm,
    rg16_unorm,
    rgba16_unorm,

    r8_uint,
    rg8_uint,
    rgba8_uint,
    bgra8_uint,
    r16_uint,
    rg16_uint,
    rgba16_uint,
    r32_uint,
    rg32_uint,
    rgb32_uint,
    rgba32_uint,

    r16_float,
    rg16_float,
    rgba16_float,
    r32_float,
    rg32_float,
    rgb32_float,
    rgba32_float,

    rgb10a2_unorm,
    rg11b10_float,

    d16_unorm,
    d24_unorm_s8_uint,
    d32_float,
    s8_uint,
    d32_float_s8_uint,

    eac_rg,
    astc_4x4_srgb,
    astc_4x4_unorm,
    bc3_srgb,
    bc3_unorm,
    bc5_rg,
    bc7_srgb,
    bc7_unorm,

    undefined, // Must remain last; preceding values are concrete texture formats.
};

struct TextureFormatInfo
{
    uint32x2 block_extent = {};
    uint32_t bytes_per_block = 0;
    bool depth = false;
    bool stencil = false;
};

[[nodiscard]] constexpr TextureFormatInfo get_texture_format_info(Format format) noexcept
{
    switch (format)
    {
    case Format::r8_srgb:
    case Format::r8_unorm:
    case Format::r8_uint:
    case Format::s8_uint:
        return {
            .block_extent = {.x = 1, .y = 1},
            .bytes_per_block = 1,
            .depth = false,
            .stencil = format == Format::s8_uint,
        };
    case Format::rg8_srgb:
    case Format::rgba4_unorm:
    case Format::r5g5b5a1_unorm:
    case Format::r5g6b5_unorm:
    case Format::rg8_unorm:
    case Format::r16_unorm:
    case Format::rg8_uint:
    case Format::r16_uint:
    case Format::r16_float:
    case Format::d16_unorm:
        return {
            .block_extent = {.x = 1, .y = 1},
            .bytes_per_block = 2,
            .depth = format == Format::d16_unorm,
            .stencil = false,
        };
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
    case Format::rg11b10_float:
    case Format::d24_unorm_s8_uint:
    case Format::d32_float:
        return {
            .block_extent = {.x = 1, .y = 1},
            .bytes_per_block = 4,
            .depth = format == Format::d24_unorm_s8_uint || format == Format::d32_float,
            .stencil = format == Format::d24_unorm_s8_uint,
        };
    case Format::rgba16_unorm:
    case Format::rgba16_uint:
    case Format::rg32_uint:
    case Format::rgba16_float:
    case Format::rg32_float:
    case Format::d32_float_s8_uint:
        return {
            .block_extent = {.x = 1, .y = 1},
            .bytes_per_block = 8,
            .depth = format == Format::d32_float_s8_uint,
            .stencil = format == Format::d32_float_s8_uint,
        };
    case Format::rgb32_uint:
    case Format::rgb32_float:
        return {
            .block_extent = {.x = 1, .y = 1},
            .bytes_per_block = 12,
        };
    case Format::rgba32_uint:
    case Format::rgba32_float:
        return {
            .block_extent = {.x = 1, .y = 1},
            .bytes_per_block = 16,
        };
    case Format::eac_rg:
    case Format::astc_4x4_srgb:
    case Format::astc_4x4_unorm:
    case Format::bc3_srgb:
    case Format::bc3_unorm:
    case Format::bc5_rg:
    case Format::bc7_srgb:
    case Format::bc7_unorm:
        return {
            .block_extent = {.x = 4, .y = 4},
            .bytes_per_block = 16,
        };
    case Format::undefined: return {};
    }
    return {};
}

enum class TextureType : uint8_t
{
    one_d,
    two_d,
    three_d,
    cube,
    two_d_array,
    cube_array,
};

enum class TextureUsage : uint32_t
{
    none = 0,
    sampled = 1u << 0u,
    storage = 1u << 1u,
    color_attachment = 1u << 2u,
    depth_stencil_attachment = 1u << 3u,
    transfer_source = 1u << 4u,
    transfer_destination = 1u << 5u,
};

enum class TextureDescriptorType : uint8_t
{
    sampled,
    storage,
};

enum class TextureAspect : uint8_t
{
    automatic,
    color,
    depth,
    stencil,
};

constexpr TextureUsage operator|(TextureUsage lhs, TextureUsage rhs) noexcept
{
    return static_cast<TextureUsage>(static_cast<uint32_t>(lhs) |  static_cast<uint32_t>(rhs));
}

enum class Filter : uint8_t
{
    nearest,
    linear,
};

enum class AddressMode : uint8_t
{
    repeat,
    mirrored_repeat,
    clamp_to_edge,
};

enum class CompareOp : uint8_t
{
    never,
    less,
    equal,
    less_equal,
    greater,
    not_equal,
    greater_equal,
    always,
};

enum class CullMode : uint8_t
{
    none,
    clockwise,
    counter_clockwise,
};

enum class BlendFactor : uint8_t
{
    zero,
    one,
    source_color,
    one_minus_source_color,
    destination_color,
    one_minus_destination_color,
    source_alpha,
    one_minus_source_alpha,
    destination_alpha,
    one_minus_destination_alpha,
    source_alpha_saturate,
};

enum class BlendOp : uint8_t
{
    add,
    subtract,
    reverse_subtract,
    minimum,
    maximum,
};

enum class IndexType : uint8_t
{
    uint16,
    uint32,
};

enum class LoadOp : uint8_t
{
    load,
    clear,
    discard,
};

enum class StoreOp : uint8_t
{
    store,
    discard,
};

enum class StencilOp : uint8_t
{
    keep,
    zero,
    replace,
    increment_clamp,
    decrement_clamp,
    invert,
    increment_wrap,
    decrement_wrap,
};

// Ordered by Vulkan's logical execution order where stages are comparable.
// A barrier's before execution scope includes the selected and logically earlier
// stages; its after execution scope includes the selected and logically later
// stages. Vertex and mesh are alternative graphics branches, depth_stencil_tests
// spans early and late tests around fragment, compute and transfer are separate
// pipelines, host is a pseudo-stage, and none/all_commands are special masks.
enum class Stage : uint64_t
{
    none = 0,
    indirect = 1ull << 6u,
    index_input = 1ull << 7u,
    vertex = 1ull << 1u,
    mesh = 1ull << 9u,
    depth_stencil_tests = 1ull << 8u,
    fragment = 1ull << 2u,
    color_output = 1ull << 4u,
    compute = 1ull << 3u,
    transfer = 1ull << 0u,
    host = 1ull << 5u, // Barrier destination only, paired with host_read.
    all_commands = 1ull << 10u, // All GPU command stages; excludes host.
};

constexpr Stage operator|(Stage lhs, Stage rhs) noexcept
{
    return static_cast<Stage>(static_cast<uint64_t>(lhs) | static_cast<uint64_t>(rhs));
}

enum class Access : uint64_t
{
    none = 0,
    transfer_read = 1ull << 0u,
    transfer_write = 1ull << 1u,
    shader_read = 1ull << 2u,
    shader_write = 1ull << 3u,
    color_read = 1ull << 4u,
    color_write = 1ull << 5u,
    depth_stencil_read = 1ull << 6u,
    depth_stencil_write = 1ull << 7u,
    indirect_read = 1ull << 8u,
    index_read = 1ull << 9u,
    host_read = 1ull << 10u,
    descriptor_read = 1ull << 11u,
};

constexpr Access operator|(Access lhs, Access rhs) noexcept
{
    return static_cast<Access>(static_cast<uint64_t>(lhs) | static_cast<uint64_t>(rhs));
}

struct DeviceCaps
{
    const char* device_name = nullptr;
    uint64_t max_push_data_size = 0;
    // Common element size for suballocating TextureHeap storage; every SizeAlign::align divides this value.
    uint64_t texture_heap_alignment = 0;
    uint64_t texture_descriptor_size = 0; // Bytes per descriptor slot.
    uint64_t sampler_descriptor_size = 0; // Bytes per descriptor slot.
    bool texture_compression_bc = false;
    bool texture_compression_astc = false;
    bool storage_input_output16 = false;
};

// A windowed device and every call using it must remain on the native
// window's message-pump thread. The window must outlive the device.
struct DeviceDesc
{
    void* window = nullptr;
    Format swapchain_format = Format::undefined;
    uint32_t desired_swapchain_image_count = 2;
};

struct DeviceInit
{
    Device* device = nullptr;
    Error error = Error::none;
};

struct SwapchainFrame
{
    RenderView* render_view = nullptr;
    uint32x2 extent = {};
};

struct TextureDesc
{
    TextureType type = TextureType::two_d;
    uint32x3 extent = {.x = 1, .y = 1, .z = 1};
    uint32_t mip_levels = 1;
    uint32_t layer_count = 1; // Vulkan array layers; cube faces are individual layers.
    Format format = Format::rgba8_unorm;
    bool mutable_format = false; // Allow format-compatible descriptor views, but could lose DCC.
    TextureUsage usage = TextureUsage::sampled;
};

struct RenderViewDesc
{
    uint32_t mip_level = 0;
    uint32_t slice = 0; // Physical array slice; cube faces are individual slices.
};

struct TextureDescriptorDesc
{
    Format format = Format::undefined; // Undefined inherits the texture format.
    TextureAspect aspect = TextureAspect::automatic; // Automatic selects color, or depth before stencil.
    uint32_t base_mip = 0;
    uint32_t mip_count = 0; // Zero selects every remaining mip level.
    uint32_t base_layer = 0; // Vulkan array layer; cube faces are individual layers.
    uint32_t layer_count = 0; // Vulkan array layers; zero selects every remaining layer.
};

struct TextureCopyDesc
{
    uint32_t mip_level = 0;
    uint32_t base_slice = 0; // Physical array slice; cube faces are individual slices.
    uint32_t slice_count = 0; // Zero selects every remaining physical slice.
    uint32x3 offset = {};
    uint32x3 extent = {}; // Zero components select the remaining mip extent.
    uint64_t row_pitch_bytes = 0;   // Zero is tightly packed.
    uint64_t slice_pitch_bytes = 0; // Zero is tightly packed.
};

struct SamplerDesc
{
    Filter min_filter = Filter::linear;
    Filter mag_filter = Filter::linear;
    Filter mip_filter = Filter::linear;
    AddressMode address_u = AddressMode::repeat;
    AddressMode address_v = AddressMode::repeat;
    AddressMode address_w = AddressMode::repeat;
    bool anisotropic = false; // Uses the API's fixed 4x profile.
    bool compare_enabled = false;
    CompareOp compare = CompareOp::less_equal;
};

struct BlendComponentState
{
    BlendFactor source = BlendFactor::one;
    BlendFactor destination = BlendFactor::zero;
    BlendOp operation = BlendOp::add;
};

struct BlendState
{
    bool enabled = false;
    BlendComponentState color = {};
    BlendComponentState alpha = {};
};

struct ColorTargetDesc
{
    Format format = Format::undefined;
    BlendState blend = {};
    uint8_t write_mask = 0xf;
};

struct RasterizationState
{
    CullMode cull = CullMode::none;
    float depth_bias_constant = 0.0f;
    float depth_bias_clamp = 0.0f;
    float depth_bias_slope = 0.0f;
};

struct StencilFaceState
{
    CompareOp compare = CompareOp::always;
    StencilOp fail = StencilOp::keep;
    StencilOp pass = StencilOp::keep;
    StencilOp depth_fail = StencilOp::keep;
    uint8_t reference = 0;
};

struct DepthStencilState
{
    bool depth_test = false;
    bool depth_write = false;
    CompareOp depth_compare = CompareOp::less_equal;
    bool stencil_test = false;
    uint8_t stencil_read_mask = 0xff;
    uint8_t stencil_write_mask = 0xff;
    StencilFaceState front = {};
    StencilFaceState back = {};
};

struct GraphicsPSODesc
{
    Span<const uint32_t> vertex_spirv = {};
    Span<const uint32_t> fragment_spirv = {};
    Span<const ColorTargetDesc> color_targets = {};
    Format depth_format = Format::undefined;
    Format stencil_format = Format::undefined;
    RasterizationState rasterization = {};
    DepthStencilState depth_stencil = {};
};

struct MeshPSODesc
{
    Span<const uint32_t> mesh_spirv = {};
    Span<const uint32_t> fragment_spirv = {};
    Span<const ColorTargetDesc> color_targets = {};
    Format depth_format = Format::undefined;
    Format stencil_format = Format::undefined;
    RasterizationState rasterization = {};
    DepthStencilState depth_stencil = {};
};

struct ClearColor
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct ColorAttachment
{
    RenderView* render_view = nullptr;
    LoadOp load = LoadOp::load;
    StoreOp store = StoreOp::store;
    ClearColor clear = {};
};

struct DepthAttachment
{
    RenderView* render_view = nullptr;
    LoadOp load = LoadOp::load;
    StoreOp store = StoreOp::store;
    float clear = 1.0f;
};

struct StencilAttachment
{
    RenderView* render_view = nullptr;
    LoadOp load = LoadOp::load;
    StoreOp store = StoreOp::store;
    uint8_t clear = 0;
};

struct RenderingDesc
{
    Span<const ColorAttachment> colors = {};
    DepthAttachment depth = {};
    StencilAttachment stencil = {};
};

// All resource destruction is immediate. Destroy resources only when no recorded or executing GPU frame uses them.
// The optional NoGraphicsAPIUtility DeleteQueue can defer destruction until a submitted frame completes.
// Wait for all submitted frames to drain before destroying the device.
[[nodiscard]] DeviceInit create_device(const DeviceDesc& desc = {}) noexcept;
void destroy_device(Device* device) noexcept;
[[nodiscard]] const DeviceCaps& get_device_caps(const Device* device) noexcept;
[[nodiscard]] bool supports_texture_format(const Device* device, Format format, TextureUsage usage) noexcept;
[[nodiscard]] uint32x2 get_drawable_extent(Device* device) noexcept;

[[nodiscard]] TimelineSemaphore* create_timeline_semaphore(Device* device, uint64_t initial_value = 0) noexcept;
void destroy_timeline_semaphore(TimelineSemaphore* semaphore) noexcept;
[[nodiscard]] uint64_t timeline_completed_value(const TimelineSemaphore* semaphore) noexcept;
void wait_timeline(TimelinePoint point) noexcept;
void wait_idle(Device* device) noexcept;

[[nodiscard]] SwapchainFrame acquire(Device* device) noexcept; // Empty while the drawable extent is zero.
void submit_and_present(Device* device, Span<CommandBuffer* const> commands, TimelinePoint completion) noexcept;

// Every non-null returned pointer is 16-byte aligned. Descriptor heaps are exact allocations;
// cpu_visible, gpu_only, and readback heaps are raw blocks for application-side suballocation.
[[nodiscard]] GpuHeap create_gpu_heap(Device* device, uint64_t byte_count, MemoryType memory = MemoryType::cpu_visible) noexcept;
void destroy_gpu_heap(const GpuHeap& heap) noexcept;

template<typename T>
[[nodiscard]] constexpr GpuRange gpu_range(GpuCpuRange<T> range) noexcept
{
    return {.gpu = range.gpu, .size = range.size};
}

[[nodiscard]] constexpr GpuRange gpu_range(const GpuHeap& heap) noexcept
{
    return gpu_range(heap.range);
}

// Texture heaps use one device-selected GPU-only memory type and must outlive every placed texture.
// Placements must satisfy get_texture_size_align(), remain non-overlapping, and not be reused before the timeline point covering their last use completes.
// DeviceCaps::texture_heap_alignment can be used as a common allocator element size, avoiding per-placement leading alignment padding.
[[nodiscard]] TextureHeap create_texture_heap(Device* device, uint64_t byte_count) noexcept;
void destroy_texture_heap(const TextureHeap& heap) noexcept;
[[nodiscard]] SizeAlign get_texture_size_align(Device* device, const TextureDesc& desc) noexcept;
[[nodiscard]] Texture* create_texture(Device* device, const TextureDesc& desc, const TextureHeap& heap, uint64_t offset) noexcept;
void destroy_texture(Texture* texture) noexcept;
[[nodiscard]] RenderView* create_render_view(Texture* texture, const RenderViewDesc& desc = {}) noexcept;
void destroy_render_view(RenderView* render_view) noexcept;
void write_texture_descriptor(Device* device,
                              void* cpu_destination,
                              const Texture* texture,
                              TextureDescriptorType type,
                              const TextureDescriptorDesc& desc = {}) noexcept;
void write_sampler_descriptor(Device* device, void* cpu_destination, const SamplerDesc& desc = {}) noexcept;

[[nodiscard]] PSO* create_graphics_pso(Device* device, const GraphicsPSODesc& desc) noexcept;
[[nodiscard]] PSO* create_mesh_pso(Device* device, const MeshPSODesc& desc) noexcept;
[[nodiscard]] PSO* create_compute_pso(Device* device, Span<const uint32_t> compute_spirv) noexcept;
void destroy_pso(PSO* pso) noexcept;

// Every begun command buffer must be included exactly once in the next submit or submit_and_present call
[[nodiscard]] CommandBuffer* begin_commands(Device* device) noexcept;
void submit(Span<CommandBuffer* const> commands, TimelinePoint completion) noexcept;

void set_texture_descriptor_heap(CommandBuffer* commands, GpuRange heap) noexcept; // Heap range must be full GpuHeap range
void set_sampler_descriptor_heap(CommandBuffer* commands, GpuRange heap) noexcept; // Heap range must be full GpuHeap range

void copy_memory(CommandBuffer* commands, GpuRange source, GpuRange destination) noexcept;
void copy_memory_to_texture(CommandBuffer* commands, GpuRange source, Texture* destination, const TextureCopyDesc& copy = {}) noexcept;
void copy_texture_to_memory(CommandBuffer* commands, Texture* source, GpuRange destination, const TextureCopyDesc& copy = {}) noexcept;

void barrier(CommandBuffer* commands, Stage before, Access before_access, Stage after, Access after_access) noexcept;

void begin_render_pass(CommandBuffer* commands, const RenderingDesc& desc) noexcept;
void end_render_pass(CommandBuffer* commands) noexcept;

void bind_pso(CommandBuffer* commands, const PSO* pso) noexcept;

void draw(CommandBuffer* commands, ByteSpan root, uint32_t vertex_count, uint32_t instance_count = 1,
          uint32_t first_vertex = 0, uint32_t first_instance = 0) noexcept;
void draw_indexed(CommandBuffer* commands, ByteSpan root, GpuRange indices, IndexType type, uint32_t index_count,
                  uint32_t instance_count = 1, uint32_t first_index = 0, int32_t vertex_offset = 0, uint32_t first_instance = 0) noexcept;
void draw_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments, uint32_t draw_count = 1, uint32_t stride = 0) noexcept;
void draw_indexed_indirect(CommandBuffer* commands, ByteSpan root, GpuRange indices, IndexType type,
                           GpuRange arguments, uint32_t draw_count = 1, uint32_t stride = 0) noexcept;
void dispatch(CommandBuffer* commands, ByteSpan root, uint32x3 group_count) noexcept;
void dispatch_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments) noexcept;
void draw_meshlets(CommandBuffer* commands, ByteSpan root, uint32x3 group_count) noexcept;
void draw_meshlets_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments, uint32_t draw_count = 1, uint32_t stride = 0) noexcept;

} // namespace gpu
