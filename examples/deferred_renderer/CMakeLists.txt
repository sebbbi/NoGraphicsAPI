set(simulation_shader ${CMAKE_CURRENT_SOURCE_DIR}/simulation.slang)
set(gbuffer_shader ${CMAKE_CURRENT_SOURCE_DIR}/gbuffer.slang)
set(deferred_lighting_shader ${CMAKE_CURRENT_SOURCE_DIR}/deferred_lighting.slang)
set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(simulation_compute_spv ${shader_dir}/simulation.comp.spv)
set(gbuffer_mesh_spv ${shader_dir}/gbuffer.mesh.spv)
set(gbuffer_fragment_spv ${shader_dir}/gbuffer.frag.spv)
set(deferred_vertex_spv ${shader_dir}/deferred.vert.spv)
set(deferred_fragment_spv ${shader_dir}/deferred.frag.spv)
set(gbuffer_shader_dependencies
    ${CMAKE_CURRENT_SOURCE_DIR}/gbuffer_shared.h
    ${CMAKE_CURRENT_SOURCE_DIR}/object_data.h)
set(simulation_shader_dependencies
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation_shared.h
    ${CMAKE_CURRENT_SOURCE_DIR}/object_data.h)

NoGraphicsAPI_compile_slang(${simulation_compute_spv} ${simulation_shader} computeMain compute
    DEPENDS ${simulation_shader_dependencies})
NoGraphicsAPI_compile_slang(${gbuffer_mesh_spv} ${gbuffer_shader} meshMain mesh
    DEPENDS ${gbuffer_shader_dependencies})
NoGraphicsAPI_compile_slang(${gbuffer_fragment_spv} ${gbuffer_shader} fragmentMain fragment
    DEPENDS ${gbuffer_shader_dependencies})
NoGraphicsAPI_compile_slang(${deferred_vertex_spv} ${deferred_lighting_shader} vertexMain vertex
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/deferred_lighting_shared.h)
NoGraphicsAPI_compile_slang(${deferred_fragment_spv} ${deferred_lighting_shader} fragmentMain fragment
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/deferred_lighting_shared.h)

NoGraphicsAPI_add_example(example_deferred_renderer
    deferred_renderer.cpp
    object_data.h
    simulation_shared.h
    gbuffer_shared.h
    deferred_lighting_shared.h
    ${simulation_shader}
    ${gbuffer_shader}
    ${deferred_lighting_shader}
    ${simulation_compute_spv}
    ${gbuffer_mesh_spv}
    ${gbuffer_fragment_spv}
    ${deferred_vertex_spv}
    ${deferred_fragment_spv}
)
target_link_libraries(example_deferred_renderer PRIVATE NoGraphicsAPIUtility::math NoGraphicsAPIUtility::textures)
target_compile_definitions(example_deferred_renderer PRIVATE
    NOGRAPHICSAPI_SIMULATION_COMPUTE_SPV_PATH="shaders/simulation.comp.spv"
    NOGRAPHICSAPI_GBUFFER_MESH_SPV_PATH="shaders/gbuffer.mesh.spv"
    NOGRAPHICSAPI_GBUFFER_FRAGMENT_SPV_PATH="shaders/gbuffer.frag.spv"
    NOGRAPHICSAPI_DEFERRED_VERTEX_SPV_PATH="shaders/deferred.vert.spv"
    NOGRAPHICSAPI_DEFERRED_FRAGMENT_SPV_PATH="shaders/deferred.frag.spv"
)
if(CMAKE_CONFIGURATION_TYPES)
    add_custom_command(TARGET example_deferred_renderer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${shader_dir} $<TARGET_FILE_DIR:example_deferred_renderer>/shaders)
endif()
