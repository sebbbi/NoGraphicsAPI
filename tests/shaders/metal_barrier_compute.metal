#include <metal_stdlib>
using namespace metal;
#include "metal_barrier_shared.h"

kernel void chainMain(constant MetalBarrierRoot& root [[buffer(0)]])
{
    root.output[0] = root.input[0] + root.value;
}

kernel void computeMain(constant MetalBarrierRoot& root [[buffer(0)]])
{
    if (root.overwrite) root.input[0] = root.value;
    else
    {
        root.input[1] = root.value;
        root.indices[0] = 0;
        root.indices[1] = 1;
        root.indices[2] = 2;
        root.arguments[0] = 3;
        root.arguments[1] = 1;
        root.arguments[2] = 0;
        root.arguments[3] = 0;
    }
}
