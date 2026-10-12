include_guard(GLOBAL)

# Apply the common build/package contract shared by SDK and plugin libraries.
function(streamfind_configure_cpp_library target export_name)
    set(options)
    set(one_value_args BUILD_DEFINE USING_DEFINE)
    cmake_parse_arguments(ARG "" "BUILD_DEFINE;USING_DEFINE" "" ${ARGN})

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        EXPORT_NAME ${export_name}
        POSITION_INDEPENDENT_CODE ON
    )
    if(UNIX AND NOT APPLE)
        set_target_properties(${target} PROPERTIES
            INSTALL_RPATH "$ORIGIN/../../core/vendors/openbabel")
    endif()
    target_compile_features(${target} PUBLIC cxx_std_20)

    if(STREAMFIND_BUILD_SHARED)
        if(ARG_BUILD_DEFINE)
            target_compile_definitions(${target} PRIVATE ${ARG_BUILD_DEFINE})
        endif()
        if(ARG_USING_DEFINE)
            target_compile_definitions(${target} INTERFACE ${ARG_USING_DEFINE})
        endif()
    endif()
endfunction()

# Keep MinGW's compiler runtimes self-contained without forcing every imported
# dependency to have a static archive. DuckDB and other shared dependencies are
# staged by their owning target/package rules below. libwinpthread is selected
# explicitly because -static-libgcc and -static-libstdc++ do not include it.
function(streamfind_static_mingw_executable target)
    if(MINGW)
        target_link_options(${target} PRIVATE
            -static-libgcc
            -static-libstdc++)
        target_link_libraries(${target} PRIVATE
            "-Wl,-Bstatic"
            "-Wl,--whole-archive"
            winpthread
            "-Wl,--no-whole-archive"
            "-Wl,-Bdynamic")
    endif()
endfunction()
