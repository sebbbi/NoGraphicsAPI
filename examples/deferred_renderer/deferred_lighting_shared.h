#pragma once

#include <NoGraphicsAPIUtility/shader_types.h>

enum class GBufferTexture : uint32
{
    albedo,
    normal_roughness,
    depth,
    count,
};

struct DeferredLightingRoot
{
    float4 camera_position;
    float4 ray_center;
    float4 ray_horizontal;
    float4 ray_vertical;
    float2 depth_linearize;
    float2 gbuffer_pixel_scale;
    uint32 gbuffer_texture_base;
};
