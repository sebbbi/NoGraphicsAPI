#include <metal_stdlib>
using namespace metal;
#include <NoGraphicsAPI/shader_shared.h>
struct Root { device uint* output; uint texture_index; uint sampler_index; uint value; uint storage_index; };
struct Samplers { sampler entries[32]; };
kernel void heapTest(constant Root& root [[buffer(0)]], constant GPUTextureHeap& pool [[buffer(1)]],
                     constant Samplers& samplers [[buffer(2)]], uint tid [[thread_position_in_grid]])
{
    ulong sampled_id = pool.base ? pool.base + root.texture_index : pool.ids[root.texture_index];
    ulong storage_id = pool.base ? pool.base + root.storage_index : pool.ids[root.storage_index];
    texture2d<float> sampled = *reinterpret_cast<thread texture2d<float>*>(&sampled_id);
    texture2d<float, access::write> storage = *reinterpret_cast<thread texture2d<float, access::write>*>(&storage_id);
    float4 color = sampled.sample(samplers.entries[root.sampler_index], float2(0.5f), level(0.0f));
    root.output[tid] = uint(round(color.x * 255.0f)) + root.value + tid;
    storage.write(color, uint2(tid & 1u, tid >> 1u));
}
