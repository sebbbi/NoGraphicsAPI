#include "../utility/src/texture_upload_tiles.hpp"

#include <stdio.h>

using namespace gpu;

static bool valid_axis(uint32 tile, uint32 size, uint32 alignment) noexcept
{
    return tile && tile <= size && (tile == size || (alignment && tile % alignment == 0));
}

int main()
{
    const uint32 sizes[]{1, 2, 3, 7, 8, 9, 15, 16, 17, 23, 31, 32, 37, 64, 137, 519};
    const uint32x3 granularities[]{
        {.x = 1, .y = 1, .z = 1}, {.x = 16, .y = 16, .z = 8}, {.x = 16, .y = 16, .z = 1}, {}, {.z = 1},
    };
    const uint32 block_bytes[]{1, 4, 8, 16};
    const uint64 capacities[]{16, 256, 272, 1024, 2048, 8192, 65536};
    uint32 cases = 0;
    for (uint32 width : sizes)
        for (uint32 height : sizes)
            for (uint32 depth : sizes)
                for (uint32x3 granularity : granularities)
                    for (uint32 bytes : block_bytes)
                        for (uint64 capacity : capacities)
                        {
                            const uint32 x = granularity.x && granularity.x < width ? granularity.x : width;
                            const uint32 y = granularity.y && granularity.y < height ? granularity.y : height;
                            const uint32 z = granularity.z && granularity.z < depth ? granularity.z : depth;
                            if (capacity < uint64(x) * y * z * bytes) continue;
                            const uint32x3 tile = detail::texture_upload_tile({.x = width, .y = height, .z = depth}, granularity, bytes, capacity);
                            if (!valid_axis(tile.x, width, granularity.x) || !valid_axis(tile.y, height, granularity.y) ||
                                !valid_axis(tile.z, depth, granularity.z) || uint64(tile.x) * tile.y * tile.z * bytes > capacity)
                            {
                                fprintf(stderr, "Invalid upload tile for %u x %u x %u blocks at %llu bytes.\n", width, height, depth, capacity);
                                return 1;
                            }
                            ++cases;
                        }
    printf("Verified %u upload tile plans, including coarse granularity and whole-mip-only transfers.\n", cases);
    return 0;
}
