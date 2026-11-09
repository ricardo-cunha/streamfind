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

add_executable(streamfind_mass_spec_filter_reason_contract
    unit/filter_reason_contract.cpp)
target_include_directories(streamfind_mass_spec_filter_reason_contract PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor")
add_test(NAME streamfind_mass_spec_filter_reason_contract
    COMMAND streamfind_mass_spec_filter_reason_contract)
set_tests_properties(streamfind_mass_spec_filter_reason_contract PROPERTIES
    LABELS "mass_spec;nta;filter")

add_executable(streamfind_mass_spec_blank_replicate_matching_contract
    unit/blank_replicate_matching_contract.cpp)
target_include_directories(streamfind_mass_spec_blank_replicate_matching_contract PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor")
add_test(NAME streamfind_mass_spec_blank_replicate_matching_contract
    COMMAND streamfind_mass_spec_blank_replicate_matching_contract)
set_tests_properties(streamfind_mass_spec_blank_replicate_matching_contract PROPERTIES
    LABELS "mass_spec;nta;blank_subtraction")

add_executable(streamfind_mass_spec_find_features_noise_quantile_contract
    unit/find_features_noise_quantile_contract.cpp
    ../src/operations/nta/nta_noise_estimation.cpp)
target_include_directories(streamfind_mass_spec_find_features_noise_quantile_contract PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor")
add_test(NAME streamfind_mass_spec_find_features_noise_quantile_contract
    COMMAND streamfind_mass_spec_find_features_noise_quantile_contract)
set_tests_properties(streamfind_mass_spec_find_features_noise_quantile_contract PROPERTIES
    LABELS "mass_spec;nta;find_features")

add_executable(streamfind_mass_spec_fragmenter_limits_contract
    unit/fragmenter_limits_contract.cpp)
target_include_directories(streamfind_mass_spec_fragmenter_limits_contract PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor")
add_test(NAME streamfind_mass_spec_fragmenter_limits_contract
    COMMAND streamfind_mass_spec_fragmenter_limits_contract)
set_tests_properties(streamfind_mass_spec_fragmenter_limits_contract PROPERTIES
    LABELS "mass_spec;fragmentation")

add_executable(streamfind_mass_spec_metfrag_installer_contract
    unit/metfrag_installer_contract.cpp
    ../src/utils/tools_resolver.cpp)
target_include_directories(streamfind_mass_spec_metfrag_installer_contract PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/include")
target_link_libraries(streamfind_mass_spec_metfrag_installer_contract PRIVATE streamfind::zlib)
target_compile_definitions(streamfind_mass_spec_metfrag_installer_contract PRIVATE
    STREAMFIND_DOMAIN_BUILDING_SHARED)
add_test(NAME streamfind_mass_spec_metfrag_installer_contract
    COMMAND streamfind_mass_spec_metfrag_installer_contract)
set_tests_properties(streamfind_mass_spec_metfrag_installer_contract PROPERTIES
    LABELS "mass_spec;dependencies;installer")

add_executable(streamfind_mass_spec_bruker_calibration_contract
    unit/bruker_calibration_contract.cpp
    ../src/readers/reader_bruker_calibration.cpp)
target_include_directories(streamfind_mass_spec_bruker_calibration_contract PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/include")

add_test(NAME streamfind_mass_spec_bruker_calibration_contract
    COMMAND streamfind_mass_spec_bruker_calibration_contract)
set_tests_properties(streamfind_mass_spec_bruker_calibration_contract PROPERTIES
    LABELS "mass_spec;reader;calibration")

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

