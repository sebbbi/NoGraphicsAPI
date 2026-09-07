#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#if !defined(min) || !defined(max)
#	error "Windows-first test requires the legacy Win32 min/max macros"
#endif

#include <NoGraphicsAPIUtility/math.hpp>

#if defined(min) || defined(max)
#	error "NoGraphicsAPIUtility math.hpp must suppress Win32 min/max macros"
#endif

static_assert(math::min(3, 7) == 3);
static_assert(math::max(3, 7) == 7);
static_assert(math::min(float3 { .x = 3.0f, .y = 8.0f, .z = -2.0f }, float3 { .x = 5.0f, .y = 4.0f, .z = -1.0f }) ==
              float3 { .x = 3.0f, .y = 4.0f, .z = -2.0f });
static_assert(math::max(float3 { .x = 3.0f, .y = 8.0f, .z = -2.0f }, float3 { .x = 5.0f, .y = 4.0f, .z = -1.0f }) ==
              float3 { .x = 5.0f, .y = 8.0f, .z = -1.0f });

int main() {
	return 0;
}
