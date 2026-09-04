#include <NoGraphicsAPI/shader_types.h>

#include <cstdint>
#include <type_traits>

template<typename T>
constexpr bool shared_pod =
    std::is_aggregate_v<T> &&
    std::is_trivial_v<T> &&
    std::is_standard_layout_v<T> &&
    std::is_trivially_copyable_v<T>;

static_assert(std::is_same_v<uint8, std::uint8_t>);
static_assert(std::is_same_v<int8, std::int8_t>);
static_assert(std::is_same_v<uint16, std::uint16_t>);
static_assert(std::is_same_v<int16, std::int16_t>);
static_assert(std::is_same_v<uint32, std::uint32_t>);
static_assert(std::is_same_v<int32, std::int32_t>);
static_assert(std::is_same_v<uint64, std::uint64_t>);
static_assert(std::is_same_v<int64, std::int64_t>);

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
