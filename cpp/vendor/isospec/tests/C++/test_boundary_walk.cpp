// Run-boundary machinery: count_confs()'s per-line boundary scans and the
// layered generator's band-boundary scans (resetPositions).
//
// Both walk, for every "line" (a joint state of marginals 1..n), a boundary
// inside marginal 0's descending lProbs array: count_confs the cutoff boundary
// of a prefix run, the layered carry both edges of a threshold band.  The
// correctness of those walks depends on how the boundary moves between
// consecutive lines and across higher-level carries, so the tests here stress
// exactly that: long sweeps with slowly-moving boundaries, molecules with
// exactly tied isotope probabilities (the boundary stalls; cutoffs repeat to
// the last bit), higher-level carries that move the boundary back up, empty
// runs, and layer schedules that make bands arbitrarily thin or wide.
//
// Everything is a property test against an independent enumeration, so these
// tests pin the behaviour for any reimplementation of the scans (galloping,
// boundary continuation, ...).

#include <cmath>
#include <cstddef>
#include <random>
#include <utility>
#include <vector>

#include "doctest.h"
#include "test_helpers.h"

using namespace IsoSpec;
using namespace test_helpers;

namespace {

// Assertion accumulator for million-configuration loops; see test_generators.
struct Tally {
    std::size_t checked = 0;
    std::size_t failed = 0;
    void expect(bool ok) { ++checked; if (!ok) ++failed; }
    bool clean() const { return failed == 0 && checked > 0; }
};

Iso make_iso(const std::vector<int>& isotope_numbers,
             const std::vector<int>& atom_counts,
             const std::vector<double>& masses,
             const std::vector<double>& probs) {
    return Iso(static_cast<int>(isotope_numbers.size()), isotope_numbers.data(),
               atom_counts.data(), masses.data(), probs.data());
}

// Walk a threshold generator to exhaustion, or until cap configurations.
// Returns the peaks and whether the cap was hit.
std::pair<std::vector<Peak>, bool> walk_threshold(IsoThresholdGenerator& g,
                                                  std::size_t cap = SIZE_MAX) {
    std::vector<Peak> out;
    while (g.advanceToNextConfiguration()) {
        out.push_back({g.mass(), g.prob()});
        if (out.size() >= cap) return {std::move(out), true};
    }
    return {std::move(out), false};
}

// A molecule whose marginals are full of exact probability ties: every isotope
// of an element equally likely.  Consecutive lines of a sweep then share the
// same cutoff to the last bit, and marginal-0 lProbs contains long runs of
// bit-identical values, so any boundary walk must handle "moved by zero" and
// ties at the boundary itself.
Iso tied_iso() {
    return make_iso({4, 2, 3},
                    {12, 20, 6},
                    {10.0, 11.0, 12.0, 13.0,   1.0, 2.0,   50.0, 51.0, 52.0},
                    {0.25, 0.25, 0.25, 0.25,   0.5, 0.5,   0.25, 0.25, 0.5});
}

}  // namespace

TEST_CASE("count_confs equals a full walk across thresholds and knobs") {
    // Formulas chosen for boundary-walk shapes: single-marginal (dimNumber==1
    // shortcut), monoisotopic marginals (single-line sweeps), many-isotope
    // elements (Se/Sn: long lines, big boundary jumps), and S-heavy molecules
    // (many near-flat sweeps).
    // Second field: deepest threshold to walk; keeps the walked counts small
    // enough for the -O0/_GLIBCXX_DEBUG configuration.
    const std::vector<std::pair<const char*, double>> formulas = {
        {"C10", 1e-9}, {"O200", 1e-9}, {"F100", 1e-9}, {"H2O1", 1e-9},
        {"C6H12O6", 1e-9}, {"Fe2O3", 1e-9}, {"Se5Sn3", 1e-6}, {"S10Se5", 1e-6},
        {"C50H100N20O20S5", 1e-4}, {"C150H250N50O50S3", 1e-4},
        // Monoisotopic elements make trivial marginals; with exactly one
        // nontrivial marginal its confs stay UNSORTED (marginalsNeedSorting is
        // false), so per-line cutoffs zigzag instead of moving monotonically.
        {"F10C10", 1e-9}, {"Na5P2S10", 1e-9}, {"F3Sn2", 1e-9}, {"P1O100", 1e-9},
    };
    for (const auto& [f, deepest] : formulas) {
        INFO("formula=" << f);
        for (double threshold : {0.5, 1e-2, 1e-4, 1e-6, 1e-9}) {
            if (threshold < deepest) continue;
            for (bool absolute : {true, false}) {
                for (bool reorder : {true, false}) {
                    CAPTURE(threshold);
                    CAPTURE(absolute);
                    CAPTURE(reorder);
                    IsoThresholdGenerator g(Iso(f), threshold, absolute, 1000, 1000, reorder);
                    const std::size_t counted = g.count_confs();
                    g.reset();
                    // Counting again after reset gives the same answer.
                    CHECK(g.count_confs() == counted);
                    g.reset();
                    CHECK(walk_threshold(g).first.size() == counted);
                }
            }
        }
    }
}

TEST_CASE("count_confs equals a full walk with exactly tied probabilities") {
    for (double threshold : {0.9, 1e-1, 1e-3, 1e-5, 1e-8}) {
        for (bool reorder : {true, false}) {
            CAPTURE(threshold);
            CAPTURE(reorder);
            IsoThresholdGenerator g(tied_iso(), threshold, false, 1000, 1000, reorder);
            const std::size_t counted = g.count_confs();
            g.reset();
            CHECK(walk_threshold(g).first.size() == counted);
        }
    }
}

TEST_CASE("count_confs and FromThreshold vs walk on random molecules") {
    // Randomized molecules with probabilities drawn from a small integer grid,
    // which makes exact ties frequent, plus random dimensionality and atom
    // counts — shapes no fixed formula list would cover.
    std::mt19937 rng(20260813);
    std::uniform_int_distribution<int> dim_d(1, 4), iso_d(1, 6), grid_d(1, 6),
        cnt_d(1, 40);  // iso_d from 1: monoisotopic elements make trivial
                       // marginals, and a lone nontrivial one stays unsorted.
    std::uniform_real_distribution<double> mass_d(1.0, 30.0);

    // Number of subisotopologues of an element: C(count + k - 1, k - 1).
    auto marginal_support = [](int count, int k) {
        double v = 1.0;
        for (int i = 1; i < k; ++i) v *= static_cast<double>(count + i) / i;
        return v;
    };

    int compared = 0;
    for (int round = 0; round < 40; ++round) {
        CAPTURE(round);
        std::vector<int> isotope_numbers, atom_counts;
        std::vector<double> masses, probs;
        // Rejection-sample until the full support is enumerable: count_confs()
        // visits every above-threshold line, so an unbounded draw could make
        // even counting intractable.
        double support;
        do {
            isotope_numbers.clear(); atom_counts.clear();
            masses.clear(); probs.clear();
            support = 1.0;
            const int dim = dim_d(rng);
            for (int e = 0; e < dim; ++e) {
                const int k = iso_d(rng);
                isotope_numbers.push_back(k);
                atom_counts.push_back(cnt_d(rng));
                support *= marginal_support(atom_counts.back(), k);
                std::vector<int> grid(k);
                int total = 0;
                for (int& gi : grid) { gi = grid_d(rng); total += gi; }
                for (int gi : grid) {
                    probs.push_back(static_cast<double>(gi) / total);
                    masses.push_back(mass_d(rng));
                }
            }
        } while (support > 200000.0);
        const double threshold = (rng() % 2) ? 1e-2 : 1e-4;

        IsoThresholdGenerator g(make_iso(isotope_numbers, atom_counts, masses, probs),
                                threshold, false);
        const std::size_t counted = g.count_confs();
        g.reset();
        auto [walked, capped] = walk_threshold(g, 300000);
        if (capped) continue;  // Cannot happen given the support bound; belt only.
        ++compared;
        CHECK(walked.size() == counted);

        // The materialized envelope (the SIMD-batched fill sized by the count)
        // must contain exactly the walked peaks.
        FixedEnvelope env = FixedEnvelope::FromThreshold(
            make_iso(isotope_numbers, atom_counts, masses, probs), threshold, false, false);
        std::vector<Peak> mat;
        mat.reserve(env.confs_no());
        for (std::size_t i = 0; i < env.confs_no(); ++i)
            mat.push_back({env.masses()[i], env.probs()[i]});
        CHECK(peaks_close(walked, mat, 1e-12));
    }
    CHECK(compared >= 30);  // The cap must stay the exception, or this tests nothing.
}

TEST_CASE("layered generator: adversarial layer schedules enumerate the full support") {
    // The layer step controls how far the band boundaries move per sweep: tiny
    // steps make thin bands (boundaries crawl; runs are often empty), huge
    // steps swallow most of the distribution in one layer, and mixed steps
    // interleave both regimes.  Whatever the schedule, running to exhaustion
    // must reproduce the full support.  The thin step is scaled per formula:
    // its cost grows with support depth times line count, and the suite's
    // debug configuration runs ~30x slower than release.
    struct Job {
        const char* formula;
        double thin;
        double mixed_a, mixed_b;
    };
    const std::vector<Job> jobs = {
        {"H2O1", -0.01, -0.005, -5.0},
        {"Fe2O3", -0.05, -0.02, -3.0},
        {"C6H12O6", -0.5, -0.1, -4.0},
        {"C10H20O2", -1.0, -0.4, -6.0},
        // Single nontrivial marginal (unsorted; see the count_confs matrix).
        {"F10C10", -0.05, -0.02, -3.0},
        {"Na5P2S10", -0.1, -0.04, -3.0},
        {"F3Sn2", -0.05, -0.02, -3.0},
    };

    for (const Job& j : jobs) {
        INFO("formula=" << j.formula);
        const std::vector<Peak> reference = enumerate_threshold_full(j.formula);

        for (int mode = 0; mode < 4; ++mode) {
            CAPTURE(mode);  // 0: thin, 1: huge, 2: alternating, 3: random
            std::mt19937 rng(4001 + mode);
            std::uniform_real_distribution<double> step_d(-4.0, -0.004);
            IsoLayeredGenerator g{Iso(j.formula)};
            std::vector<Peak> got;
            int layer = 0;
            do {
                while (g.advanceToNextConfigurationWithinLayer())
                    got.push_back({g.mass(), g.prob()});
                ++layer;
            } while (g.nextLayer(mode == 0 ? j.thin :
                                 mode == 1 ? -30.0 :
                                 mode == 2 ? (layer % 2 ? j.mixed_a : j.mixed_b)
                                           : step_d(rng)));
            CHECK(got.size() == reference.size());
            CHECK(peaks_close(got, reference, 1e-12));
        }
    }
}

TEST_CASE("layered generator: tied probabilities under adversarial schedules") {
    // Exact ties + thin layers: band edges repeatedly land exactly on runs of
    // bit-identical lProbs, the hardest case for either boundary scan.
    std::vector<Peak> reference;
    {
        IsoThresholdGenerator g(tied_iso(), 0.0, true);
        reference = walk_threshold(g).first;
    }
    REQUIRE(reference.size() > 1000);

    for (double step : {-0.05, -1.0, -25.0}) {
        CAPTURE(step);
        IsoLayeredGenerator g{tied_iso()};
        std::vector<Peak> got;
        do {
            while (g.advanceToNextConfigurationWithinLayer())
                got.push_back({g.mass(), g.prob()});
        } while (g.nextLayer(step));
        CHECK(got.size() == reference.size());
        CHECK(peaks_close(got, reference, 1e-12));
    }
}

TEST_CASE("layered generator: every emitted peak lies within its layer's band") {
    // Each configuration must be emitted in the one layer whose band contains
    // it: lprob below the previous threshold (or it would have been emitted
    // earlier) and at-or-above the current one (or it is not yet due).  The
    // tolerance covers the rounding difference between the generator's
    // rebased comparisons and lprob() recomputed here.
    auto check_bands = [](IsoLayeredGenerator&& g) {
        double last_threshold = 1.0;  // Log-prob, so effectively +infinity.
        Tally t;
        int layers = 0;
        bool more = true;
        while (more && layers < 30) {
            const double current_threshold = g.get_currentLThreshold();
            while (g.advanceToNextConfigurationWithinLayer())
                t.expect(g.lprob() >= current_threshold - 1e-9 &&
                         g.lprob() <= last_threshold + 1e-9);
            last_threshold = current_threshold;
            more = g.nextLayer(-1.0);
            ++layers;
        }
        CHECK(t.clean());
        CHECK(layers >= 10);
    };
    check_bands(IsoLayeredGenerator{Iso("C10H20O2")});
    check_bands(IsoLayeredGenerator{tied_iso()});
}

TEST_CASE("Binned below-1.0 target agrees with a binned deep-threshold envelope") {
    // The target<1.0 branch of Binned() rides the layered generator (its bin
    // filler drives layers directly), so this pins the whole layered pathway
    // end to end: every partial bin must also exist in a much deeper
    // enumeration, with no more probability than the deep bin holds, and the
    // kept mass must reach the target.
    Iso iso("C50H100N20O20S5");
    FixedEnvelope deep_env = FixedEnvelope::FromThreshold(Iso(iso, true), 1e-10, true, false);
    for (double width : {1.0, 0.25}) {
        for (double middle : {0.0, 0.3}) {
            FixedEnvelope deep = FixedEnvelope(deep_env).bin(width, middle);
            std::vector<Peak> deep_sorted;
            for (std::size_t i = 0; i < deep.confs_no(); ++i)
                deep_sorted.push_back({deep.masses()[i], deep.probs()[i]});
            sort_peaks(deep_sorted);

            for (double target : {0.9, 0.999, 0.9999}) {
                CAPTURE(width);
                CAPTURE(middle);
                CAPTURE(target);
                FixedEnvelope part = FixedEnvelope::Binned(Iso(iso, true), target, width, middle);
                CHECK(part.get_total_prob() >= target - 1e-9);
                Tally t;
                for (std::size_t i = 0; i < part.confs_no(); ++i) {
                    // Find the matching deep bin: it must exist and dominate.
                    bool ok = false;
                    for (const Peak& d : deep_sorted)
                        if (std::fabs(d.mass - part.masses()[i]) < width * 1e-6) {
                            ok = part.probs()[i] <= d.prob + 1e-9;
                            break;
                        }
                    t.expect(ok);
                }
                CHECK(t.clean());
            }
        }
    }
}

TEST_CASE("Binned at-least-1.0 target equals binning the full envelope") {
    for (const char* f : {"C6H12O6", "Fe2O3", "C10H20O2"}) {
        INFO("formula=" << f);
        FixedEnvelope full = FixedEnvelope::Binned(Iso(f), 1.0, 1.0, 0.0);
        FixedEnvelope manual = FixedEnvelope::FromThreshold(Iso(f), 0.0, true, false).bin(1.0, 0.0);

        std::vector<Peak> a, b;
        for (std::size_t i = 0; i < full.confs_no(); ++i)
            a.push_back({full.masses()[i], full.probs()[i]});
        for (std::size_t i = 0; i < manual.confs_no(); ++i)
            if (manual.probs()[i] > 0.0)
                b.push_back({manual.masses()[i], manual.probs()[i]});
        CHECK(peaks_close(a, b, 1e-9));
    }
}

TEST_CASE("single-atom layered generator under adversarial schedules") {
    // The SingleAtomMarginal instantiation shares the carry/resetPositions
    // machinery; drive it with the same thin/wide layer schedules.  Tied
    // probabilities included: three of five isotopes equally likely.
    const std::vector<int> isotope_numbers = {5, 5};
    const std::vector<int> atom_counts = {1, 1};
    const std::vector<double> masses = {1.0, 10.0, 100.0, 1000.0, 10000.0,
                                        2.0, 20.0, 200.0, 2000.0, 20000.0};
    const std::vector<double> probs = {0.2, 0.2, 0.2, 0.15, 0.25,
                                       0.1, 0.2, 0.3, 0.15, 0.25};

    std::vector<Peak> reference;
    {
        IsoThresholdGenerator g(make_iso(isotope_numbers, atom_counts, masses, probs),
                                0.0, true);
        reference = walk_threshold(g).first;
    }
    REQUIRE(reference.size() == 25);

    for (double step : {-0.03, -2.0, -40.0}) {
        CAPTURE(step);
        IsoLayeredGeneratorTemplate<SingleAtomMarginal<true>> g{
            make_iso(isotope_numbers, atom_counts, masses, probs)};
        std::vector<Peak> got;
        do {
            while (g.advanceToNextConfigurationWithinLayer())
                got.push_back({g.mass(), g.prob()});
        } while (g.nextLayer(step));
        CHECK(got.size() == reference.size());
        CHECK(peaks_close(got, reference, 1e-12));
    }
}

TEST_CASE("stochastic generator rides the layered bands consistently") {
    // IsoStochasticGenerator drives the layered generator's within-layer
    // advance internally; a high-precision request forces it deep into the
    // layer machinery.  Counts must exactly exhaust the sample regardless.
    for (const char* f : {"C50H100N20O20S5", "C10H20O2"}) {
        INFO("formula=" << f);
        std::mt19937 rng(987);
        IsoStochasticGenerator g(Iso(f), 20000, 0.99999, 5.0, rng);
        double total = 0.0;
        while (g.advanceToNextConfiguration())
            total += g.prob();
        CHECK(total == doctest::Approx(20000.0));
    }
}
