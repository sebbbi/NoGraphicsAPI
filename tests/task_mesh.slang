#include "task_mesh_shared.h"

[[vk::push_constant]] ConstantBuffer<TaskMeshRoot> root;

struct TaskTestVertex
{
    float4 position : SV_Position;
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(task_test_batch_size, 1, 1)]
void meshMain(uint lane : SV_GroupIndex, uint3 group_id : SV_GroupID, in payload TaskTestPayload compacted,
    out vertices TaskTestVertex vertices[4], out indices uint3 triangles[2])
{
    uint id = compacted.ids[group_id.x];
    if (lane == 0) InterlockedAdd(root.data->visits[min(id, task_test_capacity)], 1);
    if (id >= root.count)
    {
        SetMeshOutputCounts(0, 0);
        return;
    }
    SetMeshOutputCounts(4, 2);
    if (lane < 4)
    {
        float x = float(id * 4 + 1 + (lane & 1) * 2) / float(task_test_width) * 2.0f - 1.0f;
        float y = float(2 + (lane >> 1) * 4) / float(task_test_height) * 2.0f - 1.0f;
        vertices[lane].position = float4(x, y, 0.5f, 1.0f);
    }
    if (lane < 2) triangles[lane] = lane == 0 ? uint3(0, 1, 2) : uint3(2, 1, 3);
}

[shader("fragment")]
float4 fragmentMain() : SV_Target0
{
    return root.color;
}
