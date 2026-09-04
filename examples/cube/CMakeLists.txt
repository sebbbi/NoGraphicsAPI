set(shader ${CMAKE_CURRENT_SOURCE_DIR}/cube.slang)
set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(vertex_spv ${shader_dir}/cube.vert.spv)
set(fragment_spv ${shader_dir}/cube.frag.spv)
set(shader_dependencies
    ${CMAKE_CURRENT_SOURCE_DIR}/cube_shared.h
    ${PROJECT_SOURCE_DIR}/include/NoGraphicsAPI/shader_types.h)

NoGraphicsAPI_compile_slang(${vertex_spv} ${shader} vertexMain vertex
    DESCRIPTOR_HEAP DEPENDS ${shader_dependencies})
NoGraphicsAPI_compile_slang(${fragment_spv} ${shader} fragmentMain fragment
    DESCRIPTOR_HEAP DEPENDS ${shader_dependencies})

set(texture_name lunarg_logo_256x256.rgba8)
set(texture_source ${CMAKE_CURRENT_SOURCE_DIR}/${texture_name})
set(texture ${CMAKE_CURRENT_BINARY_DIR}/${texture_name})
file(SIZE ${texture_source} texture_size)
if(NOT texture_size EQUAL 262144)
    message(FATAL_ERROR "Cube RGBA8 texture must be exactly 262144 bytes")
endif()
configure_file(${texture_source} ${texture} COPYONLY)

NoGraphicsAPI_add_example(example_cube
    cube.cpp
    cube_shared.h
    ${shader}
    ${texture_source}
    NOTICE.md
    LICENSE-Apache-2.0.txt
    ${vertex_spv}
    ${fragment_spv}
)
target_compile_definitions(example_cube PRIVATE
    NOGRAPHICSAPI_CUBE_VERTEX_SPV_PATH="${vertex_spv}"
    NOGRAPHICSAPI_CUBE_FRAGMENT_SPV_PATH="${fragment_spv}"
    NOGRAPHICSAPI_CUBE_TEXTURE_PATH="${texture}"
)
