cmake_minimum_required(VERSION 3.20)

project(streamfind_vendored_zlib LANGUAGES C)

add_library(
  streamfind_zlib STATIC
  adler32.c
  compress.c
  crc32.c
  deflate.c
  gzclose.c
  gzlib.c
  gzread.c
  gzwrite.c
  infback.c
  inffast.c
  inflate.c
  inftrees.c
  trees.c
  uncompr.c
  zutil.c
)

add_library(streamfind::zlib ALIAS streamfind_zlib)

target_include_directories(
  streamfind_zlib
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(
  streamfind_zlib
  PRIVATE
    ZLIB_BUILD
    $<$<BOOL:${MSVC}>:_CRT_SECURE_NO_DEPRECATE>
    $<$<BOOL:${MSVC}>:_CRT_NONSTDC_NO_DEPRECATE>
)

set_target_properties(
  streamfind_zlib
  PROPERTIES
    OUTPUT_NAME z
    POSITION_INDEPENDENT_CODE ON
)
