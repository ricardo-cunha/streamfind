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
    )
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
