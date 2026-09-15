add_compile_definitions(
    STREAMFIND_TEST_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)

add_executable(streamfind_project_lifecycle_contract unit/project_lifecycle_contract.cpp)
target_link_libraries(streamfind_project_lifecycle_contract PRIVATE streamfind::cpp_core)
add_test(NAME streamfind_project_lifecycle_contract COMMAND streamfind_project_lifecycle_contract)
set_tests_properties(streamfind_project_lifecycle_contract PROPERTIES LABELS "framework")

add_executable(streamfind_plugin_abi_contract unit/plugin_abi_contract.cpp)
target_link_libraries(streamfind_plugin_abi_contract PRIVATE streamfind::cpp_sdk)
add_test(NAME streamfind_plugin_abi_contract COMMAND streamfind_plugin_abi_contract)
set_tests_properties(streamfind_plugin_abi_contract PROPERTIES LABELS "framework")

add_executable(streamfind_plugin_manifest_validation unit/plugin_manifest_validation.cpp)
target_link_libraries(streamfind_plugin_manifest_validation PRIVATE streamfind::cpp_sdk)
add_test(NAME streamfind_plugin_manifest_validation COMMAND streamfind_plugin_manifest_validation)
set_tests_properties(streamfind_plugin_manifest_validation PROPERTIES LABELS "framework")

add_executable(streamfind_plugin_loader_failure_contract unit/plugin_loader_failure_contract.cpp)
target_link_libraries(streamfind_plugin_loader_failure_contract PRIVATE streamfind::cpp_core)
add_test(NAME streamfind_plugin_loader_failure_contract COMMAND streamfind_plugin_loader_failure_contract)
set_tests_properties(streamfind_plugin_loader_failure_contract PROPERTIES LABELS "framework")

add_executable(streamfind_plugin_configuration_validation unit/plugin_configuration_validation.cpp)
target_link_libraries(streamfind_plugin_configuration_validation PRIVATE streamfind::cpp_core)
add_test(NAME streamfind_plugin_configuration_validation COMMAND streamfind_plugin_configuration_validation)
set_tests_properties(streamfind_plugin_configuration_validation PROPERTIES LABELS "framework")

add_executable(streamfind_dynamic_plugin_package_contract unit/dynamic_plugin_package_contract.cpp)
target_link_libraries(streamfind_dynamic_plugin_package_contract PRIVATE streamfind::cpp_sdk)

add_executable(streamfind_dynamic_schema_contract unit/dynamic_schema_contract.cpp)
target_link_libraries(streamfind_dynamic_schema_contract PRIVATE streamfind::cpp_core)
add_test(NAME streamfind_dynamic_schema_contract COMMAND streamfind_dynamic_schema_contract)
set_tests_properties(streamfind_dynamic_schema_contract PROPERTIES LABELS "framework")
add_library(streamfind_stage9_fixture SHARED fixtures/dynamic_plugin.cpp)
target_include_directories(streamfind_stage9_fixture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include)
set_target_properties(streamfind_stage9_fixture PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME streamfind_stage9_fixture
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stage9-plugin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stage9-plugin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stage9-plugin")
if(WIN32)
    set(STREAMFIND_DYNAMIC_PLATFORM "windows-x86_64")
    set(STREAMFIND_DYNAMIC_LIBRARY "streamfind_stage9_fixture.dll")
else()
    set(STREAMFIND_DYNAMIC_PLATFORM "linux-x86_64")
    set(STREAMFIND_DYNAMIC_LIBRARY "libstreamfind_stage9_fixture.so")
endif()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dynamic_plugin.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/stage9-plugin.json"
    @ONLY)
add_custom_command(TARGET streamfind_stage9_fixture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/stage9-plugin.json"
        "$<TARGET_FILE_DIR:streamfind_stage9_fixture>/plugin.json")
add_dependencies(streamfind_dynamic_plugin_package_contract streamfind_stage9_fixture)
target_compile_definitions(streamfind_dynamic_plugin_package_contract PRIVATE
    STREAMFIND_STAGE9_PACKAGE_DIR="${CMAKE_BINARY_DIR}/stage9-plugin")
add_test(NAME streamfind_dynamic_plugin_package_contract COMMAND streamfind_dynamic_plugin_package_contract)
set_tests_properties(streamfind_dynamic_plugin_package_contract PROPERTIES LABELS "framework")


add_executable(streamfind_project_conformance_contract unit/project_conformance_contract.cpp)
target_link_libraries(streamfind_project_conformance_contract PRIVATE streamfind::cpp_core)
target_compile_definitions(streamfind_project_conformance_contract PRIVATE
    STREAMFIND_CONFORMANCE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/project/project_conformance.json"
    STREAMFIND_DOMAIN_SCHEMA_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/project/domain_schema_manifest.json"
)
add_test(NAME streamfind_project_conformance_contract COMMAND streamfind_project_conformance_contract)
set_tests_properties(streamfind_project_conformance_contract PROPERTIES LABELS "framework")

add_executable(streamfind_project_table_store_manifest_contract unit/project_table_store_manifest_contract.cpp)
target_link_libraries(streamfind_project_table_store_manifest_contract PRIVATE streamfind::cpp_core)
target_compile_definitions(streamfind_project_table_store_manifest_contract PRIVATE
    STREAMFIND_CATALOGUE_PATH="${STREAMFIND_SEMANTIC_DIR}/catalogue.duckdb"
)
add_test(NAME streamfind_project_table_store_manifest_contract COMMAND streamfind_project_table_store_manifest_contract)
set_tests_properties(streamfind_project_table_store_manifest_contract PROPERTIES
    LABELS "framework"
    ENVIRONMENT "STREAMFIND_CATALOGUE=${STREAMFIND_SEMANTIC_DIR}/catalogue.duckdb")

add_executable(streamfind_execution_lifecycle_contract unit/execution_lifecycle_contract.cpp)
target_link_libraries(streamfind_execution_lifecycle_contract PRIVATE streamfind::cpp_core)
target_compile_definitions(streamfind_execution_lifecycle_contract PRIVATE
    STREAMFIND_MULTIPROJECT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/project/multiproject.json"
    STREAMFIND_EXECUTION_LIFECYCLE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/execution/execution_lifecycle.json"
    STREAMFIND_MCP_EXECUTION_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/mcp/execution_contract.json"
)
add_test(NAME streamfind_execution_lifecycle_contract COMMAND streamfind_execution_lifecycle_contract)
set_tests_properties(streamfind_execution_lifecycle_contract PROPERTIES LABELS "framework")

add_executable(streamfind_project_isolation_contract unit/project_isolation_contract.cpp)
target_link_libraries(streamfind_project_isolation_contract PRIVATE streamfind::cpp_core)
target_compile_definitions(streamfind_project_isolation_contract PRIVATE
    STREAMFIND_CATALOGUE_PATH="${STREAMFIND_SEMANTIC_DIR}/catalogue.duckdb"
    STREAMFIND_MULTIPROJECT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/project/multiproject.json"
    STREAMFIND_DOMAIN_SCHEMA_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/project/domain_schema_manifest.json"
)
add_test(NAME streamfind_project_isolation_contract COMMAND streamfind_project_isolation_contract)
set_tests_properties(streamfind_project_isolation_contract PROPERTIES
    LABELS "framework"
    ENVIRONMENT "STREAMFIND_CATALOGUE=${STREAMFIND_SEMANTIC_DIR}/catalogue.duckdb")

add_executable(streamfind_dependency_smoke unit/dependency_smoke.cpp)
target_link_libraries(streamfind_dependency_smoke PRIVATE streamfind::duckdb streamfind::openbabel)
target_include_directories(streamfind_dependency_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/openbabel/openbabel-3-2-0/include
)
if(MSVC)
    target_link_options(streamfind_dependency_smoke PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:streamfind_openbabel>")
elseif(UNIX)
    target_link_options(streamfind_dependency_smoke PRIVATE
        "-Wl,--whole-archive"
        "$<TARGET_FILE:streamfind_openbabel>"
        "-Wl,--no-whole-archive"
    )
endif()
add_test(NAME streamfind_dependency_smoke COMMAND streamfind_dependency_smoke)
set_tests_properties(streamfind_dependency_smoke PROPERTIES
    LABELS "framework"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/openbabel/openbabel-3-2-0/data"
)

foreach(_test_target IN ITEMS
    streamfind_project_table_store_manifest_contract
    streamfind_project_isolation_contract
)
    add_dependencies(${_test_target} streamfind_aggregate_catalogue)
    if(WIN32)
        add_custom_command(TARGET ${_test_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:streamfind::duckdb>"
                "$<TARGET_FILE_DIR:${_test_target}>"
        )
    endif()
endforeach()
