include(${CMAKE_CURRENT_LIST_DIR}/cmake/zstd.cmake)
streamfind_mass_spec_configure_zstd(${CMAKE_CURRENT_SOURCE_DIR}/../../vendor)

add_library(streamfind_mass_spec_plugin SHARED
    src/utils/tools_resolver.cpp
    src/readers/reader.cpp
    src/readers/reader_agilent.cpp
    src/readers/reader_agilent_ims.cpp
    src/readers/reader_agilent_chemstation.cpp
    src/readers/reader_bruker.cpp
    src/readers/reader_sciex.cpp
    src/readers/reader_thermo.cpp
    src/operations/base.cpp
    src/operations/chromatograms/operations.cpp
    src/operations/nta/operations.cpp
    src/operations/nta/nta_deconvolution.cpp
    src/operations/nta/nta_alignment.cpp
    src/operations/nta/nta_annotation.cpp
    src/operations/nta/nta_assign_transformation_products.cpp
    src/operations/nta/nta_blank_subtraction.cpp
    src/operations/nta/nta_componentization.cpp
    src/operations/nta/nta_correction_algorithms.cpp
    src/operations/nta/nta_filters.cpp
    src/operations/nta/nta_gap_filling.cpp
    src/operations/nta/nta_metfrag_runner.cpp
    src/operations/nta/nta_suspect_screening.cpp
    src/operations/nta/nta_read_csv_suspect_targets.cpp
    src/operations/nta/nta_load_features_ms1.cpp
    src/operations/nta/nta_load_features_ms2.cpp

    src/utils/base.cpp
    src/utils/target_csv.cpp
    src/utils/chromatograms.cpp
    src/utils/nta.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/simdutf/simdutf.cpp"
    src/plugin_entrypoint.cpp)

streamfind_configure_cpp_library(streamfind_mass_spec_plugin mass_spec
    BUILD_DEFINE STREAMFIND_DOMAIN_BUILDING_SHARED
    USING_DEFINE STREAMFIND_DOMAIN_USING_SHARED)
streamfind_add_semantic_catalogue(streamfind_mass_spec_plugin mass_spec
    "${CMAKE_CURRENT_SOURCE_DIR}/semantic")
target_include_directories(streamfind_mass_spec_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/pugixml-1.14/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/simdutf"
)
target_link_libraries(streamfind_mass_spec_plugin PRIVATE streamfind::cpp_sdk streamfind::zstd)

set_target_properties(streamfind_mass_spec_plugin PROPERTIES OUTPUT_NAME "streamfind_mass_spec")
if(WIN32)
    set_target_properties(streamfind_mass_spec_plugin PROPERTIES PREFIX "")
endif()
add_custom_command(TARGET streamfind_mass_spec_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
        "$<TARGET_FILE_DIR:streamfind_mass_spec_plugin>/plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/semantic_catalogue/catalogue.duckdb"
        "$<TARGET_FILE_DIR:streamfind_mass_spec_plugin>/catalogue.duckdb")
streamfind_validate_plugin_build(streamfind_mass_spec_plugin mass_spec)
streamfind_stage_mingw_plugin_runtime(streamfind_mass_spec_plugin mass_spec)
if(MSVC)
    target_compile_definitions(streamfind_mass_spec_plugin PRIVATE
        NOMINMAX
        strcasecmp=_stricmp
        strncasecmp=_strnicmp)
endif()

if(MSVC)
    target_compile_definitions(streamfind_mass_spec_plugin PRIVATE
        NOMINMAX
        strcasecmp=_stricmp
        strncasecmp=_strnicmp
    )
endif()

install(TARGETS streamfind_mass_spec_plugin
    RUNTIME DESTINATION plugins/mass_spec
    LIBRARY DESTINATION plugins/mass_spec
    ARCHIVE DESTINATION sdk/lib)

install(TARGETS libzstd_static
    EXPORT streamfind-cpp-targets
    ARCHIVE DESTINATION sdk/lib
    LIBRARY DESTINATION sdk/lib
    RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/zstd/LICENSE"
    DESTINATION licenses RENAME zstd-LICENSE.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/zstd/COPYING"
    DESTINATION licenses RENAME zstd-COPYING.txt)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/semantic/"
    DESTINATION plugins/mass_spec/ontology)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
    DESTINATION plugins/mass_spec)