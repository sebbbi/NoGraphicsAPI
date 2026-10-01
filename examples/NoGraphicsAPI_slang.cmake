find_program(NOGRAPHICSAPI_SLANGC NAMES slangc REQUIRED)
if(APPLE)
    find_program(NOGRAPHICSAPI_XCRUN NAMES xcrun REQUIRED)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(NOGRAPHICSAPI_METAL_SDK iphoneos)
        set(NOGRAPHICSAPI_METAL_TARGET "air64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        set(NOGRAPHICSAPI_METAL_SDK macosx)
        set(NOGRAPHICSAPI_METAL_TARGET "air64-apple-macos${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    set(NOGRAPHICSAPI_SHADER_EXTENSION metallib CACHE INTERNAL "Native shader artifact extension" FORCE)
    set(NOGRAPHICSAPI_SLANG_MINIMUM 2026.18.2)
else()
    find_program(NOGRAPHICSAPI_SPIRV_VAL NAMES spirv-val REQUIRED)
    set(NOGRAPHICSAPI_SHADER_EXTENSION spv CACHE INTERNAL "Native shader artifact extension" FORCE)
    set(NOGRAPHICSAPI_SLANG_MINIMUM 2026.14.1)
endif()

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
    "${NOGRAPHICSAPI_SLANGC}" -version Slang ${NOGRAPHICSAPI_SLANG_MINIMUM}
    "([0-9]+\\.[0-9]+(\\.[0-9]+)?)")
if(NOT APPLE)
    NoGraphicsAPI_require_tool_version(
        "${NOGRAPHICSAPI_SPIRV_VAL}" --version SPIRV-Tools 2026.3
        "SPIRV-Tools v([0-9]+\\.[0-9]+)")
endif()

function(NoGraphicsAPI_compile_slang output source entry stage)
    cmake_parse_arguments(SLANG "" "" "DEPENDS;DEFINES;OPTIONS" ${ARGN})
    get_filename_component(output_dir "${output}" DIRECTORY)
    set(dependencies ${source} ${SLANG_DEPENDS}
        ${PROJECT_SOURCE_DIR}/include/NoGraphicsAPI/types.h
        ${PROJECT_SOURCE_DIR}/include/NoGraphicsAPI/shader.slang
        ${PROJECT_SOURCE_DIR}/include/NoGraphicsAPI/shader_shared.h
        ${PROJECT_SOURCE_DIR}/utility/include/NoGraphicsAPIUtility/shader_types.h)
    set(common_options
        -entry ${entry} -stage ${stage}
        -fvk-use-c-layout -matrix-layout-row-major
        -I ${CMAKE_CURRENT_SOURCE_DIR}
        -I ${PROJECT_SOURCE_DIR}/include
        -I ${PROJECT_SOURCE_DIR}/utility/include)
    foreach(define IN LISTS SLANG_DEFINES)
        list(APPEND common_options -D${define})
    endforeach()
    list(APPEND common_options ${SLANG_OPTIONS})
    if(APPLE)
        add_custom_command(
            OUTPUT ${output}
            BYPRODUCTS ${output}.metal ${output}.air
            COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir}
            COMMAND ${NOGRAPHICSAPI_SLANGC} ${source} -target metal -DNOGRAPHICSAPI_METAL
                ${common_options} -o ${output}.metal
            COMMAND ${NOGRAPHICSAPI_XCRUN} -sdk ${NOGRAPHICSAPI_METAL_SDK} metal -std=metal3.0
                -target ${NOGRAPHICSAPI_METAL_TARGET} -c ${output}.metal -o ${output}.air
            COMMAND ${NOGRAPHICSAPI_XCRUN} -sdk ${NOGRAPHICSAPI_METAL_SDK} metallib ${output}.air -o ${output}
            DEPENDS ${dependencies}
            VERBATIM
            COMMENT "Compiling Slang ${stage} shader ${entry} to metallib"
        )
    else()
        set(options -capability spvDescriptorHeapEXT)
        if(stage STREQUAL "mesh" OR stage STREQUAL "amplification")
            list(APPEND options -capability spvMeshShadingEXT)
        endif()
        add_custom_command(
            OUTPUT ${output}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir}
            COMMAND ${NOGRAPHICSAPI_SLANGC} ${source} -target spirv -profile spirv_1_5
                -emit-spirv-directly -fvk-use-entrypoint-name ${common_options} ${options} -o ${output}
            COMMAND ${NOGRAPHICSAPI_SPIRV_VAL} --target-env vulkan1.4 --scalar-block-layout ${output}
            DEPENDS ${dependencies}
            VERBATIM
            COMMENT "Compiling and validating Slang ${stage} shader ${entry} to SPIR-V"
        )
    endif()
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
