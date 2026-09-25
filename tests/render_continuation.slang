[shader("vertex")]
float4 vertexMain(uint vertex_index : SV_VertexID) : SV_Position
{
    return float4(float2((vertex_index << 1) & 2, vertex_index & 2) * 2.0f - 1.0f, 0.0f, 1.0f);
}

[shader("fragment")]
float4 fragmentMain() : SV_Target0
{
    return float4(1.0f, 0.0f, 0.0f, 1.0f);
}
