#include "upload_queue_shared.h"

[[vk::push_constant]] ConstantBuffer<UploadQueueRoot> root;

[shader("compute")]
[numthreads(upload_queue_thread_count, 1, 1)]
void computeMain(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x < root.count) root.destination[thread.x] += root.source[thread.x];
}
