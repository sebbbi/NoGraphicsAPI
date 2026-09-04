cmake_minimum_required(VERSION 3.24)

project(NoGraphicsAPI VERSION 0.1.0 LANGUAGES CXX)

if(MINGW)
    message(FATAL_ERROR "NoGraphicsAPI does not support MinGW")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NoGraphicsAPI requires a 64-bit target")
endif()

include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(NOGRAPHICSAPI_BUILD_EXAMPLES "Build NoGraphicsAPI examples" OFF)
option(NOGRAPHICSAPI_BUILD_TESTS "Build NoGraphicsAPI tests" OFF)

include(cmake/NoGraphicsAPI_math.cmake)

# CMake adds /EHsc to MSVC's default flags. Remove it before adding the explicit
# no-exceptions mode below so cl.exe does not report an option-override warning.
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

function(NoGraphicsAPI_disable_exceptions target)
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /EHs-c- /GR-)
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
    endif()
endfunction()

FetchContent_Declare(
    OffsetAllocator
    GIT_REPOSITORY https://github.com/sebbbi/OffsetAllocator.git
    GIT_TAG 3610a7377088b1e8c8f1525f458c96038a4e6fc0
    SOURCE_SUBDIR NoGraphicsAPI_no_upstream_cmake
)
FetchContent_MakeAvailable(OffsetAllocator)

add_library(NoGraphicsAPI_offset_allocator STATIC
    ${offsetallocator_SOURCE_DIR}/offsetAllocator.cpp
)
target_include_directories(NoGraphicsAPI_offset_allocator PUBLIC
    $<BUILD_INTERFACE:${offsetallocator_SOURCE_DIR}>
)
target_compile_features(NoGraphicsAPI_offset_allocator PUBLIC cxx_std_20)
NoGraphicsAPI_disable_exceptions(NoGraphicsAPI_offset_allocator)

find_package(Vulkan 1.4.357 REQUIRED)

add_library(NoGraphicsAPI STATIC
    include/NoGraphicsAPI/NoGraphicsAPI.hpp
    src/NoGraphicsAPI.cpp
)
add_library(NoGraphicsAPI::NoGraphicsAPI ALIAS NoGraphicsAPI)
set_target_properties(NoGraphicsAPI PROPERTIES EXPORT_NAME NoGraphicsAPI)
set_target_properties(NoGraphicsAPI_offset_allocator PROPERTIES EXPORT_NAME offset_allocator)

target_compile_features(NoGraphicsAPI PUBLIC cxx_std_20)
target_link_libraries(NoGraphicsAPI
    PUBLIC NoGraphicsAPI::math Vulkan::Headers
    PRIVATE Vulkan::Vulkan NoGraphicsAPI_offset_allocator
)
NoGraphicsAPI_disable_exceptions(NoGraphicsAPI)

if(MSVC)
    target_compile_options(NoGraphicsAPI PRIVATE /W4 /permissive-)
else()
    target_compile_options(NoGraphicsAPI PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wno-missing-field-initializers
    )
endif()

if(NOGRAPHICSAPI_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(NOGRAPHICSAPI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS
        NoGraphicsAPI
        NoGraphicsAPI_math
        NoGraphicsAPI_offset_allocator
    EXPORT NoGraphicsAPITargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/NoGraphicsAPI
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

configure_package_config_file(
    cmake/NoGraphicsAPIConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/NoGraphicsAPIConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NoGraphicsAPI
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/NoGraphicsAPIConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT NoGraphicsAPITargets
    FILE NoGraphicsAPITargets.cmake
    NAMESPACE NoGraphicsAPI::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NoGraphicsAPI
)
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/NoGraphicsAPIConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/NoGraphicsAPIConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NoGraphicsAPI
)
install(FILES
        LICENSE
        THIRD_PARTY_NOTICES.md
        examples/cube/LICENSE-Apache-2.0.txt
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND NOGRAPHICSAPI_BUILD_EXAMPLES)
    cmake_path(GET Vulkan_INCLUDE_DIR PARENT_PATH vulkan_sdk_dir)
    foreach(example_target IN ITEMS
            example_triangle
            example_cube
            example_deferred_renderer)
        get_target_property(example_binary_dir ${example_target} BINARY_DIR)
        set_target_properties(${example_target} PROPERTIES
            VS_DEBUGGER_WORKING_DIRECTORY "${example_binary_dir}"
            VS_DEBUGGER_ENVIRONMENT
                "VK_LAYER_PATH=${vulkan_sdk_dir}/Bin\nVK_LOADER_LAYERS_DISABLE=~implicit~")
    endforeach()

    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        PROPERTY VS_STARTUP_PROJECT example_triangle)
endif()
