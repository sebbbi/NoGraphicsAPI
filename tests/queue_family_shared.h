#pragma once

#include <NoGraphicsAPIUtility/shader_types.h>

static const uint32 queue_test_width = 17;
static const uint32 queue_test_height = 9;

struct QueueFamilyRoot
{
    uint32* source;
    uint32* destination;
    uint32 source_texture;
    uint32 destination_texture;
};
