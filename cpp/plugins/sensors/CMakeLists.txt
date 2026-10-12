add_library(streamfind_sensors_plugin SHARED src/plugin_entrypoint.cpp)
streamfind_configure_cpp_library(streamfind_sensors_plugin sensors
    BUILD_DEFINE STREAMFIND_DOMAIN_BUILDING_SHARED
    USING_DEFINE STREAMFIND_DOMAIN_USING_SHARED)
streamfind_add_semantic_catalogue(streamfind_sensors_plugin sensors
    "${CMAKE_CURRENT_SOURCE_DIR}/semantic")
target_include_directories(streamfind_sensors_plugin PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:sdk/include>
)
set_target_properties(streamfind_sensors_plugin PROPERTIES OUTPUT_NAME "streamfind_sensors")
if(WIN32)
    set_target_properties(streamfind_sensors_plugin PROPERTIES PREFIX "")
endif()
target_include_directories(streamfind_sensors_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include")
target_include_directories(streamfind_sensors_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor")
target_include_directories(streamfind_sensors_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_custom_command(TARGET streamfind_sensors_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
        "$<TARGET_FILE_DIR:streamfind_sensors_plugin>/plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/semantic_catalogue/catalogue.duckdb"
        "$<TARGET_FILE_DIR:streamfind_sensors_plugin>/catalogue.duckdb")
streamfind_validate_plugin_build(streamfind_sensors_plugin sensors)
streamfind_stage_mingw_plugin_runtime(streamfind_sensors_plugin sensors)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/semantic/"
    DESTINATION plugins/sensors/ontology)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
    DESTINATION plugins/sensors)
install(TARGETS streamfind_sensors_plugin
    RUNTIME DESTINATION plugins/sensors
    LIBRARY DESTINATION plugins/sensors)
if(STREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT)
    install(TARGETS streamfind_sensors_plugin ARCHIVE DESTINATION sdk/lib)
endif()
