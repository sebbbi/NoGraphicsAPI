#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <cstddef>
#include <cstdint>

namespace
{

constexpr int skipped = 77;
constexpr std::size_t batch_command_count = 20;
constexpr std::uint32_t batch_submission_count = 4;
constexpr std::uint32_t test_gpu_heap_size = 1024u * 1024u;

constexpr std::uint64_t element_count(std::uint64_t byte_count, std::uint64_t element_size) noexcept
{
    return 1 + (byte_count - 1) / element_size;
}

bool valid_size_align(gpu::SizeAlign size_align, std::uint64_t common_alignment) noexcept
{
    return size_align.size != 0 && size_align.align != 0 && (size_align.align & (size_align.align - 1)) == 0 &&
           common_alignment >= size_align.align && common_alignment % size_align.align == 0;
}

bool test_gpu_heaps(gpu::Device* device) noexcept
{
    constexpr std::uint64_t sizes[]{1, 15, 16, 17, test_gpu_heap_size};
    constexpr std::size_t size_count = sizeof(sizes) / sizeof(sizes[0]);
    gpu::GpuHeap heaps[size_count]{};
    bool valid = true;
    for (std::size_t index = 0; index < size_count; ++index)
    {
        heaps[index] = gpu::create_gpu_heap(device, sizes[index]);
        const gpu::GpuRange range = gpu::gpu_range(heaps[index]);
        const gpu::GpuRange nested_range = gpu::gpu_range(heaps[index].range);
        valid = valid && heaps[index].range.cpu && heaps[index].range.gpu && heaps[index].range.size == sizes[index] && heaps[index].owner &&
                reinterpret_cast<std::uintptr_t>(heaps[index].range.cpu) % 16 == 0 &&
                reinterpret_cast<std::uintptr_t>(heaps[index].range.gpu) % 16 == 0 &&
                range.gpu == heaps[index].range.gpu && range.size == heaps[index].range.size &&
                nested_range.gpu == range.gpu && nested_range.size == range.size;
    }
    const gpu::GpuHeap readback = gpu::create_gpu_heap(device, 1, gpu::MemoryType::readback);
    const gpu::GpuHeap gpu_only = gpu::create_gpu_heap(device, 1, gpu::MemoryType::gpu_only);
    valid = valid && readback.range.cpu && readback.range.gpu && reinterpret_cast<std::uintptr_t>(readback.range.cpu) % 16 == 0 &&
            reinterpret_cast<std::uintptr_t>(readback.range.gpu) % 16 == 0 && readback.range.size == 1 && readback.owner &&
            !gpu_only.range.cpu && gpu_only.range.gpu && reinterpret_cast<std::uintptr_t>(gpu_only.range.gpu) % 16 == 0 &&
            gpu_only.range.size == 1 && gpu_only.owner;
    for (const gpu::GpuHeap& heap : heaps)
        gpu::destroy_gpu_heap(heap);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(gpu_only);
    return valid;
}

bool test_descriptor_heaps(gpu::Device* device, const gpu::DeviceCaps& caps, gpu::TimelineSemaphore* timeline,
                           std::uint64_t& next_timeline_value) noexcept
{
    const gpu::GpuHeap texture_heap = gpu::create_gpu_heap(device, caps.texture_descriptor_size, gpu::MemoryType::texture_descriptor_heap);
    if (!texture_heap.range.cpu || !texture_heap.range.gpu || texture_heap.range.size != caps.texture_descriptor_size ||
        !texture_heap.owner || reinterpret_cast<std::uintptr_t>(texture_heap.range.cpu) % 16 != 0 ||
        reinterpret_cast<std::uintptr_t>(texture_heap.range.gpu) % 16 != 0)
    {
        return false;
    }
    const gpu::GpuHeap sampler_heap =
        gpu::create_gpu_heap(device, caps.sampler_descriptor_size, gpu::MemoryType::sampler_descriptor_heap);
    if (!sampler_heap.range.cpu || !sampler_heap.range.gpu || sampler_heap.range.size != caps.sampler_descriptor_size ||
        !sampler_heap.owner || reinterpret_cast<std::uintptr_t>(sampler_heap.range.cpu) % 16 != 0 ||
        reinterpret_cast<std::uintptr_t>(sampler_heap.range.gpu) % 16 != 0)
    {
        gpu::destroy_gpu_heap(texture_heap);
        return false;
    }

    gpu::write_sampler_descriptor(device, sampler_heap.range.cpu,
                                  {
                                      .min_filter = gpu::Filter::nearest,
                                      .mip_filter = gpu::Filter::nearest,
                                      .address_u = gpu::AddressMode::clamp_to_edge,
                                      .address_w = gpu::AddressMode::clamp_to_edge,
                                      .anisotropic = true,
                                      .compare_enabled = true,
                                      .compare = gpu::CompareOp::greater_equal,
                                  });

    gpu::CommandBuffer* commands = gpu::begin_commands(device);
    gpu::set_texture_descriptor_heap(commands, gpu::gpu_range(texture_heap));
    gpu::set_sampler_descriptor_heap(commands, gpu::gpu_range(sampler_heap));
    const gpu::TimelinePoint completion{
        .semaphore = timeline,
        .value = ++next_timeline_value,
    };
    gpu::submit({commands}, completion);
    gpu::wait_timeline(completion);
    gpu::destroy_gpu_heap(texture_heap);
    gpu::destroy_gpu_heap(sampler_heap);
    return true;
}

bool test_batch_growth_and_reuse(gpu::Device* device,
                                 gpu::TimelineSemaphore* timeline,
                                 std::uint64_t& next_timeline_value,
                                 gpu::TimelinePoint& final_completion) noexcept
{
    gpu::CommandBuffer* high_water_commands[batch_command_count]{};
    bool valid = true;
    for (std::uint32_t submission = 0; submission < batch_submission_count; ++submission)
    {
        gpu::CommandBuffer* commands[batch_command_count]{};
        for (std::size_t index = 0; index < batch_command_count; ++index)
        {
            commands[index] = gpu::begin_commands(device);
            if (submission == 0)
                high_water_commands[index] = commands[index];
            else if (submission == 1)
                valid = valid && commands[index] == high_water_commands[index];
        }

        final_completion = {
            .semaphore = timeline,
            .value = ++next_timeline_value,
        };
        gpu::submit(commands, final_completion);
        if (submission < 2)
        {
            gpu::wait_timeline(final_completion);
            gpu::wait_idle(device);
        }
    }
    return valid;
}

gpu::Format combined_depth_stencil_format(gpu::Device* device) noexcept
{
    constexpr gpu::TextureUsage usage =
        gpu::TextureUsage::depth_stencil_attachment;
    if (gpu::supports_texture_format(
            device, gpu::Format::d24_unorm_s8_uint, usage))
    {
        return gpu::Format::d24_unorm_s8_uint;
    }
    if (gpu::supports_texture_format(
            device, gpu::Format::d32_float_s8_uint, usage))
    {
        return gpu::Format::d32_float_s8_uint;
    }
    return gpu::Format::undefined;
}

void record_attachment_subresource_passes(gpu::CommandBuffer* commands,
                                          gpu::RenderView* color_view,
                                          gpu::RenderView* depth_stencil_view) noexcept
{
    if (color_view)
    {
        const gpu::ColorAttachment attachment{
            .render_view = color_view,
            .load = gpu::LoadOp::clear,
            .clear = {.x = 0.1f, .y = 0.2f, .z = 0.3f, .w = 1.0f},
        };
        gpu::begin_render_pass(commands, {.colors = {&attachment, 1}});
        gpu::end_render_pass(commands);
    }
    if (depth_stencil_view)
    {
        const gpu::RenderingDesc rendering{
            .depth = {
                .render_view = depth_stencil_view,
                .load = gpu::LoadOp::clear,
                .clear = 0.5f,
            },
            .stencil = {
                .render_view = depth_stencil_view,
                .load = gpu::LoadOp::clear,
                .clear = 7,
            },
        };
        gpu::begin_render_pass(commands, rendering);
        gpu::end_render_pass(commands);
    }
}

bool test_placed_textures(gpu::Device* device,
                          const gpu::DeviceCaps& caps,
                          gpu::TimelineSemaphore* timeline,
                          std::uint64_t& next_timeline_value,
                          gpu::TimelinePoint& final_completion) noexcept
{
    const std::uint64_t texture_heap_alignment = caps.texture_heap_alignment;
    constexpr gpu::TextureDesc first_desc{
        .extent = {.x = 17, .y = 9, .z = 1},
        .usage = gpu::TextureUsage::sampled,
    };
    constexpr gpu::TextureDesc second_desc{
        .extent = {.x = 31, .y = 15, .z = 1},
        .mip_levels = 3,
        .usage = gpu::TextureUsage::sampled,
    };
    const gpu::SizeAlign first_size_align = gpu::get_texture_size_align(device, first_desc);
    const gpu::SizeAlign second_size_align = gpu::get_texture_size_align(device, second_desc);
    if (!valid_size_align(first_size_align, texture_heap_alignment) || !valid_size_align(second_size_align, texture_heap_alignment))
        return false;

    std::uint64_t heap_elements = element_count(first_size_align.size, texture_heap_alignment);
    const std::uint64_t second_offset = heap_elements * texture_heap_alignment;
    heap_elements += element_count(second_size_align.size, texture_heap_alignment);

    constexpr gpu::TextureUsage broad_usage =
        gpu::TextureUsage::sampled | gpu::TextureUsage::storage | gpu::TextureUsage::transfer_destination;
    const bool has_broad_3d = gpu::supports_texture_format(device, gpu::Format::rgba32_float, broad_usage);
    constexpr gpu::TextureDesc broad_3d_desc{
        .type = gpu::TextureType::three_d,
        .extent = {.x = 16, .y = 8, .z = 4},
        .mip_levels = 4,
        .format = gpu::Format::rgba32_float,
        .usage = broad_usage,
    };
    gpu::SizeAlign broad_3d_size_align{};
    std::uint64_t broad_3d_offset = 0;
    if (has_broad_3d)
    {
        broad_3d_size_align = gpu::get_texture_size_align(device, broad_3d_desc);
        if (!valid_size_align(broad_3d_size_align, texture_heap_alignment))
            return false;
        broad_3d_offset = heap_elements * texture_heap_alignment;
        heap_elements += element_count(broad_3d_size_align.size, texture_heap_alignment);
    }

    constexpr gpu::TextureDesc mutable_desc{
        .extent = {.x = 19, .y = 11, .z = 1},
        .mip_levels = 3,
        .mutable_format = true,
        .usage = gpu::TextureUsage::sampled,
    };
    const gpu::SizeAlign mutable_size_align = gpu::get_texture_size_align(device, mutable_desc);
    if (!valid_size_align(mutable_size_align, texture_heap_alignment))
        return false;
    const std::uint64_t mutable_offset = heap_elements * texture_heap_alignment;
    heap_elements += element_count(mutable_size_align.size, texture_heap_alignment);

    gpu::Format compressed_format = gpu::Format::undefined;
    if (caps.texture_compression_bc && gpu::supports_texture_format(device, gpu::Format::bc7_unorm, gpu::TextureUsage::sampled))
        compressed_format = gpu::Format::bc7_unorm;
    else if (caps.texture_compression_astc && gpu::supports_texture_format(device, gpu::Format::astc_4x4_unorm, gpu::TextureUsage::sampled))
        compressed_format = gpu::Format::astc_4x4_unorm;
    const bool has_compressed = compressed_format != gpu::Format::undefined;
    const gpu::TextureDesc compressed_desc{
        .extent = {.x = 20, .y = 12, .z = 1},
        .mip_levels = 4,
        .format = compressed_format,
        .usage = gpu::TextureUsage::sampled,
    };
    gpu::SizeAlign compressed_size_align{};
    std::uint64_t compressed_offset = 0;
    if (has_compressed)
    {
        compressed_size_align = gpu::get_texture_size_align(device, compressed_desc);
        if (!valid_size_align(compressed_size_align, texture_heap_alignment))
            return false;
        compressed_offset = heap_elements * texture_heap_alignment;
        heap_elements += element_count(compressed_size_align.size, texture_heap_alignment);
    }

    constexpr gpu::TextureUsage color_usage = gpu::TextureUsage::color_attachment;
    const bool has_color = gpu::supports_texture_format(device, gpu::Format::rgba8_unorm, color_usage);
    constexpr gpu::TextureDesc color_desc{
        .type = gpu::TextureType::cube,
        .extent = {.x = 8, .y = 8, .z = 1},
        .mip_levels = 3,
        .layer_count = 6,
        .usage = color_usage,
    };
    gpu::SizeAlign color_size_align{};
    std::uint64_t color_offset = 0;
    if (has_color)
    {
        color_size_align = gpu::get_texture_size_align(device, color_desc);
        if (!valid_size_align(color_size_align, texture_heap_alignment))
            return false;
        color_offset = heap_elements * texture_heap_alignment;
        heap_elements += element_count(color_size_align.size, texture_heap_alignment);
    }

    const gpu::Format depth_stencil_format = combined_depth_stencil_format(device);
    const bool has_depth_stencil = depth_stencil_format != gpu::Format::undefined;
    constexpr gpu::TextureUsage sampled_depth_stencil_usage =
        gpu::TextureUsage::sampled | gpu::TextureUsage::depth_stencil_attachment;
    const bool sampled_depth_stencil = has_depth_stencil && gpu::supports_texture_format(device, depth_stencil_format, sampled_depth_stencil_usage);
    const gpu::TextureDesc depth_stencil_desc{
        .type = gpu::TextureType::two_d_array,
        .extent = {.x = 8, .y = 8, .z = 1},
        .mip_levels = 3,
        .layer_count = 2,
        .format = depth_stencil_format,
        .usage = sampled_depth_stencil ? sampled_depth_stencil_usage : gpu::TextureUsage::depth_stencil_attachment,
    };
    gpu::SizeAlign depth_stencil_size_align{};
    std::uint64_t depth_stencil_offset = 0;
    if (has_depth_stencil)
    {
        depth_stencil_size_align = gpu::get_texture_size_align(device, depth_stencil_desc);
        if (!valid_size_align(depth_stencil_size_align, texture_heap_alignment))
            return false;
        depth_stencil_offset = heap_elements * texture_heap_alignment;
        heap_elements += element_count(depth_stencil_size_align.size, texture_heap_alignment);
    }

    const std::uint64_t heap_size = heap_elements * texture_heap_alignment;
    gpu::TextureHeap texture_heap = gpu::create_texture_heap(device, heap_size);
    gpu::Texture* first = gpu::create_texture(device, first_desc, texture_heap, 0);
    gpu::Texture* second = gpu::create_texture(device, second_desc, texture_heap, second_offset);
    gpu::Texture* broad_3d = has_broad_3d ? gpu::create_texture(device, broad_3d_desc, texture_heap, broad_3d_offset) : nullptr;
    gpu::Texture* mutable_texture = gpu::create_texture(device, mutable_desc, texture_heap, mutable_offset);
    gpu::Texture* compressed = has_compressed ? gpu::create_texture(device, compressed_desc, texture_heap, compressed_offset) : nullptr;
    gpu::Texture* color = has_color ? gpu::create_texture(device, color_desc, texture_heap, color_offset) : nullptr;
    gpu::Texture* depth_stencil =
        has_depth_stencil ? gpu::create_texture(device, depth_stencil_desc, texture_heap, depth_stencil_offset) : nullptr;
#if defined(NDEBUG)
    if (sampled_depth_stencil)
    {
        const gpu::GpuHeap descriptor_heap =
            gpu::create_gpu_heap(device, caps.texture_descriptor_size, gpu::MemoryType::texture_descriptor_heap);
        gpu::write_texture_descriptor(device, descriptor_heap.range.cpu, depth_stencil, gpu::TextureDescriptorType::sampled);
        gpu::write_texture_descriptor(device, descriptor_heap.range.cpu, depth_stencil, gpu::TextureDescriptorType::sampled,
                                      {.aspect = gpu::TextureAspect::stencil});
        gpu::destroy_gpu_heap(descriptor_heap);
    }
#endif
    gpu::CommandBuffer* commands = gpu::begin_commands(device);
    gpu::TextureHeap recording_heap = gpu::create_texture_heap(device, first_size_align.size);
    gpu::RenderView* color_render_view = color ? gpu::create_render_view(color, {.mip_level = 2, .slice = 5}) : nullptr;
    gpu::RenderView* depth_stencil_render_view =
        depth_stencil ? gpu::create_render_view(depth_stencil, {.mip_level = 1, .slice = 1}) : nullptr;
    record_attachment_subresource_passes(commands, color_render_view, depth_stencil_render_view);
    final_completion = {
        .semaphore = timeline,
        .value = ++next_timeline_value,
    };
    gpu::submit({commands}, final_completion);
    gpu::wait_timeline(final_completion);
    gpu::destroy_render_view(depth_stencil_render_view);
    gpu::destroy_render_view(color_render_view);
    gpu::destroy_texture(depth_stencil);
    gpu::destroy_texture(color);
    gpu::destroy_texture(compressed);
    gpu::destroy_texture(mutable_texture);
    gpu::destroy_texture(broad_3d);
    gpu::destroy_texture(second);
    gpu::destroy_texture(first);
    gpu::destroy_texture_heap(recording_heap);
    gpu::destroy_texture_heap(texture_heap);
    return true;
}

} // namespace

int main()
{
    const gpu::DeviceInit device_init = gpu::create_device();
    if (device_init.error == gpu::Error::unsupported)
        return skipped;
    if (device_init.error != gpu::Error::none)
        return 1;

    gpu::Device* device = device_init.device;
    const gpu::DeviceCaps& caps = gpu::get_device_caps(device);
    if (caps.texture_heap_alignment == 0 ||
        (caps.texture_heap_alignment & (caps.texture_heap_alignment - 1)) != 0 ||
        caps.texture_descriptor_size == 0 ||
        caps.sampler_descriptor_size == 0 ||
        !test_gpu_heaps(device))
    {
        gpu::destroy_device(device);
        return 1;
    }
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    std::uint64_t next_timeline_value = 0;
    gpu::TimelinePoint final_completion{};
    if (!test_descriptor_heaps(device, caps, timeline, next_timeline_value))
    {
        gpu::destroy_timeline_semaphore(timeline);
        gpu::destroy_device(device);
        return 1;
    }

    if (!test_placed_textures(device, caps, timeline, next_timeline_value, final_completion))
    {
        gpu::destroy_timeline_semaphore(timeline);
        gpu::destroy_device(device);
        return 1;
    }

    if (!test_batch_growth_and_reuse(device, timeline, next_timeline_value,
                                     final_completion))
    {
        gpu::wait_timeline(final_completion);
        gpu::wait_idle(device);
        gpu::destroy_timeline_semaphore(timeline);
        gpu::destroy_device(device);
        return 1;
    }

    gpu::wait_timeline(final_completion);
    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_device(device);
    return 0;
}
