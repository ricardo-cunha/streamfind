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

#include <atomic>
#include <cstdlib>
#include <cstring>
#include "isa_kernels.h"

// Compiled at the baseline, always. It must not be built for a raised -march:
// it runs before anything has established what the CPU can do.

#if !ISOSPEC_ISA_DISPATCH
// No dispatch: the kernels are compiled straight into this translation unit at
// whatever -march the build used, and the "selection" is a constant. This is
// what a plain unity build, and the R package, get.
#define ISOSPEC_ISA_NAMESPACE isa_native
#define ISOSPEC_ISA_NAME "native"
#include "isa_kernel_impl.h"
#endif

namespace IsoSpec
{

#if ISOSPEC_ISA_DISPATCH

// Defined by the isa_kernels_*.cpp translation units, each compiled with its own
// -march. Only ever referenced through these declarations: nothing from those
// TUs is inlined here, which is the whole point.
namespace isa_baseline { extern const IsaKernels kernels; }
#if ISOSPEC_ISA_BUILD_X86
namespace isa_avx { extern const IsaKernels kernels; }
namespace isa_v3 { extern const IsaKernels kernels; }
namespace isa_v4 { extern const IsaKernels kernels; }
#endif

namespace
{

const IsaKernels* level_table(IsaLevel level)
{
    switch(level)
    {
        case ISA_LEVEL_BASELINE: return &isa_baseline::kernels;
#if ISOSPEC_ISA_BUILD_X86
        case ISA_LEVEL_AVX:      return &isa_avx::kernels;
        case ISA_LEVEL_V3:       return &isa_v3::kernels;
        case ISA_LEVEL_V4:       return &isa_v4::kernels;
#else
        case ISA_LEVEL_AVX:      return nullptr;
        case ISA_LEVEL_V3:       return nullptr;
        case ISA_LEVEL_V4:       return nullptr;
#endif
        case ISA_LEVEL_COUNT:
        default:                 return nullptr;
    }
}

// Whether the CPU (and the OS, which must have enabled the register state) can
// run a level. __builtin_cpu_supports covers both: libgcc's initialiser checks
// the CPUID feature bits and XCR0, so an AVX-capable CPU under a kernel that
// does not save the wide registers correctly reports false.
bool cpu_supports(IsaLevel level)
{
    if(level == ISA_LEVEL_BASELINE)
        return true;  // architecturally guaranteed, by definition of "baseline"

#if ISOSPEC_ISA_BUILD_X86 && (defined(__GNUC__) || defined(__clang__))
    switch(level)
    {
        case ISA_LEVEL_AVX:
            return __builtin_cpu_supports("avx");
        case ISA_LEVEL_V3:
            return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
        case ISA_LEVEL_V4:
            // x86-64-v4 is F+BW+DQ+VL together; a CPU with only some of those
            // cannot run code compiled for the set.
            return __builtin_cpu_supports("avx512f")
                && __builtin_cpu_supports("avx512bw")
                && __builtin_cpu_supports("avx512dq")
                && __builtin_cpu_supports("avx512vl");
        default:
            return false;
    }
#else
    // No way to ask: stay on the level that is always safe.
    return false;
#endif
}

// A level is usable only if it is both present in this binary and runnable here.
// "Present" is not a given: on a non-x86 build, and on a toolchain too old for
// the AVX-512 flags, the corresponding TU compiles to nothing and its table has
// no kernels.
bool level_usable(IsaLevel level)
{
    const IsaKernels* t = level_table(level);
    return t != nullptr && t->fill_run != nullptr && cpu_supports(level);
}

IsaLevel level_from_name(const char* s)
{
    if(strcmp(s, "baseline") == 0) return ISA_LEVEL_BASELINE;
    if(strcmp(s, "avx") == 0)      return ISA_LEVEL_AVX;
    if(strcmp(s, "v3") == 0)       return ISA_LEVEL_V3;
    if(strcmp(s, "v4") == 0)       return ISA_LEVEL_V4;
    return ISA_LEVEL_COUNT;        // unrecognised
}

IsaLevel best_available_level()
{
    IsaLevel best = ISA_LEVEL_BASELINE;
    for(int l = ISA_LEVEL_BASELINE; l < ISA_LEVEL_COUNT; l++)
        if(level_usable(static_cast<IsaLevel>(l)))
            best = static_cast<IsaLevel>(l);
    return best;
}

// The selection. Resolved on first use rather than during static
// initialisation, so it cannot lose a race with the initialisation of
// anything it depends on -- but deliberately *not* via a function-local
// static: that would need a runtime guard (a lock, on most ABIs) to stay
// safe if two threads race to initialize it for the first time, which is
// unnecessary here. Computing the answer is a pure, deterministic function
// of CPU features and an environment variable, so letting two racing
// threads compute and publish it twice is harmless; only the publication
// itself (the pointer store below) needs to be atomic.
struct Selection {
    IsaLevel level;
    const IsaKernels* kernels;
};

Selection compute_selection()
{
    Selection s;
    s.level = best_available_level();

    // An override lets a user work around a CPU whose wide units are a trap
    // (or slower), and lets the test suite exercise every level the host can
    // actually run. Ignored, rather than rejected, if it names something
    // unavailable -- a stray environment variable must not stop the library.
    const char* env = getenv("ISOSPEC_ISA_LEVEL");
    if(env != nullptr)
    {
        const IsaLevel wanted = level_from_name(env);
        if(wanted != ISA_LEVEL_COUNT && level_usable(wanted))
            s.level = wanted;
    }

    s.kernels = level_table(s.level);
    return s;
}

std::atomic<const IsaKernels*> g_kernels{nullptr};
std::atomic<IsaLevel> g_level{ISA_LEVEL_BASELINE};

Selection selection()
{
    const IsaKernels* k = g_kernels.load(std::memory_order_acquire);
    if(k != nullptr)
        return Selection{g_level.load(std::memory_order_relaxed), k};

    const Selection s = compute_selection();
    g_level.store(s.level, std::memory_order_relaxed);
    g_kernels.store(s.kernels, std::memory_order_release);
    return s;
}

}  // anonymous namespace

const IsaKernels& isa_kernels()
{
    return *selection().kernels;
}

IsaLevel current_isa_level()
{
    return selection().level;
}

IsaLevel max_supported_isa_level()
{
    return best_available_level();
}

bool isa_level_available(IsaLevel level)
{
    return level_usable(level);
}

bool set_isa_level(IsaLevel level)
{
    if(!level_usable(level))
        return false;
    g_level.store(level, std::memory_order_relaxed);
    g_kernels.store(level_table(level), std::memory_order_release);
    return true;
}

#else  // !ISOSPEC_ISA_DISPATCH

const IsaKernels& isa_kernels()
{
    return isa_native::kernels;
}

IsaLevel current_isa_level() { return ISA_LEVEL_BASELINE; }

IsaLevel max_supported_isa_level() { return ISA_LEVEL_BASELINE; }

bool isa_level_available(IsaLevel level) { return level == ISA_LEVEL_BASELINE; }

bool set_isa_level(IsaLevel level) { return level == ISA_LEVEL_BASELINE; }

#endif  // ISOSPEC_ISA_DISPATCH

namespace
{

// What this translation unit's own -march gives. It is compiled with the
// build's ambient flags -- exactly the ones isa_kernels_baseline.cpp gets, since
// IsaDispatch.cmake raises -march only for the raised levels' sources -- so this is
// the honest answer for the baseline level too, including the `-march=native`
// builds where "baseline" is in fact as wide as the host allows.
const char* ambient_simd_name()
{
#if defined(__AVX512F__)
    return "avx512";
#elif defined(__AVX2__)
    return "avx2";
#elif defined(__AVX__)
    return "avx";
#elif ISOSPEC_ISA_BUILD_X86
    return "sse2";
#elif defined(__aarch64__) || defined(__ARM_NEON) || defined(_M_ARM64)
    return "neon";
#else
    return "simd";
#endif
}

}  // anonymous namespace

const char* active_simd_level()
{
    const IsaKernels& k = isa_kernels();

    // The stub tables: a level that was not built (fill_run == nullptr), or one
    // built without any SIMD to build with, which publishes a kernel that emits
    // nothing and leaves the whole run to the caller's scalar drain.
    if(k.fill_run == nullptr || k.lanes <= 1)
        return "scalar";

#if ISOSPEC_ISA_DISPATCH
    // The raised levels are known exactly: their sources are compiled with the
    // flags naming them, and refuse to publish a table otherwise.
    switch(current_isa_level())
    {
        case ISA_LEVEL_AVX: return "avx";
        case ISA_LEVEL_V3: return "avx2";
        case ISA_LEVEL_V4: return "avx512";
        default:           break;  // baseline: whatever the ambient flags gave
    }
#endif

    return ambient_simd_name();
}

const char* isa_level_name(IsaLevel level)
{
    switch(level)
    {
        case ISA_LEVEL_BASELINE: return "baseline";
        case ISA_LEVEL_AVX:      return "avx";
        case ISA_LEVEL_V3:       return "v3";
        case ISA_LEVEL_V4:       return "v4";
        case ISA_LEVEL_COUNT:
        default:                 return "unknown";
    }
}

}  // namespace IsoSpec
