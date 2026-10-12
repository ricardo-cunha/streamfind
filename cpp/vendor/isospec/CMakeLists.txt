#############################################################
#############################################################
# CMake configuration

cmake_minimum_required(VERSION 3.12)

# CMake script for the IsoSpec library
# Author: Anonymous and Filippo Rusconi
# This script was present in the source tree but was
# highly rudimentary. I have implemented most of what is
# here (Filippo Rusconi).
# Last Modifs: July 2020

############################################################
############################################################
# Basic information about project

# The version lives in one place only: the VERSION file at the top of the tree.
# pyproject.toml reads that same file (see its [[tool.dynamic-metadata]] block),
# so the C++ library and the Python package can no longer drift apart the way
# they had by 2.4.0, when CMake said 2.3.4 and the wheel said 2.3.5.
#
# Read before project(), which is where it is needed. Keep the file to a single
# bare version number on one line: everything reading it -- CMake here, the
# wheel's regex, anything a packager writes -- assumes exactly that.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ISOSPEC_VERSION_RAW)
string(STRIP "${ISOSPEC_VERSION_RAW}" ISOSPEC_VERSION)

project(IsoSpec VERSION ${ISOSPEC_VERSION}
    DESCRIPTION "A program to calculate isotopic clusters"
    HOMEPAGE_URL "https://github.com/MatteoLacki/IsoSpec")

# The call above, make these variables available:
# ${PROJECT_NAME}_VERSION        # full version string
# ${PROJECT_NAME}_VERSION_MAJOR
# ${PROJECT_NAME}_VERSION_MINOR
# ${PROJECT_NAME}_VERSION_PATCH

message("Library version: ${${PROJECT_NAME}_VERSION}")

# The soname carries MAJOR.MINOR, not just MAJOR.
#
# It has to encode ABI identity, and this library's ABI is not tracked closely
# enough for a hand-maintained counter to be trustworthy: 2.4.0 grew
# FixedEnvelope from 96 to 216 bytes (it gained the ownership members behind the
# aligned-allocation work) and that would have shipped under the unchanged
# soname libIsoSpec++.so.2, silently corrupting every program still linked
# against 2.3.x -- FixedEnvelope is returned by value from FromThreshold() and
# friends, so callers stack-allocate it.
#
# Tying the soname to MINOR makes that mistake unrepresentable, at the price of
# forcing dependents to rebuild on every minor release even when nothing broke.
# For a library with this few reverse dependencies that is the right trade.
#
# It does NOT cover an ABI break within a patch series (2.4.0 -> 2.4.1 share a
# soname); the layout assertion in tests/C++/test_fixed_envelope.cpp is what
# guards that.
#
# Note for packagers: bumping MINOR now renames the Debian binary package
# (libisospec++2t64 -> libisospec++2.4t64) and needs the usual transition;
# debian/rules hardcodes ISOSPEC_LIB_SOVERSION and must be updated to match.
set(${PROJECT_NAME}_LIB_SOVERSION "${${PROJECT_NAME}_VERSION_MAJOR}.${${PROJECT_NAME}_VERSION_MINOR}")
message("Library soname: ${${PROJECT_NAME}_LIB_SOVERSION}")

# Command to enable debug mode
# cmake -DCMAKE_BUILD_TYPE=Debug


# This setting is also typically overridden by
# configuring the build system like this:
# cmake -DCMAKE_INSTALL_PREFIX=/usr
# It needs to be set before include(GNUInstallDirs)
# below, because it roots all the other dirs' configs.

if(NOT CMAKE_INSTALL_PREFIX)
    set(CMAKE_INSTALL_PREFIX /usr)
endif()

# This module sets a number of installation directories
# according to the GNU coding standards. It takes the
# CMAKE_INSTALL_PREFIX and then add the directories to it.
include(GNUInstallDirs)

# Add folder where are supportive functions
# that contain toolchains: for UNIX/WIN/MAC
# that contain modules useful on any system.
set(CMAKE_UTILS_PATH ${CMAKE_CURRENT_SOURCE_DIR}/CMakeStuff)
set(CMAKE_TOOLCHAINS_PATH ${CMAKE_UTILS_PATH}/toolchains)
set(CMAKE_MODULE_PATH ${CMAKE_UTILS_PATH}/modules)
#message("CMAKE_MODULE_PATH:" ${CMAKE_MODULE_PATH})

# This says that when compiling, .h files must also be
# searche in the current directory where cpp files are
# compiled.
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Include the system's uname that fills in SYSTEM_UNAME_S.
# Sets WIN64 if SYSTEM_UNAME_S is "^.*MING64.*"
include(${CMAKE_UTILS_PATH}/systemUname.cmake)

# Include the various colors we want to use in the output
include(${CMAKE_UTILS_PATH}/outputColors.cmake)

set(CMAKE_COLOR_MAKEFILE ON)
set(CMAKE_VERBOSE_MAKEFILE ON)

message("")
message("\n${BoldRed}Configuring build for project ${PROJECT_NAME}${ColourReset}\n")
message("")

# This export will allow using the flags to be used by
# youcompleteme (vim plugin).
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json" )
    execute_process( COMMAND cmake -E copy_if_different
        ${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json
        ${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json
        )
endif()


# We want C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
message(STATUS "${BoldGreen}CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER}${ColourReset}")

#############################################################
# We do not want warnings for unknown pragmas:
message(STATUS "${BoldGreen}Setting definition -Wno-unknown-pragmas.${ColourReset}")
add_definitions(-Wno-unknown-pragmas)

# Enable warnings and possibly treat them as errors
message(STATUS "${BoldGreen}Setting definition -Wall.${ColourReset}")
add_definitions(-Wall)
message(STATUS "${BoldGreen}Setting definition -Wextra.${ColourReset}")
add_definitions(-Wextra)
message(STATUS "${BoldGreen}Setting definition -pedantic.${ColourReset}")
add_definitions(-pedantic)

if(WARN_AS_ERROR)
    message(STATUS "${BoldYellow}Setting definition -Werror.${ColourReset}")
    add_definitions(-Werror)
endif()

message(STATUS "${BoldGreen}CMAKE_SOURCE_DIR: ${CMAKE_SOURCE_DIR}${ColourReset}")
message(STATUS "${BoldGreen}CMAKE_BINARY_DIR: ${CMAKE_BINARY_DIR}${ColourReset}")


#############################################################
#############################################################
# Platform-specific CMake configuration

# MXE is a pretty cool cross-compilation environment that allows one
# to build Win software right into GNU/Linux.
if(MXE)

    # Run the following cmake command line:
    # x86_64-w64-mingw32.shared-cmake -DCMAKE_BUILD_TYPE=Release -DMXE=1 ../development
    include(${CMAKE_TOOLCHAINS_PATH}/mxe-toolchain-$ENV{USER}.cmake)

    # Set the name to the systemUname variable because in this situation that name
    # is not found, it it passed as a flag in the command line.
    set(SYSTEM_UNAME_S "mxe")

elseif(UNIX AND NOT APPLE)

    include(${CMAKE_TOOLCHAINS_PATH}/unix-toolchain.cmake)

elseif(WIN32 OR _WIN32)

    if(WIN10MINGW64)
        include(${CMAKE_TOOLCHAINS_PATH}/win10-mingw64-toolchain.cmake)
    endif()

endif()

message("")
message("${BoldRed}The build toolchain is: ${SYSTEM_UNAME_S}${ColourReset}")
message("")


#############################################################
#############################################################
# Essential software configuration

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Type of build, options are: None, Debug, Release, RelWithDebInfo, MinSizeRel."
        FORCE)
endif(NOT CMAKE_BUILD_TYPE)

if(CMAKE_BUILD_TYPE MATCHES "Release")
    #message(STATUS "Compiling in release mode.")
    add_definitions("-DQT_NO_DEBUG_OUTPUT")
endif()

if(CMAKE_BUILD_TYPE MATCHES "Debug")
    #message(STATUS "Compiling in debug mode.")
    message(STATUS "Add definition -ggdb3 to format debug output for GDB.")
    add_definitions(-ggdb3)
endif()

if(CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
    #message(STATUS "Compiling in release with debug info mode.")
endif(CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")

message(STATUS "${BoldYellow}CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}.${ColourReset}")
message(STATUS "${BoldYellow}CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}.${ColourReset}")

# message(STATUS CMAKE_MODULE_PATH: ${CMAKE_MODULE_PATH})

# Now actually deal with the source code.
add_subdirectory(src)

# if (UNIX AND NOT APPLE)
#   add_subdirectory(skbuild)
# endif()
