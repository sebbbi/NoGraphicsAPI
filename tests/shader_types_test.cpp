#include <NoGraphicsAPIUtility/shader_types.h>

template<typename T>
constexpr bool shared_pod =
    __is_aggregate(T) &&
    __is_trivial(T) &&
    __is_standard_layout(T) &&
    __is_trivially_copyable(T);

static_assert(sizeof(uint8) == 1 && static_cast<uint8>(-1) > 0);
static_assert(sizeof(int8) == 1 && static_cast<int8>(-1) < 0);
static_assert(sizeof(uint16) == 2 && static_cast<uint16>(-1) > 0);
static_assert(sizeof(int16) == 2 && static_cast<int16>(-1) < 0);
static_assert(sizeof(uint32) == 4 && static_cast<uint32>(-1) > 0);
static_assert(sizeof(int32) == 4 && static_cast<int32>(-1) < 0);
static_assert(sizeof(uint64) == 8 && static_cast<uint64>(-1) > 0);
static_assert(sizeof(int64) == 8 && static_cast<int64>(-1) < 0);

static_assert(shared_pod<float2>);
static_assert(shared_pod<float3>);
static_assert(shared_pod<float4>);
static_assert(shared_pod<int2>);
static_assert(shared_pod<int3>);
static_assert(shared_pod<int4>);
static_assert(shared_pod<uint2>);
static_assert(shared_pod<uint3>);
static_assert(shared_pod<uint4>);
static_assert(shared_pod<float3x3>);
static_assert(shared_pod<float3x4>);
static_assert(shared_pod<float4x4>);
static_assert(shared_pod<quaternion>);
static_assert(shared_pod<float16_t>);
static_assert(shared_pod<float16_t2>);
static_assert(shared_pod<float16_t3>);
static_assert(shared_pod<float16_t4>);
static_assert(shared_pod<int16_t2>);
static_assert(shared_pod<int16_t3>);
static_assert(shared_pod<int16_t4>);
static_assert(shared_pod<uint16_t2>);
static_assert(shared_pod<uint16_t3>);
static_assert(shared_pod<uint16_t4>);

static_assert(sizeof(float2) == 8);
static_assert(sizeof(float3) == 12);
static_assert(sizeof(float4) == 16);
static_assert(sizeof(float3x3) == 36);
static_assert(sizeof(float3x4) == 48);
static_assert(sizeof(float4x4) == 64);
static_assert(sizeof(quaternion) == 16);
static_assert(sizeof(float16_t) == 2);

constexpr float3 floats{.x = 1.0f, .y = 2.0f, .z = 3.0f};
constexpr float4x4 matrix{
    .rows = {
        {.x = 1.0f},
        {.y = 2.0f},
        {.z = 3.0f},
        {.w = 4.0f},
    },
};

static_assert(floats[0] == 1.0f && floats[2] == 3.0f);
static_assert(matrix[0][0] == 1.0f && matrix[3][3] == 4.0f);

int main()
{
    return 0;
}
