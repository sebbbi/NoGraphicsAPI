#pragma once

#include <NoGraphicsAPIUtility/shader_types.h>

struct UploadQueueRoot
{
    uint32* source;
    uint32* destination;
    uint32 count;
    uint32 root_alignment_padding0[3];
};

static const uint32 upload_queue_thread_count = 64;

#if !defined(__SLANG__)
static_assert(sizeof(UploadQueueRoot) == 32);
#endif
