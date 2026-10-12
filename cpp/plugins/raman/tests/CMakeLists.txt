add_executable(streamfind_raman_manifest_contract unit/manifest_contract.cpp)
target_compile_definitions(streamfind_raman_manifest_contract PRIVATE
    RAMAN_PLUGIN_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/../plugin.json")
add_test(NAME streamfind_raman_manifest_contract COMMAND streamfind_raman_manifest_contract)
set_tests_properties(streamfind_raman_manifest_contract PROPERTIES LABELS "raman;plugin")
