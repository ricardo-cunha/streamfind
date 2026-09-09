add_compile_definitions(
    STREAMFIND_TEST_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)

add_executable(streamfind_project_tests unit/project_smoke.cpp)
target_link_libraries(streamfind_project_tests PRIVATE streamfind::core)
add_test(NAME streamfind_project_tests COMMAND streamfind_project_tests)
set_tests_properties(streamfind_project_tests PROPERTIES LABELS "framework")

add_executable(streamfind_mcp_tests unit/mcp_smoke.cpp)
target_link_libraries(streamfind_mcp_tests PRIVATE streamfind::core streamfind_mass_spec)
target_compile_definitions(streamfind_mcp_tests PRIVATE
    STREAMFIND_CATALOGUE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../semantic/generated/catalogue.duckdb"
)
add_test(NAME streamfind_mcp_tests COMMAND streamfind_mcp_tests)
set_tests_properties(streamfind_mcp_tests PROPERTIES LABELS "framework")

add_executable(streamfind_mass_spec_interface_tests unit/mass_spec_interface_smoke.cpp)
target_link_libraries(streamfind_mass_spec_interface_tests PRIVATE streamfind::core streamfind_mass_spec)
target_compile_definitions(streamfind_mass_spec_interface_tests PRIVATE
    STREAMFIND_CATALOGUE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../semantic/generated/catalogue.duckdb"
)
add_test(NAME streamfind_mass_spec_interface_tests COMMAND streamfind_mass_spec_interface_tests)
set_tests_properties(streamfind_mass_spec_interface_tests PROPERTIES LABELS "mass-spec-interface")


add_executable(streamfind_nta_interface_tests unit/nta_interface_smoke.cpp)
target_link_libraries(streamfind_nta_interface_tests PRIVATE streamfind::core streamfind_mass_spec)
target_compile_definitions(streamfind_nta_interface_tests PRIVATE
    STREAMFIND_CATALOGUE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../semantic/generated/catalogue.duckdb"
)
add_test(NAME streamfind_nta_interface_tests COMMAND streamfind_nta_interface_tests)
set_tests_properties(streamfind_nta_interface_tests PROPERTIES LABELS "nta-interface")

add_executable(streamfind_conformance_tests unit/conformance.cpp)
target_link_libraries(streamfind_conformance_tests PRIVATE streamfind::core)
target_compile_definitions(streamfind_conformance_tests PRIVATE
    STREAMFIND_CONFORMANCE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/project/project_conformance.json"
    STREAMFIND_DOMAIN_SCHEMA_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/project/domain_schema_manifest.json"
)
add_test(NAME streamfind_conformance_tests COMMAND streamfind_conformance_tests)
set_tests_properties(streamfind_conformance_tests PROPERTIES LABELS "framework")

add_executable(streamfind_project_table_store_manifest_tests unit/project_table_store_manifest.cpp)
target_link_libraries(streamfind_project_table_store_manifest_tests PRIVATE streamfind::core)
target_compile_definitions(streamfind_project_table_store_manifest_tests PRIVATE
    STREAMFIND_CATALOGUE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../semantic/generated/catalogue.duckdb"
)
add_test(NAME streamfind_project_table_store_manifest_tests COMMAND streamfind_project_table_store_manifest_tests)
set_tests_properties(streamfind_project_table_store_manifest_tests PROPERTIES LABELS "framework")

add_executable(streamfind_execution_contract_tests unit/execution_contract_smoke.cpp)
target_link_libraries(streamfind_execution_contract_tests PRIVATE streamfind::core)
target_compile_definitions(streamfind_execution_contract_tests PRIVATE
    STREAMFIND_MULTIPROJECT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/project/multiproject.json"
    STREAMFIND_EXECUTION_LIFECYCLE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/execution/execution_lifecycle.json"
    STREAMFIND_MCP_EXECUTION_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/mcp/execution_contract.json"
)
add_test(NAME streamfind_execution_contract_tests COMMAND streamfind_execution_contract_tests)
set_tests_properties(streamfind_execution_contract_tests PROPERTIES LABELS "framework")

add_executable(streamfind_multiproject_tests unit/multiproject_smoke.cpp)
target_link_libraries(streamfind_multiproject_tests PRIVATE streamfind::core)
target_compile_definitions(streamfind_multiproject_tests PRIVATE
    STREAMFIND_MULTIPROJECT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/project/multiproject.json"
    STREAMFIND_DOMAIN_SCHEMA_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/project/domain_schema_manifest.json"
)
add_test(NAME streamfind_multiproject_tests COMMAND streamfind_multiproject_tests)
set_tests_properties(streamfind_multiproject_tests PROPERTIES LABELS "framework")

add_executable(streamfind_vendor_tests unit/vendor_smoke.cpp)
target_link_libraries(streamfind_vendor_tests PRIVATE streamfind::duckdb streamfind::openbabel)
target_include_directories(streamfind_vendor_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/openbabel/openbabel-3-2-0/include
)
if(MSVC)
    target_link_options(streamfind_vendor_tests PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:streamfind_openbabel>")
elseif(UNIX)
    target_link_options(streamfind_vendor_tests PRIVATE
        "-Wl,--whole-archive"
        "$<TARGET_FILE:streamfind_openbabel>"
        "-Wl,--no-whole-archive"
    )
endif()
add_test(NAME streamfind_vendor_tests COMMAND streamfind_vendor_tests)
set_tests_properties(streamfind_vendor_tests PROPERTIES
    LABELS "framework"
    WORKING_DIRECTORY "${STREAMFIND_OPENBABEL_DATA_DIR}"
)

foreach(_test_target IN ITEMS
    streamfind_project_tests
    streamfind_mcp_tests
    streamfind_mass_spec_interface_tests

    streamfind_nta_interface_tests
    streamfind_conformance_tests
    streamfind_project_table_store_manifest_tests
    streamfind_execution_contract_tests
    streamfind_multiproject_tests
    streamfind_vendor_tests
)
    add_custom_command(TARGET ${_test_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STREAMFIND_SEMANTIC_DIR}/catalogue.duckdb"
            "$<TARGET_FILE_DIR:${_test_target}>"
    )
    if(WIN32)
        add_custom_command(TARGET ${_test_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:streamfind::duckdb>"
                "$<TARGET_FILE_DIR:${_test_target}>"
        )
    endif()
endforeach()
