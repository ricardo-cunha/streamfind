include_guard(GLOBAL)

function(streamfind_add_openbabel vendor_root)
    set(_root "${vendor_root}/openbabel")
    set(_ob_root "${_root}/openbabel-3-2-0")
    set(_inchi_root "${_root}/inchi-iupac-1.07.5")
    set(_inchi_base "${_root}/INCHI_BASE")
    set(STREAMFIND_OPENBABEL_DATA_DIR "${_ob_root}/data")
    file(GLOB _ob_sources CONFIGURE_DEPENDS
        "${_ob_root}/src/*.cpp"
        "${_ob_root}/src/math/matrix3x3.cpp"
        "${_ob_root}/src/math/spacegroup.cpp"
        "${_ob_root}/src/math/transform3d.cpp"
        "${_ob_root}/src/math/vector3.cpp"
        "${_ob_root}/src/stereo/*.cpp"
        "${_ob_root}/src/ops/gen2D.cpp"
        "${_ob_root}/src/depict/depict.cpp"
        "${_ob_root}/src/depict/svgpainter.cpp"
        "${_ob_root}/src/descriptors/groupcontrib.cpp"
        "${_ob_root}/src/formats/mdlformat.cpp"
        "${_ob_root}/src/formats/smilesformat.cpp"
        "${_ob_root}/src/formats/svgformat.cpp"
        "${_ob_root}/src/formats/getinchi.cpp"
        "${_ob_root}/src/formats/inchiformat.cpp"
    )
    list(FILTER _ob_sources EXCLUDE REGEX
        "/(RDKitConv|conformersearch|confsearch|distgeom|dlhandler_unix|doxygen_pages)\\.cpp$"
    )
    if(WIN32)
        set(STREAMFIND_OB_HAVE_CONIO_H 1)
        set(STREAMFIND_OB_MODULE_EXTENSION ".obf")
    else()
        set(STREAMFIND_OB_HAVE_CONIO_H 0)
        set(STREAMFIND_OB_MODULE_EXTENSION ".so")
        list(FILTER _ob_sources EXCLUDE REGEX "/dlhandler_win32\\.cpp$")
    endif()
    set(STREAMFIND_OPENBABEL_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/streamfind-openbabel-config")
    file(MAKE_DIRECTORY "${STREAMFIND_OPENBABEL_CONFIG_DIR}/openbabel")
    configure_file(
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/openbabel_babelconfig.h.in"
        "${STREAMFIND_OPENBABEL_CONFIG_DIR}/openbabel/babelconfig.h"
        @ONLY
    )
    file(GLOB _inchi_sources CONFIGURE_DEPENDS
        "${_ob_root}/src/formats/libinchi/*.c"
        "${_inchi_root}/src/ichilnct.c"
        "${_inchi_root}/src/inchi_dll_a.c"
        "${_inchi_root}/src/inchi_dll_b.c"
        "${_inchi_root}/src/inchi_dll_main.c"
    )

    set(_includes
        "${STREAMFIND_OPENBABEL_CONFIG_DIR}"
        "${_ob_root}/include"
        "${_ob_root}/include/inchi"
        "${_ob_root}/src"
        "${_ob_root}/data"
        "${_ob_root}/src/formats/libinchi"
        "${_inchi_root}/src"
        "${_inchi_base}/src"
        "${_root}"
    )

    add_library(streamfind_inchi STATIC ${_inchi_sources})
    add_library(streamfind_openbabel STATIC ${_ob_sources})
    add_library(streamfind::inchi ALIAS streamfind_inchi)
    add_library(streamfind::openbabel ALIAS streamfind_openbabel)

    foreach(_target IN ITEMS streamfind_inchi streamfind_openbabel)
        set_target_properties(${_target} PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            POSITION_INDEPENDENT_CODE ON
        )
        target_include_directories(${_target} PRIVATE ${_includes})
        target_compile_definitions(${_target} PRIVATE TARGET_API_LIB)
        if(MSVC)
            target_compile_definitions(${_target} PRIVATE
                NOMINMAX
                strcasecmp=_stricmp
                strncasecmp=_strnicmp
            )
        endif()
        target_compile_options(${_target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-w>
            $<$<CXX_COMPILER_ID:MSVC>:/W0;/wd4244>
        )
    endforeach()

    target_include_directories(streamfind_inchi PRIVATE ${_includes})
    target_include_directories(streamfind_openbabel PRIVATE ${_includes})
    target_link_libraries(streamfind_openbabel PUBLIC streamfind::inchi)

    set(STREAMFIND_OPENBABEL_DATA_DIR "${_ob_root}/data" PARENT_SCOPE)
endfunction()
