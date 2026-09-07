include(NoGraphicsAPI_slang.cmake)

add_library(NoGraphicsAPI_example_support STATIC
    common/example_support.cpp
    common/example_support.hpp
)
NoGraphicsAPI_disable_exceptions(NoGraphicsAPI_example_support)
target_link_libraries(NoGraphicsAPI_example_support PUBLIC NoGraphicsAPI::NoGraphicsAPI)
target_include_directories(NoGraphicsAPI_example_support PUBLIC common)
if(WIN32)
    target_link_libraries(NoGraphicsAPI_example_support PUBLIC user32)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(NoGraphicsAPI_example_support PUBLIC PkgConfig::XCB)
endif()

add_subdirectory(triangle)
add_subdirectory(cube)
add_subdirectory(deferred_renderer)
