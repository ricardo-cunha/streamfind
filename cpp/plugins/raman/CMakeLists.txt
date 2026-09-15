add_library(streamfind_raman_plugin SHARED
    src/plugin_entrypoint.cpp
    src/operations/operations.cpp
    src/utils/error.cpp)
target_link_libraries(streamfind_raman_plugin PRIVATE streamfind::cpp_sdk)
streamfind_configure_cpp_library(streamfind_raman_plugin raman
    BUILD_DEFINE STREAMFIND_DOMAIN_BUILDING_SHARED
    USING_DEFINE STREAMFIND_DOMAIN_USING_SHARED)
streamfind_add_semantic_catalogue(streamfind_raman_plugin raman
    "${CMAKE_CURRENT_SOURCE_DIR}/semantic")
target_include_directories(streamfind_raman_plugin PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(streamfind_raman_plugin PROPERTIES OUTPUT_NAME "streamfind_raman")
target_include_directories(streamfind_raman_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include")
target_include_directories(streamfind_raman_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor")
target_include_directories(streamfind_raman_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_custom_command(TARGET streamfind_raman_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
        "$<TARGET_FILE_DIR:streamfind_raman_plugin>/plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/semantic_catalogue/catalogue.duckdb"
        "$<TARGET_FILE_DIR:streamfind_raman_plugin>/catalogue.duckdb")
streamfind_validate_plugin_build(streamfind_raman_plugin raman)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/semantic/"
    DESTINATION share/streamfind/semantic/raman)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
    DESTINATION share/streamfind/plugins/raman)
install(TARGETS streamfind_raman_plugin
    RUNTIME DESTINATION share/streamfind/plugins/raman
    LIBRARY DESTINATION share/streamfind/plugins/raman
    ARCHIVE DESTINATION lib)
