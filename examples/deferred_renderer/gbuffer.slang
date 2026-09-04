#include "gbuffer_shared.h"

struct GBufferVertexOutput
{
    float4 position : SV_Position;
    float3 albedo : COLOR0;
    float3 world_normal : NORMAL0;
};

[[vk::push_constant]] ConstantBuffer<GBufferRoot> root;

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void meshMain(uint thread_index : SV_GroupIndex,
              uint3 group_id : SV_GroupID,
              out vertices GBufferVertexOutput vertices[24],
              out indices uint3 triangles[12])
{
    SetMeshOutputCounts(24, 12);

    uint object_index = group_id.x + group_id.y * object_grid_width;
    if (thread_index < 24)
    {
        ObjectData object = root.objects[object_index];
        // Bit i stores the positive coordinate sign for face-split vertex i.
        float3 cube_position = float3(
            float((0x6fc6c0u >> thread_index) & 1u),
            float((0xc3f06cu >> thread_index) & 1u),
            float((0xf66c06u >> thread_index) & 1u)) * 2.0f - 1.0f;
        uint face = thread_index >> 2u;
        float3 cube_normal = float3(
            float(face == 4u) - float(face == 0u),
            float(face == 3u) - float(face == 2u),
            float(face == 5u) - float(face == 1u));
        float3 world_position = mul(root.orientation, float4(cube_position, 1.0f)) + object.position;
        float3 object_color = float3(
            float(object_index & 127u) / 127.0f,
            float((object_index >> 7u) & 127u) / 127.0f,
            float((object_index >> 14u) & 63u) / 63.0f);

        GBufferVertexOutput output;
        output.position = mul(root.view_projection, float4(world_position, 1.0f));
        output.albedo = float3(0.55f, 0.015f, 0.01f) + object_color * float3(0.35f, 0.08f, 0.05f);
        output.world_normal = mul(root.orientation, float4(cube_normal, 0.0f));
        vertices[thread_index] = output;
    }

    if (thread_index < 12)
    {
        uint base_vertex = (thread_index >> 1u) * 4u;
        triangles[thread_index] = (thread_index & 1u) == 0u
            ? uint3(base_vertex, base_vertex + 1u, base_vertex + 2u)
            : uint3(base_vertex + 2u, base_vertex + 3u, base_vertex);
    }
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
