cmake_minimum_required(VERSION 3.21)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../rust/Cargo.toml" STREAMFIND_CARGO_VERSION_LINE
    REGEX "^version = \"[0-9]+\\.[0-9]+\\.[0-9]+\"$" LIMIT_COUNT 1)
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" STREAMFIND_PROJECT_VERSION
    "${STREAMFIND_CARGO_VERSION_LINE}")
if(NOT STREAMFIND_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "rust/Cargo.toml workspace package version is missing or invalid")
endif()
project(streamfind_cpp VERSION ${STREAMFIND_PROJECT_VERSION} LANGUAGES C CXX)
include(CMakePackageConfigHelpers)

option(STREAMFIND_BUILD_TESTS "Build native tests" ON)
option(STREAMFIND_BUILD_SHARED "Build shared native libraries" OFF)
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
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/streamfind_cpp_library.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/streamfind_semantic_catalogue.cmake)
streamfind_configure_dependencies(${CMAKE_CURRENT_SOURCE_DIR}/vendor)

# Keep the internal DuckDB imported target out of the installed export set.
# Consumers receive a package-facing interface that resolves to the installed
# import library instead of the build-time target name.
add_library(streamfind_duckdb_runtime INTERFACE)
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
    core/src/mcp.cpp
    core/src/catalogue.cpp
    core/src/catalogue_binding.cpp
    core/src/project_table_store.cpp
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
    EXPORT_NAME core
)

# Compile-time install prefix so the catalogue search chain can resolve the
# installed share/streamfind data directory when STREAMFIND_CATALOGUE is unset.
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

target_include_directories(streamfind_cpp_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/include>
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor>
)

if(STREAMFIND_BUILD_SHARED)
    target_compile_definitions(streamfind_cpp_core INTERFACE STREAMFIND_CORE_USING_SHARED)
endif()

install(
    TARGETS streamfind_cpp_core
    EXPORT streamfind-cpp-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(
    TARGETS streamfind_zlib
    EXPORT streamfind-cpp-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(
    DIRECTORY core/include/
    DESTINATION include
)
install(
    DIRECTORY sdk/include/
    DESTINATION include
)
install(
    FILES vendor/nlohmann/json.hpp
    DESTINATION include/nlohmann
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
    DESTINATION lib/cmake/streamfind
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/streamfindConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/streamfindConfig.cmake"
    INSTALL_DESTINATION lib/cmake/streamfind
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
    DESTINATION lib/cmake/streamfind
)

if(STREAMFIND_DUCKDB_RUNTIME AND EXISTS "${STREAMFIND_DUCKDB_RUNTIME}")
    if(WIN32)
        install(FILES "${STREAMFIND_DUCKDB_RUNTIME}" DESTINATION bin)
    else()
        install(FILES "${STREAMFIND_DUCKDB_RUNTIME}" DESTINATION lib)
    endif()
endif()

if(STREAMFIND_DUCKDB_STATIC_LIBRARIES)
    install(FILES ${STREAMFIND_DUCKDB_STATIC_LIBRARIES} DESTINATION lib)
    install(DIRECTORY "${STREAMFIND_DUCKDB_INCLUDE_DIR}/" DESTINATION include)
endif()

if(STREAMFIND_DUCKDB_IMPORT_LIBRARY AND EXISTS "${STREAMFIND_DUCKDB_IMPORT_LIBRARY}")
    install(FILES "${STREAMFIND_DUCKDB_IMPORT_LIBRARY}" DESTINATION lib)
endif()

# catalogue.duckdb is a required installation artifact (the runtime knowledge
# base of the MCP servers); install it next to the DuckDB runtime under share.
install(FILES "${STREAMFIND_SEMANTIC_DIR}/catalogue.duckdb" DESTINATION share/streamfind)
install(FILES "${STREAMFIND_CORE_SEMANTIC_DIR}/catalogue.duckdb" DESTINATION share/streamfind/core)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/core/semantic/"
    DESTINATION share/streamfind/semantic/core)

add_subdirectory(sdk)
add_dependencies(streamfind_framework streamfind_cpp_sdk)
add_subdirectory(plugins)
streamfind_add_aggregate_semantic_catalogue(
    streamfind_aggregate_catalogue
    "${CMAKE_CURRENT_SOURCE_DIR}/core/semantic"
    "${STREAMFIND_SEMANTIC_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/mass_spec/semantic"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/raman/semantic"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/sensors/semantic")
add_dependencies(streamfind_aggregate_catalogue
    streamfind_core_catalogue
    streamfind_cpp_mass_spec_catalogue
    streamfind_cpp_raman_catalogue
    streamfind_cpp_sensors_catalogue)
add_custom_target(streamfind_plugins)
add_dependencies(streamfind_plugins
    streamfind_cpp_mass_spec
    streamfind_cpp_raman
    streamfind_cpp_sensors)

if(STREAMFIND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    add_dependencies(streamfind_mcp_tests streamfind_aggregate_catalogue)
    add_dependencies(streamfind_cpp_mass_spec_interface_tests streamfind_aggregate_catalogue)
    add_dependencies(streamfind_nta_interface_tests streamfind_aggregate_catalogue)
    add_dependencies(streamfind_registration_contract_tests streamfind_aggregate_catalogue)
    add_dependencies(streamfind_project_table_store_manifest_tests streamfind_core_catalogue)
    add_dependencies(streamfind_multiproject_tests streamfind_core_catalogue)
endif()

add_executable(streamfind_mcp mcp/src/streamfind-mcp.cpp mcp/src/static_plugin_composition.cpp)
add_dependencies(streamfind_mcp streamfind_core_catalogue streamfind_aggregate_catalogue)
target_link_libraries(streamfind_mcp PRIVATE streamfind::cpp_core streamfind_cpp_mass_spec streamfind_cpp_raman streamfind_cpp_sensors)
add_executable(streamfind_cli tools/streamfind-cli.cpp)
target_link_libraries(streamfind_cli PRIVATE streamfind::cpp_core streamfind_cpp_mass_spec)

# Runtime dependencies: the exes link vendored DuckDB, OpenBabel, zstd, inchi
# and zlib as static libraries on both platforms, so the only runtime
# dependency shipped next to the binaries is the DuckDB DLL on Windows
# (installed explicitly above from STREAMFIND_DUCKDB_RUNTIME). No dependency
# scan is needed — system DLLs/SOs are the OS's responsibility.
install(
    TARGETS streamfind_mcp streamfind_cli
    EXPORT streamfind-cpp-targets
    RUNTIME DESTINATION bin
)

install(
    TARGETS streamfind_cpp_mass_spec streamfind_cpp_raman streamfind_cpp_sensors
    EXPORT streamfind-cpp-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/plugins/mass_spec/include/"
    DESTINATION include
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/plugins/raman/include/"
    DESTINATION include
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/plugins/sensors/include/"
    DESTINATION include
)

# CPack configuration: set variables BEFORE include(CPack) so they reach the
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
