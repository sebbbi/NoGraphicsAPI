#pragma once

#include <NoGraphicsAPIUtility/shader_types.h>

struct CubeVertex
{
    float4 position;
    float2 uv;
};

struct CubeRootArguments
{
    CubeVertex* vertices;
    uint32 root_alignment_padding0[2];
    float4x4 transform;
};

#if !defined(__SLANG__)
static_assert(sizeof(CubeRootArguments) == 80);
#endif
