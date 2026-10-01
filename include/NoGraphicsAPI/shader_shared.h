#pragma once

#if defined(__SLANG__)
#define GPU_HEAP_UINT64 uint64_t
#define GPU_HEAP_DEVICE
#elif defined(__METAL_VERSION__)
#define GPU_HEAP_UINT64 ulong
#define GPU_HEAP_DEVICE device
#else
#include <NoGraphicsAPI/types.h>
#define GPU_HEAP_UINT64 uint64
#define GPU_HEAP_DEVICE
#endif

struct GPUTextureHeap
{
    GPU_HEAP_UINT64 base;
    GPU_HEAP_DEVICE GPU_HEAP_UINT64* ids;
};

#undef GPU_HEAP_UINT64
#undef GPU_HEAP_DEVICE
