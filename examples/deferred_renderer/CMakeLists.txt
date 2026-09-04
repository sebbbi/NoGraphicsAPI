set(gbuffer_shader ${CMAKE_CURRENT_SOURCE_DIR}/gbuffer.slang)
set(deferred_lighting_shader ${CMAKE_CURRENT_SOURCE_DIR}/deferred_lighting.slang)
set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(gbuffer_vertex_spv ${shader_dir}/gbuffer.vert.spv)
set(gbuffer_fragment_spv ${shader_dir}/gbuffer.frag.spv)
set(deferred_vertex_spv ${shader_dir}/deferred.vert.spv)
set(deferred_fragment_spv ${shader_dir}/deferred.frag.spv)
set(gbuffer_shader_dependencies
    ${CMAKE_CURRENT_SOURCE_DIR}/gbuffer_shared.h
    ${PROJECT_SOURCE_DIR}/include/NoGraphicsAPI/shader_types.h)
set(deferred_lighting_shader_dependencies
    ${CMAKE_CURRENT_SOURCE_DIR}/deferred_lighting_shared.h
    ${PROJECT_SOURCE_DIR}/include/NoGraphicsAPI/shader_types.h)

NoGraphicsAPI_compile_slang(${gbuffer_vertex_spv} ${gbuffer_shader} vertexMain vertex
    DESCRIPTOR_HEAP DEPENDS ${gbuffer_shader_dependencies})
NoGraphicsAPI_compile_slang(${gbuffer_fragment_spv} ${gbuffer_shader} fragmentMain fragment
    DESCRIPTOR_HEAP DEPENDS ${gbuffer_shader_dependencies})
NoGraphicsAPI_compile_slang(${deferred_vertex_spv} ${deferred_lighting_shader} vertexMain vertex
    DESCRIPTOR_HEAP DEPENDS ${deferred_lighting_shader_dependencies})
NoGraphicsAPI_compile_slang(${deferred_fragment_spv} ${deferred_lighting_shader} fragmentMain fragment
    DESCRIPTOR_HEAP DEPENDS ${deferred_lighting_shader_dependencies})

NoGraphicsAPI_add_example(example_deferred_renderer
    deferred_renderer.cpp
    gbuffer_shared.h
    deferred_lighting_shared.h
    ${gbuffer_shader}
    ${deferred_lighting_shader}
    ${gbuffer_vertex_spv}
    ${gbuffer_fragment_spv}
    ${deferred_vertex_spv}
    ${deferred_fragment_spv}
)
target_compile_definitions(example_deferred_renderer PRIVATE
    NOGRAPHICSAPI_GBUFFER_VERTEX_SPV_PATH="${gbuffer_vertex_spv}"
    NOGRAPHICSAPI_GBUFFER_FRAGMENT_SPV_PATH="${gbuffer_fragment_spv}"
    NOGRAPHICSAPI_DEFERRED_VERTEX_SPV_PATH="${deferred_vertex_spv}"
    NOGRAPHICSAPI_DEFERRED_FRAGMENT_SPV_PATH="${deferred_fragment_spv}"
)
