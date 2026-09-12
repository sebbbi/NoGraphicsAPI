#include <NoGraphicsAPIUtility/texture_upload.hpp>

#include <assert.h>

namespace gpu
{

void upload_texture(UploadQueue& queue, Texture* destination, const TextureDesc& description, ByteSpan source, const TextureCopyDesc& copy) noexcept
{
    assert(copy.mip_level < description.mip_levels && !copy.row_pitch_bytes && !copy.slice_pitch_bytes);
    const uint64 capacity = queue.stats().capacity;
    const TextureFormatInfo format = get_texture_format_info(description.format);
    assert(format.bytes_per_block && capacity >= format.bytes_per_block && !(format.depth && format.stencil));
    TextureCopyDesc region = copy;
    uint32 mip_width = description.extent.x >> copy.mip_level;
    uint32 mip_height = description.extent.y >> copy.mip_level;
    uint32 mip_depth = description.extent.z >> copy.mip_level;
    if (!mip_width) mip_width = 1;
    if (!mip_height) mip_height = 1;
    if (!mip_depth) mip_depth = 1;
    if (!region.extent.x) region.extent.x = mip_width - region.offset.x;
    if (!region.extent.y) region.extent.y = mip_height - region.offset.y;
    if (!region.extent.z) region.extent.z = mip_depth - region.offset.z;
    if (!region.slice_count) region.slice_count = description.layer_count - region.base_slice;
    const bool volume = description.type == TextureType::three_d;
    const uint32 slices = volume ? region.extent.z : region.slice_count;
    const uint32 columns = (region.extent.x + format.block_extent.x - 1) / format.block_extent.x;
    const uint32 rows = (region.extent.y + format.block_extent.y - 1) / format.block_extent.y;
    const uint64 row_bytes = uint64(columns) * format.bytes_per_block;
    const uint64 slice_bytes = row_bytes * rows;
    assert(region.extent.x && region.extent.y && slices && source.data && source.size == slice_bytes * slices);
    for (uint32 slice = 0; slice < slices;)
    {
        TextureCopyDesc part = region;
        part.base_slice = volume ? 0 : region.base_slice + slice;
        part.offset.z = volume ? region.offset.z + slice : 0;
        if (slice_bytes <= capacity)
        {
            const uint32 count = uint32(capacity / slice_bytes < slices - slice ? capacity / slice_bytes : slices - slice);
            part.extent.z = volume ? count : 1;
            part.slice_count = volume ? 1 : count;
            queue.upload_texture(destination, {source.data + slice * slice_bytes, size_t(count * slice_bytes)}, part);
            slice += count;
            continue;
        }
        part.extent.z = 1;
        part.slice_count = 1;
        for (uint32 row = 0; row < rows;)
        {
            part.offset.y = region.offset.y + row * format.block_extent.y;
            if (row_bytes <= capacity)
            {
                const uint32 count = uint32(capacity / row_bytes < rows - row ? capacity / row_bytes : rows - row);
                part.extent.y = count * format.block_extent.y;
                if (part.extent.y > region.extent.y - row * format.block_extent.y) part.extent.y = region.extent.y - row * format.block_extent.y;
                queue.upload_texture(destination, {source.data + slice * slice_bytes + row * row_bytes, size_t(count * row_bytes)}, part);
                row += count;
                continue;
            }
            part.extent.y = format.block_extent.y;
            if (part.extent.y > region.extent.y - row * format.block_extent.y) part.extent.y = region.extent.y - row * format.block_extent.y;
            for (uint32 column = 0; column < columns;)
            {
                const uint32 count = uint32(capacity / format.bytes_per_block < columns - column ? capacity / format.bytes_per_block : columns - column);
                part.offset.x = region.offset.x + column * format.block_extent.x;
                part.extent.x = count * format.block_extent.x;
                if (part.extent.x > region.extent.x - column * format.block_extent.x) part.extent.x = region.extent.x - column * format.block_extent.x;
                queue.upload_texture(destination,
                    {source.data + slice * slice_bytes + row * row_bytes + column * format.bytes_per_block, size_t(count * format.bytes_per_block)}, part);
                column += count;
            }
            ++row;
        }
        ++slice;
    }
}

} // namespace gpu
