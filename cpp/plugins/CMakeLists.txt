# Discover each plugin from its own directory. A plugin directory is enabled
# when it contains a CMakeLists.txt; the directory name is its canonical plugin
# and its CMake target follows streamfind_<plugin>_plugin.
file(GLOB STREAMFIND_PLUGIN_DIRECTORIES
    CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_LIST_DIR}"
    LIST_DIRECTORIES true
    "${CMAKE_CURRENT_LIST_DIR}/*")
list(SORT STREAMFIND_PLUGIN_DIRECTORIES)

set(STREAMFIND_ENABLED_PLUGINS "ALL" CACHE STRING
    "Semicolon-separated plugin plugins to build, or ALL")
string(TOUPPER "${STREAMFIND_ENABLED_PLUGINS}" _enabled_plugins_upper)
set(_build_all_plugins FALSE)
if(_enabled_plugins_upper STREQUAL "ALL" OR STREAMFIND_ENABLED_PLUGINS STREQUAL "")
    set(_build_all_plugins TRUE)
endif()

set(STREAMFIND_PLUGIN_DOMAINS)
set(STREAMFIND_PLUGIN_TARGETS)
set(STREAMFIND_PLUGIN_SEMANTIC_DIRS)
foreach(_plugin_plugin IN LISTS STREAMFIND_PLUGIN_DIRECTORIES)
    if(NOT IS_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/${_plugin_plugin}" OR
       NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${_plugin_plugin}/CMakeLists.txt")
        continue()
    endif()
    if(NOT _build_all_plugins AND
       NOT _plugin_plugin IN_LIST STREAMFIND_ENABLED_PLUGINS)
        continue()
    endif()
    set(STREAMFIND_BUNDLED_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/manifests/${_plugin_plugin}/plugin.json")
    configure_file("${_plugin_plugin}/plugin.json.in" "${STREAMFIND_BUNDLED_MANIFEST}" @ONLY)
    add_subdirectory("${_plugin_plugin}")
    set(_plugin_target "streamfind_${_plugin_plugin}_plugin")
    if(NOT TARGET ${_plugin_target})
        message(FATAL_ERROR
            "Plugin ${_plugin_plugin} must define ${_plugin_target}")
    endif()
    set_target_properties(${_plugin_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/native/native.plugin.${_plugin_plugin}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/native/native.plugin.${_plugin_plugin}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/native/native.plugin.${_plugin_plugin}")
    if(UNIX AND NOT APPLE)
        # Native plugins are installed three directories below the package
        # root; resolve their shared runtime dependencies from package/bin.
        set_target_properties(${_plugin_target} PROPERTIES
            INSTALL_RPATH "$ORIGIN/../../../bin")
    endif()
    target_compile_definitions(${_plugin_target} PRIVATE STREAMFIND_BUNDLED_PLUGIN_VERSION="${PROJECT_VERSION}")
    set_property(TARGET ${_plugin_target} APPEND PROPERTY LINK_DEPENDS "${STREAMFIND_BUNDLED_MANIFEST}")
    list(APPEND STREAMFIND_PLUGIN_DOMAINS "${_plugin_plugin}")
    list(APPEND STREAMFIND_PLUGIN_TARGETS "${_plugin_target}")
    list(APPEND STREAMFIND_PLUGIN_SEMANTIC_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/${_plugin_plugin}/semantic")
endforeach()

if(NOT STREAMFIND_PLUGIN_DOMAINS)
    message(FATAL_ERROR
        "No enabled C++ plugins were discovered. STREAMFIND_ENABLED_PLUGINS='${STREAMFIND_ENABLED_PLUGINS}'")
endif()

set(STREAMFIND_PLUGIN_DOMAINS "${STREAMFIND_PLUGIN_DOMAINS}" PARENT_SCOPE)
set(STREAMFIND_PLUGIN_TARGETS "${STREAMFIND_PLUGIN_TARGETS}" PARENT_SCOPE)
set(STREAMFIND_PLUGIN_SEMANTIC_DIRS "${STREAMFIND_PLUGIN_SEMANTIC_DIRS}" PARENT_SCOPE)
