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

// The batched kernels at x86-64-v3: AVX2+FMA, 4 doubles per vector. This is the level the measured speedups were taken at.
//
// NOTE TO THE BUILD SYSTEM: compile as its own translation unit with
// `-mavx2 -mfma` (or -march=x86-64-v3). The guard below is the
// authority, not the build system: if the flags did not arrive, the level
// publishes an empty table and dispatch simply never selects it, rather than
// silently shipping baseline-width code under this level's name.

#include "platform.h"
#include "isa_kernels.h"

#if ISOSPEC_ISA_BUILD_X86 && defined(__AVX2__) && defined(__FMA__)

#define ISOSPEC_ISA_NAMESPACE isa_v3
#define ISOSPEC_ISA_NAME "v3 (AVX2+FMA)"
#include "isa_kernel_impl.h"

#else

namespace IsoSpec
{
namespace isa_v3
{
extern const IsaKernels kernels;
extern const IsaKernels kernels = { nullptr, nullptr, "v3 (not built)", 0 };
}  // namespace isa_v3
}  // namespace IsoSpec

#endif
