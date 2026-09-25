#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <assert.h>

namespace gpu::detail
{

inline uint32x3 texture_upload_tile(uint32x3 blocks, uint32x3 granularity, uint32 bytes_per_block, uint64 capacity) noexcept
{
    if (!granularity.x) granularity.x = blocks.x;
    if (!granularity.y) granularity.y = blocks.y;
    if (!granularity.z) granularity.z = blocks.z;
    const uint32x3 minimum{
        .x = granularity.x < blocks.x ? granularity.x : blocks.x,
        .y = granularity.y < blocks.y ? granularity.y : blocks.y,
        .z = granularity.z < blocks.z ? granularity.z : blocks.z,
    };
    assert(capacity >= uint64(minimum.x) * minimum.y * minimum.z * bytes_per_block);
    uint32x3 tile{};
    uint64 limit = capacity / (uint64(minimum.y) * minimum.z * bytes_per_block);
    tile.x = limit < blocks.x ? uint32(limit) : blocks.x;
    if (tile.x < blocks.x) tile.x -= tile.x % granularity.x;
    limit = capacity / (uint64(tile.x) * minimum.z * bytes_per_block);
    tile.y = limit < blocks.y ? uint32(limit) : blocks.y;
    if (tile.y < blocks.y) tile.y -= tile.y % granularity.y;
    limit = capacity / (uint64(tile.x) * tile.y * bytes_per_block);
    tile.z = limit < blocks.z ? uint32(limit) : blocks.z;
    if (tile.z < blocks.z) tile.z -= tile.z % granularity.z;
    return tile;
}

} // namespace gpu::detail
