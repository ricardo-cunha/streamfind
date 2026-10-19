add_library(streamfind_statistics_plugin SHARED
    src/plugin_entrypoint.cpp
    src/operations/operations.cpp
    src/operations/statistics.cpp
    src/utils/error.cpp)
target_link_libraries(streamfind_statistics_plugin PRIVATE streamfind::cpp_sdk)
streamfind_configure_cpp_library(streamfind_statistics_plugin statistics
    BUILD_DEFINE STREAMFIND_DOMAIN_BUILDING_SHARED
    USING_DEFINE STREAMFIND_DOMAIN_USING_SHARED)
streamfind_add_semantic_catalogue(streamfind_statistics_plugin statistics
    "${CMAKE_CURRENT_SOURCE_DIR}/semantic")
target_include_directories(streamfind_statistics_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/include")
set_target_properties(streamfind_statistics_plugin PROPERTIES OUTPUT_NAME "streamfind_statistics")
if(WIN32)
    set_target_properties(streamfind_statistics_plugin PROPERTIES PREFIX "")
endif()
add_custom_command(TARGET streamfind_statistics_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
        "$<TARGET_FILE_DIR:streamfind_statistics_plugin>/plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/semantic_catalogue/catalogue.duckdb"
        "$<TARGET_FILE_DIR:streamfind_statistics_plugin>/catalogue.duckdb"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        "$<TARGET_FILE_DIR:streamfind_statistics_plugin>/resources")
streamfind_validate_plugin_build(streamfind_statistics_plugin statistics)
streamfind_stage_mingw_plugin_runtime(streamfind_statistics_plugin statistics)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/semantic/"
    DESTINATION plugins/statistics/ontology)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/"
    DESTINATION plugins/statistics/resources)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
    DESTINATION plugins/statistics)
install(TARGETS streamfind_statistics_plugin
    RUNTIME DESTINATION plugins/statistics
    LIBRARY DESTINATION plugins/statistics)
if(STREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT)
    install(TARGETS streamfind_statistics_plugin ARCHIVE DESTINATION sdk/lib)
endif()
if(STREAMFIND_BUILD_TESTS)
    add_subdirectory(tests)
endif()
