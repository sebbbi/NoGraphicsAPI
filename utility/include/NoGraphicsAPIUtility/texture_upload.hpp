#pragma once

#include <NoGraphicsAPIUtility/upload_queue.hpp>

namespace gpu
{

// Description must match the destination. Source contains the tightly packed region; pitches must be zero.
// Combined depth/stencil formats are unsupported. Splits respect the selected queue's texture granularity;
// staging must fit at least one granularity-sized block (clipped at the mip edge).
void upload_texture(UploadQueue& queue, Texture* destination, const TextureDesc& description,
                    ByteSpan source, const TextureCopyDesc& copy = {}) noexcept;

} // namespace gpu
