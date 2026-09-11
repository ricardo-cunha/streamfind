#ifndef NTA_ANNOTATION_H
#define NTA_ANNOTATION_H

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <algorithm>
#include <cmath>
#include <numeric>

#include <string>

namespace nta {
  namespace api { struct NTA_FEATURE_ROW; struct NTA_FEATURES; }
  namespace api { class PROJECT_NON_TARGET_ANALYSIS; }
  using PROJECT_NON_TARGET_ANALYSIS = api::PROJECT_NON_TARGET_ANALYSIS;
}

namespace nta
{
  namespace annotation
  {
    // MARK: ISOTOPE
    struct ISOTOPE
    {
      std::string element;
      std::string isotope;
      float mass_distance;
      float abundance;
      float abundance_monoisotopic;
      int min;
      int max;

      ISOTOPE(const std::string &e, const std::string &i, float md, float ab, float ab_mono, int mi, int ma)
          : element(e), isotope(i), mass_distance(md), abundance(ab), abundance_monoisotopic(ab_mono), min(mi), max(ma) {}
    };

    // MARK: ISOTOPE_SET
    struct ISOTOPE_SET
    {
      std::vector<ISOTOPE> data = {
          ISOTOPE("C", "13C", 1.0033548378, 0.01078, 0.988922, 1, 60),
          ISOTOPE("H", "2H", 1.0062767, 0.00015574, 0.99984426, 0, 120),
          ISOTOPE("B", "10B", 0.996809, 0.199, 0.801, 0, 2),
          ISOTOPE("N", "15N", 0.9970349, 0.003663, 0.996337, 0, 10),
          ISOTOPE("O", "17O", 1.004217, 0.00037, 0.99763, 0, 20),
          ISOTOPE("O", "18O", 2.004246, 0.00200, 0.99763, 0, 20),
          ISOTOPE("Mg", "25Mg", 0.999711, 0.10, 0.7899, 0, 2),
          ISOTOPE("Mg", "26Mg", 1.995796, 0.1101, 0.7899, 0, 2),
          ISOTOPE("Si", "29Si", 0.999568, 0.04683, 0.92230, 0, 6),
          ISOTOPE("Si", "30Si", 1.996844, 0.03087, 0.92230, 0, 6),
          ISOTOPE("S", "33S", 0.999388, 0.00750, 0.95018, 0, 4),
          ISOTOPE("S", "34S", 1.995796, 0.04215, 0.95018, 0, 4),
          ISOTOPE("S", "36S", 3.995010, 0.00017, 0.95018, 0, 4),
          ISOTOPE("Cl", "37Cl", 1.997050, 0.24229, 0.75771, 0, 6),
          ISOTOPE("Br", "81Br", 1.997953, 0.49314, 0.50686, 0, 4),
          ISOTOPE("K", "41K", 1.998119, 0.0673, 0.9327, 0, 2),
          ISOTOPE("Ca", "44Ca", 3.998159, 0.02086, 0.96941, 0, 2),
          ISOTOPE("Fe", "54Fe", -1.004391, 0.05845, 0.91754, 0, 2),
          ISOTOPE("Fe", "57Fe", 2.995294, 0.02119, 0.91754, 0, 2),
          ISOTOPE("Cu", "65Cu", 1.998204, 0.3085, 0.6915, 0, 2),
          ISOTOPE("Zn", "66Zn", 1.999059, 0.2773, 0.4917, 0, 2),
          ISOTOPE("Zn", "68Zn", 3.995796, 0.1845, 0.4917, 0, 2),
          ISOTOPE("Se", "77Se", 0.997953, 0.0763, 0.4961, 0, 2),
          ISOTOPE("Se", "78Se", 1.996004, 0.2377, 0.4961, 0, 2),
          ISOTOPE("Se", "80Se", 3.995010, 0.4961, 0.4961, 0, 2)};

      void filter(const std::vector<std::string> &el)
      {
        std::unordered_set<std::string> el_set(el.begin(), el.end());
        std::vector<ISOTOPE> data_filtered;
        for (const ISOTOPE &iso : data)
        {
          if (el_set.find(iso.element) != el_set.end())
          {
            data_filtered.push_back(iso);
          }
        }
        data = data_filtered;
      }

      void set_ranges(const std::unordered_map<std::string, std::pair<int, int>> &ranges)
      {
        for (auto &iso : data)
        {
          const auto it = ranges.find(iso.element);
          if (it != ranges.end())
          {
            iso.min = it->second.first;
            iso.max = it->second.second;
          }
        }
      }
    };

    // MARK: ISOTOPE_COMBINATIONS
    struct ISOTOPE_COMBINATIONS
    {
      std::vector<int> step;
      std::vector<std::string> isotopes_str;
      std::vector<float> abundances;
      std::vector<float> abundances_monoisotopic;
      std::vector<int> min;
      std::vector<int> max;
      std::vector<std::vector<std::string>> tensor_combinations;
      std::vector<std::vector<float>> tensor_mass_distances;
      std::vector<std::vector<float>> tensor_abundances;
      std::vector<float> mass_distances;
      int length;

      ISOTOPE_COMBINATIONS(ISOTOPE_SET &isotopes, const int &max_number_elements);
    };

    // MARK: ISOTOPE_CHAIN
    struct ISOTOPE_CHAIN
    {
      std::vector<nta::api::NTA_FEATURE_ROW> chain;
      std::vector<int> candidate_indices;
      std::vector<int> charge;
      std::vector<int> step;
      std::vector<float> mz;
      std::vector<float> rt;
      std::vector<float> mzr;
      std::vector<std::string> isotope;
      std::vector<float> mass_distance;
      std::vector<float> theoretical_mass_distance;
      std::vector<float> mass_distance_error;
      std::vector<float> time_error;
      std::vector<float> abundance;
      std::vector<float> theoretical_abundance_min;
      std::vector<float> theoretical_abundance_max;
      float number_carbons;
      int length;

      ISOTOPE_CHAIN(const int &z, const nta::api::NTA_FEATURE_ROW &mono_ion, float mono_mzr);
    };

    // MARK: ADDUCT
    struct ADDUCT
    {
      std::string element;
      int polarity;
      std::string cat;
      std::string type;
      int charge;
      int multiplicity;
      float mass_distance;

      ADDUCT(const std::string &e, const int &p, const std::string &c, const std::string &t, const float &md, const int &z, const int &m = 1)
          : element(e), polarity(p), cat(c), type(t), charge(z), multiplicity(m), mass_distance(md) {}
    };

    // MARK: ADDUCT_SET
    struct ADDUCT_SET
    {
      std::vector<ADDUCT> neutralizers{
          ADDUCT("H", 1, "[M+H]+", "[M+H]+", -1.007276, 1),
          ADDUCT("H", -1, "[M-H]-", "[M-H]-", 1.007276, 1)};

      std::vector<ADDUCT> all_adducts{
          ADDUCT("H", 1, "adduct", "[M+H]+", 1.007276f, 1, 1),
          ADDUCT("Na", 1, "adduct", "[M+Na]+", 22.989218f, 1, 1),
          ADDUCT("K", 1, "adduct", "[M+K]+", 38.963158f, 1, 1),
          ADDUCT("NH4", 1, "adduct", "[M+NH4]+", 18.033823f, 1, 1),
          ADDUCT("ACN+H", 1, "adduct", "[M+ACN+H]+", 42.033823f, 1, 1),
          ADDUCT("CH3OH+H", 1, "adduct", "[M+CH3OH+H]+", 33.033489f, 1, 1),
          ADDUCT("2H", 1, "adduct", "[2M+H]+", 1.007276f, 1, 2),
          ADDUCT("2Na", 1, "adduct", "[2M+Na]+", 22.989218f, 1, 2),
          ADDUCT("2K", 1, "adduct", "[2M+K]+", 38.963158f, 1, 2),
          ADDUCT("2NH4", 1, "adduct", "[2M+NH4]+", 18.033823f, 1, 2),
          ADDUCT("-H", -1, "adduct", "[M-H]-", -1.007276f, 1, 1),
          ADDUCT("Cl", -1, "adduct", "[M+Cl]-", 34.969402f, 1, 1),
          ADDUCT("Br", -1, "adduct", "[M+Br]-", 78.918885f, 1, 1),
          ADDUCT("CHO2", -1, "adduct", "[M+CHO2]-", 44.998201f, 1, 1),
          ADDUCT("CH3COO", -1, "adduct", "[M+CH3COO]-", 59.013851f, 1, 1),
          ADDUCT("FA-H", -1, "adduct", "[M+FA-H]-", 44.998201f, 1, 1),
          ADDUCT("2-H", -1, "adduct", "[2M-H]-", -1.007276f, 1, 2),
          ADDUCT("2Cl", -1, "adduct", "[2M+Cl]-", 34.969402f, 1, 2),
          ADDUCT("2FA-H", -1, "adduct", "[2M+FA-H]-", 44.998201f, 1, 2)};

      float neutralizer(const int &pol);
      std::vector<ADDUCT> adducts(const int &pol);
    };

    // MARK: FRAGMENT_LOSS
    struct FRAGMENT_LOSS
    {
      std::string name;
      std::string formula;
      float mass_loss;
      int polarity;

      FRAGMENT_LOSS(const std::string &n, const std::string &f, float ml, int p)
          : name(n), formula(f), mass_loss(ml), polarity(p) {}
    };

    // MARK: FRAGMENT_LOSS_SET
    struct FRAGMENT_LOSS_SET
    {
      std::vector<FRAGMENT_LOSS> all_losses{
          FRAGMENT_LOSS("water", "H2O", 18.010565, 0),        // neutral, both polarities
          FRAGMENT_LOSS("carbon dioxide", "CO2", 43.989829, 0), // neutral, both polarities
          FRAGMENT_LOSS("ammonia", "NH3", 17.026549, 1),      // positive mode
          FRAGMENT_LOSS("carbon monoxide", "CO", 27.994915, 0), // neutral, both polarities
          FRAGMENT_LOSS("methyl", "CH3", 15.023475, 0),       // neutral, both polarities
          FRAGMENT_LOSS("formic acid", "CH2O2", 46.005479, -1), // negative mode
          FRAGMENT_LOSS("hydrogen chloride", "HCl", 35.976678, 0),
          FRAGMENT_LOSS("hydrogen fluoride", "HF", 20.006229, 0),
          FRAGMENT_LOSS("sulfur dioxide", "SO2", 63.961901, 0),
          FRAGMENT_LOSS("sulfur trioxide", "SO3", 79.956815, 0),
          FRAGMENT_LOSS("sulfuric acid", "H2SO4", 97.967379, 0),
          FRAGMENT_LOSS("methanol", "CH3OH", 32.026215, 0),
          FRAGMENT_LOSS("ethylene", "C2H4", 28.031300, 0),
          FRAGMENT_LOSS("acetylene", "C2H2", 26.015650, 0),
          FRAGMENT_LOSS("nitric oxide", "NO", 29.997989, 0),
          FRAGMENT_LOSS("nitrogen dioxide", "NO2", 45.992904, 0),
          FRAGMENT_LOSS("nitrous acid", "HNO2", 46.005479, 0),
          FRAGMENT_LOSS("nitric acid", "HNO3", 62.000394, 0),
          FRAGMENT_LOSS("methylene", "CH2", 14.015650, 0),
          FRAGMENT_LOSS("ethanol", "C2H6O", 46.041865, 0),
          FRAGMENT_LOSS("phosphorous acid", "HPO3", 79.966331, 0),
          FRAGMENT_LOSS("phosphoric acid", "H3PO4", 97.976896, 0)
      };

      std::vector<FRAGMENT_LOSS> losses(const int &pol);
    };

    // MARK: CANDIDATE_CHAIN
    struct CANDIDATE_CHAIN
    {
      std::vector<nta::api::NTA_FEATURE_ROW> chain;
      std::vector<int> indices;
      std::unordered_map<int, float> isotope_theoretical_mass_distance;
      std::unordered_map<int, float> isotope_theoretical_abundance_min;
      std::unordered_map<int, float> isotope_theoretical_abundance_max;

      void clear();
      int size() const;
      void sort_by_mz();
      std::vector<float> get_chain_mzr(float ppm) const;
      float get_max_mzr(float ppm) const;

      void find_isotopic_candidates(const nta::api::NTA_FEATURE_ROW &ft,
                                     const nta::api::NTA_FEATURES &fts,
                                     const int &ft_index,
                                     const int &maxIsotopes,
                                     const std::vector<int> *component_indices = nullptr,
                                     const std::unordered_set<int> *assigned_features = nullptr);

      void annotate_isotopes(const ISOTOPE_COMBINATIONS &combinations,
                              const int &maxIsotopes,
                              const int &maxCharge,
                              const int &maxGaps,                             float ppm,                              bool debug = false);

      void find_adduct_candidates(const nta::api::NTA_FEATURE_ROW &ft,
                                   const nta::api::NTA_FEATURES &fts,
                                   const int &ft_index,
                                   const std::vector<int> *component_indices = nullptr);

      void annotate_adducts(float ppm, bool debug = false);

      void find_fragment_candidates(const nta::api::NTA_FEATURE_ROW &ft,
                                     const nta::api::NTA_FEATURES &fts,
                                     const int &ft_index,
                                     const std::vector<int> *component_indices = nullptr);

      void annotate_fragments(float ppm, bool debug = false);
    };

    struct ANNOTATION_CANDIDATE
    {
      std::string cat;
      std::string type;
      std::string parent_feature;
      std::string element_or_delta;
      double mass_error_da = 0.0;
      double mass_error_ppm = 0.0;
      double rt_error = 0.0;
      double rel_intensity = 0.0;
      double expected_rel_intensity_min = 0.0;
      double expected_rel_intensity_max = 0.0;
      double score = 0.0;
      int parent_index = -1;
      int feature_index = -1;
      int priority = 0;
      bool is_default = false;
      std::string label;
    };

    // Helper function
    bool is_max_gap_reached(const int &current_step, const int &maxGaps, const std::vector<int> &steps);

    void annotate_components_impl(
      nta::PROJECT_NON_TARGET_ANALYSIS &nta_data,
        int maxIsotopes,
        int maxCharge,
        int maxGaps,
        float ppm,
        const std::vector<std::string> &isotopeElements,
        const std::string &debugComponent = "",
        const std::string &debugAnalysis = "");

  } // namespace annotation
} // namespace nta

#endif
