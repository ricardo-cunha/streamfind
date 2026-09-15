# Discover each plugin from its own directory. A plugin directory is enabled
# when it contains a CMakeLists.txt; the directory name is its canonical domain
# and its CMake target follows streamfind_<domain>_plugin.
file(GLOB STREAMFIND_PLUGIN_DIRECTORIES
    RELATIVE "${CMAKE_CURRENT_LIST_DIR}"
    LIST_DIRECTORIES true
    "${CMAKE_CURRENT_LIST_DIR}/*")
list(SORT STREAMFIND_PLUGIN_DIRECTORIES)

set(STREAMFIND_PLUGIN_DOMAINS)
set(STREAMFIND_PLUGIN_TARGETS)
set(STREAMFIND_PLUGIN_SEMANTIC_DIRS)
foreach(_plugin_domain IN LISTS STREAMFIND_PLUGIN_DIRECTORIES)
    if(NOT IS_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/${_plugin_domain}" OR
       NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${_plugin_domain}/CMakeLists.txt")
        continue()
    endif()
    add_subdirectory("${_plugin_domain}")
    set(_plugin_target "streamfind_${_plugin_domain}_plugin")
    if(NOT TARGET ${_plugin_target})
        message(FATAL_ERROR
            "Plugin ${_plugin_domain} must define ${_plugin_target}")
    endif()
    list(APPEND STREAMFIND_PLUGIN_DOMAINS "${_plugin_domain}")
    list(APPEND STREAMFIND_PLUGIN_TARGETS "${_plugin_target}")
    list(APPEND STREAMFIND_PLUGIN_SEMANTIC_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/${_plugin_domain}/semantic")
endforeach()

if(NOT STREAMFIND_PLUGIN_DOMAINS)
    message(FATAL_ERROR "No C++ plugins were discovered under ${CMAKE_CURRENT_LIST_DIR}")
endif()

set(STREAMFIND_PLUGIN_DOMAINS "${STREAMFIND_PLUGIN_DOMAINS}" PARENT_SCOPE)
set(STREAMFIND_PLUGIN_TARGETS "${STREAMFIND_PLUGIN_TARGETS}" PARENT_SCOPE)
set(STREAMFIND_PLUGIN_SEMANTIC_DIRS "${STREAMFIND_PLUGIN_SEMANTIC_DIRS}" PARENT_SCOPE)
