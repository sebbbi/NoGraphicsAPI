#if defined(_CPPUNWIND) || defined(__EXCEPTIONS) || defined(__cpp_exceptions)
#error NoGraphicsAPI tests must be compiled with C++ exceptions disabled
#endif

#if defined(_CPPRTTI) || defined(__GXX_RTTI)
#error NoGraphicsAPI tests must be compiled with RTTI disabled
#endif

#if defined(__clang__)
#if __has_feature(cxx_exceptions) || __has_feature(cxx_rtti)
#error NoGraphicsAPI tests must be compiled with C++ exceptions and RTTI disabled
#endif
#endif

#if defined(_HAS_EXCEPTIONS) && _HAS_EXCEPTIONS
#error NoGraphicsAPI tests must use the no-exceptions standard-library mode
#endif

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <type_traits>
#include <vector>

template<typename T>
concept CompleteType = requires { sizeof(T); };

template<typename T>
concept HasImplicitRootDraw = requires(T* commands) { gpu::draw(commands, 3u); };

template<typename T>
concept HasImplicitRootDispatch = requires(T* commands) {
    gpu::dispatch(commands, gpu::uint32x3{.x = 1, .y = 1, .z = 1});
};

template<typename T>
concept HasImplicitRootMeshDraw = requires(T* commands) {
    gpu::draw_meshlets(commands, gpu::uint32x3{.x = 1, .y = 1, .z = 1});
};

template<typename Root>
concept RootCommandApi = requires(gpu::CommandBuffer* commands, const Root& root, gpu::GpuRange range) {
    gpu::draw(commands, root, 3u);
    gpu::draw_indexed(commands, root, range, gpu::IndexType::uint32, 3u);
    gpu::draw_indirect(commands, root, range);
    gpu::draw_indexed_indirect(commands, root, range, gpu::IndexType::uint32, range);
    gpu::dispatch(commands, root, {.x = 1, .y = 1, .z = 1});
    gpu::dispatch_indirect(commands, root, range);
    gpu::draw_meshlets(commands, root, {.x = 1, .y = 1, .z = 1});
    gpu::draw_meshlets_indirect(commands, root, range);
};

template<typename T>
concept RawRootCommandApi = requires(T* commands, const void* root, std::size_t root_size, gpu::GpuRange range) {
    gpu::draw(commands, root, root_size, 3u);
    gpu::draw_indexed(commands, root, root_size, range, gpu::IndexType::uint32, 3u);
    gpu::draw_indirect(commands, root, root_size, range);
    gpu::draw_indexed_indirect(commands, root, root_size, range, gpu::IndexType::uint32, range);
    gpu::dispatch(commands, root, root_size, {.x = 1, .y = 1, .z = 1});
    gpu::dispatch_indirect(commands, root, root_size, range);
    gpu::draw_meshlets(commands, root, root_size, {.x = 1, .y = 1, .z = 1});
    gpu::draw_meshlets_indirect(commands, root, root_size, range);
};

template<typename Root>
concept TypedPointerRawDraw = requires(gpu::CommandBuffer* commands, const Root* root, std::size_t root_size) {
    gpu::draw(commands, root, root_size, 3u);
};

template<typename T>
concept NullRootCommandApi = requires(T* commands, gpu::GpuRange range) {
    gpu::draw(commands, nullptr, 0, 3u);
    gpu::draw_indexed(commands, nullptr, 0, range, gpu::IndexType::uint32, 3u);
    gpu::draw_indirect(commands, nullptr, 0, range);
    gpu::draw_indexed_indirect(commands, nullptr, 0, range, gpu::IndexType::uint32, range);
    gpu::dispatch(commands, nullptr, 0, {.x = 1, .y = 1, .z = 1});
    gpu::dispatch_indirect(commands, nullptr, 0, range);
    gpu::draw_meshlets(commands, nullptr, 0, {.x = 1, .y = 1, .z = 1});
    gpu::draw_meshlets_indirect(commands, nullptr, 0, range);
};

template<typename T>
concept EmptyRootCommandApi = requires(T* commands, gpu::GpuRange range) {
    gpu::draw(commands, {}, 3u);
    gpu::draw_indexed(commands, {}, range, gpu::IndexType::uint32, 3u);
    gpu::draw_indirect(commands, {}, range);
    gpu::draw_indexed_indirect(commands, {}, range, gpu::IndexType::uint32, range);
    gpu::dispatch(commands, {}, {.x = 1, .y = 1, .z = 1});
    gpu::dispatch_indirect(commands, {}, range);
    gpu::draw_meshlets(commands, {}, {.x = 1, .y = 1, .z = 1});
    gpu::draw_meshlets_indirect(commands, {}, range);
};

template<typename T>
constexpr bool plain_api_data =
    std::is_aggregate_v<T> && std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>;

struct ApiRoot
{
    std::uint64_t vertices;
    std::uint32_t texture_index;
    float scale;
};

struct CustomAddressRoot
{
    std::uint32_t value;

    const CustomAddressRoot* operator&() const noexcept
    {
        return nullptr;
    }
};

struct OddSizedRoot
{
    std::uint8_t values[3];
};

struct NonTrivialRoot
{
    std::uint32_t value;

    ~NonTrivialRoot() noexcept
    {
    }
};

static_assert(!CompleteType<gpu::Device> && !CompleteType<gpu::Texture> && !CompleteType<gpu::RenderView> &&
              !CompleteType<gpu::PSO> && !CompleteType<gpu::CommandBuffer> &&
              !CompleteType<gpu::TimelineSemaphore> && !CompleteType<gpu::GpuHeapOwner> &&
              !CompleteType<gpu::TextureHeapOwner>);
static_assert(sizeof(void*) == 8);
static_assert(std::is_trivially_copyable_v<ApiRoot> && sizeof(ApiRoot) % 4 == 0);
static_assert(!HasImplicitRootDraw<gpu::CommandBuffer>);
static_assert(!HasImplicitRootDispatch<gpu::CommandBuffer>);
static_assert(!HasImplicitRootMeshDraw<gpu::CommandBuffer>);
static_assert(RootCommandApi<ApiRoot>);
static_assert(!RootCommandApi<std::uint32_t>);
static_assert(!RootCommandApi<std::uint32_t[4]>);
static_assert(!RootCommandApi<std::nullptr_t>);
static_assert(!RootCommandApi<volatile ApiRoot>);
static_assert(!RootCommandApi<OddSizedRoot>);
static_assert(!RootCommandApi<NonTrivialRoot>);
static_assert(!RawRootCommandApi<gpu::CommandBuffer>);
static_assert(!TypedPointerRawDraw<ApiRoot>);
static_assert(!NullRootCommandApi<gpu::CommandBuffer>);
static_assert(EmptyRootCommandApi<gpu::CommandBuffer>);

static_assert(std::is_same_v<std::underlying_type_t<gpu::Error>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::MemoryType>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::Format>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::TextureType>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::TextureUsage>, std::uint32_t> &&
              std::is_same_v<std::underlying_type_t<gpu::TextureDescriptorType>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::Filter>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::AddressMode>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::CompareOp>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::CullMode>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::BlendFactor>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::BlendOp>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::IndexType>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::LoadOp>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::StoreOp>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::StencilOp>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<gpu::Stage>, std::uint64_t> &&
              std::is_same_v<std::underlying_type_t<gpu::Access>, std::uint64_t>);

static_assert(static_cast<std::uint8_t>(gpu::Error::none) == 0 &&
              static_cast<std::uint8_t>(gpu::Error::unsupported) == 1 &&
              static_cast<std::uint8_t>(gpu::Error::device_lost) == 2 &&
              static_cast<std::uint8_t>(gpu::Error::driver_error) == 3);
static_assert(static_cast<std::uint32_t>(gpu::TextureUsage::sampled | gpu::TextureUsage::storage) == 3);
static_assert(static_cast<std::uint64_t>(gpu::Stage::vertex | gpu::Stage::fragment) == 6);
static_assert(static_cast<std::uint64_t>(gpu::Stage::all_commands) == (1ull << 10u));
static_assert(static_cast<std::uint64_t>(gpu::Stage::all_commands | gpu::Stage::host) ==
              ((1ull << 10u) | (1ull << 5u)));
static_assert(static_cast<std::uint64_t>(gpu::Access::shader_read | gpu::Access::shader_write) == 12);

using ConstWordSpan = gpu::Span<const std::uint32_t>;
using MutableWordSpan = gpu::Span<std::uint32_t>;
using WordVector = std::vector<std::uint32_t>;
using OtherWordVector = std::vector<std::uint64_t>;
constexpr std::uint32_t shader_words[]{0x07230203u, 0x00010600u};
constexpr ConstWordSpan array_span{shader_words};
constexpr ConstWordSpan pointer_span{shader_words, 2};

constexpr bool has_spirv_header(ConstWordSpan words) noexcept
{
    return words.size == 2 && words.data[0] == 0x07230203u;
}

static_assert(std::is_standard_layout_v<ConstWordSpan> && std::is_trivially_copyable_v<ConstWordSpan> &&
              sizeof(ConstWordSpan) == sizeof(void*) + sizeof(std::size_t));
static_assert(std::is_constructible_v<ConstWordSpan, std::initializer_list<std::uint32_t>> &&
              !std::is_constructible_v<gpu::Span<std::uint32_t>, std::initializer_list<std::uint32_t>>);
static_assert(std::is_constructible_v<MutableWordSpan, WordVector&>);
static_assert(std::is_constructible_v<ConstWordSpan, WordVector&>);
static_assert(std::is_constructible_v<ConstWordSpan, const WordVector&>);
static_assert(!std::is_constructible_v<MutableWordSpan, const WordVector&>);
static_assert(std::is_constructible_v<MutableWordSpan, WordVector>);
static_assert(std::is_constructible_v<ConstWordSpan, WordVector>);
static_assert(std::is_convertible_v<WordVector, ConstWordSpan>);
static_assert(!std::is_constructible_v<ConstWordSpan, OtherWordVector&>);
static_assert(!std::is_constructible_v<ConstWordSpan, OtherWordVector>);
static_assert(std::is_constructible_v<gpu::Span<gpu::CommandBuffer* const>, std::vector<gpu::CommandBuffer*>&>);
static_assert(array_span.data == shader_words && array_span.size == 2);
static_assert(pointer_span.data == shader_words && pointer_span.size == 2);
static_assert(has_spirv_header({0x07230203u, 0x00010600u}));

constexpr gpu::ByteSpan empty_byte_span{};
static_assert(std::is_standard_layout_v<gpu::ByteSpan> && std::is_trivially_copyable_v<gpu::ByteSpan> &&
              sizeof(gpu::ByteSpan) == sizeof(void*) + sizeof(std::size_t));
static_assert(empty_byte_span.data == nullptr && empty_byte_span.size == 0);
static_assert(std::is_constructible_v<gpu::ByteSpan, const void*, std::size_t>);
static_assert(std::is_convertible_v<const ApiRoot&, gpu::ByteSpan>);
static_assert(!std::is_convertible_v<const OddSizedRoot&, gpu::ByteSpan>);
static_assert(!std::is_convertible_v<const NonTrivialRoot&, gpu::ByteSpan>);
static_assert(!std::is_convertible_v<const std::uint32_t&, gpu::ByteSpan>);
static_assert(!std::is_convertible_v<const volatile ApiRoot&, gpu::ByteSpan>);

constexpr gpu::Format texture_formats[]{
    gpu::Format::r8_srgb,       gpu::Format::rg8_srgb,      gpu::Format::rgba8_srgb,
    gpu::Format::bgra8_srgb,    gpu::Format::rgba4_unorm,   gpu::Format::r5g5b5a1_unorm,
    gpu::Format::r5g6b5_unorm,  gpu::Format::r8_unorm,      gpu::Format::rg8_unorm,
    gpu::Format::rgba8_unorm,   gpu::Format::bgra8_unorm,   gpu::Format::r16_unorm,
    gpu::Format::rg16_unorm,    gpu::Format::rgba16_unorm,  gpu::Format::r8_uint,
    gpu::Format::rg8_uint,      gpu::Format::rgba8_uint,    gpu::Format::bgra8_uint,
    gpu::Format::r16_uint,      gpu::Format::rg16_uint,     gpu::Format::rgba16_uint,
    gpu::Format::r32_uint,      gpu::Format::rg32_uint,     gpu::Format::rgb32_uint,
    gpu::Format::rgba32_uint,   gpu::Format::r16_float,     gpu::Format::rg16_float,
    gpu::Format::rgba16_float,  gpu::Format::r32_float,     gpu::Format::rg32_float,
    gpu::Format::rgb32_float,   gpu::Format::rgba32_float,  gpu::Format::rgb10a2_unorm,
    gpu::Format::rg11b10_float, gpu::Format::d16_unorm,     gpu::Format::d24_unorm_s8_uint,
    gpu::Format::d32_float,     gpu::Format::s8_uint,       gpu::Format::d32_float_s8_uint,
    gpu::Format::eac_rg,        gpu::Format::astc_4x4_srgb, gpu::Format::astc_4x4_unorm,
    gpu::Format::bc3_srgb,      gpu::Format::bc3_unorm,     gpu::Format::bc5_rg,
    gpu::Format::bc7_srgb,      gpu::Format::bc7_unorm,
};
constexpr std::size_t texture_format_count = sizeof(texture_formats) / sizeof(texture_formats[0]);

constexpr bool valid_texture_formats() noexcept
{
    for (std::size_t index = 0; index < texture_format_count; ++index)
    {
        const gpu::TextureFormatInfo info = gpu::get_texture_format_info(texture_formats[index]);
        if (static_cast<std::uint8_t>(texture_formats[index]) != index || info.block_extent.x == 0 ||
            info.block_extent.y == 0 || info.bytes_per_block == 0)
        {
            return false;
        }
    }
    return true;
}

static_assert(texture_format_count == static_cast<std::uint8_t>(gpu::Format::undefined));
static_assert(valid_texture_formats());
static_assert(gpu::get_texture_format_info(gpu::Format::rgba8_unorm).bytes_per_block == 4);
static_assert(gpu::get_texture_format_info(gpu::Format::astc_4x4_unorm).block_extent.x == 4);
static_assert(gpu::get_texture_format_info(gpu::Format::d24_unorm_s8_uint).depth);
static_assert(gpu::get_texture_format_info(gpu::Format::d24_unorm_s8_uint).stencil);
static_assert(gpu::get_texture_format_info(gpu::Format::undefined).bytes_per_block == 0);

static_assert(plain_api_data<gpu::uint32x2> && plain_api_data<gpu::uint32x3> &&
              plain_api_data<gpu::TextureFormatInfo> && plain_api_data<gpu::DeviceCaps> &&
              plain_api_data<gpu::DeviceDesc> && plain_api_data<gpu::DeviceInit> &&
              plain_api_data<gpu::SwapchainFrame> && plain_api_data<gpu::TextureDesc> &&
              plain_api_data<gpu::RenderViewDesc> &&
              plain_api_data<gpu::TextureDescriptorDesc> && plain_api_data<gpu::TextureCopyDesc> &&
              plain_api_data<gpu::SamplerDesc> && plain_api_data<gpu::BlendComponentState> &&
              plain_api_data<gpu::BlendState> && plain_api_data<gpu::ColorTargetDesc> &&
              plain_api_data<gpu::RasterizationState> && plain_api_data<gpu::StencilFaceState> &&
              plain_api_data<gpu::DepthStencilState> && plain_api_data<gpu::GraphicsPSODesc> &&
              plain_api_data<gpu::MeshPSODesc> && plain_api_data<gpu::ClearColor> && plain_api_data<gpu::ColorAttachment> &&
              plain_api_data<gpu::DepthAttachment> && plain_api_data<gpu::StencilAttachment> &&
              plain_api_data<gpu::RenderingDesc> && plain_api_data<gpu::SizeAlign> &&
              plain_api_data<gpu::GpuRange> && plain_api_data<gpu::GpuCpuRange<gpu::byte>> && plain_api_data<gpu::GpuCpuRange<std::uint32_t>> &&
              plain_api_data<gpu::GpuHeap> && plain_api_data<gpu::TextureHeap> &&
              plain_api_data<gpu::TimelinePoint>);

static_assert(sizeof(gpu::uint32x2) == 8 && alignof(gpu::uint32x2) == 4 &&
              offsetof(gpu::uint32x2, x) == 0 && offsetof(gpu::uint32x2, y) == 4);
static_assert(sizeof(gpu::uint32x3) == 12 && alignof(gpu::uint32x3) == 4 &&
              offsetof(gpu::uint32x3, x) == 0 && offsetof(gpu::uint32x3, y) == 4 && offsetof(gpu::uint32x3, z) == 8);
static_assert(sizeof(gpu::TextureFormatInfo) == 16 &&
              offsetof(gpu::TextureFormatInfo, block_extent) == 0 &&
              offsetof(gpu::TextureFormatInfo, bytes_per_block) == 8 &&
              offsetof(gpu::TextureFormatInfo, stencil) == 13);
static_assert(sizeof(gpu::SwapchainFrame) == 16 && offsetof(gpu::SwapchainFrame, extent) == 8);
static_assert(sizeof(gpu::TextureDesc) == 32 && offsetof(gpu::TextureDesc, extent) == 4);
static_assert(sizeof(gpu::TextureCopyDesc) == 56 && offsetof(gpu::TextureCopyDesc, offset) == 12 &&
              offsetof(gpu::TextureCopyDesc, extent) == 24);
static_assert(sizeof(gpu::GpuRange) == 16 && offsetof(gpu::GpuRange, gpu) == 0 &&
              offsetof(gpu::GpuRange, size) == 8);
static_assert(sizeof(gpu::GpuCpuRange<gpu::byte>) == 24 && offsetof(gpu::GpuCpuRange<gpu::byte>, cpu) == 0 &&
              offsetof(gpu::GpuCpuRange<gpu::byte>, gpu) == 8 && offsetof(gpu::GpuCpuRange<gpu::byte>, size) == 16);
static_assert(sizeof(gpu::GpuCpuRange<std::uint32_t>) == sizeof(gpu::GpuCpuRange<gpu::byte>));
static_assert(sizeof(gpu::SizeAlign) == 16 && offsetof(gpu::SizeAlign, size) == 0 &&
              offsetof(gpu::SizeAlign, align) == 8);
static_assert(sizeof(gpu::ClearColor) == 16 && offsetof(gpu::ClearColor, x) == 0 &&
              offsetof(gpu::ClearColor, y) == 4 && offsetof(gpu::ClearColor, z) == 8 && offsetof(gpu::ClearColor, w) == 12);
static_assert(sizeof(gpu::GpuHeap) == 32 && offsetof(gpu::GpuHeap, range) == 0 && offsetof(gpu::GpuHeap, owner) == 24);
static_assert(sizeof(gpu::TextureHeap) == 16 && offsetof(gpu::TextureHeap, size) == 0 && offsetof(gpu::TextureHeap, owner) == 8);
static_assert(sizeof(gpu::TimelinePoint) == 16 && offsetof(gpu::TimelinePoint, semaphore) == 0 &&
              offsetof(gpu::TimelinePoint, value) == 8);

constexpr gpu::DeviceCaps default_caps{};
static_assert(default_caps.device_name == nullptr && default_caps.max_push_data_size == 0 &&
              default_caps.texture_heap_alignment == 0 &&
              default_caps.texture_descriptor_size == 0 &&
              default_caps.texture_descriptor_stride == 0 &&
              default_caps.sampler_descriptor_size == 0 &&
              default_caps.sampler_descriptor_stride == 0 && !default_caps.texture_compression_bc &&
              !default_caps.texture_compression_astc && !default_caps.storage_input_output16);
constexpr gpu::DeviceInit default_device_init{};
static_assert(default_device_init.device == nullptr &&
              default_device_init.error == gpu::Error::none);
constexpr gpu::DeviceDesc default_device_desc{};
static_assert(default_device_desc.window == nullptr &&
              default_device_desc.swapchain_format == gpu::Format::undefined &&
              default_device_desc.desired_swapchain_image_count == 2);
constexpr gpu::uint32x2 default_uint32x2{};
static_assert(default_uint32x2.x == 0 && default_uint32x2.y == 0);
constexpr gpu::uint32x3 default_uint32x3{};
static_assert(default_uint32x3.x == 0 && default_uint32x3.y == 0 && default_uint32x3.z == 0);
constexpr gpu::SwapchainFrame default_swapchain_frame{};
static_assert(default_swapchain_frame.render_view == nullptr &&
              default_swapchain_frame.extent.x == 0 && default_swapchain_frame.extent.y == 0);

constexpr gpu::TextureDesc default_texture{};
static_assert(default_texture.type == gpu::TextureType::two_d && default_texture.extent.x == 1 &&
              default_texture.extent.y == 1 && default_texture.extent.z == 1 &&
              default_texture.mip_levels == 1 && default_texture.layer_count == 1 &&
              default_texture.format == gpu::Format::rgba8_unorm &&
              !default_texture.mutable_format &&
              default_texture.usage == gpu::TextureUsage::sampled);
constexpr gpu::TextureDesc mutable_texture{.mutable_format = true};
static_assert(mutable_texture.mutable_format);
constexpr gpu::TextureDesc sized_texture{
    .extent = {.x = 64, .y = 32, .z = 1},
    .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination,
};
static_assert(sized_texture.type == gpu::TextureType::two_d && sized_texture.extent.x == 64 &&
              sized_texture.extent.y == 32 && sized_texture.extent.z == 1 &&
              sized_texture.mip_levels == 1);
constexpr gpu::RenderViewDesc default_render_view{};
static_assert(default_render_view.mip_level == 0 && default_render_view.slice == 0);
constexpr gpu::TextureDescriptorDesc default_texture_descriptor{};
static_assert(default_texture_descriptor.format == gpu::Format::undefined &&
              default_texture_descriptor.base_mip == 0 &&
              default_texture_descriptor.mip_count == 0 &&
              default_texture_descriptor.base_layer == 0 &&
              default_texture_descriptor.layer_count == 0);
constexpr gpu::TextureCopyDesc default_texture_copy{};
static_assert(default_texture_copy.mip_level == 0 && default_texture_copy.base_slice == 0 &&
              default_texture_copy.slice_count == 0 && default_texture_copy.offset.x == 0 &&
              default_texture_copy.offset.y == 0 && default_texture_copy.offset.z == 0 &&
              default_texture_copy.extent.x == 0 && default_texture_copy.extent.y == 0 &&
              default_texture_copy.extent.z == 0 && default_texture_copy.row_pitch_bytes == 0 &&
              default_texture_copy.slice_pitch_bytes == 0);
constexpr gpu::SamplerDesc default_sampler{};
static_assert(default_sampler.min_filter == gpu::Filter::linear &&
              default_sampler.mag_filter == gpu::Filter::linear &&
              default_sampler.mip_filter == gpu::Filter::linear &&
              default_sampler.address_u == gpu::AddressMode::repeat &&
              default_sampler.address_v == gpu::AddressMode::repeat &&
              default_sampler.address_w == gpu::AddressMode::repeat &&
              !default_sampler.anisotropic && !default_sampler.compare_enabled &&
              default_sampler.compare == gpu::CompareOp::less_equal);

constexpr gpu::BlendComponentState default_blend_component{};
static_assert(default_blend_component.source == gpu::BlendFactor::one &&
              default_blend_component.destination == gpu::BlendFactor::zero &&
              default_blend_component.operation == gpu::BlendOp::add);
constexpr gpu::ColorTargetDesc default_color_target{};
static_assert(default_color_target.format == gpu::Format::undefined &&
              !default_color_target.blend.enabled && default_color_target.write_mask == 0xf);
constexpr gpu::RasterizationState default_rasterization{};
static_assert(default_rasterization.cull == gpu::CullMode::none &&
              default_rasterization.depth_bias_constant == 0.0f &&
              default_rasterization.depth_bias_clamp == 0.0f &&
              default_rasterization.depth_bias_slope == 0.0f);
constexpr gpu::StencilFaceState default_stencil_face{};
static_assert(default_stencil_face.compare == gpu::CompareOp::always &&
              default_stencil_face.fail == gpu::StencilOp::keep &&
              default_stencil_face.pass == gpu::StencilOp::keep &&
              default_stencil_face.depth_fail == gpu::StencilOp::keep &&
              default_stencil_face.reference == 0);
constexpr gpu::DepthStencilState default_depth_stencil{};
static_assert(!default_depth_stencil.depth_test && !default_depth_stencil.depth_write &&
              default_depth_stencil.depth_compare == gpu::CompareOp::less_equal &&
              !default_depth_stencil.stencil_test &&
              default_depth_stencil.stencil_read_mask == 0xff &&
              default_depth_stencil.stencil_write_mask == 0xff);

constexpr gpu::GraphicsPSODesc default_graphics_pso{};
static_assert(default_graphics_pso.vertex_spirv.size == 0 &&
              default_graphics_pso.fragment_spirv.size == 0 &&
              default_graphics_pso.color_targets.size == 0 &&
              default_graphics_pso.depth_format == gpu::Format::undefined &&
              default_graphics_pso.stencil_format == gpu::Format::undefined);
constexpr gpu::MeshPSODesc default_mesh_pso{};
static_assert(default_mesh_pso.mesh_spirv.size == 0 &&
              default_mesh_pso.fragment_spirv.size == 0 &&
              default_mesh_pso.color_targets.size == 0 &&
              default_mesh_pso.depth_format == gpu::Format::undefined &&
              default_mesh_pso.stencil_format == gpu::Format::undefined);
constexpr gpu::ColorAttachment default_color_attachment{};
static_assert(default_color_attachment.render_view == nullptr &&
              default_color_attachment.load == gpu::LoadOp::load &&
              default_color_attachment.store == gpu::StoreOp::store &&
              default_color_attachment.clear.x == 0.0f &&
              default_color_attachment.clear.y == 0.0f &&
              default_color_attachment.clear.z == 0.0f && default_color_attachment.clear.w == 1.0f);
constexpr gpu::DepthAttachment default_depth_attachment{};
static_assert(default_depth_attachment.render_view == nullptr &&
              default_depth_attachment.load == gpu::LoadOp::load &&
              default_depth_attachment.store == gpu::StoreOp::store &&
              default_depth_attachment.clear == 1.0f);
constexpr gpu::StencilAttachment default_stencil_attachment{};
static_assert(default_stencil_attachment.render_view == nullptr &&
              default_stencil_attachment.load == gpu::LoadOp::load &&
              default_stencil_attachment.store == gpu::StoreOp::store &&
              default_stencil_attachment.clear == 0);
constexpr gpu::RenderingDesc default_rendering{};
static_assert(default_rendering.colors.size == 0 &&
              default_rendering.depth.render_view == nullptr &&
              default_rendering.stencil.render_view == nullptr);

constexpr gpu::GpuRange default_range{};
constexpr gpu::GpuCpuRange<gpu::byte> default_cpu_range{};
constexpr gpu::GpuCpuRange<std::uint32_t> default_typed_cpu_range{};
constexpr gpu::SizeAlign default_size_align{};
constexpr gpu::GpuHeap default_heap{};
constexpr gpu::TextureHeap default_texture_heap{};
constexpr gpu::TimelinePoint default_timeline_point{};
static_assert(default_range.gpu == nullptr && default_range.size == 0);
static_assert(default_cpu_range.cpu == nullptr && default_cpu_range.gpu == nullptr && default_cpu_range.size == 0);
static_assert(gpu::gpu_range(default_cpu_range).gpu == nullptr && gpu::gpu_range(default_cpu_range).size == 0);
static_assert(default_typed_cpu_range.cpu == nullptr && default_typed_cpu_range.gpu == nullptr && default_typed_cpu_range.size == 0);
static_assert(gpu::gpu_range(default_typed_cpu_range).gpu == nullptr && gpu::gpu_range(default_typed_cpu_range).size == 0);
static_assert(default_size_align.size == 0 && default_size_align.align == 0);
static_assert(default_heap.range.cpu == nullptr && default_heap.range.gpu == nullptr && default_heap.range.size == 0 && default_heap.owner == nullptr);
static_assert(gpu::gpu_range(default_heap).gpu == nullptr && gpu::gpu_range(default_heap).size == 0);
static_assert(default_texture_heap.size == 0 && default_texture_heap.owner == nullptr);
static_assert(default_timeline_point.semaphore == nullptr && default_timeline_point.value == 0);

using CreateGpuHeapFunction = gpu::GpuHeap (*)(gpu::Device*, std::uint64_t, gpu::MemoryType) noexcept;
using DestroyGpuHeapFunction = void (*)(const gpu::GpuHeap&) noexcept;
using CreateTextureHeapFunction = gpu::TextureHeap (*)(gpu::Device*, std::uint64_t) noexcept;
using DestroyTextureHeapFunction = void (*)(const gpu::TextureHeap&) noexcept;
using GetTextureSizeAlignFunction = gpu::SizeAlign (*)(gpu::Device*, const gpu::TextureDesc&) noexcept;
using CreateTextureFunction = gpu::Texture* (*)(gpu::Device*, const gpu::TextureDesc&, const gpu::TextureHeap&, std::uint64_t) noexcept;
using CreateDeviceFunction = gpu::DeviceInit (*)(const gpu::DeviceDesc&) noexcept;
using GetDrawableExtentFunction = gpu::uint32x2 (*)(gpu::Device*) noexcept;
using AcquireFunction = gpu::SwapchainFrame (*)(gpu::Device*) noexcept;
using CommandBatch = gpu::Span<gpu::CommandBuffer* const>;
using CreateComputePSOFunction = gpu::PSO* (*)(gpu::Device*, gpu::Span<const std::uint32_t>) noexcept;
using SubmitFunction = void (*)(CommandBatch, gpu::TimelinePoint) noexcept;
using SubmitAndPresentFunction = void (*)(gpu::Device*, CommandBatch, gpu::TimelinePoint) noexcept;
using SetHeapFunction = void (*)(gpu::CommandBuffer*, gpu::GpuRange) noexcept;
using CopyMemoryFunction = void (*)(gpu::CommandBuffer*, gpu::GpuRange, gpu::GpuRange) noexcept;
using CopyMemoryToTextureFunction = void (*)(gpu::CommandBuffer*, gpu::GpuRange, gpu::Texture*,
                                             const gpu::TextureCopyDesc&) noexcept;
using CopyTextureToMemoryFunction = void (*)(gpu::CommandBuffer*, gpu::Texture*, gpu::GpuRange,
                                             const gpu::TextureCopyDesc&) noexcept;
using WriteTextureDescriptorFunction = void (*)(gpu::Device*, void*, const gpu::Texture*,
                                                gpu::TextureDescriptorType,
                                                const gpu::TextureDescriptorDesc&) noexcept;
using WriteSamplerDescriptorFunction = void (*)(gpu::Device*, void*,
                                                const gpu::SamplerDesc&) noexcept;
using DrawFunction = void (*)(gpu::CommandBuffer*, gpu::ByteSpan, std::uint32_t, std::uint32_t, std::uint32_t,
                              std::uint32_t) noexcept;
using DrawIndexedFunction = void (*)(gpu::CommandBuffer*, gpu::ByteSpan, gpu::GpuRange, gpu::IndexType,
                                     std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t,
                                     std::uint32_t) noexcept;
using DrawIndirectFunction = void (*)(gpu::CommandBuffer*, gpu::ByteSpan, gpu::GpuRange, std::uint32_t,
                                      std::uint32_t) noexcept;
using DrawIndexedIndirectFunction = void (*)(gpu::CommandBuffer*, gpu::ByteSpan, gpu::GpuRange,
                                             gpu::IndexType, gpu::GpuRange, std::uint32_t,
                                             std::uint32_t) noexcept;
using DispatchFunction = void (*)(gpu::CommandBuffer*, gpu::ByteSpan, gpu::uint32x3) noexcept;
using DispatchIndirectFunction = void (*)(gpu::CommandBuffer*, gpu::ByteSpan, gpu::GpuRange) noexcept;
static_assert(std::is_same_v<decltype(&gpu::create_gpu_heap), CreateGpuHeapFunction>);
static_assert(std::is_same_v<decltype(&gpu::destroy_gpu_heap), DestroyGpuHeapFunction>);
static_assert(std::is_same_v<decltype(&gpu::create_texture_heap), CreateTextureHeapFunction>);
static_assert(std::is_same_v<decltype(&gpu::destroy_texture_heap), DestroyTextureHeapFunction>);
static_assert(std::is_same_v<decltype(&gpu::get_texture_size_align), GetTextureSizeAlignFunction>);
static_assert(std::is_same_v<decltype(&gpu::create_texture), CreateTextureFunction>);
static_assert(std::is_same_v<decltype(&gpu::create_device), CreateDeviceFunction>);
static_assert(std::is_same_v<decltype(&gpu::get_drawable_extent), GetDrawableExtentFunction>);
static_assert(std::is_same_v<decltype(&gpu::acquire), AcquireFunction>);
static_assert(std::is_same_v<decltype(&gpu::create_compute_pso), CreateComputePSOFunction>);
static_assert(std::is_same_v<decltype(&gpu::submit), SubmitFunction>);
static_assert(std::is_same_v<decltype(&gpu::submit_and_present), SubmitAndPresentFunction>);
static_assert(std::is_same_v<decltype(&gpu::set_texture_heap), SetHeapFunction>);
static_assert(std::is_same_v<decltype(&gpu::set_sampler_heap), SetHeapFunction>);
static_assert(std::is_same_v<decltype(&gpu::copy_memory), CopyMemoryFunction>);
static_assert(std::is_same_v<decltype(&gpu::copy_memory_to_texture), CopyMemoryToTextureFunction>);
static_assert(std::is_same_v<decltype(&gpu::copy_texture_to_memory), CopyTextureToMemoryFunction>);
static_assert(std::is_same_v<decltype(&gpu::write_texture_descriptor), WriteTextureDescriptorFunction>);
static_assert(std::is_same_v<decltype(&gpu::write_sampler_descriptor), WriteSamplerDescriptorFunction>);
static_assert(std::is_same_v<decltype(&gpu::draw), DrawFunction>);
static_assert(std::is_same_v<decltype(&gpu::draw_indexed), DrawIndexedFunction>);
static_assert(std::is_same_v<decltype(&gpu::draw_indirect), DrawIndirectFunction>);
static_assert(std::is_same_v<decltype(&gpu::draw_indexed_indirect), DrawIndexedIndirectFunction>);
static_assert(std::is_same_v<decltype(&gpu::dispatch), DispatchFunction>);
static_assert(std::is_same_v<decltype(&gpu::dispatch_indirect), DispatchIndirectFunction>);
static_assert(std::is_same_v<decltype(&gpu::draw_meshlets), DispatchFunction>);
static_assert(std::is_same_v<decltype(&gpu::draw_meshlets_indirect), DrawIndirectFunction>);
static_assert(std::is_constructible_v<CommandBatch, std::initializer_list<gpu::CommandBuffer*>>);

[[maybe_unused]] void compile_api_surface(gpu::Device* device, gpu::Texture* texture,
                                          gpu::RenderView* render_view, gpu::PSO* pso,
                                          gpu::CommandBuffer* commands, gpu::TimelineSemaphore* semaphore,
                                          const ApiRoot& root, void* descriptor)
{
    gpu::DeviceInit device_init = gpu::create_device();
    gpu::DeviceInit window_device_init = gpu::create_device({
        .window = descriptor,
        .swapchain_format = gpu::Format::bgra8_srgb,
        .desired_swapchain_image_count = 3,
    });
    const gpu::DeviceCaps& caps = gpu::get_device_caps(device);
    const bool supported = gpu::supports_texture_format(device, gpu::Format::rgba8_unorm, gpu::TextureUsage::sampled);
    gpu::destroy_device(device);

    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device, 1);
    const std::uint64_t completed = gpu::timeline_completed_value(semaphore);
    gpu::wait_timeline({.semaphore = semaphore, .value = 1});
    gpu::destroy_timeline_semaphore(timeline);

    gpu::uint32x2 drawable_extent = gpu::get_drawable_extent(device);
    gpu::SwapchainFrame frame = gpu::acquire(device);
    gpu::submit_and_present(device, {commands}, {.semaphore = semaphore, .value = 2});

    gpu::GpuHeap bytes = gpu::create_gpu_heap(device, 64);
    gpu::GpuHeap gpu_only = gpu::create_gpu_heap(device, 64, gpu::MemoryType::gpu_only);
    gpu::GpuHeap texture_descriptors = gpu::create_gpu_heap(device, 64, gpu::MemoryType::texture_descriptor_heap);
    gpu::GpuHeap sampler_descriptors = gpu::create_gpu_heap(device, 64, gpu::MemoryType::sampler_descriptor_heap);
    gpu::GpuRange range = gpu::gpu_range(bytes);
    gpu::destroy_gpu_heap(sampler_descriptors);
    gpu::destroy_gpu_heap(texture_descriptors);
    gpu::destroy_gpu_heap(gpu_only);
    gpu::destroy_gpu_heap(bytes);

    const gpu::SizeAlign texture_size_align = gpu::get_texture_size_align(device, {});
    gpu::TextureHeap texture_heap = gpu::create_texture_heap(device, texture_size_align.size);
    gpu::Texture* created_texture = gpu::create_texture(device, {}, texture_heap, 0);
    gpu::RenderView* created_render_view = gpu::create_render_view(texture);
    gpu::write_texture_descriptor(device, descriptor, texture, gpu::TextureDescriptorType::sampled);
    gpu::write_sampler_descriptor(device, descriptor);
    gpu::destroy_render_view(created_render_view);
    gpu::destroy_texture(created_texture);
    gpu::destroy_texture_heap(texture_heap);

    gpu::PSO* graphics = gpu::create_graphics_pso(device, {});
    gpu::PSO* mesh = gpu::create_mesh_pso(device, {});
    gpu::PSO* compute = gpu::create_compute_pso(device, shader_words);
    gpu::destroy_pso(graphics);
    gpu::destroy_pso(mesh);
    gpu::destroy_pso(compute);
    gpu::destroy_pso(pso);

    gpu::CommandBuffer* a = gpu::begin_commands(device);
    gpu::CommandBuffer* b = gpu::begin_commands(device);
    gpu::bind_pso(a, pso);
    gpu::set_texture_heap(a, range);
    gpu::set_sampler_heap(a, range);
    gpu::begin_render_pass(a, {
                                  .colors = {{.render_view = render_view}},
                              });
    gpu::draw(a, root, 3);
    gpu::draw_indexed(a, root, range, gpu::IndexType::uint32, 3);
    gpu::draw_indirect(a, root, range);
    gpu::draw_indexed_indirect(a, root, range, gpu::IndexType::uint32, range);
    gpu::draw_meshlets(a, root, {.x = 1, .y = 1, .z = 1});
    gpu::draw_meshlets_indirect(a, root, range);
    gpu::end_render_pass(a);
    gpu::dispatch(a, root, {.x = 1, .y = 1, .z = 1});
    gpu::dispatch_indirect(a, root, range);
    gpu::copy_memory(a, range, range);
    gpu::copy_memory_to_texture(a, range, texture);
    gpu::copy_texture_to_memory(a, texture, range);
    gpu::barrier(a, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::fragment, gpu::Access::shader_read);
    gpu::submit({a, b}, {.semaphore = semaphore, .value = 3});
    gpu::wait_idle(device);

    (void)device_init;
    (void)window_device_init;
    (void)caps;
    (void)supported;
    (void)completed;
    (void)drawable_extent;
    (void)frame;
    (void)texture_size_align;
}

int main()
{
    const ApiRoot root{};
    const gpu::ByteSpan bytes = root;
    const bool valid_bytes = bytes.data == reinterpret_cast<const std::byte*>(&root) && bytes.size == sizeof(root);
    const CustomAddressRoot custom_address_root{};
    const gpu::ByteSpan custom_address_bytes = custom_address_root;
    const bool valid_custom_address =
        custom_address_bytes.data == reinterpret_cast<const std::byte*>(std::addressof(custom_address_root));
    return has_spirv_header(WordVector{0x07230203u, 0x00010600u}) && valid_bytes && valid_custom_address ? 0 : 1;
}
