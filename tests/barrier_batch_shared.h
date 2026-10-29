#pragma once
#include <NoGraphicsAPIUtility/shader_types.h>

struct BarrierBatchRoot
{
    uint32* input;
    uint32* output;
    uint32 value;
    uint32 padding[3];
};

#if !defined(__SLANG__)
#include <stddef.h>
static_assert(sizeof(BarrierBatchRoot) == 32 && offsetof(BarrierBatchRoot, value) == 16);
#endif
