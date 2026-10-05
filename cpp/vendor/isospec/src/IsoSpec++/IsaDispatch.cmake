# Runtime ISA dispatch, for the CMake builds.
#
# The two batched fill kernels are compiled once per instruction-set level, each
# into its own translation unit with its own -march, and the one to run is chosen
# from the CPU at startup. They must never share a compilation with the rest of
# the library: a raised -march would otherwise reach every inline function in the
# headers, and the linker could then pick an AVX-512 copy of one for a code path
# reached on a CPU without it. See isa_kernels.h for the full reasoning.
#
# Include this, then add ${isospec_ISA_SRCS} to your target's sources. It sets
# the per-source flags and defines ISOSPEC_ISA_DISPATCH=1 for the directory.
#
# The kernel sources deliberately get no -march of their own beyond the level
# they implement: the baseline level has to stay runnable on any CPU of the
# target architecture, since it is what dispatch falls back to.

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)

set(_isa_dir ${CMAKE_CURRENT_LIST_DIR})

# Worth turning off when building exclusively for the machine that will run the
# result (-march=native, say): dispatching costs an indirect call per marginal-0
# run -- measured at 0.5-3% on the binned fill and 2-7% on the threshold fill --
# which a single-level build compiled for the actual CPU does not pay.
option(ISOSPEC_ISA_DISPATCH "Build several instruction-set levels and select at run time" ON)

if(NOT ISOSPEC_ISA_DISPATCH)
	set(isospec_ISA_SRCS "")
	message(STATUS "IsoSpec: runtime ISA dispatch disabled; single-level build")
	return()
endif()

set(isospec_ISA_SRCS
	${_isa_dir}/isa_kernels_baseline.cpp
	${_isa_dir}/isa_kernels_avx.cpp
	${_isa_dir}/isa_kernels_v3.cpp
	${_isa_dir}/isa_kernels_v4.cpp
	)

set(_isa_avx_flags -mavx)
set(_isa_v3_flags -mavx2 -mfma)
set(_isa_v4_flags -mavx512f -mavx512bw -mavx512dq -mavx512vl)

# Only offer a level its flags where the compiler will take them; they are
# meaningless off x86, and older toolchains may not know the AVX-512 ones. The
# .cpp files then self-check the predefined macros those flags produce, so a
# level whose flags did not arrive publishes an empty table and dispatch simply
# never selects it -- rather than it being quietly built at the wrong width.
check_cxx_compiler_flag("-mavx" ISOSPEC_CXX_ACCEPTS_AVX)
check_cxx_compiler_flag("-mavx2 -mfma" ISOSPEC_CXX_ACCEPTS_V3)
check_cxx_compiler_flag("-mavx512f -mavx512bw -mavx512dq -mavx512vl" ISOSPEC_CXX_ACCEPTS_V4)

if(ISOSPEC_CXX_ACCEPTS_AVX)
	set_source_files_properties(${_isa_dir}/isa_kernels_avx.cpp
		PROPERTIES COMPILE_OPTIONS "${_isa_avx_flags}")
	message(STATUS "IsoSpec: building ISA level avx (AVX, no FMA)")
else()
	message(STATUS "IsoSpec: ISA level avx not available with this compiler/target")
endif()

if(ISOSPEC_CXX_ACCEPTS_V3)
	set_source_files_properties(${_isa_dir}/isa_kernels_v3.cpp
		PROPERTIES COMPILE_OPTIONS "${_isa_v3_flags}")
	message(STATUS "IsoSpec: building ISA level v3 (AVX2+FMA)")
else()
	message(STATUS "IsoSpec: ISA level v3 not available with this compiler/target")
endif()

if(ISOSPEC_CXX_ACCEPTS_V4)
	set_source_files_properties(${_isa_dir}/isa_kernels_v4.cpp
		PROPERTIES COMPILE_OPTIONS "${_isa_v4_flags}")
	message(STATUS "IsoSpec: building ISA level v4 (AVX-512)")
else()
	message(STATUS "IsoSpec: ISA level v4 not available with this compiler/target")
endif()

add_definitions(-DISOSPEC_ISA_DISPATCH=1)
