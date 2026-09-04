#include "simulation_shared.h"

[[vk::push_constant]] ConstantBuffer<SimulationRoot> root;

[shader("compute")]
[numthreads(simulation_thread_count, 1, 1)]
void computeMain(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint object_index = dispatch_thread_id.x;
    ObjectData object = root.objects[object_index];
    float inverse_distance = rsqrt(dot(object.position, object.position) + gravity_softening * gravity_softening);
    float gravity_scale = -central_gravity * inverse_distance * inverse_distance * inverse_distance * root.delta_seconds;
    object.velocity += object.position * gravity_scale;
    object.position += object.velocity * root.delta_seconds;
    root.objects[object_index] = object;
}
