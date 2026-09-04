#include "gbuffer_shared.h"

struct GBufferVertexOutput
{
    float4 position : SV_Position;
    float3 albedo : COLOR0;
    float3 world_normal : NORMAL0;
};

[[vk::push_constant]] ConstantBuffer<GBufferRoot> root;

[shader("vertex")]
GBufferVertexOutput vertexMain(uint vertex_index : SV_VertexID, uint instance_index : SV_InstanceID)
{
    GBufferVertex vertex = root.vertices[vertex_index];
    ObjectData object = root.objects[instance_index];
    float3 world_position = mul(object.transform, float4(vertex.position, 1.0f));

    float3 object_color = float3(
        float(instance_index & 127u) / 127.0f,
        float((instance_index >> 7u) & 127u) / 127.0f,
        float((instance_index >> 14u) & 63u) / 63.0f);

    GBufferVertexOutput output;
    output.position = mul(root.view_projection, float4(world_position, 1.0f));
    output.albedo = float3(0.55f, 0.015f, 0.01f) + object_color * float3(0.35f, 0.08f, 0.05f);
    output.world_normal = mul(object.transform, float4(vertex.normal, 0.0f));
    return output;
}

struct GBufferOutput
{
    float4 albedo : SV_Target0;
    float4 normal_roughness : SV_Target1;
};

[shader("fragment")]
GBufferOutput fragmentMain(GBufferVertexOutput input)
{
    GBufferOutput output;
    output.albedo = float4(input.albedo, 1.0f);
    output.normal_roughness = float4(normalize(input.world_normal), 0.55f);
    return output;
}
