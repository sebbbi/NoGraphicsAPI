#include <NoGraphicsAPI/shader.slang>
#include "barrier_batch_shared.h"
GPU_ROOT(BarrierBatchRoot, root, 16);

[numthreads(64, 1, 1)]
void computeMain(uint lane : SV_DispatchThreadID)
{
    root.output[lane] = root.input[lane] + root.value;
}
