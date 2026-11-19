#pragma once

include_guard(GLOBAL)

# Run the SDK-owned structural/package validator after a plugin is linked.
function(streamfind_validate_plugin_build target domain)
    add_dependencies(${target} streamfind_sdk_plugin_validator)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND $<TARGET_FILE:streamfind_sdk_plugin_validator>
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}"
            --output-dir "$<TARGET_FILE_DIR:${target}>"
            --manifest "${STREAMFIND_BUNDLED_MANIFEST}"
            --domain "${domain}"
            --library "$<TARGET_FILE_NAME:${target}>"
        VERBATIM)
endfunction()

# Stage runtime dependencies in the single package runtime directory. The
# host and plugins use the same bin directory, so no duplicate DLL copies are
# needed in plugin or core vendor subdirectories.
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
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_BINARY_DIR}/bin"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_mingw_runtime_path}"
                "${CMAKE_BINARY_DIR}/bin")
    endforeach()
    if(STREAMFIND_DUCKDB_RUNTIME AND EXISTS "${STREAMFIND_DUCKDB_RUNTIME}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_BINARY_DIR}/bin"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STREAMFIND_DUCKDB_RUNTIME}"
                "${CMAKE_BINARY_DIR}/bin")
    endif()
    if(domain STREQUAL "mass_spec" AND TARGET streamfind_openbabel_runtime)
        # Open Babel's runtime target owns development-tree staging and the
        # root CMakeLists owns the CPack install rule. Do not copy the same
        # data directory again from the mass-spec plugin post-build hook: on
        # Windows the second recursive copy can collide with a locked or
        # read-only generated file in the shared bin directory.
        add_dependencies(${target} streamfind_openbabel_runtime)
    endif()
endfunction()

# Arrow is linked statically, but its compression backends remain DLL
# dependencies on MinGW. Keep them in the shared runtime vendor tree so the
# launcher and the worker resolve the same dependency closure.
function(streamfind_stage_arrow_runtime target)
    if(NOT MINGW OR NOT WIN32 OR NOT TARGET Arrow::arrow_static)
        return()
    endif()
    get_target_property(_arrow_location Arrow::arrow_static IMPORTED_LOCATION_RELEASE)
    if(NOT _arrow_location)
        get_target_property(_arrow_location Arrow::arrow_static IMPORTED_LOCATION)
    endif()
    if(NOT _arrow_location)
        message(FATAL_ERROR "Unable to locate the Arrow static library")
    endif()
    get_filename_component(_arrow_prefix "${_arrow_location}/../.." ABSOLUTE)
    set(_arrow_runtime_dir "${CMAKE_BINARY_DIR}/bin")
    if(NOT EXISTS "${_arrow_prefix}/share/doc/arrow/LICENSE.txt")
        message(FATAL_ERROR "Arrow license file is missing: ${_arrow_prefix}/share/doc/arrow/LICENSE.txt")
    endif()
    install(FILES "${_arrow_prefix}/share/doc/arrow/LICENSE.txt"
        DESTINATION licenses RENAME apache-arrow-LICENSE.txt)
    foreach(_arrow_runtime IN ITEMS
        libbrotlicommon.dll
        libbrotlidec.dll
        libbrotlienc.dll
        libbz2-1.dll
        liblz4.dll
        libzstd.dll
        zlib1.dll)
        if(NOT EXISTS "${_arrow_prefix}/bin/${_arrow_runtime}")
            message(FATAL_ERROR
                "Arrow runtime dependency is missing: ${_arrow_prefix}/bin/${_arrow_runtime}")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_arrow_runtime_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_arrow_prefix}/bin/${_arrow_runtime}" "${_arrow_runtime_dir}")
        install(FILES "${_arrow_prefix}/bin/${_arrow_runtime}"
            DESTINATION bin)
    endforeach()
endfunction()
