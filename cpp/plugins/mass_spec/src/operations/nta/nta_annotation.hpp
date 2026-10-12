#ifndef NTA_ANNOTATION_H
#include "utils/nta.hpp"
#include "streamfind/sdk/debug_session.hpp"

#define NTA_ANNOTATION_H

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

#include <string>

namespace streamfind::mass_spec::nta {
  namespace utils { struct NTA_FEATURE_ROW; }
  namespace api { struct NTA_FEATURES; }
  class NtaProjectData;
}

namespace streamfind::mass_spec::nta
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
      std::vector<ISOTOPE> data;

      ISOTOPE_SET();

      void filter(const std::vector<std::string> &el)
      {
        std::unordered_set<std::string> el_set(el.begin(), el.end());
        for (const std::string &requested : el_set)
        {
          const auto known = std::find_if(data.begin(), data.end(), [&](const ISOTOPE &iso) {
            return iso.element == requested;
          });
          if (known == data.end())
            throw std::invalid_argument("unknown isotope element: " + requested);
        }
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
      std::vector<std::vector<int>> combinations_by_step;
      int length;

      ISOTOPE_COMBINATIONS(ISOTOPE_SET &isotopes, const int &max_number_elements);
    };

    // MARK: ISOTOPE_CHAIN
    struct ISOTOPE_CHAIN
    {
      std::vector<::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW> chain;
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

      ISOTOPE_CHAIN(const int &z, const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &mono_ion, float mono_mzr);
    };

    // MARK: ADDUCT
    struct ADDUCT
    {
      std::string element;
      std::string formula;
      int polarity;
      std::string cat;
      std::string type;
      int charge;
      int multiplicity;
      float mass_distance;

      ADDUCT(const std::string &e, const int &p, const std::string &c, const std::string &t, const float &md, const int &z, const int &m = 1)
          : element(e), formula(), polarity(p), cat(c), type(t), charge(z), multiplicity(m), mass_distance(md) {}
    };

    // MARK: ADDUCT_SET
    struct ADDUCT_SET
    {
      ADDUCT_SET();
      std::vector<ADDUCT> neutralizers{
          ADDUCT("H", 1, "[M+H]+", "[M+H]+", 0.0f, 1),
          ADDUCT("H", -1, "[M-H]-", "[M-H]-", 0.0f, 1)};
      std::vector<ADDUCT> all_adducts;

      float neutralizer(const int &pol);
      std::vector<ADDUCT> adducts(const int &pol);
    };

    // MARK: FRAGMENT_LOSS
    struct FRAGMENT_LOSS
    {
      std::string name;
      std::string formula;
      std::string expression;
      float mass_loss;
      int polarity;

      FRAGMENT_LOSS(const std::string &n, const std::string &f, float ml, int p, const std::string &e = {})
          : name(n), formula(f), expression(e), mass_loss(ml), polarity(p) {}
    };

    // MARK: FRAGMENT_LOSS_SET
    struct FRAGMENT_LOSS_SET
    {
      FRAGMENT_LOSS_SET();
      std::vector<FRAGMENT_LOSS> all_losses;

      std::vector<FRAGMENT_LOSS> losses(const int &pol);
    };

    // MARK: CANDIDATE_CHAIN
    struct CANDIDATE_CHAIN
    {
      std::vector<::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW> chain;
      std::vector<int> indices;
      std::unordered_map<int, float> isotope_theoretical_mass_distance;
      std::unordered_map<int, float> isotope_theoretical_abundance_min;
      std::unordered_map<int, float> isotope_theoretical_abundance_max;

      void clear();
      int size() const;
      void sort_by_mz();
      std::vector<float> get_chain_mzr(float ppm) const;
      float get_max_mzr(float ppm) const;

      void find_isotopic_candidates(const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &ft,
                                     const ::streamfind::mass_spec::nta::api::NTA_FEATURES &fts,
                                     const int &ft_index,
                                     const int &maxIsotopes,
                                     const std::vector<int> *component_indices = nullptr,
                                     const std::unordered_set<int> *assigned_features = nullptr);

      void annotate_isotopes(const ISOTOPE_COMBINATIONS &combinations,
                              const int &maxIsotopes,
                              const int &maxCharge,
                              const int &maxGaps,                             float ppm,                              bool debug = false);

      void find_adduct_candidates(const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &ft,
                                   const ::streamfind::mass_spec::nta::api::NTA_FEATURES &fts,
                                   const int &ft_index,
                                   const std::vector<int> *component_indices = nullptr);

      void annotate_adducts(float ppm, bool debug = false);

      void find_fragment_candidates(const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &ft,
                                     const ::streamfind::mass_spec::nta::api::NTA_FEATURES &fts,
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
      bool is_dimer = false;
      std::string relation_id;
      std::string label;
    };

    // Helper function
    bool is_max_gap_reached(const int &current_step, const int &maxGaps, const std::vector<int> &steps);

    void annotate_components_impl(
      ::streamfind::mass_spec::nta::NtaProjectData &nta_data,
        int maxIsotopes,
        int maxCharge,
        int maxGaps,
        float ppm,
        const std::vector<std::string> &isotopeElements,
        const std::vector<std::string> &modifications,
        bool useDefaultModifications,
        const std::string &debugComponent = "",
        const std::string &debugAnalysis = "",
        sdk::DebugSession *debug = nullptr);

  } // namespace annotation
} // namespace streamfind::mass_spec::nta

#endif

namespace streamfind::mass_spec::nta::annotate_components { STREAMFIND_DOMAIN_API nlohmann::json run(sdk::PluginProjectAccess &, const nlohmann::json &); }
