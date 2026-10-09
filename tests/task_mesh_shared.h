#pragma once

#include "task_shader_common.h"

struct TaskMeshRoot
{
    TaskTestData* data;
    uint32 count;
    uint32 visible_mask;
    float4 color;
};

#if !defined(__SLANG__)
static_assert(sizeof(TaskMeshRoot) == 32);
#endif
