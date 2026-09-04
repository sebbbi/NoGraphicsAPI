if(NOT TARGET NoGraphicsAPI_math)
    get_filename_component(_NoGraphicsAPI_math_root
        "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

    if(WIN32 AND NOT MSVC)
        message(FATAL_ERROR
            "NoGraphicsAPI Windows builds require an MSVC-compatible compiler")
    endif()

    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles([=[
#if defined(_M_ARM64EC) || (!defined(_M_X64) && !defined(__x86_64__))
#error NoGraphicsAPI requires an x86-64 target
#endif
static_assert(sizeof(void*) == 8, "NoGraphicsAPI requires 64-bit pointers");
int main() { return 0; }
]=] NOGRAPHICSAPI_TARGET_X86_64)
    if(NOT NOGRAPHICSAPI_TARGET_X86_64)
        message(FATAL_ERROR "NoGraphicsAPI requires an x86-64 target")
    endif()

    add_library(NoGraphicsAPI_math INTERFACE)
    target_compile_features(NoGraphicsAPI_math INTERFACE cxx_std_20)
    if(MSVC)
        target_compile_options(NoGraphicsAPI_math INTERFACE /arch:AVX2)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(NoGraphicsAPI_math INTERFACE -mavx2 -mfma)
    else()
        message(FATAL_ERROR "NoGraphicsAPI requires a compiler with AVX2 and FMA target support")
    endif()
    target_include_directories(NoGraphicsAPI_math BEFORE INTERFACE
        $<BUILD_INTERFACE:${_NoGraphicsAPI_math_root}/include>
        $<INSTALL_INTERFACE:include>
    )

    unset(_NoGraphicsAPI_math_root)
endif()

if(NOT TARGET NoGraphicsAPI::math)
    add_library(NoGraphicsAPI::math ALIAS NoGraphicsAPI_math)
endif()
