find_program(NOGRAPHICSAPI_SLANGC NAMES slangc REQUIRED)
find_program(NOGRAPHICSAPI_SPIRV_VAL NAMES spirv-val REQUIRED)

function(NoGraphicsAPI_require_tool_version program argument name minimum pattern)
    execute_process(
        COMMAND ${program} ${argument}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    string(STRIP "${output}${error}" version_text)
    string(REGEX MATCH "${pattern}" match "${version_text}")
    set(version "${CMAKE_MATCH_1}")
    if(NOT result EQUAL 0 OR
       NOT match OR
       version VERSION_LESS minimum)
        message(FATAL_ERROR
            "NoGraphicsAPI examples require ${name} ${minimum} or newer; "
            "found '${version_text}' at ${program}")
    endif()
endfunction()

NoGraphicsAPI_require_tool_version(
    "${NOGRAPHICSAPI_SLANGC}" -version Slang 2026.14.1
    "([0-9]+\\.[0-9]+\\.[0-9]+)")
NoGraphicsAPI_require_tool_version(
    "${NOGRAPHICSAPI_SPIRV_VAL}" --version SPIRV-Tools 2026.3
    "SPIRV-Tools v([0-9]+\\.[0-9]+)")

function(NoGraphicsAPI_compile_slang output source entry stage)
    cmake_parse_arguments(SLANG "DESCRIPTOR_HEAP" "DEFINE" "DEPENDS" ${ARGN})
    set(options)
    if(SLANG_DESCRIPTOR_HEAP)
        list(APPEND options
            -fvk-use-c-layout
            -matrix-layout-row-major
            -capability spvDescriptorHeapEXT
            -I ${CMAKE_CURRENT_SOURCE_DIR}
            -I ${PROJECT_SOURCE_DIR}/include)
    endif()
    if(SLANG_DEFINE)
        list(APPEND options -D${SLANG_DEFINE}=1)
    endif()

    get_filename_component(output_dir "${output}" DIRECTORY)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir}
        COMMAND ${NOGRAPHICSAPI_SLANGC}
            ${source}
            -target spirv
            -profile spirv_1_5
            -emit-spirv-directly
            -fvk-use-entrypoint-name
            ${options}
            -entry ${entry}
            -stage ${stage}
            -o ${output}
        COMMAND ${NOGRAPHICSAPI_SPIRV_VAL}
            --target-env vulkan1.4 --scalar-block-layout ${output}
        DEPENDS ${source} ${SLANG_DEPENDS}
        VERBATIM
        COMMENT "Compiling Slang ${stage} shader ${entry}"
    )
endfunction()

function(NoGraphicsAPI_add_example target)
    add_executable(${target} ${ARGN})
    foreach(source IN LISTS ARGN)
        if(source MATCHES "\\.slang$")
            set_source_files_properties(${source} PROPERTIES HEADER_FILE_ONLY TRUE)
            source_group("Shaders" FILES ${source})
        endif()
    endforeach()
    NoGraphicsAPI_disable_exceptions(${target})
    target_link_libraries(${target} PRIVATE NoGraphicsAPI_example_support)
endfunction()
