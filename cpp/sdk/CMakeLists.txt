# SDK library boundary
add_library(streamfind_cpp_sdk ${STREAMFIND_CPP_LIBRARY_TYPE}
    src/sdk.cpp
    src/semantic_resources.cpp
    src/semantic_validator.cpp
    src/catalogue_builder.cpp
    src/plugin_manifest.cpp
    src/dynamic_plugin_manager.cpp
    src/plugin_data_service.cpp)
target_sources(streamfind_cpp_sdk PRIVATE src/plugin_host_access.cpp)
add_library(streamfind::cpp_sdk ALIAS streamfind_cpp_sdk)

streamfind_configure_cpp_library(streamfind_cpp_sdk sdk
    BUILD_DEFINE STREAMFIND_SDK_BUILDING_SHARED
    USING_DEFINE STREAMFIND_SDK_USING_SHARED)

target_link_libraries(streamfind_cpp_sdk PUBLIC streamfind::cpp_core)
target_compile_definitions(streamfind_cpp_sdk PRIVATE
    STREAMFIND_JENA_VENDOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../vendor/apache-jena")
target_include_directories(streamfind_cpp_sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core/include>
        $<INSTALL_INTERFACE:include>
)

add_executable(streamfind_sdk_catalogue tools/streamfind-sdk-catalogue.cpp)
target_link_libraries(streamfind_sdk_catalogue PRIVATE streamfind::cpp_sdk)
target_compile_features(streamfind_sdk_catalogue PRIVATE cxx_std_20)
if(WIN32 AND STREAMFIND_DUCKDB_RUNTIME)
    add_custom_command(TARGET streamfind_sdk_catalogue POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STREAMFIND_DUCKDB_RUNTIME}"
            "$<TARGET_FILE_DIR:streamfind_sdk_catalogue>")
endif()

add_executable(streamfind_sdk_plugin_validator
    tools/streamfind-sdk-plugin-validator.cpp)
target_link_libraries(streamfind_sdk_plugin_validator PRIVATE streamfind::cpp_sdk)
target_compile_features(streamfind_sdk_plugin_validator PRIVATE cxx_std_20)

install(TARGETS streamfind_sdk_catalogue RUNTIME DESTINATION bin)

install(
    TARGETS streamfind_cpp_sdk
    EXPORT streamfind-cpp-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/apache-jena/"
    DESTINATION share/streamfind/tools/jena)
