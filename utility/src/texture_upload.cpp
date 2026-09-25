#include <NoGraphicsAPIUtility/texture_upload.hpp>
#include "texture_upload_tiles.hpp"

#include <assert.h>
#include <string.h>

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
    uint32x3 granularity = queue.state_.texture_granularity;
    if (!volume) granularity.z = 1;
    const uint32x3 tile = detail::texture_upload_tile({.x = columns, .y = rows, .z = slices}, granularity, format.bytes_per_block, capacity);

    for (uint32 slice = 0; slice < slices; slice += tile.z)
    {
        TextureCopyDesc part = region;
        part.base_slice = volume ? 0 : region.base_slice + slice;
        part.offset.z = volume ? region.offset.z + slice : 0;
        const uint32 part_slices = tile.z < slices - slice ? tile.z : slices - slice;
        part.extent.z = volume ? part_slices : 1;
        part.slice_count = volume ? 1 : part_slices;
        for (uint32 row = 0; row < rows; row += tile.y)
        {
            part.offset.y = region.offset.y + row * format.block_extent.y;
            const uint32 part_rows = tile.y < rows - row ? tile.y : rows - row;
            part.extent.y = part_rows * format.block_extent.y;
            if (part.extent.y > region.extent.y - row * format.block_extent.y) part.extent.y = region.extent.y - row * format.block_extent.y;
            for (uint32 column = 0; column < columns; column += tile.x)
            {
                const uint32 part_columns = tile.x < columns - column ? tile.x : columns - column;
                part.offset.x = region.offset.x + column * format.block_extent.x;
                part.extent.x = part_columns * format.block_extent.x;
                if (part.extent.x > region.extent.x - column * format.block_extent.x) part.extent.x = region.extent.x - column * format.block_extent.x;
                const uint64 packed_row_bytes = uint64(part_columns) * format.bytes_per_block;
                const uint64 packed_slice_bytes = packed_row_bytes * part_rows;
                const GpuCpuRange<byte> staging = queue.reserve(packed_slice_bytes * part_slices);
                if (part_columns == columns && part_rows == rows)
                {
                    memcpy(staging.cpu, source.data + slice * slice_bytes, size_t(staging.size));
                }
                else
                {
                    for (uint32 z = 0; z < part_slices; ++z)
                    {
                        const byte* input = source.data + (slice + z) * slice_bytes + row * row_bytes + column * format.bytes_per_block;
                        byte* output = staging.cpu + z * packed_slice_bytes;
                        if (part_columns == columns)
                        {
                            memcpy(output, input, size_t(packed_slice_bytes));
                        }
                        else
                        {
                            for (uint32 y = 0; y < part_rows; ++y)
                                memcpy(output + y * packed_row_bytes, input + y * row_bytes, size_t(packed_row_bytes));
                        }
                    }
                }
                copy_memory_to_texture(queue.begin(), gpu_range(staging), destination, part);
                ++queue.state_.operation_count;
            }
        }
    }
}

} // namespace gpu
