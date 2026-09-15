include_guard(GLOBAL)

function(streamfind_detect_platform out_var)
    if(WIN32)
        set(_platform windows-x64)
    elseif(APPLE)
        set(_platform macos-arm64)
    else()
        set(_platform linux-x64)
    endif()

    set(${out_var} "${_platform}" PARENT_SCOPE)
endfunction()

function(streamfind_configure_duckdb vendor_root)
    streamfind_detect_platform(_platform)

    set(_root "${vendor_root}/duckdb")
    set(_include "${_root}/include")

    if(NOT EXISTS "${_include}/duckdb.h")
        message(FATAL_ERROR "DuckDB headers not found under ${_include}")
    endif()

    if(WIN32)
        set(_imported "${_root}/lib/${_platform}/duckdb.lib")
        set(_runtime "${_root}/lib/${_platform}/duckdb.dll")
        if(NOT EXISTS "${_imported}" OR NOT EXISTS "${_runtime}")
            message(FATAL_ERROR "DuckDB Windows import/runtime libraries not found under ${_root}/lib/${_platform}")
        endif()

        add_library(streamfind_duckdb SHARED IMPORTED GLOBAL)
        set_target_properties(streamfind_duckdb PROPERTIES
            IMPORTED_IMPLIB "${_imported}"
            IMPORTED_LOCATION "${_runtime}"
            INTERFACE_INCLUDE_DIRECTORIES "${_include}"
        )
        set(_import_library "${_imported}")
        set(_static_libraries "")
    else()
        find_file(_shared
            NAMES libduckdb.so libduckdb.dylib
            PATHS "${_root}/lib/${_platform}"
            NO_DEFAULT_PATH
        )
        find_file(_static
            NAMES libduckdb_static.a
            PATHS "${_root}/lib/${_platform}"
            NO_DEFAULT_PATH
        )

        if(_shared)
            add_library(streamfind_duckdb SHARED IMPORTED GLOBAL)
            set_target_properties(streamfind_duckdb PROPERTIES
                IMPORTED_LOCATION "${_shared}"
                INTERFACE_INCLUDE_DIRECTORIES "${_include}"
            )
            set(_runtime "${_shared}")
            set(_static_libraries "")
        elseif(_static)
            find_package(Threads REQUIRED)
            set(_extension_loader "${_root}/lib/${_platform}/libduckdb_generated_extension_loader.a")
            if(NOT EXISTS "${_extension_loader}")
                message(FATAL_ERROR
                    "Complete DuckDB static package required: ${_extension_loader} is missing")
            endif()
            file(GLOB _static_extensions CONFIGURE_DEPENDS
                "${_root}/lib/${_platform}/lib*.a"
            )
            list(REMOVE_ITEM _static_extensions "${_static}")
            list(SORT _static_extensions)
            set(_static_libraries "${_static};${_static_extensions}")
            add_library(streamfind_duckdb INTERFACE)
            target_include_directories(streamfind_duckdb INTERFACE
                "$<BUILD_INTERFACE:${_include}>"
                "$<INSTALL_INTERFACE:include>"
            )
            target_link_libraries(streamfind_duckdb INTERFACE
                "-Wl,--start-group"
                ${_static_libraries}
                "-Wl,--end-group"
                Threads::Threads
                ${CMAKE_DL_LIBS}
            )
            set(_runtime "")
        else()
            message(FATAL_ERROR "DuckDB library not found under ${_root}/lib/${_platform}")
        endif()
    endif()

    add_library(streamfind::duckdb ALIAS streamfind_duckdb)
    set(STREAMFIND_DUCKDB_RUNTIME "${_runtime}" PARENT_SCOPE)
    set(STREAMFIND_DUCKDB_IMPORT_LIBRARY "${_import_library}" PARENT_SCOPE)
    set(STREAMFIND_DUCKDB_STATIC_LIBRARIES "${_static_libraries}" PARENT_SCOPE)
    set(STREAMFIND_DUCKDB_INCLUDE_DIR "${_include}" PARENT_SCOPE)
    set(STREAMFIND_PLATFORM_TAG "${_platform}" PARENT_SCOPE)
endfunction()

function(streamfind_configure_dependencies vendor_root)
    set(_vendor_root "${vendor_root}")
    streamfind_configure_duckdb("${_vendor_root}")


    if(NOT TARGET streamfind::zlib)
        add_subdirectory(
            "${_vendor_root}/zlib/zlib-develop"
            "${CMAKE_BINARY_DIR}/streamfind-vendored-zlib"
            EXCLUDE_FROM_ALL
        )
    endif()

    set(STREAMFIND_VENDOR_DIR "${_vendor_root}" PARENT_SCOPE)
    set(STREAMFIND_DUCKDB_RUNTIME "${STREAMFIND_DUCKDB_RUNTIME}" PARENT_SCOPE)
    set(STREAMFIND_DUCKDB_IMPORT_LIBRARY "${STREAMFIND_DUCKDB_IMPORT_LIBRARY}" PARENT_SCOPE)
    set(STREAMFIND_DUCKDB_STATIC_LIBRARIES "${STREAMFIND_DUCKDB_STATIC_LIBRARIES}" PARENT_SCOPE)
    set(STREAMFIND_DUCKDB_INCLUDE_DIR "${STREAMFIND_DUCKDB_INCLUDE_DIR}" PARENT_SCOPE)
    set(STREAMFIND_PLATFORM_TAG "${STREAMFIND_PLATFORM_TAG}" PARENT_SCOPE)
endfunction()
