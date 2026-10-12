# SDK library boundary
add_library(streamfind_cpp_sdk ${STREAMFIND_CPP_LIBRARY_TYPE}
    src/sdk.cpp
    src/semantic_resources.cpp
    src/semantic_validator.cpp
    src/catalogue_builder.cpp
    src/plugin_manifest.cpp
    src/dynamic_plugin_manager.cpp
    src/plugin_data_service.cpp
    src/debug_session.cpp)
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
        $<INSTALL_INTERFACE:sdk/include>
)

add_executable(streamfind_sdk_catalogue tools/streamfind-sdk-catalogue.cpp)
target_link_libraries(streamfind_sdk_catalogue PRIVATE streamfind::cpp_sdk)
target_compile_features(streamfind_sdk_catalogue PRIVATE cxx_std_20)
streamfind_static_mingw_executable(streamfind_sdk_catalogue)
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
streamfind_static_mingw_executable(streamfind_sdk_plugin_validator)
if(WIN32 AND STREAMFIND_DUCKDB_RUNTIME)
    add_custom_command(TARGET streamfind_sdk_plugin_validator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STREAMFIND_DUCKDB_RUNTIME}"
            "$<TARGET_FILE_DIR:streamfind_sdk_plugin_validator>")
endif()
if(WIN32 AND MINGW)
    if(NOT STREAMFIND_MINGW_RUNTIME_DIR)
        get_filename_component(STREAMFIND_MINGW_RUNTIME_DIR
            "${CMAKE_CXX_COMPILER}" DIRECTORY)
    endif()
    foreach(_mingw_runtime IN ITEMS
        libgcc_s_seh-1.dll
        libstdc++-6.dll
        libwinpthread-1.dll)
        if(NOT EXISTS "${STREAMFIND_MINGW_RUNTIME_DIR}/${_mingw_runtime}")
            message(FATAL_ERROR
                "Required MinGW runtime is missing: ${STREAMFIND_MINGW_RUNTIME_DIR}/${_mingw_runtime}")
        endif()
        foreach(_streamfind_sdk_target IN ITEMS
            streamfind_sdk_catalogue
            streamfind_sdk_plugin_validator)
            add_custom_command(TARGET ${_streamfind_sdk_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${STREAMFIND_MINGW_RUNTIME_DIR}/${_mingw_runtime}"
                    "$<TARGET_FILE_DIR:${_streamfind_sdk_target}>")
        endforeach()
        if(STREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT)
            install(FILES "${STREAMFIND_MINGW_RUNTIME_DIR}/${_mingw_runtime}" DESTINATION sdk/bin)
        endif()
    endforeach()
endif()

if(STREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT)
    install(TARGETS streamfind_sdk_catalogue streamfind_sdk_plugin_validator
        RUNTIME DESTINATION sdk/bin)
    install(
        TARGETS streamfind_cpp_sdk
        EXPORT streamfind-cpp-targets
        ARCHIVE DESTINATION sdk/lib
        LIBRARY DESTINATION sdk/lib
        RUNTIME DESTINATION sdk/bin
    )
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/apache-jena/"
        DESTINATION sdk/tools/jena)
endif()
