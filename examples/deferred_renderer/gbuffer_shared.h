#pragma once

#include "object_data.h"

struct GBufferRoot
{
    ObjectData* objects;
    uint32 root_alignment_padding0[2];
    float4x4 view_projection;
    float3x4 orientation;
};

static const uint32 object_grid_width = 512;
static const uint32 gbuffer_thread_count = 32;

#if !defined(__SLANG__)
static_assert(sizeof(GBufferRoot) == 128);
#endif
