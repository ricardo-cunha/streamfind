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
