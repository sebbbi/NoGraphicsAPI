#pragma once

#include <NoGraphicsAPIUtility/shader_types.h>

static const uint32 task_test_batch_size = 32;
static const uint32 task_test_capacity = 96;
static const uint32 task_test_batches = task_test_capacity / task_test_batch_size;
static const uint32 task_test_width = task_test_capacity * 4;
static const uint32 task_test_height = 8;

struct TaskTestPayload
{
    uint32 ids[task_test_batch_size];
};

struct TaskTestData
{
    uint32 visibility[task_test_capacity];
    uint32 batch_counts[task_test_batches];
    uint32 visits[task_test_capacity + 1];
};
