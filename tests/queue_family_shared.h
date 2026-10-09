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
    uint32 root_alignment_padding0[2];
};

#if !defined(__SLANG__)
static_assert(sizeof(QueueFamilyRoot) == 32);
#endif
