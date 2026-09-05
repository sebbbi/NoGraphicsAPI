add_library(NoGraphicsAPIUtility_types INTERFACE)
add_library(NoGraphicsAPIUtility::types ALIAS NoGraphicsAPIUtility_types)
set_target_properties(NoGraphicsAPIUtility_types PROPERTIES EXPORT_NAME types)
target_compile_features(NoGraphicsAPIUtility_types INTERFACE cxx_std_20)
target_include_directories(NoGraphicsAPIUtility_types INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

add_library(NoGraphicsAPIUtility_math INTERFACE)
add_library(NoGraphicsAPIUtility::math ALIAS NoGraphicsAPIUtility_math)
set_target_properties(NoGraphicsAPIUtility_math PROPERTIES EXPORT_NAME math)
target_link_libraries(NoGraphicsAPIUtility_math INTERFACE NoGraphicsAPIUtility::types)
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(NoGraphicsAPIUtility_math INTERFACE /arch:AVX2)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(NoGraphicsAPIUtility_math INTERFACE -mavx2 -mfma)
else()
    message(FATAL_ERROR "NoGraphicsAPIUtility math requires a compiler with AVX2 and FMA target support")
endif()

add_library(NoGraphicsAPIUtility_allocators STATIC
    include/NoGraphicsAPIUtility/bump_allocator.hpp
    include/NoGraphicsAPIUtility/delete_queue.hpp
    include/NoGraphicsAPIUtility/heap_allocator.hpp
    src/bump_allocator.cpp
    src/delete_queue.cpp
    src/heap_allocator.cpp
)
add_library(NoGraphicsAPIUtility::allocators ALIAS NoGraphicsAPIUtility_allocators)
set_target_properties(NoGraphicsAPIUtility_allocators PROPERTIES EXPORT_NAME allocators)
target_compile_features(NoGraphicsAPIUtility_allocators PUBLIC cxx_std_20)
target_include_directories(NoGraphicsAPIUtility_allocators PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(NoGraphicsAPIUtility_allocators PUBLIC NoGraphicsAPI::NoGraphicsAPI)
NoGraphicsAPI_disable_exceptions(NoGraphicsAPIUtility_allocators)
if(MSVC)
    target_compile_options(NoGraphicsAPIUtility_allocators PRIVATE /W4 /permissive-)
else()
    target_compile_options(NoGraphicsAPIUtility_allocators PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_library(NoGraphicsAPIUtility_textures STATIC
    include/NoGraphicsAPIUtility/texture_allocator.hpp
    src/texture_allocator.cpp
)
add_library(NoGraphicsAPIUtility::textures ALIAS NoGraphicsAPIUtility_textures)
set_target_properties(NoGraphicsAPIUtility_textures PROPERTIES EXPORT_NAME textures)
target_compile_features(NoGraphicsAPIUtility_textures PUBLIC cxx_std_20)
target_link_libraries(NoGraphicsAPIUtility_textures PUBLIC NoGraphicsAPIUtility::allocators NoGraphicsAPI::NoGraphicsAPI)
NoGraphicsAPI_disable_exceptions(NoGraphicsAPIUtility_textures)
if(MSVC)
    target_compile_options(NoGraphicsAPIUtility_textures PRIVATE /W4 /permissive-)
else()
    target_compile_options(NoGraphicsAPIUtility_textures PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_library(NoGraphicsAPIUtility_utility INTERFACE)
add_library(NoGraphicsAPIUtility::utility ALIAS NoGraphicsAPIUtility_utility)
set_target_properties(NoGraphicsAPIUtility_utility PROPERTIES EXPORT_NAME utility)
target_link_libraries(NoGraphicsAPIUtility_utility INTERFACE NoGraphicsAPIUtility::math NoGraphicsAPIUtility::allocators NoGraphicsAPIUtility::textures)

install(TARGETS
        NoGraphicsAPIUtility_types
        NoGraphicsAPIUtility_math
        NoGraphicsAPIUtility_allocators
        NoGraphicsAPIUtility_textures
        NoGraphicsAPIUtility_utility
    EXPORT NoGraphicsAPIUtilityTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/NoGraphicsAPIUtility
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

configure_package_config_file(
    cmake/NoGraphicsAPIUtilityConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/NoGraphicsAPIUtilityConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NoGraphicsAPIUtility
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/NoGraphicsAPIUtilityConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT NoGraphicsAPIUtilityTargets
    FILE NoGraphicsAPIUtilityTargets.cmake
    NAMESPACE NoGraphicsAPIUtility::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NoGraphicsAPIUtility
)
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/NoGraphicsAPIUtilityConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/NoGraphicsAPIUtilityConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NoGraphicsAPIUtility
)
