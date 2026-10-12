message("")
message("${BoldRed}UNIX non APPLE environment${ColourReset}")
message("")
message("~~~~~~ Instructions ~~~~~~")
message("On UNIX/GNU-Linux, please run the configuration like this:")
message("cmake -G \"Unix Makefiles\" -DCMAKE_BUILD_TYPE=<Debug | Release> -DCMAKE_INSTALL_PREFIX=</usr | your_dir> ../development")
message("")

set(CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES /usr/include)
set(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES /usr/include)

## platform dependent compiler flags:
include(CheckCXXCompilerFlag)

if(WITH_FPIC)
	add_definitions(-fPIC)
endif()

# Install cmake module
install(FILES ${CMAKE_MODULE_PATH}/FindIsoSpec++.cmake 
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/isospec++)

# Install cmake config
configure_file(${CMAKE_MODULE_PATH}/IsoSpec++Config.cmake.in
	${CMAKE_BINARY_DIR}/IsoSpec++Config.cmake)
install(FILES ${CMAKE_BINARY_DIR}/IsoSpec++Config.cmake 
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/isospec++)

# Install the PkgConfig config file (only substitute the @VAR@
# because we need to preserve all the ${prefix} strings like
# they are.
configure_file(${CMAKE_MODULE_PATH}/pkgconfig/libisospec++.pc.in
	${CMAKE_BINARY_DIR}/libisospec++.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/libisospec++.pc 
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# Documentation and examples require source assets that are not part of the
# vendored runtime library payload. Keep them opt-in for downstream builds.
option(ISOSPEC_BUILD_DOCUMENTATION "Build IsoSpec documentation" OFF)
if(ISOSPEC_BUILD_DOCUMENTATION)
  add_subdirectory(man)
endif()

option(ISOSPEC_BUILD_EXAMPLES "Build IsoSpec examples" OFF)
if(ISOSPEC_BUILD_EXAMPLES)
  add_subdirectory(Examples)
endif()

