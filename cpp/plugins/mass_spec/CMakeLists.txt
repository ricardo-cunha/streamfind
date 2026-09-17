include(${CMAKE_CURRENT_LIST_DIR}/cmake/openbabel.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/zstd.cmake)
streamfind_add_openbabel(${CMAKE_CURRENT_SOURCE_DIR}/../../vendor)
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
    src/operations/nta.cpp
    src/operations/chromatograms.cpp
    src/methods/chromatograms_processing_methods.cpp
    src/utils/chromatogram_peaks.cpp

    src/methods/nta_processing_methods.cpp

    src/methods/nta_deconvolution.cpp
    src/utils/nta.cpp
    src/methods/nta_annotation.cpp
    src/methods/nta_componentization.cpp
    src/methods/nta_blank_subtraction.cpp
    src/methods/nta_filters.cpp
    src/methods/nta_gap_filling.cpp
    src/methods/nta_alignment.cpp
    src/methods/nta_suspect_screening.cpp
    src/methods/nta_assign_transformation_products.cpp
    src/methods/nta_metfrag_runner.cpp
    src/methods/nta_correction_algorithms.cpp
    src/utils/openbabel_adapter.cpp
    src/utils/openbabel_c_api.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/simdutf/simdutf.cpp"
    src/plugin_entrypoint.cpp)
streamfind_configure_cpp_library(streamfind_mass_spec_plugin mass_spec
    BUILD_DEFINE STREAMFIND_DOMAIN_BUILDING_SHARED
    USING_DEFINE STREAMFIND_DOMAIN_USING_SHARED)
streamfind_add_semantic_catalogue(streamfind_mass_spec_plugin mass_spec
    "${CMAKE_CURRENT_SOURCE_DIR}/semantic")
target_include_directories(streamfind_mass_spec_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(streamfind_mass_spec_plugin PRIVATE streamfind::cpp_sdk streamfind::zstd streamfind::openbabel)
if(UNIX)
    target_link_options(streamfind_mass_spec_plugin PRIVATE
        "-Wl,--whole-archive"
        "$<TARGET_FILE:streamfind_openbabel>"
        "-Wl,--no-whole-archive"
    )
endif()

set_target_properties(streamfind_mass_spec_plugin PROPERTIES OUTPUT_NAME "streamfind_mass_spec")
add_custom_command(TARGET streamfind_mass_spec_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
        "$<TARGET_FILE_DIR:streamfind_mass_spec_plugin>/plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/semantic_catalogue/catalogue.duckdb"
        "$<TARGET_FILE_DIR:streamfind_mass_spec_plugin>/catalogue.duckdb")
streamfind_validate_plugin_build(streamfind_mass_spec_plugin mass_spec)
if(MSVC)
    target_compile_definitions(streamfind_mass_spec_plugin PRIVATE
        NOMINMAX
        strcasecmp=_stricmp
        strncasecmp=_strnicmp)
endif()

target_include_directories(streamfind_mass_spec_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/pugixml-1.14/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/simdutf"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/openbabel/openbabel-3-2-0/include"
)
if(MSVC)
    target_compile_definitions(streamfind_mass_spec_plugin PRIVATE
        NOMINMAX
        strcasecmp=_stricmp
        strncasecmp=_strnicmp
    )
endif()

install(
    TARGETS streamfind_inchi streamfind_openbabel
    EXPORT streamfind-cpp-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(TARGETS streamfind_mass_spec_plugin
    RUNTIME DESTINATION share/streamfind/plugins/mass_spec
    LIBRARY DESTINATION share/streamfind/plugins/mass_spec
    ARCHIVE DESTINATION lib)
install(TARGETS libzstd_static
    EXPORT streamfind-cpp-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/zstd/LICENSE"
    DESTINATION licenses RENAME zstd-LICENSE.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/zstd/COPYING"
    DESTINATION licenses RENAME zstd-COPYING.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/openbabel/openbabel-3-2-0/COPYING"
    DESTINATION licenses RENAME openbabel-COPYING.txt)
install(
    DIRECTORY "${STREAMFIND_OPENBABEL_DATA_DIR}/"
    DESTINATION share/streamfind/openbabel/data
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/semantic/"
    DESTINATION share/streamfind/semantic/mass_spec)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
    DESTINATION share/streamfind/plugins/mass_spec)