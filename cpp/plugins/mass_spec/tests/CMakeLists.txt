add_compile_definitions(
    STREAMFIND_TEST_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../../.."
    STREAMFIND_DATA_ROOT="${STREAMFIND_DATA_ROOT}"
)

add_executable(streamfind_mass_spec_mcp_discovery_contract
    unit/mcp_discovery_contract.cpp)
target_link_libraries(streamfind_mass_spec_mcp_discovery_contract PRIVATE streamfind::cpp_core)
target_compile_definitions(streamfind_mass_spec_mcp_discovery_contract PRIVATE
    STREAMFIND_TEST_SERVICE_EXECUTABLE="$<TARGET_FILE:streamfind_service>")
add_dependencies(streamfind_mass_spec_mcp_discovery_contract streamfind_service)
add_test(NAME streamfind_mass_spec_mcp_discovery_contract
    COMMAND streamfind_mass_spec_mcp_discovery_contract)
set_tests_properties(streamfind_mass_spec_mcp_discovery_contract PROPERTIES
    LABELS "mass_spec;framework"
    ENVIRONMENT "STREAMFIND_CATALOGUE=${STREAMFIND_SEMANTIC_DIR}/catalogue.duckdb")
if(WIN32)
    add_custom_command(TARGET streamfind_mass_spec_mcp_discovery_contract POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:streamfind::duckdb>"
            "$<TARGET_FILE_DIR:streamfind_mass_spec_mcp_discovery_contract>")
endif()

add_executable(streamfind_mass_spec_project_isolation_contract
    unit/project_isolation_contract.cpp)
target_link_libraries(streamfind_mass_spec_project_isolation_contract PRIVATE streamfind::cpp_core)
target_compile_definitions(streamfind_mass_spec_project_isolation_contract PRIVATE
    STREAMFIND_CATALOGUE_PATH="${STREAMFIND_SEMANTIC_DIR}/catalogue.duckdb"
    STREAMFIND_MULTIPROJECT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../../core/tests/fixtures/project/multiproject.json"
    STREAMFIND_DOMAIN_SCHEMA_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/project/domain_schema_manifest.json")
add_test(NAME streamfind_mass_spec_project_isolation_contract
    COMMAND streamfind_mass_spec_project_isolation_contract)
set_tests_properties(streamfind_mass_spec_project_isolation_contract PROPERTIES
    LABELS "mass_spec;framework"
    ENVIRONMENT "STREAMFIND_CATALOGUE=${STREAMFIND_SEMANTIC_DIR}/catalogue.duckdb")

add_executable(streamfind_mass_spec_data_contract unit/data_contract.cpp)
target_link_libraries(streamfind_mass_spec_data_contract PRIVATE streamfind::cpp_core)
add_test(NAME streamfind_mass_spec_data_contract COMMAND streamfind_mass_spec_data_contract)
set_tests_properties(streamfind_mass_spec_data_contract PROPERTIES LABELS "mass_spec;data")

foreach(_test_target IN ITEMS
    streamfind_mass_spec_mcp_discovery_contract
    streamfind_mass_spec_project_isolation_contract)
    add_dependencies(${_test_target} streamfind_aggregate_catalogue)
    if(WIN32)
        add_custom_command(TARGET ${_test_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:streamfind::duckdb>"
                "$<TARGET_FILE_DIR:${_test_target}>")
    endif()
endforeach()

