add_executable(streamfind_sensors_manifest_contract unit/manifest_contract.cpp)
target_compile_definitions(streamfind_sensors_manifest_contract PRIVATE
    SENSORS_PLUGIN_MANIFEST="${STREAMFIND_BUNDLED_MANIFEST}")
add_test(NAME streamfind_sensors_manifest_contract COMMAND streamfind_sensors_manifest_contract)
set_tests_properties(streamfind_sensors_manifest_contract PROPERTIES LABELS "sensors;plugin")

add_executable(streamfind_sensors_operation_contract
    unit/operation_contract.cpp
    ../src/operations/timeseries.cpp
    ../src/operations/validation.cpp
    ../src/operations/filter.cpp
    ../src/operations/resample.cpp
    ../src/operations/alignment.cpp
    ../src/operations/project.cpp
    ../src/operations/analysis.cpp)
target_include_directories(streamfind_sensors_operation_contract PRIVATE
    ../../sdk/include)
target_link_libraries(streamfind_sensors_operation_contract PRIVATE
    streamfind::cpp_sdk)
add_test(NAME streamfind_sensors_operation_contract
    COMMAND streamfind_sensors_operation_contract)
set_tests_properties(streamfind_sensors_operation_contract PROPERTIES
    LABELS "sensors;operation")
