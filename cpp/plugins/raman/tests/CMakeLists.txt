add_executable(streamfind_raman_normalization_unit
    unit/normalization_test.cpp
    ../src/operations/spectral_normalization.cpp)
target_include_directories(streamfind_raman_normalization_unit PRIVATE
    ../src/operations)
add_test(NAME streamfind_raman_normalization_unit
    COMMAND streamfind_raman_normalization_unit)
set_tests_properties(streamfind_raman_normalization_unit PROPERTIES
    LABELS "raman;unit")

add_executable(streamfind_raman_savgol_unit
    unit/spectral_savgol_test.cpp
    ../src/operations/spectral_savgol.cpp)
target_include_directories(streamfind_raman_savgol_unit PRIVATE
    ../src/operations)
add_test(NAME streamfind_raman_savgol_unit
    COMMAND streamfind_raman_savgol_unit)
set_tests_properties(streamfind_raman_savgol_unit PROPERTIES
    LABELS "raman;unit")

add_executable(streamfind_raman_sif_reader_unit
    unit/sif_reader_test.cpp
    ../src/readers/sif_reader.cpp)
set_target_properties(streamfind_raman_sif_reader_unit PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_include_directories(streamfind_raman_sif_reader_unit PRIVATE ../src)
target_compile_definitions(streamfind_raman_sif_reader_unit PRIVATE
    RAMAN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME streamfind_raman_sif_reader_unit
    COMMAND streamfind_raman_sif_reader_unit)
set_tests_properties(streamfind_raman_sif_reader_unit PROPERTIES
    LABELS "raman;unit")

add_executable(streamfind_raman_normalization_operation_contract
    unit/normalization_operation_contract.cpp
    ../src/operations/operations.cpp
    ../src/operations/spectral_normalization.cpp
    ../src/operations/spectral_savgol.cpp
    ../src/readers/asc_reader.cpp
    ../src/readers/format_reader.cpp
    ../src/readers/sif_reader.cpp
    ../src/readers/smf_reader.cpp
    ../src/readers/text_reader.cpp
    ../src/readers/wip_reader.cpp)
target_include_directories(streamfind_raman_normalization_operation_contract PRIVATE
    ../src
    ../../sdk/include)
target_link_libraries(streamfind_raman_normalization_operation_contract PRIVATE
    streamfind::cpp_sdk)
target_compile_definitions(streamfind_raman_normalization_operation_contract PRIVATE
    RAMAN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME streamfind_raman_normalization_operation_contract
    COMMAND streamfind_raman_normalization_operation_contract)
set_tests_properties(streamfind_raman_normalization_operation_contract PROPERTIES
    LABELS "raman;operation")
