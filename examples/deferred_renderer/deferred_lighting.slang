#include "deferred_lighting_shared.h"

struct FullscreenVertex
{
    float4 position : SV_Position;
    float3 world_ray : TEXCOORD0;
};

[[vk::push_constant]] ConstantBuffer<DeferredLightingRoot> root;

[shader("vertex")]
FullscreenVertex vertexMain(uint vertex_index : SV_VertexID)
{
    static const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2( 3.0f, -1.0f),
        float2(-1.0f,  3.0f),
    };

    float2 ndc = positions[vertex_index];
    FullscreenVertex output;
    output.position = float4(ndc, 0.0f, 1.0f);
    output.world_ray = root.ray_center.xyz + root.ray_horizontal.xyz * ndc.x + root.ray_vertical.xyz * ndc.y;
    return output;
}

[shader("fragment")]
float4 fragmentMain(FullscreenVertex input) : SV_Target0
{
    uint texture_base = root.gbuffer_texture_base;
    Texture2D<float4> albedo_texture = ResourceDescriptorHeap[texture_base + uint(GBufferTexture::albedo)];
    Texture2D<float4> normal_roughness_texture = ResourceDescriptorHeap[texture_base + uint(GBufferTexture::normal_roughness)];
    Texture2D<float> depth_texture = ResourceDescriptorHeap[texture_base + uint(GBufferTexture::depth)];

    int3 location = int3(int2(input.position.xy * root.gbuffer_pixel_scale), 0);
    float depth = depth_texture.Load(location);
    if (depth >= 1.0f) return float4(0.008f, 0.012f, 0.025f, 1.0f);

    float3 albedo = albedo_texture.Load(location).rgb;
    float4 normal_roughness = normal_roughness_texture.Load(location);
    float3 normal = normalize(normal_roughness.xyz);
    float roughness = normal_roughness.w;
    float view_depth = root.depth_linearize.x / (root.depth_linearize.y - depth);
    float3 world_position = root.camera_position.xyz + input.world_ray * view_depth;

    float3 light_vector = float3(240.0f, 360.0f, 120.0f) - world_position;
    float light_distance_squared = max(dot(light_vector, light_vector), 0.001f);
    float3 light_direction = light_vector * rsqrt(light_distance_squared);
    float diffuse = saturate(dot(normal, light_direction));
    float attenuation = 1.0f / (1.0f + 0.0000055555556f * light_distance_squared);

    float3 sun_direction = normalize(float3(0.35f, 0.75f, 0.45f));
    float sun = saturate(dot(normal, sun_direction));
    float3 view_direction = normalize(root.camera_position.xyz - world_position);
    float3 half_direction = normalize(light_direction + view_direction);
    float specular_power = lerp(96.0f, 8.0f, roughness);
    float specular = pow(saturate(dot(normal, half_direction)), specular_power) * (1.0f - roughness) * attenuation;

    float3 diffuse_light = float3(0.035f, 0.045f, 0.070f) + diffuse * attenuation * float3(1.8f, 1.45f, 1.1f) + sun * float3(0.30f, 0.36f, 0.45f);
    float3 color = albedo * diffuse_light + specular * float3(1.0f, 0.85f, 0.65f);
    return float4(color, 1.0f);
}
