#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <cstddef>
#include <cstdint>

namespace
{

constexpr int skipped = 77;
constexpr std::size_t batch_command_count = 20;
constexpr std::uint32_t batch_submission_count = 4;

struct DescriptorHeapData
{
    std::uint32_t first;
    std::uint32_t second;
};

bool test_ordinary_allocations(gpu::Device* device) noexcept
{
    gpu::GpuAllocation<std::uint32_t> first = gpu::gpu_malloc<std::uint32_t>(device, 4);
    gpu::GpuAllocation<std::uint32_t> second = gpu::gpu_malloc<std::uint32_t>(device, 8);
    const bool valid = first.cpu && first.gpu && first.size == 4 * sizeof(std::uint32_t) && first.allocation_owner &&
                       first.allocation_token != 0 && second.cpu && second.gpu &&
                       second.size == 8 * sizeof(std::uint32_t) && second.allocation_owner &&
                       second.allocation_token != 0;
    gpu::gpu_free(first);
    gpu::gpu_free(second);
    return valid;
}

bool test_descriptor_heaps(gpu::Device* device, const gpu::DeviceCaps& caps, gpu::TimelineSemaphore* timeline,
                           std::uint64_t& next_timeline_value) noexcept
{
    constexpr std::uint64_t heap_alignment = 256;
    gpu::GpuAllocation<std::byte> texture_heap =
        gpu::gpu_malloc(device, caps.texture_descriptor_stride, gpu::MemoryType::texture_heap, heap_alignment);
    if (!texture_heap.cpu || !texture_heap.gpu || texture_heap.size != caps.texture_descriptor_stride ||
        reinterpret_cast<std::uintptr_t>(texture_heap.gpu) % heap_alignment != 0)
    {
        return false;
    }
    gpu::GpuAllocation<std::byte> sampler_heap =
        gpu::gpu_malloc(device, caps.sampler_descriptor_stride, gpu::MemoryType::sampler_heap);
    if (!sampler_heap.cpu || !sampler_heap.gpu || sampler_heap.size != caps.sampler_descriptor_stride)
    {
        gpu::gpu_free(texture_heap);
        return false;
    }

    gpu::GpuAllocation<DescriptorHeapData> typed_texture_heap =
        gpu::gpu_malloc<DescriptorHeapData>(device, 1, gpu::MemoryType::texture_heap);
    gpu::GpuAllocation<DescriptorHeapData> typed_sampler_heap =
        gpu::gpu_malloc<DescriptorHeapData>(device, 1, gpu::MemoryType::sampler_heap);
    const bool typed_heaps_valid =
        typed_texture_heap.cpu && typed_texture_heap.gpu && typed_texture_heap.size == sizeof(DescriptorHeapData) &&
        reinterpret_cast<std::uintptr_t>(typed_texture_heap.cpu) % alignof(DescriptorHeapData) == 0 &&
        reinterpret_cast<std::uintptr_t>(typed_texture_heap.gpu) % alignof(DescriptorHeapData) == 0 &&
        typed_sampler_heap.cpu && typed_sampler_heap.gpu && typed_sampler_heap.size == sizeof(DescriptorHeapData) &&
        reinterpret_cast<std::uintptr_t>(typed_sampler_heap.cpu) % alignof(DescriptorHeapData) == 0 &&
        reinterpret_cast<std::uintptr_t>(typed_sampler_heap.gpu) % alignof(DescriptorHeapData) == 0;
    gpu::gpu_free(typed_texture_heap);
    gpu::gpu_free(typed_sampler_heap);
    if (!typed_heaps_valid)
    {
        gpu::gpu_free(texture_heap);
        gpu::gpu_free(sampler_heap);
        return false;
    }

    gpu::write_sampler_descriptor(device, sampler_heap.cpu,
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
    gpu::set_texture_heap(commands, gpu::gpu_range(texture_heap));
    gpu::set_sampler_heap(commands, gpu::gpu_range(sampler_heap));
    const gpu::TimelinePoint completion{
        .semaphore = timeline,
        .value = ++next_timeline_value,
    };
    gpu::submit({commands}, completion);
    gpu::gpu_free(texture_heap);
    gpu::gpu_free(sampler_heap);
    gpu::wait_timeline(completion);
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
    if (caps.texture_descriptor_size == 0 ||
        caps.texture_descriptor_stride < caps.texture_descriptor_size ||
        caps.sampler_descriptor_size == 0 ||
        caps.sampler_descriptor_stride < caps.sampler_descriptor_size ||
        !test_ordinary_allocations(device))
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

    gpu::Texture* color = nullptr;
    constexpr gpu::TextureUsage color_usage = gpu::TextureUsage::color_attachment;
    if (gpu::supports_texture_format(device, gpu::Format::rgba8_unorm, color_usage))
    {
        color = gpu::create_texture(
            device,
            {
                .type = gpu::TextureType::cube,
                .width = 8,
                .height = 8,
                .mip_levels = 3,
                .usage = color_usage,
            });
    }
    gpu::Texture* depth_stencil = nullptr;
    const gpu::Format depth_stencil_format =
        combined_depth_stencil_format(device);
    if (depth_stencil_format != gpu::Format::undefined)
    {
        depth_stencil = gpu::create_texture(
            device,
            {
                .type = gpu::TextureType::two_d_array,
                .width = 8,
                .height = 8,
                .mip_levels = 3,
                .layer_count = 2,
                .format = depth_stencil_format,
                .usage = gpu::TextureUsage::depth_stencil_attachment,
            });
    }

    gpu::RenderView* color_render_view = color
        ? gpu::create_render_view(
              color,
              {
                  .mip_level = 2,
                  .slice = 5,
              })
        : nullptr;
    gpu::RenderView* depth_stencil_render_view = depth_stencil
        ? gpu::create_render_view(
              depth_stencil,
              {
                  .mip_level = 1,
                  .slice = 1,
              })
        : nullptr;

    if (color || depth_stencil)
    {
        gpu::CommandBuffer* commands = gpu::begin_commands(device);
        record_attachment_subresource_passes(
            commands, color_render_view, depth_stencil_render_view);
        final_completion = {
            .semaphore = timeline,
            .value = ++next_timeline_value,
        };
        gpu::submit({commands}, final_completion);
        gpu::destroy_render_view(depth_stencil_render_view);
        gpu::destroy_render_view(color_render_view);
        gpu::destroy_texture(depth_stencil);
        gpu::destroy_texture(color);
        gpu::wait_timeline(final_completion);
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

    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_device(device);
    return 0;
}
