add_executable(streamfind_statistics_unit
    unit/statistics_test.cpp
    ../src/operations/statistics.cpp)
target_include_directories(streamfind_statistics_unit PRIVATE ../src/operations)
add_test(NAME streamfind_statistics_unit COMMAND streamfind_statistics_unit)
set_tests_properties(streamfind_statistics_unit PROPERTIES LABELS "statistics;unit")

add_executable(streamfind_statistics_operation_contract
    unit/operation_contract_test.cpp
    ../src/operations/operations.cpp
    ../src/operations/statistics.cpp)
target_include_directories(streamfind_statistics_operation_contract PRIVATE
    ../src/operations
    ../../sdk/include)
target_link_libraries(streamfind_statistics_operation_contract PRIVATE streamfind::cpp_sdk)
add_test(NAME streamfind_statistics_operation_contract
    COMMAND streamfind_statistics_operation_contract)
set_tests_properties(streamfind_statistics_operation_contract PROPERTIES
    LABELS "statistics;operation;contract")
