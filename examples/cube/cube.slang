#include "cube_shared.h"

[[vk::push_constant]] ConstantBuffer<CubeRootArguments> root;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 fragment_position : TEXCOORD1;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertex_id : SV_VertexID)
{
    CubeVertex vertex = root.vertices[vertex_id];
    float4 clip_position = mul(root.transform, vertex.position);

    VertexOutput output;
    output.position = clip_position;
    output.uv = vertex.uv;
    output.fragment_position = clip_position.xyz;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
    Texture2D<float4> texture = ResourceDescriptorHeap[0];
    SamplerState sampler = SamplerDescriptorHeap[0];

    float3 normal = normalize(cross(ddx(input.fragment_position), ddy(input.fragment_position)));
    float light = max(0.0f, dot(float3(0.424f, 0.566f, 0.707f), normal));
    float4 texel = texture.Sample(sampler, input.uv);
    return float4(light * texel.rgb, texel.a);
}
