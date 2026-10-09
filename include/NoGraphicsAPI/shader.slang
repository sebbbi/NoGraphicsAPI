#pragma once

#define GPU_ROOT_SELECT(_1, _2, _3, macro, ...) macro
#define GPU_ROOT(...) GPU_ROOT_SELECT(__VA_ARGS__, GPU_ROOT_IMPL, GPU_ROOT_DEFAULT)(__VA_ARGS__)

#ifdef NOGRAPHICSAPI_METAL

#include <NoGraphicsAPI/shader_shared.h>

#define GPU_ROOT_DEFAULT(type, name) GPU_ROOT_IMPL(type, name, 16)
#define GPU_ROOT_IMPL(type, name, alignment) \
    [[vk::binding(0)]] ConstantBuffer<type> name##_storage : register(b0); \
    static type name = gpu_root_aligned_storage(name##_storage, alignment)
#define GPU_ADDRESS(value) __getAddress(value)

[ForceInline]
ConstantBuffer<T> gpu_root_aligned_storage<T>(ConstantBuffer<T> storage, constexpr int alignment)
{
    static_assert(sizeof(T) % 16 == 0, "Root structs require a 16-byte stride and native vector alignment; pointed-to data uses C layout");
    __intrinsic_asm "(__builtin_assume((reinterpret_cast<ulong>($0) & ($1 - 1)) == 0), $0)";
}

[[vk::binding(1)]] ConstantBuffer<GPUTextureHeap> gpu_texture_heap : register(b1);

// Metal requires a struct around resource elements in an argument buffer; each sampler still occupies exactly eight bytes.
__target_intrinsic(metal, "GPUSampler")
struct GPUSampler { uint64_t value; };
[[vk::binding(2)]] StructuredBuffer<GPUSampler> gpu_sampler_heap : register(t2);

T gpu_sampler_value<T : IOpaqueDescriptor>(GPUSampler value)
{
    __requirePrelude(R"(struct GPUSampler { sampler value; }; static_assert(sizeof(GPUSampler) == 8);)");
    __intrinsic_asm "($0.value)";
}

T gpu_texture_from_id<T : IOpaqueDescriptor>(uint64_t resource_id)
{
    __requirePrelude(R"(template<typename T> T gpu_texture_from_id(ulong id) { return *reinterpret_cast<thread T*>(&id); })");
    __intrinsic_asm "gpu_texture_from_id<$TR>($0)";
}

[ForceInline]
T gpu_texture<T : IOpaqueDescriptor>(uint index)
{
    return gpu_texture_from_id<T>(gpu_texture_heap.base != 0 ? gpu_texture_heap.base + uint64_t(index) : gpu_texture_heap.ids[index]);
}

[ForceInline]
T gpu_sampler<T : IOpaqueDescriptor = SamplerState>(uint index)
{
    return gpu_sampler_value<T>(gpu_sampler_heap[index]);
}

#else

#define GPU_ROOT_DEFAULT(type, name) GPU_ROOT_IMPL(type, name, 16)
#define GPU_ROOT_IMPL(type, name, alignment) [[vk::binding(0, 0)]] ConstantBuffer<type> name
#define GPU_ADDRESS(value) (&(value))

[ForceInline]
T gpu_texture<T : IOpaqueDescriptor>(uint index)
{
    return getDescriptorFromHandle(DescriptorHandle<T>(uint2(index, 0)));
}

[ForceInline]
T gpu_sampler<T : IOpaqueDescriptor = SamplerState>(uint index)
{
    return getDescriptorFromHandle(DescriptorHandle<T>(uint2(index, 0)));
}

#endif

[ForceInline]
uint gpu_nonuniform_index(uint index)
{
    __target_switch
    {
    case metal: return index;
    default: return NonUniformResourceIndex(index);
    }
}

[ForceInline]
uint gpu_wave_prefix_count_bits(bool value)
{
    __target_switch
    {
    case metal: __intrinsic_asm "simd_prefix_exclusive_sum(uint($0))";
    default: return WavePrefixCountBits(value);
    }
}
