include(${CMAKE_CURRENT_LIST_DIR}/cmake/openbabel.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/zstd.cmake)
streamfind_add_openbabel(${CMAKE_CURRENT_SOURCE_DIR}/../../vendor)
streamfind_mass_spec_configure_zstd(${CMAKE_CURRENT_SOURCE_DIR}/../../vendor)

add_library(streamfind_cpp_mass_spec ${STREAMFIND_CPP_LIBRARY_TYPE}
    src/register.cpp
    src/tools_resolver.cpp
    src/reader.cpp
    src/reader_agilent.cpp
    src/reader_agilent_ims.cpp
    src/reader_agilent_chemstation.cpp
    src/reader_bruker.cpp
    src/reader_sciex.cpp
    src/reader_thermo.cpp
    src/mass_spec.cpp
    src/chromatograms_processing_methods.cpp
    src/nta_processing_methods.cpp
    src/nta_deconvolution.cpp
    src/nta.cpp
    src/nta_annotation.cpp
    src/nta_componentization.cpp
    src/nta_blank_subtraction.cpp
    src/nta_filters.cpp
    src/nta_gap_filling.cpp
    src/nta_alignment.cpp
    src/nta_suspect_screening.cpp
    src/nta_assign_transformation_products.cpp
    src/nta_metfrag_runner.cpp
    src/nta_correction_algorithms.cpp
    src/openbabel_adapter.cpp
    src/openbabel_c_api.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/simdutf/simdutf.cpp"
)
streamfind_configure_cpp_library(streamfind_cpp_mass_spec mass_spec
    BUILD_DEFINE STREAMFIND_DOMAIN_BUILDING_SHARED
    USING_DEFINE STREAMFIND_DOMAIN_USING_SHARED)
streamfind_add_semantic_catalogue(streamfind_cpp_mass_spec mass_spec
    "${CMAKE_CURRENT_SOURCE_DIR}/semantic")
target_include_directories(streamfind_cpp_mass_spec PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(streamfind_cpp_mass_spec PUBLIC streamfind::cpp_sdk streamfind::zstd streamfind::openbabel)
if(UNIX)
    target_link_options(streamfind_cpp_mass_spec INTERFACE
        "-Wl,--whole-archive"
        "$<TARGET_FILE:streamfind_openbabel>"
        "-Wl,--no-whole-archive"
    )
endif()

target_include_directories(streamfind_cpp_mass_spec PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/pugixml-1.14/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/simdutf"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/openbabel/openbabel-3-2-0/include"
)
if(MSVC)
    target_compile_definitions(streamfind_cpp_mass_spec PRIVATE
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