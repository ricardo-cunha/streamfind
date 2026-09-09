cmake_minimum_required(VERSION 3.21)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../rust/Cargo.toml" STREAMFIND_CARGO_VERSION_LINE
    REGEX "^version = \"[0-9]+\\.[0-9]+\\.[0-9]+\"$" LIMIT_COUNT 1)
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" STREAMFIND_PROJECT_VERSION
    "${STREAMFIND_CARGO_VERSION_LINE}")
if(NOT STREAMFIND_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "rust/Cargo.toml workspace package version is missing or invalid")
endif()
project(streamfind_core VERSION ${STREAMFIND_PROJECT_VERSION} LANGUAGES C CXX)

option(STREAMFIND_BUILD_TESTS "Build native tests" ON)
option(STREAMFIND_BUILD_SHARED "Build shared native libraries" OFF)

if(STREAMFIND_BUILD_TESTS)
    enable_testing()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/streamfind_dependencies.cmake)
streamfind_configure_dependencies(${CMAKE_CURRENT_SOURCE_DIR}/vendor)

set(STREAMFIND_SEMANTIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../semantic/generated")


# Version single-source-of-truth: project(VERSION) drives a generated
# src/version.cpp so the library, the MCP server, and the release archives all
# report the same version.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/version.cpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.cpp
    @ONLY
)

set(STREAMFIND_CORE_SOURCES
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.cpp
    src/api.cpp
    src/project.cpp
    src/mcp.cpp
    src/catalogue.cpp
    src/catalogue_binding.cpp
    src/project_table_store.cpp
    src/tools_resolver.cpp
)

if(STREAMFIND_BUILD_SHARED)
    add_library(streamfind_core SHARED ${STREAMFIND_CORE_SOURCES})
    target_compile_definitions(streamfind_core PRIVATE STREAMFIND_CORE_BUILDING_SHARED)
else()
    add_library(streamfind_core STATIC ${STREAMFIND_CORE_SOURCES})
endif()

add_library(streamfind::core ALIAS streamfind_core)

set_target_properties(streamfind_core PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    EXPORT_NAME core
)

# Compile-time install prefix so the catalogue search chain can resolve the
# installed share/streamfind data directory when STREAMFIND_CATALOGUE is unset.
target_compile_definitions(streamfind_core PRIVATE
    STREAMFIND_INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}"
    STREAMFIND_FRAMEWORK_VERSION=\"${PROJECT_VERSION}\"
)

target_compile_features(streamfind_core PUBLIC cxx_std_20)

target_link_libraries(streamfind_core
    PUBLIC
        streamfind::duckdb
        streamfind::openbabel
        streamfind::zlib
)
if(UNIX)
    target_link_options(streamfind_core INTERFACE
        "-Wl,--whole-archive"
        "$<TARGET_FILE:streamfind_openbabel>"
        "-Wl,--no-whole-archive"
    )
endif()

target_include_directories(streamfind_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor>
)

if(STREAMFIND_BUILD_SHARED)
    target_compile_definitions(streamfind_core INTERFACE STREAMFIND_CORE_USING_SHARED)
endif()

install(
    TARGETS streamfind_core
    EXPORT streamfind-core-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

if(STREAMFIND_DUCKDB_STATIC_LIBRARIES)
    install(TARGETS streamfind_duckdb EXPORT streamfind-core-targets)
endif()

install(
    TARGETS streamfind_inchi streamfind_openbabel streamfind_zlib
    EXPORT streamfind-core-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(
    DIRECTORY include/
    DESTINATION include
)

install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/../NOTICE.md"
    DESTINATION .
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/duckdb/LICENSE"
    DESTINATION licenses RENAME duckdb-LICENSE.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/openbabel/openbabel-3-2-0/COPYING"
    DESTINATION licenses RENAME openbabel-COPYING.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/zstd/LICENSE"
    DESTINATION licenses RENAME zstd-LICENSE.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/zstd/COPYING"
    DESTINATION licenses RENAME zstd-COPYING.txt)
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
    EXPORT streamfind-core-targets
    FILE streamfind-core-targets.cmake
    NAMESPACE streamfind::
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

add_subdirectory(domains)

install(
    DIRECTORY "${STREAMFIND_OPENBABEL_DATA_DIR}/"
    DESTINATION share/streamfind/openbabel/data
)

if(STREAMFIND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

add_executable(streamfind_mcp tools/streamfind-mcp.cpp)
target_link_libraries(streamfind_mcp PRIVATE streamfind::core streamfind_mass_spec streamfind_raman streamfind_sensors)
add_executable(streamfind_cli tools/streamfind-cli.cpp)
target_link_libraries(streamfind_cli PRIVATE streamfind::core)

# Runtime dependencies: the exes link vendored DuckDB, OpenBabel, zstd, inchi
# and zlib as static libraries on both platforms, so the only runtime
# dependency shipped next to the binaries is the DuckDB DLL on Windows
# (installed explicitly above from STREAMFIND_DUCKDB_RUNTIME). No dependency
# scan is needed — system DLLs/SOs are the OS's responsibility.
install(
    TARGETS streamfind_mcp streamfind_cli
    EXPORT streamfind-core-targets
    RUNTIME DESTINATION bin
)

install(
    TARGETS streamfind_mass_spec streamfind_raman streamfind_sensors
    EXPORT streamfind-core-targets
    ARCHIVE DESTINATION lib
)

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/domains/mass_spec/include/"
    DESTINATION include
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/domains/raman/include/"
    DESTINATION include
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/domains/sensors/include/"
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
