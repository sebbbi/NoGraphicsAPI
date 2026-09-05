#pragma once

#include <NoGraphicsAPIUtility/shader_types.h>

#if !defined(__SLANG__)

// The public math API owns min/max function names. Suppress Win32's legacy
// function-like macros whether Windows headers are included before or after us.
#	if defined(_WIN32) && !defined(NOMINMAX)
#		define NOMINMAX
#	endif
#	if defined(min)
#		undef min
#	endif
#	if defined(max)
#		undef max
#	endif

#	include <bit>
#	include <cassert>
#	include <cmath>
#	include <cstddef>
#	include <type_traits>

	static_assert(sizeof(void*) == 8, "NoGraphicsAPI math requires a 64-bit pointer ABI");

#	if defined(__MINGW32__)
#		error "NoGraphicsAPI math does not support MinGW"
#	endif
#	if defined(_M_ARM64EC) || (!defined(_M_X64) && !defined(__x86_64__))
#		error "NoGraphicsAPI math requires an x86-64 target"
#	endif
#	if !defined(__AVX2__)
#		error "NoGraphicsAPI math requires AVX2"
#	endif
#	if (defined(__clang__) || defined(__GNUC__)) && !defined(__FMA__)
#		error "NoGraphicsAPI math requires FMA"
#	endif

#	include <immintrin.h>

namespace NoGraphicsAPI_math_detail {

	inline __m128 load(const float4& value) noexcept {
		return _mm_loadu_ps(&value.x);
	}

	inline float4 store(__m128 value) noexcept {
		float4 result;
		_mm_storeu_ps(&result.x, value);
		return result;
	}

	inline __m128 linear_combination(__m128 coefficients,
	                                 __m128 row0,
	                                 __m128 row1,
	                                 __m128 row2,
	                                 __m128 row3) noexcept {
		__m128 result = _mm_mul_ps(_mm_permute_ps(coefficients, 0x00), row0);
		result = _mm_fmadd_ps(_mm_permute_ps(coefficients, 0x55), row1, result);
		result = _mm_fmadd_ps(_mm_permute_ps(coefficients, 0xaa), row2, result);
		return _mm_fmadd_ps(_mm_permute_ps(coefficients, 0xff), row3, result);
	}

	inline __m128 linear_combination(const float4& coefficients,
	                                 const float4& row0,
	                                 const float4& row1,
	                                 const float4& row2,
	                                 const float4& row3) noexcept {
		return linear_combination(
			load(coefficients), load(row0), load(row1), load(row2), load(row3));
	}

	inline __m256 linear_combination(__m256 coefficients,
	                                 __m256 row0,
	                                 __m256 row1,
	                                 __m256 row2,
	                                 __m256 row3) noexcept {
		__m256 result = _mm256_mul_ps(_mm256_permute_ps(coefficients, 0x00), row0);
		result = _mm256_fmadd_ps(_mm256_permute_ps(coefficients, 0x55), row1, result);
		result = _mm256_fmadd_ps(_mm256_permute_ps(coefficients, 0xaa), row2, result);
		return _mm256_fmadd_ps(_mm256_permute_ps(coefficients, 0xff), row3, result);
	}

	inline float4x4 mul(const float4x4& lhs, const float4x4& rhs) noexcept {
		const __m256 rhs01 = _mm256_loadu_ps(&rhs.rows[0].x);
		const __m256 rhs23 = _mm256_loadu_ps(&rhs.rows[2].x);
		const __m256 row0 = _mm256_permute2f128_ps(rhs01, rhs01, 0x00);
		const __m256 row1 = _mm256_permute2f128_ps(rhs01, rhs01, 0x11);
		const __m256 row2 = _mm256_permute2f128_ps(rhs23, rhs23, 0x00);
		const __m256 row3 = _mm256_permute2f128_ps(rhs23, rhs23, 0x11);
		float4x4 result;
		_mm256_storeu_ps(&result.rows[0].x, linear_combination(_mm256_loadu_ps(&lhs.rows[0].x), row0, row1, row2, row3));
		_mm256_storeu_ps(&result.rows[2].x, linear_combination(_mm256_loadu_ps(&lhs.rows[2].x), row0, row1, row2, row3));
		return result;
	}

	inline float3x4 mul(const float3x4& lhs, const float3x4& rhs) noexcept {
		const __m256 rhs01 = _mm256_loadu_ps(&rhs.rows[0].x);
		const __m128 rhs2 = load(rhs.rows[2]);
		const __m256 row0 = _mm256_permute2f128_ps(rhs01, rhs01, 0x00);
		const __m256 row1 = _mm256_permute2f128_ps(rhs01, rhs01, 0x11);
		const __m256 row2 = _mm256_insertf128_ps(_mm256_castps128_ps256(rhs2), rhs2, 1);
		const __m256 lhs01 = _mm256_loadu_ps(&lhs.rows[0].x);
		__m256 result01 = _mm256_blend_ps(_mm256_setzero_ps(), lhs01, 0x88);
		result01 = _mm256_fmadd_ps(_mm256_permute_ps(lhs01, 0x00), row0, result01);
		result01 = _mm256_fmadd_ps(_mm256_permute_ps(lhs01, 0x55), row1, result01);
		result01 = _mm256_fmadd_ps(_mm256_permute_ps(lhs01, 0xaa), row2, result01);

		const __m128 lhs2 = load(lhs.rows[2]);
		__m128 result2 = _mm_blend_ps(_mm_setzero_ps(), lhs2, 0x8);
		result2 = _mm_fmadd_ps(_mm_permute_ps(lhs2, 0x00), load(rhs.rows[0]), result2);
		result2 = _mm_fmadd_ps(_mm_permute_ps(lhs2, 0x55), load(rhs.rows[1]), result2);
		result2 = _mm_fmadd_ps(_mm_permute_ps(lhs2, 0xaa), rhs2, result2);

		float3x4 result;
		_mm256_storeu_ps(&result.rows[0].x, result01);
		_mm_storeu_ps(&result.rows[2].x, result2);
		return result;
	}

	inline float4 mul(const float4x4& matrix, const float4& vector) noexcept {
		__m128 row0 = load(matrix.rows[0]);
		__m128 row1 = load(matrix.rows[1]);
		__m128 row2 = load(matrix.rows[2]);
		__m128 row3 = load(matrix.rows[3]);
		_MM_TRANSPOSE4_PS(row0, row1, row2, row3);
		return store(linear_combination(load(vector), row0, row1, row2, row3));
	}

	inline float3 mul(const float3x4& matrix, const float4& vector) noexcept {
		__m128 row0 = load(matrix.rows[0]);
		__m128 row1 = load(matrix.rows[1]);
		__m128 row2 = load(matrix.rows[2]);
		__m128 row3 = _mm_setzero_ps();
		_MM_TRANSPOSE4_PS(row0, row1, row2, row3);
		const float4 result = store(linear_combination(load(vector), row0, row1, row2, row3));
		return { result.x, result.y, result.z };
	}

	inline float4 mul(const float4& vector, const float4x4& matrix) noexcept {
		return store(linear_combination(vector, matrix.rows[0], matrix.rows[1], matrix.rows[2], matrix.rows[3]));
	}

	inline float4x4 transpose(const float4x4& matrix) noexcept {
		__m128 row0 = load(matrix.rows[0]);
		__m128 row1 = load(matrix.rows[1]);
		__m128 row2 = load(matrix.rows[2]);
		__m128 row3 = load(matrix.rows[3]);
		_MM_TRANSPOSE4_PS(row0, row1, row2, row3);
		return { { store(row0), store(row1), store(row2), store(row3) } };
	}

} // namespace NoGraphicsAPI_math_detail

#	define NOGRAPHICSAPI_VECTOR_EQUAL(type, count) \
		constexpr bool operator==(type lhs, type rhs) noexcept { \
			for (std::size_t index = 0; index != count; ++index) \
				if (lhs[index] != rhs[index]) return false; \
			return true; \
		} \
		constexpr bool operator!=(type lhs, type rhs) noexcept { \
			return !(lhs == rhs); \
		}

NOGRAPHICSAPI_VECTOR_EQUAL(float2, 2)
NOGRAPHICSAPI_VECTOR_EQUAL(float3, 3)
NOGRAPHICSAPI_VECTOR_EQUAL(float4, 4)
NOGRAPHICSAPI_VECTOR_EQUAL(int2, 2)
NOGRAPHICSAPI_VECTOR_EQUAL(int3, 3)
NOGRAPHICSAPI_VECTOR_EQUAL(int4, 4)
NOGRAPHICSAPI_VECTOR_EQUAL(uint2, 2)
NOGRAPHICSAPI_VECTOR_EQUAL(uint3, 3)
NOGRAPHICSAPI_VECTOR_EQUAL(uint4, 4)

#	undef NOGRAPHICSAPI_VECTOR_EQUAL

#	define NOGRAPHICSAPI_VECTOR_OPERATORS(type, scalar_type, count) \
		constexpr type operator+(type lhs, type rhs) noexcept { \
			type result {}; \
			for (std::size_t i = 0; i != count; ++i) \
				result[i] = lhs[i] + rhs[i]; \
			return result; \
		} \
		constexpr type operator-(type lhs, type rhs) noexcept { \
			type result {}; \
			for (std::size_t i = 0; i != count; ++i) \
				result[i] = lhs[i] - rhs[i]; \
			return result; \
		} \
		constexpr type operator*(type lhs, type rhs) noexcept { \
			type result {}; \
			for (std::size_t i = 0; i != count; ++i) \
				result[i] = lhs[i] * rhs[i]; \
			return result; \
		} \
		constexpr type operator/(type lhs, type rhs) noexcept { \
			type result {}; \
			for (std::size_t i = 0; i != count; ++i) \
				result[i] = lhs[i] / rhs[i]; \
			return result; \
		} \
		constexpr type operator+(type value, scalar_type scalar) noexcept { \
			type result {}; \
			for (std::size_t i = 0; i != count; ++i) \
				result[i] = value[i] + scalar; \
			return result; \
		} \
		constexpr type operator-(type value, scalar_type scalar) noexcept { \
			type result {}; \
			for (std::size_t i = 0; i != count; ++i) \
				result[i] = value[i] - scalar; \
			return result; \
		} \
		constexpr type operator*(type value, scalar_type scalar) noexcept { \
			type result {}; \
			for (std::size_t i = 0; i != count; ++i) \
				result[i] = value[i] * scalar; \
			return result; \
		} \
		constexpr type operator/(type value, scalar_type scalar) noexcept { \
			type result {}; \
			for (std::size_t i = 0; i != count; ++i) \
				result[i] = value[i] / scalar; \
			return result; \
		} \
		constexpr type operator+(scalar_type scalar, type value) noexcept { \
			return value + scalar; \
		} \
		constexpr type operator*(scalar_type scalar, type value) noexcept { \
			return value * scalar; \
		} \
		constexpr type& operator+=(type& lhs, type rhs) noexcept { \
			lhs = lhs + rhs; \
			return lhs; \
		} \
		constexpr type& operator-=(type& lhs, type rhs) noexcept { \
			lhs = lhs - rhs; \
			return lhs; \
		} \
		constexpr type& operator*=(type& lhs, scalar_type rhs) noexcept { \
			lhs = lhs * rhs; \
			return lhs; \
		} \
		constexpr type& operator/=(type& lhs, scalar_type rhs) noexcept { \
			lhs = lhs / rhs; \
			return lhs; \
		}

NOGRAPHICSAPI_VECTOR_OPERATORS(float2, float, 2)
NOGRAPHICSAPI_VECTOR_OPERATORS(int2, int32, 2)
NOGRAPHICSAPI_VECTOR_OPERATORS(int3, int32, 3)
NOGRAPHICSAPI_VECTOR_OPERATORS(int4, int32, 4)
NOGRAPHICSAPI_VECTOR_OPERATORS(uint2, uint32, 2)
NOGRAPHICSAPI_VECTOR_OPERATORS(uint3, uint32, 3)
NOGRAPHICSAPI_VECTOR_OPERATORS(uint4, uint32, 4)

#	undef NOGRAPHICSAPI_VECTOR_OPERATORS

#	define NOGRAPHICSAPI_FLOAT3_BINARY_OPERATOR(symbol) \
		constexpr float3 operator symbol(float3 lhs, float3 rhs) noexcept { \
			return { lhs.x symbol rhs.x, lhs.y symbol rhs.y, lhs.z symbol rhs.z }; \
		}

NOGRAPHICSAPI_FLOAT3_BINARY_OPERATOR(+)
NOGRAPHICSAPI_FLOAT3_BINARY_OPERATOR(-)
NOGRAPHICSAPI_FLOAT3_BINARY_OPERATOR(*)
NOGRAPHICSAPI_FLOAT3_BINARY_OPERATOR(/)

#	undef NOGRAPHICSAPI_FLOAT3_BINARY_OPERATOR

#	define NOGRAPHICSAPI_FLOAT3_SCALAR_OPERATOR(symbol) \
		constexpr float3 operator symbol(float3 value, float scalar) noexcept { \
			return { value.x symbol scalar, value.y symbol scalar, value.z symbol scalar }; \
		}

NOGRAPHICSAPI_FLOAT3_SCALAR_OPERATOR(+)
NOGRAPHICSAPI_FLOAT3_SCALAR_OPERATOR(-)
NOGRAPHICSAPI_FLOAT3_SCALAR_OPERATOR(*)
NOGRAPHICSAPI_FLOAT3_SCALAR_OPERATOR(/)

#	undef NOGRAPHICSAPI_FLOAT3_SCALAR_OPERATOR

constexpr float3 operator+(float scalar, float3 value) noexcept {
	return value + scalar;
}
constexpr float3 operator*(float scalar, float3 value) noexcept {
	return value * scalar;
}
constexpr float3& operator+=(float3& lhs, float3 rhs) noexcept {
	lhs = lhs + rhs;
	return lhs;
}
constexpr float3& operator-=(float3& lhs, float3 rhs) noexcept {
	lhs = lhs - rhs;
	return lhs;
}
constexpr float3& operator*=(float3& lhs, float rhs) noexcept {
	lhs = lhs * rhs;
	return lhs;
}
constexpr float3& operator/=(float3& lhs, float rhs) noexcept {
	lhs = lhs / rhs;
	return lhs;
}

#	define NOGRAPHICSAPI_FLOAT4_BINARY_OPERATOR(symbol, intrinsic) \
		constexpr float4 operator symbol(float4 lhs, float4 rhs) noexcept { \
			if (!std::is_constant_evaluated()) \
				return NoGraphicsAPI_math_detail::store(intrinsic(NoGraphicsAPI_math_detail::load(lhs), NoGraphicsAPI_math_detail::load(rhs))); \
			return { lhs.x symbol rhs.x, lhs.y symbol rhs.y, lhs.z symbol rhs.z, lhs.w symbol rhs.w }; \
		}

NOGRAPHICSAPI_FLOAT4_BINARY_OPERATOR(+, _mm_add_ps)
NOGRAPHICSAPI_FLOAT4_BINARY_OPERATOR(-, _mm_sub_ps)
NOGRAPHICSAPI_FLOAT4_BINARY_OPERATOR(*, _mm_mul_ps)
NOGRAPHICSAPI_FLOAT4_BINARY_OPERATOR(/, _mm_div_ps)

#	undef NOGRAPHICSAPI_FLOAT4_BINARY_OPERATOR

#	define NOGRAPHICSAPI_FLOAT4_SCALAR_OPERATOR(symbol, intrinsic) \
		constexpr float4 operator symbol(float4 value, float scalar) noexcept { \
			if (!std::is_constant_evaluated()) \
				return NoGraphicsAPI_math_detail::store(intrinsic(NoGraphicsAPI_math_detail::load(value), _mm_set1_ps(scalar))); \
			return { value.x symbol scalar, value.y symbol scalar, value.z symbol scalar, value.w symbol scalar }; \
		}

NOGRAPHICSAPI_FLOAT4_SCALAR_OPERATOR(+, _mm_add_ps)
NOGRAPHICSAPI_FLOAT4_SCALAR_OPERATOR(-, _mm_sub_ps)
NOGRAPHICSAPI_FLOAT4_SCALAR_OPERATOR(*, _mm_mul_ps)
NOGRAPHICSAPI_FLOAT4_SCALAR_OPERATOR(/, _mm_div_ps)

#	undef NOGRAPHICSAPI_FLOAT4_SCALAR_OPERATOR

constexpr float4 operator+(float scalar, float4 value) noexcept {
	return value + scalar;
}
constexpr float4 operator*(float scalar, float4 value) noexcept {
	return value * scalar;
}
constexpr float4& operator+=(float4& lhs, float4 rhs) noexcept {
	lhs = lhs + rhs;
	return lhs;
}
constexpr float4& operator-=(float4& lhs, float4 rhs) noexcept {
	lhs = lhs - rhs;
	return lhs;
}
constexpr float4& operator*=(float4& lhs, float rhs) noexcept {
	lhs = lhs * rhs;
	return lhs;
}
constexpr float4& operator/=(float4& lhs, float rhs) noexcept {
	lhs = lhs / rhs;
	return lhs;
}

constexpr float2 operator-(float2 value) noexcept {
	return { -value.x, -value.y };
}
constexpr float3 operator-(float3 value) noexcept {
	return { -value.x, -value.y, -value.z };
}
constexpr float4 operator-(float4 value) noexcept {
	return { -value.x, -value.y, -value.z, -value.w };
}
constexpr int2 operator-(int2 value) noexcept {
	return { -value.x, -value.y };
}
constexpr int3 operator-(int3 value) noexcept {
	return { -value.x, -value.y, -value.z };
}
constexpr int4 operator-(int4 value) noexcept {
	return { -value.x, -value.y, -value.z, -value.w };
}

constexpr quaternion operator-(quaternion value) noexcept {
	return { -value.x, -value.y, -value.z, -value.w };
}
constexpr quaternion operator+(quaternion lhs, quaternion rhs) noexcept {
	return { lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z, lhs.w + rhs.w };
}
constexpr quaternion operator-(quaternion lhs, quaternion rhs) noexcept {
	return { lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z, lhs.w - rhs.w };
}
constexpr quaternion operator*(quaternion value, float scalar) noexcept {
	return { value.x * scalar, value.y * scalar, value.z * scalar, value.w * scalar };
}
constexpr quaternion operator*(float scalar, quaternion value) noexcept {
	return value * scalar;
}
constexpr quaternion operator/(quaternion value, float scalar) noexcept {
	return { value.x / scalar, value.y / scalar, value.z / scalar, value.w / scalar };
}
constexpr quaternion& operator+=(quaternion& lhs, quaternion rhs) noexcept {
	lhs = lhs + rhs;
	return lhs;
}
constexpr quaternion& operator-=(quaternion& lhs, quaternion rhs) noexcept {
	lhs = lhs - rhs;
	return lhs;
}
constexpr quaternion& operator*=(quaternion& lhs, float rhs) noexcept {
	lhs = lhs * rhs;
	return lhs;
}
constexpr quaternion& operator/=(quaternion& lhs, float rhs) noexcept {
	lhs = lhs / rhs;
	return lhs;
}
constexpr quaternion operator*(quaternion lhs, quaternion rhs) noexcept {
	return {
		lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
		lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
		lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
		lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
	};
}

namespace math {

	inline constexpr float pi = 3.14159265358979323846f;
	inline constexpr float half_pi = pi * 0.5f;
	inline constexpr float two_pi = pi * 2.0f;

	constexpr float2 splat_float2(float value) noexcept {
		return { value, value };
	}
	constexpr float3 splat_float3(float value) noexcept {
		return { value, value, value };
	}
	constexpr float4 splat_float4(float value) noexcept {
		return { value, value, value, value };
	}
	constexpr int2 splat_int2(int32 value) noexcept {
		return { value, value };
	}
	constexpr int3 splat_int3(int32 value) noexcept {
		return { value, value, value };
	}
	constexpr int4 splat_int4(int32 value) noexcept {
		return { value, value, value, value };
	}
	constexpr uint2 splat_uint2(uint32 value) noexcept {
		return { value, value };
	}
	constexpr uint3 splat_uint3(uint32 value) noexcept {
		return { value, value, value };
	}
	constexpr uint4 splat_uint4(uint32 value) noexcept {
		return { value, value, value, value };
	}

	constexpr float2 to_float2(float3 value) noexcept {
		return { value.x, value.y };
	}
	constexpr float2 to_float2(float4 value) noexcept {
		return { value.x, value.y };
	}
	constexpr float2 to_float2(int2 value) noexcept {
		return { static_cast<float>(value.x), static_cast<float>(value.y) };
	}
	constexpr float2 to_float2(uint2 value) noexcept {
		return { static_cast<float>(value.x), static_cast<float>(value.y) };
	}
	constexpr float3 to_float3(float2 value, float z = 0.0f) noexcept {
		return { value.x, value.y, z };
	}
	constexpr float3 to_float3(float4 value) noexcept {
		return { value.x, value.y, value.z };
	}
	constexpr float3 to_float3(int3 value) noexcept {
		return { static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z) };
	}
	constexpr float3 to_float3(uint3 value) noexcept {
		return { static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z) };
	}
	constexpr float4 to_float4(float3 value, float w = 0.0f) noexcept {
		return { value.x, value.y, value.z, w };
	}
	constexpr float4 to_float4(int4 value) noexcept {
		return { static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z), static_cast<float>(value.w) };
	}
	constexpr float4 to_float4(uint4 value) noexcept {
		return { static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z), static_cast<float>(value.w) };
	}
	constexpr float4 to_float4(quaternion value) noexcept {
		return { value.x, value.y, value.z, value.w };
	}
	constexpr int2 to_int2(float2 value) noexcept {
		return { static_cast<int32>(value.x), static_cast<int32>(value.y) };
	}
	constexpr int2 to_int2(uint2 value) noexcept {
		return { static_cast<int32>(value.x), static_cast<int32>(value.y) };
	}
	constexpr int3 to_int3(float3 value) noexcept {
		return { static_cast<int32>(value.x), static_cast<int32>(value.y), static_cast<int32>(value.z) };
	}
	constexpr int3 to_int3(uint3 value) noexcept {
		return { static_cast<int32>(value.x), static_cast<int32>(value.y), static_cast<int32>(value.z) };
	}
	constexpr int4 to_int4(float4 value) noexcept {
		return { static_cast<int32>(value.x), static_cast<int32>(value.y), static_cast<int32>(value.z), static_cast<int32>(value.w) };
	}
	constexpr int4 to_int4(uint4 value) noexcept {
		return { static_cast<int32>(value.x), static_cast<int32>(value.y), static_cast<int32>(value.z), static_cast<int32>(value.w) };
	}
	constexpr uint2 to_uint2(float2 value) noexcept {
		return { static_cast<uint32>(value.x), static_cast<uint32>(value.y) };
	}
	constexpr uint2 to_uint2(int2 value) noexcept {
		return { static_cast<uint32>(value.x), static_cast<uint32>(value.y) };
	}
	constexpr uint3 to_uint3(float3 value) noexcept {
		return { static_cast<uint32>(value.x), static_cast<uint32>(value.y), static_cast<uint32>(value.z) };
	}
	constexpr uint3 to_uint3(int3 value) noexcept {
		return { static_cast<uint32>(value.x), static_cast<uint32>(value.y), static_cast<uint32>(value.z) };
	}
	constexpr uint4 to_uint4(float4 value) noexcept {
		return { static_cast<uint32>(value.x), static_cast<uint32>(value.y), static_cast<uint32>(value.z), static_cast<uint32>(value.w) };
	}
	constexpr uint4 to_uint4(int4 value) noexcept {
		return { static_cast<uint32>(value.x), static_cast<uint32>(value.y), static_cast<uint32>(value.z), static_cast<uint32>(value.w) };
	}
	constexpr quaternion to_quaternion(float4 value) noexcept {
		return { value.x, value.y, value.z, value.w };
	}

	constexpr float min(float lhs, float rhs) noexcept {
		return lhs < rhs ? lhs : rhs;
	}
	constexpr int32 min(int32 lhs, int32 rhs) noexcept {
		return lhs < rhs ? lhs : rhs;
	}
	constexpr uint32 min(uint32 lhs, uint32 rhs) noexcept {
		return lhs < rhs ? lhs : rhs;
	}
	constexpr float max(float lhs, float rhs) noexcept {
		return lhs > rhs ? lhs : rhs;
	}
	constexpr int32 max(int32 lhs, int32 rhs) noexcept {
		return lhs > rhs ? lhs : rhs;
	}
	constexpr uint32 max(uint32 lhs, uint32 rhs) noexcept {
		return lhs > rhs ? lhs : rhs;
	}
	constexpr float clamp(float value, float low, float high) noexcept {
		return min(max(value, low), high);
	}
	constexpr int32 clamp(int32 value, int32 low, int32 high) noexcept {
		return min(max(value, low), high);
	}
	constexpr uint32 clamp(uint32 value, uint32 low, uint32 high) noexcept {
		return min(max(value, low), high);
	}

#	define NOGRAPHICSAPI_COMPONENT_MATH(type, scalar_type, count) \
		constexpr type min(type lhs, type rhs) noexcept { \
			type result {}; \
			for (std::size_t i = 0; i != count; ++i) \
				result[i] = min(lhs[i], rhs[i]); \
			return result; \
		} \
		constexpr type max(type lhs, type rhs) noexcept { \
			type result {}; \
			for (std::size_t i = 0; i != count; ++i) \
				result[i] = max(lhs[i], rhs[i]); \
			return result; \
		} \
		constexpr type clamp(type value, type low, type high) noexcept { \
			return min(max(value, low), high); \
		} \
		constexpr type clamp(type value, scalar_type low, scalar_type high) noexcept { \
			type result {}; \
			for (std::size_t i = 0; i != count; ++i) \
				result[i] = clamp(value[i], low, high); \
			return result; \
		}

	NOGRAPHICSAPI_COMPONENT_MATH(float2, float, 2)
	NOGRAPHICSAPI_COMPONENT_MATH(float3, float, 3)
	NOGRAPHICSAPI_COMPONENT_MATH(float4, float, 4)
	NOGRAPHICSAPI_COMPONENT_MATH(int2, int32, 2)
	NOGRAPHICSAPI_COMPONENT_MATH(int3, int32, 3)
	NOGRAPHICSAPI_COMPONENT_MATH(int4, int32, 4)
	NOGRAPHICSAPI_COMPONENT_MATH(uint2, uint32, 2)
	NOGRAPHICSAPI_COMPONENT_MATH(uint3, uint32, 3)
	NOGRAPHICSAPI_COMPONENT_MATH(uint4, uint32, 4)
#	undef NOGRAPHICSAPI_COMPONENT_MATH

	constexpr float abs(float value) noexcept {
		return value < 0.0f ? -value : value;
	}
	constexpr int32 abs(int32 value) noexcept {
		return value < 0 ? -value : value;
	}
	constexpr float2 abs(float2 value) noexcept {
		return { abs(value.x), abs(value.y) };
	}
	constexpr float3 abs(float3 value) noexcept {
		return { abs(value.x), abs(value.y), abs(value.z) };
	}
	constexpr float4 abs(float4 value) noexcept {
		return { abs(value.x), abs(value.y), abs(value.z), abs(value.w) };
	}
	constexpr int2 abs(int2 value) noexcept {
		return { abs(value.x), abs(value.y) };
	}
	constexpr int3 abs(int3 value) noexcept {
		return { abs(value.x), abs(value.y), abs(value.z) };
	}
	constexpr int4 abs(int4 value) noexcept {
		return { abs(value.x), abs(value.y), abs(value.z), abs(value.w) };
	}

	inline float2 floor(float2 value) noexcept {
		return { std::floor(value.x), std::floor(value.y) };
	}
	inline float3 floor(float3 value) noexcept {
		return { std::floor(value.x), std::floor(value.y), std::floor(value.z) };
	}
	inline float4 floor(float4 value) noexcept {
		return { std::floor(value.x), std::floor(value.y), std::floor(value.z), std::floor(value.w) };
	}
	inline float2 ceil(float2 value) noexcept {
		return { std::ceil(value.x), std::ceil(value.y) };
	}
	inline float3 ceil(float3 value) noexcept {
		return { std::ceil(value.x), std::ceil(value.y), std::ceil(value.z) };
	}
	inline float4 ceil(float4 value) noexcept {
		return { std::ceil(value.x), std::ceil(value.y), std::ceil(value.z), std::ceil(value.w) };
	}
	inline float2 round(float2 value) noexcept {
		return { std::round(value.x), std::round(value.y) };
	}
	inline float3 round(float3 value) noexcept {
		return { std::round(value.x), std::round(value.y), std::round(value.z) };
	}
	inline float4 round(float4 value) noexcept {
		return { std::round(value.x), std::round(value.y), std::round(value.z), std::round(value.w) };
	}

	constexpr float dot(float2 lhs, float2 rhs) noexcept {
		return lhs.x * rhs.x + lhs.y * rhs.y;
	}
	constexpr float dot(float3 lhs, float3 rhs) noexcept {
		return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
	}
	constexpr float dot(float4 lhs, float4 rhs) noexcept {
		if (!std::is_constant_evaluated())
			return _mm_cvtss_f32(_mm_dp_ps(NoGraphicsAPI_math_detail::load(lhs), NoGraphicsAPI_math_detail::load(rhs), 0xf1));
		return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z + lhs.w * rhs.w;
	}
	constexpr float dot(quaternion lhs, quaternion rhs) noexcept {
		return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z + lhs.w * rhs.w;
	}
	constexpr float3 cross(float3 lhs, float3 rhs) noexcept {
		return { lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z, lhs.x * rhs.y - lhs.y * rhs.x };
	}
	inline float rsqrt(float value) noexcept {
		return 1.0f / std::sqrt(value);
	}
	inline float length(float2 value) noexcept {
		return std::sqrt(dot(value, value));
	}
	inline float length(float3 value) noexcept {
		return std::sqrt(dot(value, value));
	}
	inline float length(float4 value) noexcept {
		return std::sqrt(dot(value, value));
	}
	inline float length(quaternion value) noexcept {
		return std::sqrt(dot(value, value));
	}
	inline float2 normalize(float2 value) noexcept {
		return value * rsqrt(dot(value, value));
	}
	inline float3 normalize(float3 value) noexcept {
		return value * rsqrt(dot(value, value));
	}
	inline float4 normalize(float4 value) noexcept {
		return value * rsqrt(dot(value, value));
	}
	inline quaternion normalize(quaternion value) noexcept {
		return value * rsqrt(dot(value, value));
	}

	constexpr float lerp(float a, float b, float t) noexcept {
		return a + (b - a) * t;
	}
	constexpr float2 lerp(float2 a, float2 b, float t) noexcept {
		return a + (b - a) * t;
	}
	constexpr float3 lerp(float3 a, float3 b, float t) noexcept {
		return a + (b - a) * t;
	}
	constexpr float4 lerp(float4 a, float4 b, float t) noexcept {
		return a + (b - a) * t;
	}
	constexpr float smoothstep(float edge0, float edge1, float value) noexcept {
		const float t = clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}
	constexpr float2 smoothstep(float edge0, float edge1, float2 value) noexcept {
		return { smoothstep(edge0, edge1, value.x), smoothstep(edge0, edge1, value.y) };
	}
	constexpr float3 smoothstep(float edge0, float edge1, float3 value) noexcept {
		return { smoothstep(edge0, edge1, value.x), smoothstep(edge0, edge1, value.y), smoothstep(edge0, edge1, value.z) };
	}
	constexpr float4 smoothstep(float edge0, float edge1, float4 value) noexcept {
		return { smoothstep(edge0, edge1, value.x), smoothstep(edge0, edge1, value.y), smoothstep(edge0, edge1, value.z), smoothstep(edge0, edge1, value.w) };
	}

	constexpr float3x3 identity3x3() noexcept {
		return { { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
	}

	constexpr float3x4 identity3x4() noexcept {
		return { { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } } };
	}

	constexpr float4x4 identity4x4() noexcept {
		return { { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } } };
	}

	constexpr float3x4 to_float3x4(const float4x4& matrix) noexcept {
		return { { matrix.rows[0], matrix.rows[1], matrix.rows[2] } };
	}

	constexpr float3x4 set_translation(const float3x4& transform, float3 value) noexcept {
		return { { { transform.rows[0].x, transform.rows[0].y, transform.rows[0].z, value.x },
				   { transform.rows[1].x, transform.rows[1].y, transform.rows[1].z, value.y },
				   { transform.rows[2].x, transform.rows[2].y, transform.rows[2].z, value.z } } };
	}

	constexpr float3 mul(const float3x3& matrix, float3 vector) noexcept {
		return { dot(matrix.rows[0], vector), dot(matrix.rows[1], vector), dot(matrix.rows[2], vector) };
	}

	constexpr float4 mul(const float4x4& matrix, float4 vector) noexcept {
		if (!std::is_constant_evaluated())
			return NoGraphicsAPI_math_detail::mul(matrix, vector);
		return { dot(matrix.rows[0], vector), dot(matrix.rows[1], vector), dot(matrix.rows[2], vector), dot(matrix.rows[3], vector) };
	}

	constexpr float3 mul(const float3x4& matrix, float4 vector) noexcept {
		if (!std::is_constant_evaluated())
			return NoGraphicsAPI_math_detail::mul(matrix, vector);
		return { dot(matrix.rows[0], vector), dot(matrix.rows[1], vector), dot(matrix.rows[2], vector) };
	}

	constexpr float3 mul(float3 vector, const float3x3& matrix) noexcept {
		return {
			vector.x * matrix.rows[0].x + vector.y * matrix.rows[1].x + vector.z * matrix.rows[2].x,
			vector.x * matrix.rows[0].y + vector.y * matrix.rows[1].y + vector.z * matrix.rows[2].y,
			vector.x * matrix.rows[0].z + vector.y * matrix.rows[1].z + vector.z * matrix.rows[2].z,
		};
	}

	constexpr float4 mul(float4 vector, const float4x4& matrix) noexcept {
		if (!std::is_constant_evaluated())
			return NoGraphicsAPI_math_detail::mul(vector, matrix);
		return {
			vector.x * matrix.rows[0].x + vector.y * matrix.rows[1].x + vector.z * matrix.rows[2].x + vector.w * matrix.rows[3].x,
			vector.x * matrix.rows[0].y + vector.y * matrix.rows[1].y + vector.z * matrix.rows[2].y + vector.w * matrix.rows[3].y,
			vector.x * matrix.rows[0].z + vector.y * matrix.rows[1].z + vector.z * matrix.rows[2].z + vector.w * matrix.rows[3].z,
			vector.x * matrix.rows[0].w + vector.y * matrix.rows[1].w + vector.z * matrix.rows[2].w + vector.w * matrix.rows[3].w,
		};
	}

	constexpr float3x3 mul(const float3x3& lhs, const float3x3& rhs) noexcept {
		float3x3 result {};
		for (std::size_t row = 0; row != 3; ++row)
			for (std::size_t column = 0; column != 3; ++column)
				for (std::size_t inner = 0; inner != 3; ++inner)
					result[row][column] += lhs[row][inner] * rhs[inner][column];
		return result;
	}

	constexpr float4x4 mul(const float4x4& lhs, const float4x4& rhs) noexcept {
		if (!std::is_constant_evaluated())
			return NoGraphicsAPI_math_detail::mul(lhs, rhs);
		float4x4 result {};
		for (std::size_t row = 0; row != 4; ++row)
			for (std::size_t column = 0; column != 4; ++column)
				for (std::size_t inner = 0; inner != 4; ++inner)
					result[row][column] += lhs[row][inner] * rhs[inner][column];
		return result;
	}

	constexpr float3x4 mul(const float3x4& lhs, const float3x4& rhs) noexcept {
		if (!std::is_constant_evaluated())
			return NoGraphicsAPI_math_detail::mul(lhs, rhs);
		float3x4 result {};
		for (std::size_t row = 0; row != 3; ++row) {
			for (std::size_t column = 0; column != 3; ++column)
				for (std::size_t inner = 0; inner != 3; ++inner)
					result[row][column] += lhs[row][inner] * rhs[inner][column];
			result[row].w = lhs[row].w;
			for (std::size_t inner = 0; inner != 3; ++inner)
				result[row].w += lhs[row][inner] * rhs[inner].w;
		}
		return result;
	}

	constexpr float3 transform_vector(const float3x4& transform, float3 vector) noexcept {
		return mul(transform, float4 { vector.x, vector.y, vector.z, 0.0f });
	}

	constexpr float3 transform_point(const float3x4& transform, float3 point) noexcept {
		return mul(transform, float4 { point.x, point.y, point.z, 1.0f });
	}

	constexpr float3x3 transpose(const float3x3& matrix) noexcept {
		return {{
			{ matrix.rows[0].x, matrix.rows[1].x, matrix.rows[2].x },
			{ matrix.rows[0].y, matrix.rows[1].y, matrix.rows[2].y },
			{ matrix.rows[0].z, matrix.rows[1].z, matrix.rows[2].z },
		}};
	}

	constexpr float4x4 transpose(const float4x4& matrix) noexcept {
		if (!std::is_constant_evaluated())
			return NoGraphicsAPI_math_detail::transpose(matrix);
		return { { { matrix.rows[0].x, matrix.rows[1].x, matrix.rows[2].x, matrix.rows[3].x },
				   { matrix.rows[0].y, matrix.rows[1].y, matrix.rows[2].y, matrix.rows[3].y },
				   { matrix.rows[0].z, matrix.rows[1].z, matrix.rows[2].z, matrix.rows[3].z },
				   { matrix.rows[0].w, matrix.rows[1].w, matrix.rows[2].w, matrix.rows[3].w } } };
	}

	constexpr float determinant(const float3x3& matrix) noexcept {
		const float3 a = matrix.rows[0];
		const float3 b = matrix.rows[1];
		const float3 c = matrix.rows[2];
		return a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x);
	}

	inline float3x3 inverse(const float3x3& matrix) noexcept {
		const float3 a = matrix.rows[0];
		const float3 b = matrix.rows[1];
		const float3 c = matrix.rows[2];
		const float det = determinant(matrix);
		assert(det != 0.0f);
		const float reciprocal = 1.0f / det;
		return { { { (b.y * c.z - b.z * c.y) * reciprocal, (a.z * c.y - a.y * c.z) * reciprocal, (a.y * b.z - a.z * b.y) * reciprocal },
				   { (b.z * c.x - b.x * c.z) * reciprocal, (a.x * c.z - a.z * c.x) * reciprocal, (a.z * b.x - a.x * b.z) * reciprocal },
				   { (b.x * c.y - b.y * c.x) * reciprocal, (a.y * c.x - a.x * c.y) * reciprocal, (a.x * b.y - a.y * b.x) * reciprocal } } };
	}

	inline float3x4 inverse(const float3x4& transform) noexcept {
		const float3x3 linear { { { transform.rows[0].x, transform.rows[0].y, transform.rows[0].z },
								  { transform.rows[1].x, transform.rows[1].y, transform.rows[1].z },
								  { transform.rows[2].x, transform.rows[2].y, transform.rows[2].z } } };
		const float3x3 inverse_linear = inverse(linear);
		const float3 translation_value { transform.rows[0].w, transform.rows[1].w, transform.rows[2].w };
		const float3 inverse_translation = -mul(inverse_linear, translation_value);
		return { { { inverse_linear.rows[0].x, inverse_linear.rows[0].y, inverse_linear.rows[0].z, inverse_translation.x },
				   { inverse_linear.rows[1].x, inverse_linear.rows[1].y, inverse_linear.rows[1].z, inverse_translation.y },
				   { inverse_linear.rows[2].x, inverse_linear.rows[2].y, inverse_linear.rows[2].z, inverse_translation.z } } };
	}

	inline float4x4 inverse(const float4x4& matrix) noexcept {
		float augmented[4][8] {};
		for (std::size_t row = 0; row != 4; ++row) {
			for (std::size_t column = 0; column != 4; ++column)
				augmented[row][column] = matrix[row][column];
			augmented[row][row + 4] = 1.0f;
		}
		for (std::size_t pivot_column = 0; pivot_column != 4; ++pivot_column) {
			std::size_t pivot_row = pivot_column;
			float pivot_magnitude = abs(augmented[pivot_row][pivot_column]);
			for (std::size_t row = pivot_column + 1; row != 4; ++row) {
				const float magnitude = abs(augmented[row][pivot_column]);
				if (magnitude > pivot_magnitude) {
					pivot_row = row;
					pivot_magnitude = magnitude;
				}
			}
			assert(pivot_magnitude != 0.0f);
			if (pivot_row != pivot_column) {
				for (std::size_t column = 0; column != 8; ++column) {
					const float temporary = augmented[pivot_column][column];
					augmented[pivot_column][column] = augmented[pivot_row][column];
					augmented[pivot_row][column] = temporary;
				}
			}
			const float reciprocal_pivot = 1.0f / augmented[pivot_column][pivot_column];
			for (std::size_t column = 0; column != 8; ++column)
				augmented[pivot_column][column] *= reciprocal_pivot;
			for (std::size_t row = 0; row != 4; ++row) {
				if (row == pivot_column) continue;
				const float factor = augmented[row][pivot_column];
				for (std::size_t column = 0; column != 8; ++column)
					augmented[row][column] -= factor * augmented[pivot_column][column];
			}
		}
		float4x4 result {};
		for (std::size_t row = 0; row != 4; ++row)
			for (std::size_t column = 0; column != 4; ++column)
				result[row][column] = augmented[row][column + 4];
		return result;
	}

	constexpr float4x4 translation(float3 value) noexcept {
		return { { { 1.0f, 0.0f, 0.0f, value.x }, { 0.0f, 1.0f, 0.0f, value.y }, { 0.0f, 0.0f, 1.0f, value.z }, { 0.0f, 0.0f, 0.0f, 1.0f } } };
	}

	constexpr float4x4 scale(float3 value) noexcept {
		return { { { value.x, 0.0f, 0.0f, 0.0f }, { 0.0f, value.y, 0.0f, 0.0f }, { 0.0f, 0.0f, value.z, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } } };
	}

	inline float4x4 rotation_x(float angle) noexcept {
		const float sine = std::sin(angle);
		const float cosine = std::cos(angle);
		return { { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, cosine, -sine, 0.0f }, { 0.0f, sine, cosine, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } } };
	}

	inline float4x4 rotation_y(float angle) noexcept {
		const float sine = std::sin(angle);
		const float cosine = std::cos(angle);
		return { { { cosine, 0.0f, sine, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { -sine, 0.0f, cosine, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } } };
	}

	inline float4x4 rotation_z(float angle) noexcept {
		const float sine = std::sin(angle);
		const float cosine = std::cos(angle);
		return { { { cosine, -sine, 0.0f, 0.0f }, { sine, cosine, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } } };
	}

	constexpr float3x3 quaternion_to_matrix(quaternion value) noexcept {
		const float xx = value.x * value.x;
		const float yy = value.y * value.y;
		const float zz = value.z * value.z;
		const float xy = value.x * value.y;
		const float xz = value.x * value.z;
		const float yz = value.y * value.z;
		const float wx = value.w * value.x;
		const float wy = value.w * value.y;
		const float wz = value.w * value.z;
		return { { { 1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz), 2.0f * (xz + wy) },
				   { 2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx) },
				   { 2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy) } } };
	}

	constexpr float4x4 rotation(quaternion value) noexcept {
		const float3x3 matrix = quaternion_to_matrix(value);
		return { { { matrix.rows[0].x, matrix.rows[0].y, matrix.rows[0].z, 0.0f },
				   { matrix.rows[1].x, matrix.rows[1].y, matrix.rows[1].z, 0.0f },
				   { matrix.rows[2].x, matrix.rows[2].y, matrix.rows[2].z, 0.0f },
				   { 0.0f, 0.0f, 0.0f, 1.0f } } };
	}

	inline float4x4 perspective_rh_zo(float vertical_field_of_view, float aspect, float near_plane, float far_plane) noexcept {
		assert(aspect != 0.0f);
		assert(near_plane > 0.0f && far_plane > 0.0f && far_plane != near_plane);
		const float focal_length = 1.0f / std::tan(vertical_field_of_view * 0.5f);
		return { { { focal_length / aspect, 0.0f, 0.0f, 0.0f },
				   { 0.0f, focal_length, 0.0f, 0.0f },
				   { 0.0f, 0.0f, far_plane / (near_plane - far_plane), near_plane * far_plane / (near_plane - far_plane) },
				   { 0.0f, 0.0f, -1.0f, 0.0f } } };
	}

	inline float4x4 ortho_rh_zo(float left, float right, float bottom, float top, float near_plane, float far_plane) noexcept {
		assert(right != left && top != bottom && far_plane != near_plane);
		return { { { 2.0f / (right - left), 0.0f, 0.0f, -(right + left) / (right - left) },
				   { 0.0f, 2.0f / (top - bottom), 0.0f, -(top + bottom) / (top - bottom) },
				   { 0.0f, 0.0f, -1.0f / (far_plane - near_plane), -near_plane / (far_plane - near_plane) },
				   { 0.0f, 0.0f, 0.0f, 1.0f } } };
	}

	inline float4x4 look_at_rh(float3 eye, float3 center, float3 up) noexcept {
		const float3 forward = normalize(center - eye);
		const float3 side = normalize(cross(forward, up));
		const float3 camera_up = cross(side, forward);
		return { { { side.x, side.y, side.z, -dot(side, eye) },
				   { camera_up.x, camera_up.y, camera_up.z, -dot(camera_up, eye) },
				   { -forward.x, -forward.y, -forward.z, dot(forward, eye) },
				   { 0.0f, 0.0f, 0.0f, 1.0f } } };
	}

	constexpr quaternion identity_quaternion() noexcept {
		return { 0.0f, 0.0f, 0.0f, 1.0f };
	}

	constexpr quaternion conjugate(quaternion value) noexcept {
		return { -value.x, -value.y, -value.z, value.w };
	}

	inline quaternion inverse(quaternion value) noexcept {
		const float length_squared = dot(value, value);
		assert(length_squared != 0.0f);
		return conjugate(value) / length_squared;
	}

	inline quaternion axis_angle(float angle, float3 axis) noexcept {
		const float half_angle = angle * 0.5f;
		const float sine = std::sin(half_angle);
		const float3 unit_axis = normalize(axis);
		return { unit_axis.x * sine, unit_axis.y * sine, unit_axis.z * sine, std::cos(half_angle) };
	}

	constexpr float3 rotate(quaternion rotation_value, float3 vector) noexcept {
		const float3 imaginary { rotation_value.x, rotation_value.y, rotation_value.z };
		const float3 uv = cross(imaginary, vector);
		const float3 uuv = cross(imaginary, uv);
		return vector + 2.0f * (rotation_value.w * uv + uuv);
	}

	inline quaternion slerp(quaternion a, quaternion b, float t) noexcept {
		float cosine = dot(a, b);
		if (cosine < 0.0f) {
			b = -b;
			cosine = -cosine;
		}
		if (cosine > 0.9995f) return normalize(a + (b - a) * t);
		cosine = clamp(cosine, -1.0f, 1.0f);
		const float angle = std::acos(cosine);
		const float reciprocal_sine = 1.0f / std::sin(angle);
		return a * (std::sin((1.0f - t) * angle) * reciprocal_sine) + b * (std::sin(t * angle) * reciprocal_sine);
	}

	constexpr uint16 float_to_half_bits(float value) noexcept {
		const uint32 bits = std::bit_cast<uint32>(value);
		const uint32 sign = (bits >> 16u) & 0x8000u;
		const uint32 exponent = (bits >> 23u) & 0xffu;
		const uint32 mantissa = bits & 0x7fffffu;
		if (exponent == 0xffu) {
			if (mantissa == 0u) return static_cast<uint16>(sign | 0x7c00u);
			uint32 payload = mantissa >> 13u;
			if (payload == 0u) payload = 1u;
			return static_cast<uint16>(sign | 0x7c00u | payload);
		}
		const int32 half_exponent = static_cast<int32>(exponent) - 127 + 15;
		if (half_exponent >= 31) return static_cast<uint16>(sign | 0x7c00u);
		if (half_exponent <= 0) {
			if (half_exponent < -10) return static_cast<uint16>(sign);
			const uint32 significand = mantissa | 0x800000u;
			const uint32 shift = static_cast<uint32>(14 - half_exponent);
			uint32 rounded = significand >> shift;
			const uint32 remainder = significand & ((1u << shift) - 1u);
			const uint32 halfway = 1u << (shift - 1u);
			if (remainder > halfway || (remainder == halfway && (rounded & 1u) != 0u)) ++rounded;
			return static_cast<uint16>(sign | rounded);
		}
		uint32 rounded = (static_cast<uint32>(half_exponent) << 10u) | (mantissa >> 13u);
		const uint32 remainder = mantissa & 0x1fffu;
		if (remainder > 0x1000u || (remainder == 0x1000u && (rounded & 1u) != 0u)) ++rounded;
		return static_cast<uint16>(sign | rounded);
	}

	constexpr uint32 pack_half_2x16(float2 value) noexcept {
		return static_cast<uint32>(float_to_half_bits(value.x)) | (static_cast<uint32>(float_to_half_bits(value.y)) << 16u);
	}

} // namespace math

constexpr float3 operator*(const float3x3& matrix, float3 vector) noexcept {
	return math::mul(matrix, vector);
}
constexpr float4 operator*(const float4x4& matrix, float4 vector) noexcept {
	return math::mul(matrix, vector);
}
constexpr float3 operator*(const float3x4& matrix, float4 vector) noexcept {
	return math::mul(matrix, vector);
}
constexpr float3 operator*(float3 vector, const float3x3& matrix) noexcept {
	return math::mul(vector, matrix);
}
constexpr float4 operator*(float4 vector, const float4x4& matrix) noexcept {
	return math::mul(vector, matrix);
}
constexpr float3x3 operator*(const float3x3& lhs, const float3x3& rhs) noexcept {
	return math::mul(lhs, rhs);
}
constexpr float3x4 operator*(const float3x4& lhs, const float3x4& rhs) noexcept {
	return math::mul(lhs, rhs);
}
constexpr float4x4 operator*(const float4x4& lhs, const float4x4& rhs) noexcept {
	return math::mul(lhs, rhs);
}
constexpr float3 operator*(quaternion rotation_value, float3 vector) noexcept {
	return math::rotate(rotation_value, vector);
}

constexpr bool operator==(quaternion lhs, quaternion rhs) noexcept {
	return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
}
constexpr bool operator!=(quaternion lhs, quaternion rhs) noexcept {
	return !(lhs == rhs);
}
constexpr bool operator==(const float3x3& lhs, const float3x3& rhs) noexcept {
	return lhs.rows[0] == rhs.rows[0] && lhs.rows[1] == rhs.rows[1] && lhs.rows[2] == rhs.rows[2];
}
constexpr bool operator==(const float3x4& lhs, const float3x4& rhs) noexcept {
	return lhs.rows[0] == rhs.rows[0] && lhs.rows[1] == rhs.rows[1] && lhs.rows[2] == rhs.rows[2];
}
constexpr bool operator==(const float4x4& lhs, const float4x4& rhs) noexcept {
	return lhs.rows[0] == rhs.rows[0] && lhs.rows[1] == rhs.rows[1] && lhs.rows[2] == rhs.rows[2] && lhs.rows[3] == rhs.rows[3];
}
constexpr bool operator!=(const float3x3& lhs, const float3x3& rhs) noexcept {
	return !(lhs == rhs);
}
constexpr bool operator!=(const float3x4& lhs, const float3x4& rhs) noexcept {
	return !(lhs == rhs);
}
constexpr bool operator!=(const float4x4& lhs, const float4x4& rhs) noexcept {
	return !(lhs == rhs);
}

#endif
