#pragma once

#include <NoGraphicsAPIUtility/shader_types.h>

struct UploadQueueRoot
{
    uint32* source;
    uint32* destination;
    uint32 count;
};

static const uint32 upload_queue_thread_count = 64;
