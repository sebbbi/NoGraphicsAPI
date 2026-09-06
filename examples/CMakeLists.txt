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

add_subdirectory(triangle)
add_subdirectory(cube)
add_subdirectory(deferred_renderer)

function(NoGraphicsAPI_make_release_gui target)
    if(WIN32)
        set_target_properties(${target} PROPERTIES
            WIN32_EXECUTABLE "$<$<CONFIG:Release>:TRUE>"
        )
        target_link_options(${target} PRIVATE
            "$<$<CONFIG:Release>:/ENTRY:mainCRTStartup>"
        )
    endif()
endfunction()

NoGraphicsAPI_make_release_gui(example_triangle)
NoGraphicsAPI_make_release_gui(example_cube)
NoGraphicsAPI_make_release_gui(example_deferred_renderer)
