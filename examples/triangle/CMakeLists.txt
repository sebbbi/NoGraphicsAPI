set(shader ${CMAKE_CURRENT_SOURCE_DIR}/triangle.slang)
set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(vertex_spv ${shader_dir}/triangle.vert.spv)
set(fragment_spv ${shader_dir}/triangle.frag.spv)

NoGraphicsAPI_compile_slang(${vertex_spv} ${shader} vertexMain vertex)
NoGraphicsAPI_compile_slang(${fragment_spv} ${shader} fragmentMain fragment)

NoGraphicsAPI_add_example(example_triangle
    triangle.cpp
    ${shader}
    ${vertex_spv}
    ${fragment_spv}
)
target_compile_definitions(example_triangle PRIVATE
    NOGRAPHICSAPI_VERTEX_SPV_PATH="${vertex_spv}"
    NOGRAPHICSAPI_FRAGMENT_SPV_PATH="${fragment_spv}"
)
