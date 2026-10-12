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

#pragma once

#include <cstddef>
#include "platform.h"

namespace IsoSpec
{

// The widest vector any kernel in this library may use: AVX-512 gives 8 doubles,
// and nothing wider exists on a target we build for.
//
// Everything about the *data layout* is sized for this, unconditionally -- not
// for the width the current translation unit happens to compile to. With runtime
// dispatch (ISOSPEC_ISA_DISPATCH) the kernel that ends up running is chosen from
// the CPU, while the arrays it reads were laid out by code compiled at the
// baseline, so sizing them for anything narrower would let an AVX-512 kernel read
// past the end. Without dispatch it is merely a few doubles of slack.
constexpr std::size_t ISOSPEC_MAX_SIMD_LANES = 8;

// Number of -infinity sentinels appended past the end of every marginal's
// lProbs array. The generators walk that array forwards and stop at the first
// entry below the cutoff, so one sentinel would suffice to end a scalar run; a
// batched kernel reads W entries ahead of the current position and needs the
// whole read to stay inside the allocation, hence one sentinel per lane of the
// widest kernel that could be selected.
//
// Applied uniformly by every marginal class, including the ones no batched
// generator currently walks, so that the contract is a single rule with no
// per-class exception to remember.
constexpr std::size_t ISOSPEC_LPROB_GUARDIANS = ISOSPEC_MAX_SIMD_LANES;

}  // namespace IsoSpec
