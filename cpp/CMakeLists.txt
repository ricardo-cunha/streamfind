cmake_minimum_required(VERSION 3.21)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../rust/Cargo.toml" STREAMFIND_CARGO_VERSION_LINE
    REGEX "^version = \"[0-9]+\\.[0-9]+\\.[0-9]+\"$" LIMIT_COUNT 1)
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" STREAMFIND_PROJECT_VERSION
    "${STREAMFIND_CARGO_VERSION_LINE}")
if(NOT STREAMFIND_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "rust/Cargo.toml workspace package version is missing or invalid")
endif()
project(streamfind_cpp VERSION ${STREAMFIND_PROJECT_VERSION} LANGUAGES C CXX)
if(WIN32)
    enable_language(RC)
endif()
include(CMakePackageConfigHelpers)

option(STREAMFIND_BUILD_TESTS "Build native tests" ON)
option(STREAMFIND_BUILD_SHARED "Build shared native libraries" OFF)
option(STREAMFIND_STATIC_MSVC_RUNTIME "Link the MSVC runtime statically" OFF)
set(STREAMFIND_APP_DIR "" CACHE PATH "Built frontend directory to package")
set(STREAMFIND_MINGW_RUNTIME_DIR "" CACHE PATH
    "MinGW runtime directory to package for Windows releases")


if(MSVC AND STREAMFIND_STATIC_MSVC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        CACHE STRING "MSVC runtime library" FORCE)
endif()
if(STREAMFIND_BUILD_SHARED)
    set(STREAMFIND_CPP_LIBRARY_TYPE SHARED)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
else()
    set(STREAMFIND_CPP_LIBRARY_TYPE STATIC)
endif()

if(STREAMFIND_BUILD_TESTS)
    enable_testing()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/core/cmake/streamfind_dependencies.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/core/cmake/streamfind_openbabel.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/streamfind_cpp_library.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/streamfind_semantic_catalogue.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/streamfind_plugin_validation.cmake)
streamfind_configure_dependencies(${CMAKE_CURRENT_SOURCE_DIR}/vendor)
streamfind_add_openbabel(${CMAKE_CURRENT_SOURCE_DIR}/vendor)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/vendor/isospec EXCLUDE_FROM_ALL)

add_library(streamfind_openbabel_runtime SHARED
    core/src/vendors/openbabel/openbabel_c_api.cpp)
set_target_properties(streamfind_openbabel_runtime PROPERTIES
    OUTPUT_NAME "openbabel_streamfind"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)
if(WIN32)
    set_target_properties(streamfind_openbabel_runtime PROPERTIES PREFIX "")
endif()
target_compile_definitions(streamfind_openbabel_runtime PRIVATE
    streamfind_OPENBABEL_BUILD_DLL
    NOMINMAX)
target_include_directories(streamfind_openbabel_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/core/src/vendors/openbabel"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pugixml-1.14/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/openbabel/openbabel-3-2-0/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/simdutf")
target_link_libraries(streamfind_openbabel_runtime PRIVATE streamfind::openbabel)
if(WIN32)
    # The service loads the development runtime from the packaged vendor
    # layout. Keep that copy synchronized whenever the runtime DLL itself is
    # rebuilt; plugin post-build hooks do not run when only this target changes.
    set(_streamfind_openbabel_dev_dir "${CMAKE_BINARY_DIR}/core/vendors/openbabel")
    add_custom_command(TARGET streamfind_openbabel_runtime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_streamfind_openbabel_dev_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:streamfind_openbabel_runtime>"
            "${_streamfind_openbabel_dev_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${STREAMFIND_OPENBABEL_DATA_DIR}"
            "${_streamfind_openbabel_dev_dir}/data")
endif()
if(MSVC)
    target_link_options(streamfind_openbabel_runtime PRIVATE
        "/WHOLEARCHIVE:$<TARGET_FILE:streamfind_openbabel>")
elseif(UNIX OR MINGW)
    target_link_options(streamfind_openbabel_runtime PRIVATE
        "-Wl,--whole-archive"
        "$<TARGET_FILE:streamfind_openbabel>"
        "-Wl,--no-whole-archive")
endif()

# Keep the internal DuckDB imported target out of the installed export set.
# Consumers receive a package-facing interface that resolves to the installed
# import library instead of the build-time target name.
add_library(streamfind_duckdb_runtime INTERFACE)
if(STREAMFIND_DUCKDB_STATIC_LIBRARIES)
    set(STREAMFIND_DUCKDB_INSTALL_LINKS
        "$<INSTALL_PREFIX>/sdk/lib/${CMAKE_STATIC_LIBRARY_PREFIX}duckdb_static${CMAKE_STATIC_LIBRARY_SUFFIX}")
    foreach(_duckdb_archive IN LISTS STREAMFIND_DUCKDB_STATIC_LIBRARIES)
        get_filename_component(_duckdb_archive_name "${_duckdb_archive}" NAME)
        list(APPEND STREAMFIND_DUCKDB_INSTALL_LINKS "$<INSTALL_PREFIX>/sdk/lib/${_duckdb_archive_name}")
    endforeach()
    target_link_libraries(streamfind_duckdb_runtime INTERFACE
        $<BUILD_INTERFACE:streamfind::duckdb>
        $<INSTALL_INTERFACE:-Wl,--start-group>
        $<INSTALL_INTERFACE:${STREAMFIND_DUCKDB_INSTALL_LINKS}>
        $<INSTALL_INTERFACE:-Wl,--end-group>
        $<INSTALL_INTERFACE:-pthread>
        $<INSTALL_INTERFACE:${CMAKE_DL_LIBS}>
    )
else()
    target_link_libraries(streamfind_duckdb_runtime INTERFACE
        $<BUILD_INTERFACE:streamfind::duckdb>
        $<INSTALL_INTERFACE:$<IF:$<PLATFORM_ID:Windows>,
            $<INSTALL_PREFIX>/lib/duckdb.lib,
            $<IF:$<PLATFORM_ID:Darwin>,
                $<INSTALL_PREFIX>/lib/libduckdb.dylib,
                $<INSTALL_PREFIX>/lib/libduckdb.so
            >
        >>
    )
endif()
add_library(streamfind::duckdb_runtime ALIAS streamfind_duckdb_runtime)

set(STREAMFIND_SEMANTIC_DIR "${CMAKE_BINARY_DIR}/semantic_catalogue")
file(MAKE_DIRECTORY "${STREAMFIND_SEMANTIC_DIR}")
set(STREAMFIND_CORE_SEMANTIC_DIR "${STREAMFIND_SEMANTIC_DIR}/core")
streamfind_add_core_semantic_catalogue(
    streamfind_core_catalogue
    "${CMAKE_CURRENT_SOURCE_DIR}/core/semantic"
    "${STREAMFIND_CORE_SEMANTIC_DIR}")


# Version single-source-of-truth: project(VERSION) drives a generated
# src/version.cpp so the library, the MCP server, and the release archives all
# report the same version.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/core/src/version.cpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.cpp
    @ONLY
)

set(STREAMFIND_CORE_SOURCES
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.cpp
    core/src/api.cpp
    core/src/project.cpp
    core/src/fingerprint.cpp
    core/src/mcp.cpp
    core/src/catalogue.cpp
    core/src/catalogue_binding.cpp
    core/src/project_table_store.cpp
    core/src/operations/operations.cpp
    core/src/plugin_loader.cpp
    core/src/plugin_configuration.cpp
    core/src/vendor_runtime.cpp
    core/src/vendors/openbabel/openbabel_adapter.cpp
)

add_library(streamfind_cpp_core ${STREAMFIND_CPP_LIBRARY_TYPE} ${STREAMFIND_CORE_SOURCES})
if(STREAMFIND_BUILD_SHARED)
    target_compile_definitions(streamfind_cpp_core PRIVATE STREAMFIND_CORE_BUILDING_SHARED)
endif()

add_library(streamfind::cpp_core ALIAS streamfind_cpp_core)
add_custom_target(streamfind_framework)
add_dependencies(streamfind_framework streamfind_cpp_core)

set_target_properties(streamfind_cpp_core PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME core
)

# Compile-time install prefix so the catalogue search chain can resolve the
# installed core data directory when STREAMFIND_CATALOGUE is unset.
target_compile_definitions(streamfind_cpp_core PRIVATE
    STREAMFIND_INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}"
    STREAMFIND_FRAMEWORK_VERSION=\"${PROJECT_VERSION}\"
)

target_compile_features(streamfind_cpp_core PUBLIC cxx_std_20)

target_link_libraries(streamfind_cpp_core
    PUBLIC
        streamfind::duckdb_runtime
        streamfind::zlib
)
if(NOT WIN32)
    target_link_libraries(streamfind_cpp_core PRIVATE streamfind_openbabel_runtime)
endif()

target_include_directories(streamfind_cpp_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/include>
        $<INSTALL_INTERFACE:sdk/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor>
)

if(STREAMFIND_BUILD_SHARED)
    target_compile_definitions(streamfind_cpp_core INTERFACE STREAMFIND_CORE_USING_SHARED)
endif()

install(
    TARGETS streamfind_cpp_core
    EXPORT streamfind-cpp-targets
    ARCHIVE DESTINATION sdk/lib
    LIBRARY DESTINATION sdk/lib
    RUNTIME DESTINATION bin
)

install(
    TARGETS streamfind_zlib
    EXPORT streamfind-cpp-targets
    ARCHIVE DESTINATION sdk/lib
    LIBRARY DESTINATION sdk/lib
    RUNTIME DESTINATION bin
)

install(
    TARGETS streamfind_inchi streamfind_openbabel
    EXPORT streamfind-cpp-targets
    ARCHIVE DESTINATION sdk/lib
    LIBRARY DESTINATION sdk/lib
    RUNTIME DESTINATION bin
)
install(TARGETS streamfind_openbabel_runtime
    EXPORT streamfind-cpp-targets
    RUNTIME DESTINATION core/vendors/openbabel
    LIBRARY DESTINATION core/vendors/openbabel
    ARCHIVE DESTINATION sdk/lib)
install(DIRECTORY "${STREAMFIND_OPENBABEL_DATA_DIR}/"
    DESTINATION core/vendors/openbabel/data)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/openbabel/openbabel-3-2-0/COPYING"
    DESTINATION licenses RENAME openbabel-COPYING.txt)

install(
    DIRECTORY core/include/
    DESTINATION sdk/include
)
install(
    DIRECTORY sdk/include/
    DESTINATION sdk/include
)
install(
    FILES vendor/nlohmann/json.hpp
    DESTINATION sdk/include/nlohmann
)

install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/../NOTICE.md"
    DESTINATION .
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/duckdb/LICENSE"
    DESTINATION licenses RENAME duckdb-LICENSE.txt)

install(TARGETS streamfind_duckdb_runtime
    EXPORT streamfind-cpp-targets)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/zlib/zlib-develop/LICENSE"
    DESTINATION licenses RENAME zlib-LICENSE.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pugixml-1.14/LICENSE"
    DESTINATION licenses RENAME pugixml-LICENSE.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/simdutf/LICENSE"
    DESTINATION licenses RENAME simdutf-LICENSE.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/nlohmann/LICENSE"
    DESTINATION licenses RENAME nlohmann-json-LICENSE.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/json-schema-validator/LICENSE"
    DESTINATION licenses RENAME json-schema-validator-LICENSE.txt)

install(
    EXPORT streamfind-cpp-targets
    FILE streamfind-cpp-targets.cmake
    NAMESPACE streamfind::
    DESTINATION sdk/cmake/streamfind
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/streamfindConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/streamfindConfig.cmake"
    INSTALL_DESTINATION sdk/cmake/streamfind
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/streamfindConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/streamfindConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/streamfindConfigVersion.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/streamfindAddPlugin.cmake"
    DESTINATION sdk/cmake/streamfind
)

if(STREAMFIND_DUCKDB_RUNTIME AND EXISTS "${STREAMFIND_DUCKDB_RUNTIME}")
    if(WIN32)
        install(FILES "${STREAMFIND_DUCKDB_RUNTIME}" DESTINATION core/vendors/duckdb)
    else()
        install(FILES "${STREAMFIND_DUCKDB_RUNTIME}" DESTINATION sdk/lib)
    endif()
endif()

if(STREAMFIND_DUCKDB_STATIC_LIBRARIES)
    install(FILES ${STREAMFIND_DUCKDB_STATIC_LIBRARIES} DESTINATION sdk/lib)
    install(DIRECTORY "${STREAMFIND_DUCKDB_INCLUDE_DIR}/" DESTINATION sdk/include)
endif()

if(MINGW AND WIN32)
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
        install(FILES "${_mingw_runtime_path}" DESTINATION core/vendors/mingw)
    endforeach()
endif()

if(STREAMFIND_DUCKDB_IMPORT_LIBRARY AND EXISTS "${STREAMFIND_DUCKDB_IMPORT_LIBRARY}")
    install(FILES "${STREAMFIND_DUCKDB_IMPORT_LIBRARY}" DESTINATION sdk/lib)
endif()

# catalogue.duckdb is a required installation artifact (the runtime knowledge
# base of the MCP servers); install it in the package core directory.
install(FILES "${STREAMFIND_SEMANTIC_DIR}/catalogue.duckdb" DESTINATION core)
install(FILES "${STREAMFIND_CORE_SEMANTIC_DIR}/catalogue.duckdb" DESTINATION core RENAME core-catalogue.duckdb)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/core/semantic/"
    DESTINATION core/ontology)

add_subdirectory(sdk)
add_dependencies(streamfind_framework streamfind_cpp_sdk)
add_subdirectory(plugins)
streamfind_add_aggregate_semantic_catalogue(
    streamfind_aggregate_catalogue
    "${CMAKE_CURRENT_SOURCE_DIR}/core/semantic"
    "${STREAMFIND_SEMANTIC_DIR}"
    ${STREAMFIND_PLUGIN_SEMANTIC_DIRS})
add_dependencies(streamfind_aggregate_catalogue
    streamfind_core_catalogue
    ${STREAMFIND_PLUGIN_TARGETS})
add_custom_target(streamfind_plugins ALL)
add_dependencies(streamfind_plugins ${STREAMFIND_PLUGIN_TARGETS})

if(STREAMFIND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    add_dependencies(streamfind_project_table_store_manifest_contract streamfind_core_catalogue)
    add_dependencies(streamfind_project_isolation_contract streamfind_core_catalogue)
endif()

function(streamfind_static_mingw_executable target)
    if(MINGW)
        target_link_options(${target} PRIVATE
            -static-libgcc
            -static-libstdc++
            -static)
    endif()
endfunction()

add_executable(streamfind_mcp core/src/streamfind-mcp.cpp)
add_dependencies(streamfind_mcp streamfind_aggregate_catalogue streamfind_cpp_sdk
    streamfind_plugins)
target_link_libraries(streamfind_mcp PRIVATE streamfind::cpp_core streamfind::cpp_sdk)
if(WIN32)
    add_executable(streamfind_mcp_launcher core/src/streamfind-mcp-launcher.cpp)
    set_target_properties(streamfind_mcp_launcher PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    streamfind_static_mingw_executable(streamfind_mcp_launcher)
endif()
set(STREAMFIND_ENABLED_PLUGINS_JSON)
foreach(_plugin_domain IN LISTS STREAMFIND_PLUGIN_DOMAINS)
    if(STREAMFIND_ENABLED_PLUGINS_JSON)
        string(APPEND STREAMFIND_ENABLED_PLUGINS_JSON ", ")
    endif()
    string(APPEND STREAMFIND_ENABLED_PLUGINS_JSON "\"${_plugin_domain}\"")
endforeach()
file(GENERATE
    OUTPUT "$<TARGET_FILE_DIR:streamfind_mcp>/streamfind.json"
    CONTENT "{\n  \"plugin_roots\": [\"${CMAKE_BINARY_DIR}/plugins\"],\n  \"enabled_plugins\": [${STREAMFIND_ENABLED_PLUGINS_JSON}]\n}\n")
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/streamfind-install.json"
    CONTENT "{\n  \"plugin_roots\": [\"../plugins\"],\n  \"enabled_plugins\": [${STREAMFIND_ENABLED_PLUGINS_JSON}]\n}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/streamfind-install.json"
    DESTINATION bin RENAME streamfind.json)
add_executable(streamfind_cli
    core/src/streamfind-mcp.cpp)
add_dependencies(streamfind_cli streamfind_aggregate_catalogue streamfind_cpp_sdk
    streamfind_plugins)
target_link_libraries(streamfind_cli PRIVATE streamfind::cpp_core streamfind::cpp_sdk)

add_executable(streamfind_service
    service/src/streamfind-service.cpp
    service/src/service_server.cpp
    service/src/service_plugin_runtime.cpp
    service/src/project_runtime_manager.cpp)
add_dependencies(streamfind_service streamfind_aggregate_catalogue streamfind_cpp_sdk
    streamfind_plugins)
target_link_libraries(streamfind_service PRIVATE streamfind::cpp_core streamfind::cpp_sdk)
target_include_directories(streamfind_service PRIVATE
    service/include
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/isospec/src/IsoSpec++")
target_link_libraries(streamfind_service PRIVATE IsoSpec++-static)
if(WIN32)
    target_link_libraries(streamfind_service PRIVATE ws2_32 comdlg32 shell32 ole32)
endif()
set_target_properties(streamfind_service PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_executable(streamfind_service_host
        service/src/streamfind-service-host.cpp)
    target_sources(streamfind_service_host PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/streamfind.rc")
    add_dependencies(streamfind_service_host streamfind_service)
    target_link_libraries(streamfind_service_host PRIVATE ws2_32 shell32)
    set_target_properties(streamfind_service_host PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON OUTPUT_NAME streamfind)
    streamfind_static_mingw_executable(streamfind_service_host)
else()
    add_executable(streamfind_service_host
        service/src/streamfind-service-launcher.cpp)
    add_dependencies(streamfind_service_host streamfind_service)
    set_target_properties(streamfind_service_host PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        OUTPUT_NAME streamfind)
endif()
streamfind_static_mingw_executable(streamfind_mcp)
streamfind_static_mingw_executable(streamfind_cli)
streamfind_static_mingw_executable(streamfind_service)

if(MSVC)
    foreach(_target IN ITEMS
        streamfind_mcp
        streamfind_cli
        streamfind_service
        streamfind_mass_spec_plugin
        streamfind_raman_plugin
        streamfind_sensors_plugin)
        if(TARGET ${_target})
            target_link_options(${_target} PRIVATE "/DELAYLOAD:duckdb.dll")
            target_link_libraries(${_target} PRIVATE delayimp)
        endif()
    endforeach()
endif()

# Vendor runtime libraries are installed under core/vendors and are resolved
# through the core runtime path setup before delayed imports or plugins load.
install(
    TARGETS streamfind_mcp streamfind_cli streamfind_service
    EXPORT streamfind-cpp-targets
    RUNTIME DESTINATION bin
)
if(WIN32)
    install(TARGETS streamfind_service_host
        EXPORT streamfind-cpp-targets
        RUNTIME DESTINATION .)
    install(TARGETS streamfind_mcp_launcher RUNTIME DESTINATION bin)
else()
    install(TARGETS streamfind_service_host
        EXPORT streamfind-cpp-targets
        RUNTIME DESTINATION .)
endif()
if(STREAMFIND_APP_DIR AND EXISTS "${STREAMFIND_APP_DIR}")
    install(DIRECTORY "${STREAMFIND_APP_DIR}/" DESTINATION app)
endif()

# cached defaults CPack::Configure writes into CPackConfig.cmake.
set(CPACK_PACKAGE_NAME "streamfind-core-cpp")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_FILE_NAME "streamfind-core-cpp-${PROJECT_VERSION}-Windows-x86_64")
else()
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_PACKAGE_FILE_NAME "streamfind-core-cpp-${PROJECT_VERSION}-Linux-${CMAKE_SYSTEM_PROCESSOR}")
endif()
include(CPack)
# Dev-tree catalogue: keep catalogue.duckdb next to the built MCP server so the
# search chain (env -> next-to-binary -> install data dir) resolves in builds.
add_custom_command(TARGET streamfind_mcp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STREAMFIND_SEMANTIC_DIR}/catalogue.duckdb"
        "$<TARGET_FILE_DIR:streamfind_mcp>"
)
if(STREAMFIND_DUCKDB_RUNTIME)
    add_custom_command(TARGET streamfind_service POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STREAMFIND_SEMANTIC_DIR}/catalogue.duckdb"
            "$<TARGET_FILE_DIR:streamfind_service>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STREAMFIND_DUCKDB_RUNTIME}"
            "$<TARGET_FILE_DIR:streamfind_service>"
        COMMENT "Staging streamfind_service runtime dependencies"
    )
else()
    add_custom_command(TARGET streamfind_service POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STREAMFIND_SEMANTIC_DIR}/catalogue.duckdb"
            "$<TARGET_FILE_DIR:streamfind_service>"
        COMMENT "Staging streamfind_service runtime dependencies"
    )
endif()
