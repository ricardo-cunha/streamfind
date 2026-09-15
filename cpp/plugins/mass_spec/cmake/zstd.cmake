include_guard(GLOBAL)

function(streamfind_mass_spec_configure_zstd vendor_root)
    set(_root "${vendor_root}/zstd")
    if(NOT EXISTS "${_root}/build/cmake/CMakeLists.txt")
        message(FATAL_ERROR "Vendored Zstandard sources not found under ${_root}")
    endif()

    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)

    add_subdirectory(
        "${_root}/build/cmake"
        "${CMAKE_BINARY_DIR}/streamfind-zstd"
        EXCLUDE_FROM_ALL
    )

    if(NOT TARGET streamfind::zstd)
        add_library(streamfind::zstd ALIAS libzstd_static)
    endif()
endfunction()
