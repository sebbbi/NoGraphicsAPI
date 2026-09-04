#include <NoGraphicsAPI/math.hpp>

#if defined(_WIN32)
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <windows.h>
#	if defined(min) || defined(max)
#		error "math.hpp must suppress Win32 min/max macros"
#	endif
#endif

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <type_traits>

static_assert(std::is_same_v<uint8, std::uint8_t>);
static_assert(std::is_same_v<int8, std::int8_t>);
static_assert(std::is_same_v<uint16, std::uint16_t>);
static_assert(std::is_same_v<int16, std::int16_t>);
static_assert(std::is_same_v<uint32, std::uint32_t>);
static_assert(std::is_same_v<int32, std::int32_t>);
static_assert(std::is_same_v<uint64, std::uint64_t>);
static_assert(std::is_same_v<int64, std::int64_t>);

#define CHECK_POD_AGGREGATE(type) \
	static_assert(std::is_aggregate_v<type>); \
	static_assert(std::is_trivial_v<type>); \
	static_assert(std::is_standard_layout_v<type>); \
	static_assert(std::is_trivially_copyable_v<type>)

CHECK_POD_AGGREGATE(float2);
CHECK_POD_AGGREGATE(float3);
CHECK_POD_AGGREGATE(float4);
CHECK_POD_AGGREGATE(int2);
CHECK_POD_AGGREGATE(int3);
CHECK_POD_AGGREGATE(int4);
CHECK_POD_AGGREGATE(uint2);
CHECK_POD_AGGREGATE(uint3);
CHECK_POD_AGGREGATE(uint4);
CHECK_POD_AGGREGATE(float3x3);
CHECK_POD_AGGREGATE(float3x4);
CHECK_POD_AGGREGATE(float4x4);
CHECK_POD_AGGREGATE(quaternion);
CHECK_POD_AGGREGATE(float16_t2);
CHECK_POD_AGGREGATE(float16_t3);
CHECK_POD_AGGREGATE(float16_t4);
CHECK_POD_AGGREGATE(int16_t2);
CHECK_POD_AGGREGATE(int16_t3);
CHECK_POD_AGGREGATE(int16_t4);
CHECK_POD_AGGREGATE(uint16_t2);
CHECK_POD_AGGREGATE(uint16_t3);
CHECK_POD_AGGREGATE(uint16_t4);

#undef CHECK_POD_AGGREGATE

static_assert(sizeof(float2) == 8);
static_assert(sizeof(float3) == 12);
static_assert(sizeof(float4) == 16);
static_assert(sizeof(float3x3) == 36);
static_assert(sizeof(float3x4) == 48);
static_assert(sizeof(float4x4) == 64);
static_assert(sizeof(quaternion) == 16);
static_assert(alignof(float4) == alignof(float));
static_assert(alignof(float3x4) == alignof(float));
static_assert(alignof(float4x4) == alignof(float));
static_assert(std::is_same_v<decltype(float3x4::rows), float4[3]>);

constexpr float16_t half_one { .bits = 0x3c00u };
constexpr float3 floats { .x = 1.0f, .y = 2.0f, .z = 3.0f };
constexpr int16_t3 narrow_ints { .x = -1, .y = 2, .z = -3 };
constexpr uint4 uints { .x = 1u, .y = 2u, .z = 3u, .w = 4u };
constexpr float3x4 aggregate_matrix{
    .rows = {
        {.x = 1.0f, .y = 2.0f, .z = 3.0f, .w = 4.0f},
        {.x = 5.0f, .y = 6.0f, .z = 7.0f, .w = 8.0f},
        {.x = 9.0f, .y = 10.0f, .z = 11.0f, .w = 12.0f},
    },
};

static_assert(half_one.bits == 0x3c00u);
static_assert(floats[0] == 1.0f && floats[2] == 3.0f);
static_assert(narrow_ints.x == -1 && narrow_ints.z == -3);
static_assert(uints.x == 1u && uints.w == 4u);
static_assert(aggregate_matrix[2][3] == 12.0f);
static_assert(math::splat_float3(2.0f) == float3 { 2.0f, 2.0f, 2.0f });
static_assert(math::to_float4(float3 { 1.0f, 2.0f, 3.0f }, 4.0f) == float4 { 1.0f, 2.0f, 3.0f, 4.0f });
static_assert(float3 { 1.0f, 2.0f, 3.0f } + float3 { 4.0f, 5.0f, 6.0f } == float3 { 5.0f, 7.0f, 9.0f });
static_assert(math::dot(float3 { 1.0f, 2.0f, 3.0f }, float3 { 4.0f, 5.0f, 6.0f }) == 32.0f);
static_assert(math::cross(float3 { 1.0f, 0.0f, 0.0f }, float3 { 0.0f, 1.0f, 0.0f }) == float3 { 0.0f, 0.0f, 1.0f });
static_assert(math::identity3x3()[1][1] == 1.0f);
static_assert(math::identity3x4()[2][3] == 0.0f);
static_assert(math::identity4x4()[3][3] == 1.0f);
static_assert(math::pack_half_2x16(float2 { 1.0f, -2.0f }) == 0xc0003c00u);

constexpr float4x4 matrix_a { .rows = {
	{ 1.0f, 2.0f, 3.0f, 4.0f },
	{ 5.0f, 6.0f, 7.0f, 8.0f },
	{ 9.0f, 10.0f, 11.0f, 12.0f },
	{ 13.0f, 14.0f, 15.0f, 16.0f },
} };
constexpr float4x4 matrix_b { .rows = {
	{ 17.0f, 18.0f, 19.0f, 20.0f },
	{ 21.0f, 22.0f, 23.0f, 24.0f },
	{ 25.0f, 26.0f, 27.0f, 28.0f },
	{ 29.0f, 30.0f, 31.0f, 32.0f },
} };
constexpr float4x4 matrix_product { .rows = {
	{ 250.0f, 260.0f, 270.0f, 280.0f },
	{ 618.0f, 644.0f, 670.0f, 696.0f },
	{ 986.0f, 1028.0f, 1070.0f, 1112.0f },
	{ 1354.0f, 1412.0f, 1470.0f, 1528.0f },
} };
constexpr float4 matrix_vector { 2.0f, -1.0f, 3.0f, 4.0f };

static_assert(matrix_a * matrix_b == matrix_product);
static_assert(matrix_a * matrix_vector == float4 { 25.0f, 57.0f, 89.0f, 121.0f });
static_assert(matrix_vector * matrix_a == float4 { 76.0f, 84.0f, 92.0f, 100.0f });
static_assert(math::identity4x4() * matrix_a == matrix_a);
static_assert(matrix_a * math::identity4x4() == matrix_a);
static_assert((matrix_a * matrix_b) * matrix_vector == matrix_a * (matrix_b * matrix_vector));
static_assert(matrix_vector * (matrix_a * matrix_b) == (matrix_vector * matrix_a) * matrix_b);
static_assert(math::transpose(math::transpose(matrix_a)) == matrix_a);
static_assert(
	math::translation({ 4.0f, 5.0f, 6.0f }) *
		float4 { 1.0f, 2.0f, 3.0f, 1.0f } ==
	float4 { 5.0f, 7.0f, 9.0f, 1.0f });
static_assert(
	math::translation({ 4.0f, 5.0f, 6.0f }) *
		float4 { 1.0f, 2.0f, 3.0f, 0.0f } ==
	float4 { 1.0f, 2.0f, 3.0f, 0.0f });
static_assert(math::to_float3x4(matrix_a).rows[2] == matrix_a.rows[2]);
constexpr float3x4 translated_identity =
	math::set_translation(math::identity3x4(), { 4.0f, 5.0f, 6.0f });
static_assert(translated_identity == float3x4 { .rows = {
	{ 1.0f, 0.0f, 0.0f, 4.0f },
	{ 0.0f, 1.0f, 0.0f, 5.0f },
	{ 0.0f, 0.0f, 1.0f, 6.0f },
} });
static_assert(
	math::transform_point(translated_identity, { 1.0f, 2.0f, 3.0f }) ==
	float3 { 5.0f, 7.0f, 9.0f });
static_assert(
	math::transform_vector(translated_identity, { 1.0f, 2.0f, 3.0f }) ==
	float3 { 1.0f, 2.0f, 3.0f });

namespace {

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			std::fprintf(stderr, "math check failed at line %d: %s\n", __LINE__, #condition); \
			return false; \
		} \
	} while (false)

	bool nearly_equal(float a, float b, float epsilon = 1e-5f) noexcept {
		return std::abs(a - b) <= epsilon;
	}

	bool nearly_equal(float3 a, float3 b, float epsilon = 1e-5f) noexcept {
		return nearly_equal(a.x, b.x, epsilon) && nearly_equal(a.y, b.y, epsilon) && nearly_equal(a.z, b.z, epsilon);
	}

	bool nearly_equal(float4 a, float4 b, float epsilon = 1e-5f) noexcept {
		return nearly_equal(a.x, b.x, epsilon) && nearly_equal(a.y, b.y, epsilon) && nearly_equal(a.z, b.z, epsilon) && nearly_equal(a.w, b.w, epsilon);
	}

	bool check_vectors() noexcept {
		float3 value { 1.0f, -2.0f, 3.0f };
		value += float3 { 2.0f, 4.0f, 6.0f };
		value *= 0.5f;
		CHECK(value == (float3 { 1.5f, 1.0f, 4.5f }));
		CHECK(math::min(value, float3 { 2.0f, 0.0f, 5.0f }) == (float3 { 1.5f, 0.0f, 4.5f }));
		CHECK(math::max(value, float3 { 2.0f, 0.0f, 5.0f }) == (float3 { 2.0f, 1.0f, 5.0f }));
		CHECK(math::clamp(value, 1.25f, 4.0f) == (float3 { 1.5f, 1.25f, 4.0f }));
		CHECK(math::abs(float3 { -1.0f, 2.0f, -3.0f }) == (float3 { 1.0f, 2.0f, 3.0f }));
		CHECK(math::floor(float3 { 1.75f, -1.25f, 2.0f }) == (float3 { 1.0f, -2.0f, 2.0f }));
		CHECK(math::ceil(float3 { 1.25f, -1.75f, 2.0f }) == (float3 { 2.0f, -1.0f, 2.0f }));
		CHECK(math::round(float3 { 1.25f, -1.75f, 2.5f }) == (float3 { 1.0f, -2.0f, 3.0f }));
		CHECK(nearly_equal(math::rsqrt(4.0f), 0.5f));
		CHECK(nearly_equal(math::length(float3 { 2.0f, 3.0f, 6.0f }), 7.0f));
		CHECK(nearly_equal(math::normalize(float3 { 0.0f, 3.0f, 4.0f }), float3 { 0.0f, 0.6f, 0.8f }));
		CHECK(nearly_equal(math::lerp(float3 { 0.0f, 2.0f, 4.0f }, float3 { 2.0f, 4.0f, 6.0f }, 0.25f), float3 { 0.5f, 2.5f, 4.5f }));
		CHECK(nearly_equal(math::smoothstep(0.0f, 1.0f, 0.5f), 0.5f));

		const uint3 dimensions { 8u, 12u, 16u };
		CHECK(dimensions / 4u == (uint3 { 2u, 3u, 4u }));
		CHECK(math::to_int3(dimensions) == (int3 { 8, 12, 16 }));
		volatile float runtime_one = 1.0f;
		float4 wide { runtime_one, -2.0f, 4.0f, 8.0f };
		const float4 other { 2.0f, 4.0f, -1.0f, 0.5f };
		CHECK(wide + other == (float4 { 3.0f, 2.0f, 3.0f, 8.5f }));
		CHECK(wide - other == (float4 { -1.0f, -6.0f, 5.0f, 7.5f }));
		CHECK(wide * other == (float4 { 2.0f, -8.0f, -4.0f, 4.0f }));
		CHECK(wide / other == (float4 { 0.5f, -0.5f, -4.0f, 16.0f }));
		wide *= 2.0f;
		wide += float4 { 1.0f, 2.0f, 3.0f, 4.0f };
		CHECK(wide == (float4 { 3.0f, -2.0f, 11.0f, 20.0f }));
		CHECK(math::dot(float4 { runtime_one, -2.0f, 4.0f, 8.0f }, other) == -6.0f);
		return true;
	}

	bool check_matrices() noexcept {
		struct MatrixStorage {
			uint32 padding;
			float4x4 value;
		};
		static_assert(offsetof(MatrixStorage, value) == sizeof(uint32));

		volatile float runtime_one = 1.0f;
		MatrixStorage stored_a { .padding = 0u, .value = matrix_a };
		MatrixStorage stored_b { .padding = 0u, .value = matrix_b };
		stored_a.value.rows[0].x *= runtime_one;
		stored_b.value.rows[3].w *= runtime_one;
		CHECK(stored_a.value * stored_b.value == matrix_product);
		CHECK(stored_a.value * matrix_vector == (float4 { 25.0f, 57.0f, 89.0f, 121.0f }));
		CHECK(matrix_vector * stored_a.value == (float4 { 76.0f, 84.0f, 92.0f, 100.0f }));
		CHECK(math::transpose(math::transpose(stored_a.value)) == stored_a.value);

		const float4x4 transform = math::translation(float3 { 4.0f, 5.0f, 6.0f }) * math::rotation_y(math::half_pi) * math::scale(float3 { 2.0f, 3.0f, 4.0f });
		const float4 transformed = transform * float4 { 1.0f, 0.0f, 0.0f, 1.0f };
		CHECK(nearly_equal(transformed, float4 { 4.0f, 5.0f, 4.0f, 1.0f }));
		CHECK(nearly_equal(math::inverse(transform) * transformed, float4 { 1.0f, 0.0f, 0.0f, 1.0f }));

		const float3x4 affine{
        .rows = {
            {.x = 2.0f, .y = 0.0f, .z = 0.0f, .w = 4.0f},
            {.x = 0.0f, .y = 3.0f, .z = 0.0f, .w = 5.0f},
            {.x = 0.0f, .y = 0.0f, .z = 4.0f, .w = 6.0f},
        },
    };
		const float3 affine_point = math::transform_point(affine, { 1.0f, 2.0f, 3.0f });
		CHECK(affine_point == (float3 { 6.0f, 11.0f, 18.0f }));
		CHECK(nearly_equal(math::transform_point(math::inverse(affine), affine_point), float3 { 1.0f, 2.0f, 3.0f }));

		float3x4 affine_rhs { .rows = {
			{ 1.0f, 2.0f, 0.0f, 7.0f },
			{ 0.0f, 1.0f, 3.0f, 8.0f },
			{ 4.0f, 0.0f, 1.0f, 9.0f },
		} };
		affine_rhs.rows[0].x *= runtime_one;
		CHECK(affine * affine_rhs == (float3x4 { .rows = {
			{ 2.0f, 4.0f, 0.0f, 18.0f },
			{ 0.0f, 3.0f, 9.0f, 29.0f },
			{ 16.0f, 0.0f, 4.0f, 42.0f },
		} }));

		const float4x4 view = math::look_at_rh({ 0.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
		CHECK(nearly_equal(view * float4 { 0.0f, 0.0f, 5.0f, 1.0f }, float4 { 0.0f, 0.0f, 0.0f, 1.0f }));
		CHECK(nearly_equal(view * float4 { 0.0f, 0.0f, 0.0f, 1.0f }, float4 { 0.0f, 0.0f, -5.0f, 1.0f }));

		constexpr float physical_near = 0.25f;
		constexpr float physical_far = 100.0f;
		const auto projected_depth = [](float4x4 projection, float distance) {
			const float4 clip = projection * float4 { 0.0f, 0.0f, -distance, 1.0f };
			return clip.z / clip.w;
		};
		const float4x4 forward = math::perspective_rh_zo(math::half_pi, 1.0f, physical_near, physical_far);
		CHECK(nearly_equal(projected_depth(forward, physical_near), 0.0f));
		CHECK(nearly_equal(projected_depth(forward, physical_far), 1.0f));
		const float4x4 reverse = math::perspective_rh_zo(math::half_pi, 1.0f, physical_far, physical_near);
		CHECK(nearly_equal(projected_depth(reverse, physical_near), 1.0f));
		CHECK(nearly_equal(projected_depth(reverse, physical_far), 0.0f));
		return true;
	}

	bool check_quaternions() noexcept {
		const quaternion quarter_turn = math::axis_angle(math::half_pi, { 0.0f, 1.0f, 0.0f });
		CHECK(nearly_equal(quarter_turn * float3 { 1.0f, 0.0f, 0.0f }, float3 { 0.0f, 0.0f, -1.0f }));
		const float3x3 matrix = math::quaternion_to_matrix(quarter_turn);
		CHECK(nearly_equal(matrix * float3 { 1.0f, 0.0f, 0.0f }, float3 { 0.0f, 0.0f, -1.0f }));
		const quaternion halfway = math::slerp(math::identity_quaternion(), quarter_turn, 0.5f);
		const float diagonal = std::sqrt(0.5f);
		CHECK(nearly_equal(halfway * float3 { 1.0f, 0.0f, 0.0f }, float3 { diagonal, 0.0f, -diagonal }));
		CHECK(nearly_equal(math::inverse(quarter_turn) * (quarter_turn * float3 { 1.0f, 2.0f, 3.0f }), float3 { 1.0f, 2.0f, 3.0f }));
		return true;
	}

	bool check_half_packing() noexcept {
		CHECK(math::pack_half_2x16({ 0.0f, -0.0f }) == 0x80000000u);
		CHECK(math::float_to_half_bits(65504.0f) == 0x7bffu);
		CHECK(math::float_to_half_bits(0x1p-14f) == 0x0400u);
		CHECK(math::float_to_half_bits(0x1p-24f) == 0x0001u);
		CHECK(math::float_to_half_bits(1.0f + 0x1p-11f) == 0x3c00u);
		CHECK(math::float_to_half_bits(1.0f + 3.0f * 0x1p-11f) == 0x3c02u);
		CHECK(math::float_to_half_bits(std::numeric_limits<float>::infinity()) == 0x7c00u);
		CHECK(math::float_to_half_bits(-std::numeric_limits<float>::infinity()) == 0xfc00u);
		const uint16 nan = math::float_to_half_bits(std::numeric_limits<float>::quiet_NaN());
		CHECK((nan & 0x7c00u) == 0x7c00u && (nan & 0x03ffu) != 0u);
		return true;
	}

#undef CHECK

} // namespace

int main() {
	if (!check_vectors()) return 1;
	if (!check_matrices()) return 1;
	if (!check_quaternions()) return 1;
	if (!check_half_packing()) return 1;
	return 0;
}
