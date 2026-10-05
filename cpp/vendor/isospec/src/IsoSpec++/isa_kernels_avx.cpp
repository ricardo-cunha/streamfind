/*
 *   Copyright (C) 2015-2020 Mateusz Łącki and Michał Startek.
 *
 *   This file is part of IsoSpec.
 *
 *   IsoSpec is free software: you can redistribute it and/or modify
 *   it under the terms of the Simplified ("2-clause") BSD licence.
 *
 *   IsoSpec is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 *   You should have received a copy of the Simplified BSD Licence
 *   along with IsoSpec.  If not, see <https://opensource.org/licenses/BSD-2-Clause>.
 */

// The batched kernels at plain AVX: 4 doubles per vector, no FMA.
//
// This level exists for the CPUs that have AVX but not AVX2 -- Sandy Bridge and
// Ivy Bridge, and AMD's Bulldozer through Excavator. Without it they fall all
// the way back to the 2-double baseline, which is a real loss: measured on a
// Piledriver Opteron 6380, going from 2 to 4 lanes is worth 1.20-1.26x on the
// binned fill (and, as everywhere, nothing on the threshold fill, which is
// bound by memory rather than by vector width).
//
// Piledriver is the pessimistic case for this level, incidentally: its 256-bit
// units are split into two 128-bit halves, so it gets none of the raw
// throughput, only the halved loop and load count. Parts with genuine 256-bit
// units should do better.
//
// NOTE TO THE BUILD SYSTEM: compile as its own translation unit with
// `-mavx`. The guard below is the
// authority, not the build system: if the flags did not arrive, the level
// publishes an empty table and dispatch simply never selects it, rather than
// silently shipping baseline-width code under this level's name.

#include "platform.h"
#include "isa_kernels.h"

#if ISOSPEC_ISA_BUILD_X86 && defined(__AVX__)

#define ISOSPEC_ISA_NAMESPACE isa_avx
#define ISOSPEC_ISA_NAME "avx (AVX, no FMA)"
#include "isa_kernel_impl.h"

#else

namespace IsoSpec
{
namespace isa_avx
{
extern const IsaKernels kernels;
extern const IsaKernels kernels = { nullptr, nullptr, "avx (not built)", 0 };
}  // namespace isa_avx
}  // namespace IsoSpec

#endif
