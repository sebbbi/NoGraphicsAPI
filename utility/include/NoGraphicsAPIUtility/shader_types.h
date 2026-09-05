#pragma once

#if defined(__SLANG__)

// Slang already provides float[2-4], int[2-4], uint[2-4], bool[2-4],
// float16_t[2-4], int16_t[2-4], uint16_t[2-4], and native matrix types.
// Redeclaring those types would hide the built-ins and can change shader
// semantics. Only add the short scalar aliases used by shared structures.
typedef uint8_t uint8;
typedef int8_t int8;
typedef uint16_t uint16;
typedef int16_t int16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint64_t uint64;
typedef int64_t int64;

#else

#	include <cstddef>
#	include <cstdint>

using uint8 = std::uint8_t;
using int8 = std::int8_t;
using uint16 = std::uint16_t;
using int16 = std::int16_t;
using uint32 = std::uint32_t;
using int32 = std::int32_t;
using uint64 = std::uint64_t;
using int64 = std::int64_t;

// CPU-side binary16 values are deliberately opaque storage. Conversion to or
// from float is kept out of the shared ABI types so every bit pattern,
// including NaNs, crosses the CPU/GPU boundary unchanged.
struct float16_t {
	uint16 bits;
};

// Slang bool vectors use 32-bit elements and are not ABI-compatible with C++
// bool aggregates. Deliberately omit CPU-side bool vector declarations.

struct float2 {
	float x;
	float y;

	constexpr float& operator[](std::size_t index) noexcept { return index == 0 ? x : y; }
	constexpr const float& operator[](std::size_t index) const noexcept { return index == 0 ? x : y; }
};

struct float3 {
	float x;
	float y;
	float z;

	constexpr float& operator[](std::size_t index) noexcept { return index == 0 ? x : (index == 1 ? y : z); }
	constexpr const float& operator[](std::size_t index) const noexcept { return index == 0 ? x : (index == 1 ? y : z); }
};

struct float4 {
	float x;
	float y;
	float z;
	float w;

	constexpr float& operator[](std::size_t index) noexcept { return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w)); }
	constexpr const float& operator[](std::size_t index) const noexcept { return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w)); }
};

struct int2 {
	int32 x;
	int32 y;

	constexpr int32& operator[](std::size_t index) noexcept { return index == 0 ? x : y; }
	constexpr const int32& operator[](std::size_t index) const noexcept { return index == 0 ? x : y; }
};

struct int3 {
	int32 x;
	int32 y;
	int32 z;

	constexpr int32& operator[](std::size_t index) noexcept { return index == 0 ? x : (index == 1 ? y : z); }
	constexpr const int32& operator[](std::size_t index) const noexcept { return index == 0 ? x : (index == 1 ? y : z); }
};

struct int4 {
	int32 x;
	int32 y;
	int32 z;
	int32 w;

	constexpr int32& operator[](std::size_t index) noexcept { return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w)); }
	constexpr const int32& operator[](std::size_t index) const noexcept { return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w)); }
};

struct uint2 {
	uint32 x;
	uint32 y;

	constexpr uint32& operator[](std::size_t index) noexcept { return index == 0 ? x : y; }
	constexpr const uint32& operator[](std::size_t index) const noexcept { return index == 0 ? x : y; }
};

struct uint3 {
	uint32 x;
	uint32 y;
	uint32 z;

	constexpr uint32& operator[](std::size_t index) noexcept { return index == 0 ? x : (index == 1 ? y : z); }
	constexpr const uint32& operator[](std::size_t index) const noexcept { return index == 0 ? x : (index == 1 ? y : z); }
};

struct uint4 {
	uint32 x;
	uint32 y;
	uint32 z;
	uint32 w;

	constexpr uint32& operator[](std::size_t index) noexcept { return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w)); }
	constexpr const uint32& operator[](std::size_t index) const noexcept { return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w)); }
};

// Slang names matrix dimensions as row-count x column-count. Matrices use
// row-major storage and multiply column vectors.
struct float3x3 {
	float3 rows[3];

	constexpr float3& operator[](std::size_t index) noexcept { return rows[index]; }
	constexpr const float3& operator[](std::size_t index) const noexcept { return rows[index]; }
};

struct float3x4 {
	float4 rows[3];

	constexpr float4& operator[](std::size_t index) noexcept { return rows[index]; }
	constexpr const float4& operator[](std::size_t index) const noexcept { return rows[index]; }
};

struct float4x4 {
	float4 rows[4];

	constexpr float4& operator[](std::size_t index) noexcept { return rows[index]; }
	constexpr const float4& operator[](std::size_t index) const noexcept { return rows[index]; }
};

struct quaternion {
	float x;
	float y;
	float z;
	float w;

	constexpr float& operator[](std::size_t index) noexcept { return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w)); }
	constexpr const float& operator[](std::size_t index) const noexcept { return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w)); }
};

struct float16_t2 {
	float16_t x;
	float16_t y;
};
struct float16_t3 {
	float16_t x;
	float16_t y;
	float16_t z;
};
struct float16_t4 {
	float16_t x;
	float16_t y;
	float16_t z;
	float16_t w;
};
struct int16_t2 {
	int16 x;
	int16 y;
};
struct int16_t3 {
	int16 x;
	int16 y;
	int16 z;
};
struct int16_t4 {
	int16 x;
	int16 y;
	int16 z;
	int16 w;
};
struct uint16_t2 {
	uint16 x;
	uint16 y;
};
struct uint16_t3 {
	uint16 x;
	uint16 y;
	uint16 z;
};
struct uint16_t4 {
	uint16 x;
	uint16 y;
	uint16 z;
	uint16 w;
};

#endif
