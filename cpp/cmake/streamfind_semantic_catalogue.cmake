include_guard(GLOBAL)

function(streamfind_add_core_semantic_catalogue target semantic_dir output_dir)
    file(GLOB semantic_sources CONFIGURE_DEPENDS "${semantic_dir}/*.ttl")
    add_custom_command(
        OUTPUT "${output_dir}/catalogue.json"
               "${output_dir}/capability_matrix.json"
               "${output_dir}/catalogue.duckdb"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
        COMMAND $<TARGET_FILE:streamfind_sdk_catalogue>
            --core-semantic "${semantic_dir}"
            --core-only --catalogue-kind core --domain streamfind
            --output-json "${output_dir}/catalogue.json"
            --output-matrix "${output_dir}/capability_matrix.json"
            --output-db "${output_dir}/catalogue.duckdb"
        DEPENDS "$<TARGET_FILE:streamfind_sdk_catalogue>" ${semantic_sources}
        VERBATIM
    )
    add_custom_target(${target} ALL
        DEPENDS "${output_dir}/catalogue.json"
                "${output_dir}/capability_matrix.json"
                "${output_dir}/catalogue.duckdb")
endfunction()

function(streamfind_add_aggregate_semantic_catalogue target core_dir output_dir)
    set(plugin_dirs ${ARGN})
    set(semantic_sources)
    set(plugin_arguments)
    foreach(plugin_dir IN LISTS plugin_dirs)
        file(GLOB plugin_sources CONFIGURE_DEPENDS "${plugin_dir}/*.ttl")
        list(APPEND semantic_sources ${plugin_sources})
        list(APPEND plugin_arguments --plugin-semantic "${plugin_dir}")
    endforeach()
    add_custom_command(
        OUTPUT "${output_dir}/catalogue.json"
               "${output_dir}/capability_matrix.json"
               "${output_dir}/catalogue.duckdb"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
        COMMAND $<TARGET_FILE:streamfind_sdk_catalogue>
            --core-semantic "${core_dir}"
            --catalogue-kind aggregate
            ${plugin_arguments}
            --output-json "${output_dir}/catalogue.json"
            --output-matrix "${output_dir}/capability_matrix.json"
            --output-db "${output_dir}/catalogue.duckdb"
        DEPENDS "$<TARGET_FILE:streamfind_sdk_catalogue>" ${semantic_sources}
        VERBATIM
    )
    add_custom_target(${target} ALL
        DEPENDS "${output_dir}/catalogue.json"
                "${output_dir}/capability_matrix.json"
                "${output_dir}/catalogue.duckdb")
endfunction()

# Generate the validated semantic catalogue owned by one C++ plugin.
function(streamfind_add_semantic_catalogue target domain semantic_dir)
    file(GLOB semantic_sources CONFIGURE_DEPENDS "${semantic_dir}/*.ttl")
    set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/semantic_catalogue")
    set(output_json "${output_dir}/catalogue.json")
    set(output_matrix "${output_dir}/capability_matrix.json")
    set(output_db "${output_dir}/catalogue.duckdb")
    add_custom_command(
        OUTPUT "${output_json}" "${output_matrix}" "${output_db}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
        COMMAND $<TARGET_FILE:streamfind_sdk_catalogue>
            --core-semantic "${CMAKE_SOURCE_DIR}/core/semantic"
            --plugin-semantic "${semantic_dir}"
            --domain "${domain}"
            --catalogue-kind plugin
            --output-json "${output_json}"
            --output-matrix "${output_matrix}"
            --output-db "${output_db}"
        DEPENDS "$<TARGET_FILE:streamfind_sdk_catalogue>" ${semantic_sources}
        VERBATIM
    )
    add_custom_target(${target}_catalogue ALL
        DEPENDS "${output_json}" "${output_matrix}" "${output_db}")
    add_dependencies(${target} ${target}_catalogue)
    install(FILES "${output_db}" DESTINATION "share/streamfind/plugins/${domain}")
endfunction()
