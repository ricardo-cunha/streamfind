include_guard(GLOBAL)

function(streamfind_add_plugin target plugin_id)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "streamfind_add_plugin: target '${target}' does not exist")
    endif()
    if(NOT plugin_id)
        message(FATAL_ERROR "streamfind_add_plugin: plugin_id is required")
    endif()

    target_link_libraries("${target}" PRIVATE streamfind::sdk)
    set_target_properties("${target}" PROPERTIES STREAMFIND_PLUGIN_ID "${plugin_id}")
endfunction()
