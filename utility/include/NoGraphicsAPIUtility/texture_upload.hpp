#pragma once

#include <NoGraphicsAPIUtility/upload_queue.hpp>

namespace gpu
{

// Description must match the destination. Source contains the tightly packed region; pitches must be zero.
// Combined depth/stencil formats are unsupported. Oversized regions split along slices, rows, then format blocks.
void upload_texture(UploadQueue& queue, Texture* destination, const TextureDesc& description,
                    ByteSpan source, const TextureCopyDesc& copy = {}) noexcept;

} // namespace gpu
