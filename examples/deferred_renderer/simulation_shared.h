#pragma once

#include "object_data.h"

struct SimulationRoot
{
    ObjectData* objects;
    float delta_seconds;
};

static const uint32 simulation_thread_count = 256;
static const float central_gravity = 81000.0f;
static const float gravity_softening = 12.0f;
