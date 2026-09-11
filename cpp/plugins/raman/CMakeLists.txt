add_library(streamfind_cpp_raman ${STREAMFIND_CPP_LIBRARY_TYPE} src/register.cpp)
streamfind_configure_cpp_library(streamfind_cpp_raman raman
    BUILD_DEFINE STREAMFIND_DOMAIN_BUILDING_SHARED
    USING_DEFINE STREAMFIND_DOMAIN_USING_SHARED)
streamfind_add_semantic_catalogue(streamfind_cpp_raman raman
    "${CMAKE_CURRENT_SOURCE_DIR}/semantic")
target_include_directories(streamfind_cpp_raman PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(streamfind_cpp_raman PUBLIC streamfind::cpp_sdk)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/semantic/"
    DESTINATION share/streamfind/semantic/raman)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
    DESTINATION share/streamfind/plugins/raman)
