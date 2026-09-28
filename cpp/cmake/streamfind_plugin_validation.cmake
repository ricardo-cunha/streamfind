#pragma once

include_guard(GLOBAL)

# Run the SDK-owned structural/package validator after a plugin is linked.
function(streamfind_validate_plugin_build target domain)
    add_dependencies(${target} streamfind_sdk_plugin_validator)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND $<TARGET_FILE:streamfind_sdk_plugin_validator>
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}"
            --output-dir "$<TARGET_FILE_DIR:${target}>"
            --manifest "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
            --domain "${domain}"
            --library "$<TARGET_FILE_NAME:${target}>"
        VERBATIM)
endfunction()

# Stage the runtime dependencies required when Windows loads a MinGW plugin by
# absolute path. The command must be declared in the plugin's own directory.
function(streamfind_stage_mingw_plugin_runtime target domain)
    if(NOT MINGW OR NOT WIN32)
        return()
    endif()
    if(NOT STREAMFIND_MINGW_RUNTIME_DIR)
        get_filename_component(STREAMFIND_MINGW_RUNTIME_DIR
            "${CMAKE_CXX_COMPILER}" DIRECTORY)
    endif()
    foreach(_mingw_runtime IN ITEMS
        libgcc_s_seh-1.dll
        libstdc++-6.dll
        libwinpthread-1.dll)
        set(_mingw_runtime_path
            "${STREAMFIND_MINGW_RUNTIME_DIR}/${_mingw_runtime}")
        if(NOT EXISTS "${_mingw_runtime_path}")
            message(FATAL_ERROR
                "Required MinGW runtime is missing: ${_mingw_runtime_path}")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_mingw_runtime_path}"
                "$<TARGET_FILE_DIR:${target}>")
        install(FILES "${_mingw_runtime_path}"
            DESTINATION "plugins/${domain}")
    endforeach()
    if(STREAMFIND_DUCKDB_RUNTIME AND EXISTS "${STREAMFIND_DUCKDB_RUNTIME}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STREAMFIND_DUCKDB_RUNTIME}"
                "$<TARGET_FILE_DIR:${target}>")
        install(FILES "${STREAMFIND_DUCKDB_RUNTIME}"
            DESTINATION "plugins/${domain}")
    endif()
endfunction()
