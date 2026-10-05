// Runtime ISA dispatch: every instruction-set level this binary carries and this
// CPU can run must compute the same thing.
//
// The batched fill kernels exist once per level (isa_kernels_*.cpp), each built
// with its own -march, and one is chosen from the CPU at startup.  Batching is
// only a chunking of a sequential walk, and the arithmetic per configuration is
// identical whatever the vector width, so:
//
//   * threshold_init's fill must be BIT-identical across levels -- same
//     configurations, same order, same operations;
//   * Binned's scatter must agree to within rounding.  It cannot be required to
//     be bit-identical, because the split between what the kernel batches and
//     what the scalar drain handles moves with the width, and the scalar drain's
//     `acc[bin] += partialProb * marginalProb` is a multiply-add that
//     -ffp-contract=fast fuses while the batched path (whose multiply already
//     happened in a vector register) cannot.  See test_simd_equivalence.cpp.
//
// Which levels are actually exercised depends on the host: a machine without
// AVX-512 cannot run the v4 kernel at all, and this file will silently skip it.
// The one thing that is always checked is that whatever IS available agrees.

#include "doctest.h"
#include "test_helpers.h"
#include "isa_kernels.h"

using namespace IsoSpec;
using test_helpers::small_formulas;

namespace {

// Restores whatever level was selected, so one test cannot perturb the others.
struct IsaLevelGuard {
    IsaLevel saved;
    IsaLevelGuard() : saved(current_isa_level()) {}
    ~IsaLevelGuard() { set_isa_level(saved); }
};

std::vector<IsaLevel> available_levels() {
    std::vector<IsaLevel> v;
    for (int l = 0; l < ISA_LEVEL_COUNT; ++l)
        if (isa_level_available(static_cast<IsaLevel>(l)))
            v.push_back(static_cast<IsaLevel>(l));
    return v;
}

}  // namespace

TEST_CASE("ISA dispatch: the baseline level is always present and selectable") {
    IsaLevelGuard guard;
    CHECK(isa_level_available(ISA_LEVEL_BASELINE));
    CHECK(set_isa_level(ISA_LEVEL_BASELINE));
    CHECK(isa_kernels().fill_run != nullptr);
    CHECK(isa_kernels().bin_run != nullptr);
    CHECK(isa_kernels().lanes >= 1);
    MESSAGE("levels available here: " << available_levels().size()
            << ", highest = " << std::string(isa_level_name(max_supported_isa_level())));
}

TEST_CASE("ISA dispatch: an unavailable level is refused, not silently accepted") {
    IsaLevelGuard guard;
    for (int l = 0; l < ISA_LEVEL_COUNT; ++l) {
        IsaLevel lv = static_cast<IsaLevel>(l);
        // set_isa_level must agree with isa_level_available, both ways round:
        // selecting a level the CPU cannot run would mean SIGILL on first use.
        CHECK(set_isa_level(lv) == isa_level_available(lv));
    }
}

TEST_CASE("ISA dispatch: wider levels report wider vectors") {
    IsaLevelGuard guard;
    std::size_t previous = 0;
    for (IsaLevel lv : available_levels()) {
        REQUIRE(set_isa_level(lv));
        const std::size_t lanes = isa_kernels().lanes;
        INFO("level " << isa_level_name(lv) << " -> " << lanes << " lanes");
        CHECK(lanes >= previous);   // monotone: a higher level is never narrower
        previous = lanes;
    }
}

TEST_CASE("ISA dispatch: FromThreshold is bit-identical across levels") {
    IsaLevelGuard guard;
    const std::vector<IsaLevel> levels = available_levels();
    if (levels.size() < 2) {
        MESSAGE("only one ISA level runnable on this host; cross-level check skipped");
        return;
    }

    for (const char* f : small_formulas()) {
        INFO("formula=" << f);
        for (double thr : {1e-4, 1e-8, 1e-12}) {
            CAPTURE(thr);

            REQUIRE(set_isa_level(levels[0]));
            FixedEnvelope ref = FixedEnvelope::FromThreshold(Iso(f), thr, true, false);

            for (std::size_t i = 1; i < levels.size(); ++i) {
                INFO("level=" << isa_level_name(levels[i]));
                REQUIRE(set_isa_level(levels[i]));
                FixedEnvelope got = FixedEnvelope::FromThreshold(Iso(f), thr, true, false);

                REQUIRE(got.confs_no() == ref.confs_no());
                for (std::size_t k = 0; k < ref.confs_no(); ++k) {
                    REQUIRE(got.masses()[k] == ref.masses()[k]);
                    REQUIRE(got.probs()[k] == ref.probs()[k]);
                }
            }
        }
    }
}

TEST_CASE("ISA dispatch: Binned agrees across levels") {
    IsaLevelGuard guard;
    const std::vector<IsaLevel> levels = available_levels();
    if (levels.size() < 2) {
        MESSAGE("only one ISA level runnable on this host; cross-level check skipped");
        return;
    }

    // Wide bins on a full enumeration: the case the batched scatter helps most,
    // and so the one where a width-dependent bug would show up first.
    for (const char* f : small_formulas()) {
        INFO("formula=" << f);
        for (double w : {1.0, 0.01}) {
            CAPTURE(w);

            REQUIRE(set_isa_level(levels[0]));
            FixedEnvelope ref = FixedEnvelope::Binned(Iso(f), 1.0, w, 0.0);

            for (std::size_t i = 1; i < levels.size(); ++i) {
                INFO("level=" << isa_level_name(levels[i]));
                REQUIRE(set_isa_level(levels[i]));
                FixedEnvelope got = FixedEnvelope::Binned(Iso(f), 1.0, w, 0.0);

                REQUIRE(got.confs_no() == ref.confs_no());
                for (std::size_t k = 0; k < ref.confs_no(); ++k) {
                    // Bin positions are integer-grid arithmetic: exact.
                    REQUIRE(got.masses()[k] == ref.masses()[k]);
                    REQUIRE(std::abs(got.probs()[k] - ref.probs()[k])
                            <= 1e-12 * std::abs(ref.probs()[k]));
                }
            }
        }
    }
}

TEST_CASE("ISA dispatch: every level fills a known distribution correctly") {
    // Guards against a level that agrees with the others because they are all
    // broken the same way: compare each one against an independent enumeration.
    IsaLevelGuard guard;
    for (IsaLevel lv : available_levels()) {
        INFO("level=" << isa_level_name(lv));
        REQUIRE(set_isa_level(lv));

        std::vector<test_helpers::Peak> expected = test_helpers::enumerate_threshold_full("C6H12O6");
        test_helpers::sort_peaks(expected);

        FixedEnvelope got = FixedEnvelope::FromThreshold(Iso("C6H12O6"), 0.0, true, false);
        std::vector<test_helpers::Peak> actual;
        for (std::size_t i = 0; i < got.confs_no(); ++i)
            actual.push_back({got.masses()[i], got.probs()[i]});
        test_helpers::sort_peaks(actual);

        REQUIRE(actual.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            REQUIRE(actual[i].mass == doctest::Approx(expected[i].mass));
            REQUIRE(actual[i].prob == doctest::Approx(expected[i].prob));
        }
    }
}

TEST_CASE("ISA dispatch: active_simd_level names what is actually running") {
    IsaLevelGuard guard;

    // The vocabulary is a contract: callers (the C ABI, IsoSpecPy) match on
    // these tokens, so a level must never report something outside the set.
    auto known = [](const std::string& s) {
        return s == "scalar" || s == "sse2" || s == "avx" || s == "avx2"
            || s == "avx512" || s == "neon" || s == "simd";
    };

    for (IsaLevel lv : available_levels()) {
        INFO("level=" << isa_level_name(lv));
        REQUIRE(set_isa_level(lv));

        const char* name = active_simd_level();
        REQUIRE(name != nullptr);
        const std::string s(name);
        CHECK(known(s));

        // "scalar" is exactly the no-vectorisation case, and nothing else is:
        // a lanes>1 kernel must not report scalar, nor a stub report a width.
        CHECK((s == "scalar") == (isa_kernels().lanes <= 1));

        // The raised levels are compiled with the flags that name them, so
        // their report is not a guess and must be exact.
        if (lv == ISA_LEVEL_AVX && isa_kernels().lanes > 1) CHECK(s == "avx");
        if (lv == ISA_LEVEL_V3 && isa_kernels().lanes > 1) CHECK(s == "avx2");
        if (lv == ISA_LEVEL_V4 && isa_kernels().lanes > 1) CHECK(s == "avx512");
    }

    MESSAGE("active_simd_level() here: " << std::string(active_simd_level()));
}
