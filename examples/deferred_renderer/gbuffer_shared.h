#pragma once

#include <NoGraphicsAPI/shader_types.h>

struct GBufferVertex
{
    float3 position;
    float3 normal;
};

struct ObjectData
{
    float3x4 transform;
};

struct GBufferRoot
{
    GBufferVertex* vertices;
    ObjectData* objects;
    float4x4 view_projection;
};
