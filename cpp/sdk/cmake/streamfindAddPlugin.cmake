include_guard(GLOBAL)

function(streamfind_add_plugin target domain_id)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "streamfind_add_plugin: target '${target}' does not exist")
    endif()
    if(NOT domain_id)
        message(FATAL_ERROR "streamfind_add_plugin: domain_id is required")
    endif()

    target_link_libraries("${target}" PRIVATE streamfind::cpp_sdk)
    set_target_properties("${target}" PROPERTIES STREAMFIND_DOMAIN_ID "${domain_id}")
endfunction()
