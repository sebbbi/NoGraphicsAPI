#include "queue_family_shared.h"

[[vk::push_constant]] ConstantBuffer<QueueFamilyRoot> root;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= queue_test_width || thread.y >= queue_test_height) return;
    uint index = thread.y * queue_test_width + thread.x;
    root.destination[index] = root.source[index] * 3 + 7;
    Texture2D<float4> source = ResourceDescriptorHeap[root.source_texture];
    RWTexture2D<float4> destination = ResourceDescriptorHeap[root.destination_texture];
    destination[thread.xy] = source.Load(int3(thread.xy, 0)).bgra;
}
