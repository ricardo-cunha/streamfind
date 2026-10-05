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

// The batched kernels at the plain target: SSE2 on x86-64, NEON on AArch64 --
// whatever is architecturally guaranteed, so this level is always present and
// is what dispatch falls back to. Built with no extra -march, deliberately.
//
// NOTE TO THE BUILD SYSTEM: this file must be compiled as its own translation
// unit, never folded into unity-build.cpp, and never with a raised -march. See
// isa_kernels.h for why.

#define ISOSPEC_ISA_NAMESPACE isa_baseline
#define ISOSPEC_ISA_NAME "baseline"
#include "isa_kernel_impl.h"
