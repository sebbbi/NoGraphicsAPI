add_subdirectory(device_info)

# The windowed examples use the Win32 message pump in common/example_support.cpp and compile
# their shaders with slangc, so they and the Slang tooling they need are Windows-only for now.
if(NOT WIN32)
    return()
endif()

include(NoGraphicsAPI_slang.cmake)

add_library(NoGraphicsAPI_example_support STATIC
    common/example_support.cpp
    common/example_support.hpp
)
NoGraphicsAPI_disable_exceptions(NoGraphicsAPI_example_support)
target_link_libraries(NoGraphicsAPI_example_support PUBLIC NoGraphicsAPI::NoGraphicsAPI user32)
target_include_directories(NoGraphicsAPI_example_support PUBLIC common)

add_subdirectory(triangle)
add_subdirectory(cube)
add_subdirectory(deferred_renderer)
