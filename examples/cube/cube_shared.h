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
    float4x4 transform;
};
