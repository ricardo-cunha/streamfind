#include "streamfind/mass_spec/nta_annotation.hpp"
#include "streamfind/mass_spec/nta.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <limits>
#include <cctype>

namespace nta
{
  namespace annotation
  {
    namespace streamfind::nta_annotation_detail
    {
      std::string fmt_num(double value, int precision)
      {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(precision) << value;
        std::string out = oss.str();
        while (out.size() > 1 && out.find('.') != std::string::npos && out.back() == '0')
          out.pop_back();
        if (!out.empty() && out.back() == '.')
          out.pop_back();
        return out;
      }

      bool starts_with(const std::string &value, const std::string &prefix)
      {
        return value.rfind(prefix, 0) == 0;
      }

      int isotope_complexity(const std::string &element_label);
      double isotope_priority_score(const std::string &element_label);

      bool is_structured_cat(const std::string &value, const std::string &cat)
      {
        return starts_with(value, "cat=" + cat);
      }

      std::string make_annotation_label(const ANNOTATION_CANDIDATE &candidate)
      {
        std::ostringstream oss;
        oss << "cat=" << candidate.cat
            << " | type=" << candidate.type
            << " | parent=" << candidate.parent_feature
            << " | element=" << candidate.element_or_delta
            << " | ppm=" << fmt_num(candidate.mass_error_ppm, 3)
            << " | rt=" << fmt_num(candidate.rt_error, 3)
            << " | rel=" << fmt_num(candidate.rel_intensity, 3);
        return oss.str();
      }

      std::string make_annotation_summary(const ANNOTATION_CANDIDATE &candidate)
      {
        if (candidate.cat.empty() || candidate.type.empty())
          return "";
        return candidate.cat + " " + candidate.type;
      }

      double score_mass(double mass_error_ppm, double ppm)
      {
        const double denom = std::max(1.0, ppm * 2.0);
        return std::max(0.0, 1.0 - (mass_error_ppm / denom));
      }

      double score_rt(double rt_error)
      {
        return 1.0 / (1.0 + rt_error);
      }

      double isotope_effective_rt_error(double rt_error)
      {
        const double grace_window = 3.0;
        if (rt_error <= grace_window)
          return 0.0;
        return rt_error - grace_window;
      }

      double score_rel(double rel, double expected_min, double expected_max)
      {
        if (expected_min <= 0.0 && expected_max <= 0.0)
        {
          if (rel <= 0.0)
            return 0.0;
          return 1.0 / (1.0 + std::abs(rel - 1.0));
        }
        if (rel >= expected_min && rel <= expected_max)
          return 1.0;
        const double dist = (rel < expected_min) ? (expected_min - rel) : (rel - expected_max);
        return 1.0 / (1.0 + dist * 5.0);
      }

      int candidate_priority(const std::string &cat, const std::string &type)
      {
        if (cat == "isotope")
          return 4;
        if (cat == "adduct")
          return (type.find("[2M+") != std::string::npos || type.find("[2M-") != std::string::npos) ? 2 : 3;
        if (cat == "loss")
          return 1;
        return 0;
      }

      double candidate_score(const ANNOTATION_CANDIDATE &candidate, double ppm)
      {
        if (candidate.cat == "isotope")
        {
          const int complexity = isotope_complexity(candidate.element_or_delta);
          if (candidate.mass_error_ppm > ppm)
            return -1.0;
          double score = 0.68 * score_mass(candidate.mass_error_ppm, ppm) +
                         0.05 * score_rt(isotope_effective_rt_error(candidate.rt_error)) +
                         0.17 * score_rel(candidate.rel_intensity, candidate.expected_rel_intensity_min, candidate.expected_rel_intensity_max) +
                         0.10 * (candidate.priority / 4.0);
          score += 0.08 * isotope_priority_score(candidate.element_or_delta);
          if (complexity > 1)
          {
            score -= 0.08 * static_cast<double>(complexity - 1);
            if (complexity >= 3)
              score -= 0.06;
          }
          return score;
        }
        double score = 0.55 * score_mass(candidate.mass_error_ppm, ppm) +
                       0.20 * score_rt(candidate.rt_error) +
                       0.15 * score_rel(candidate.rel_intensity, candidate.expected_rel_intensity_min, candidate.expected_rel_intensity_max) +
                       0.10 * (candidate.priority / 4.0);
        if (candidate.type.find("[2M+") != std::string::npos || candidate.type.find("[2M-") != std::string::npos)
          score -= 0.03;
        return score;
      }

      bool candidate_better(const ANNOTATION_CANDIDATE &lhs, const ANNOTATION_CANDIDATE &rhs)
      {
        if (lhs.score != rhs.score)
          return lhs.score > rhs.score;
        if (lhs.mass_error_ppm != rhs.mass_error_ppm)
          return lhs.mass_error_ppm < rhs.mass_error_ppm;
        const double lhs_rt_error = (lhs.cat == "isotope") ? isotope_effective_rt_error(lhs.rt_error) : lhs.rt_error;
        const double rhs_rt_error = (rhs.cat == "isotope") ? isotope_effective_rt_error(rhs.rt_error) : rhs.rt_error;
        if (lhs_rt_error != rhs_rt_error)
          return lhs_rt_error < rhs_rt_error;
        if (lhs.priority != rhs.priority)
          return lhs.priority > rhs.priority;
        if (lhs.cat == "isotope" && rhs.cat == "isotope")
        {
          const double lhs_priority = isotope_priority_score(lhs.element_or_delta);
          const double rhs_priority = isotope_priority_score(rhs.element_or_delta);
          if (lhs_priority != rhs_priority)
            return lhs_priority > rhs_priority;
          const int lhs_complexity = isotope_complexity(lhs.element_or_delta);
          const int rhs_complexity = isotope_complexity(rhs.element_or_delta);
          if (lhs_complexity != rhs_complexity)
            return lhs_complexity < rhs_complexity;
        }
        return lhs.parent_index < rhs.parent_index;
      }

      std::string resolve_root_parent_feature(const ANNOTATION_CANDIDATE &candidate,
                                              const std::unordered_map<int, ANNOTATION_CANDIDATE> &best_candidate)
      {
        if (candidate.cat != "isotope")
          return candidate.parent_feature;

        std::string resolved_parent = candidate.parent_feature;
        int current_parent_index = candidate.parent_index;
        std::unordered_set<int> visited;

        while (current_parent_index >= 0 && visited.insert(current_parent_index).second)
        {
          const auto it = best_candidate.find(current_parent_index);
          if (it == best_candidate.end())
            break;

          resolved_parent = it->second.parent_feature;
          if (it->second.is_default || it->second.cat != "isotope")
            break;

          current_parent_index = it->second.parent_index;
        }

        return resolved_parent;
      }

      bool candidate_equals(const ANNOTATION_CANDIDATE &lhs, const ANNOTATION_CANDIDATE &rhs)
      {
        return lhs.cat == rhs.cat &&
               lhs.type == rhs.type &&
               lhs.parent_feature == rhs.parent_feature &&
               lhs.element_or_delta == rhs.element_or_delta &&
               lhs.parent_index == rhs.parent_index &&
               lhs.feature_index == rhs.feature_index &&
               lhs.is_default == rhs.is_default;
      }

      bool relation_candidate_creates_cycle(const ANNOTATION_CANDIDATE &candidate,
                                            const std::unordered_map<int, ANNOTATION_CANDIDATE> &state)
      {
        if (candidate.is_default || candidate.parent_index < 0)
          return false;

        const int origin = candidate.feature_index;
        int current = candidate.parent_index;
        std::unordered_set<int> visited;

        while (current >= 0 && visited.insert(current).second)
        {
          if (current == origin)
            return true;

          const auto it = state.find(current);
          if (it == state.end())
            return false;

          if (it->second.is_default || it->second.parent_index < 0 || it->second.parent_index == current)
            return false;

          current = it->second.parent_index;
        }

        return false;
      }

      bool relation_chain_reaches_root(int feature_idx,
                                       const std::unordered_map<int, ANNOTATION_CANDIDATE> &state,
                                       std::unordered_set<int> &visited)
      {
        if (!visited.insert(feature_idx).second)
          return false;

        const auto it = state.find(feature_idx);
        if (it == state.end())
          return false;

        const auto &candidate = it->second;
        if (candidate.is_default)
          return true;

        if (candidate.parent_index < 0 || candidate.parent_index == feature_idx)
          return false;

        const auto parent_it = state.find(candidate.parent_index);
        if (parent_it == state.end())
          return false;

        const auto &parent = parent_it->second;
        if (candidate.cat == "adduct")
          return parent.is_default;
        if (candidate.cat == "loss")
        {
          if (parent.is_default)
            return true;
          if (parent.cat != "loss")
            return false;
          return relation_chain_reaches_root(candidate.parent_index, state, visited);
        }
        return false;
      }

      bool relation_candidate_is_valid(const ANNOTATION_CANDIDATE &candidate,
                                       const std::unordered_map<int, ANNOTATION_CANDIDATE> &state)
      {
        if (candidate.is_default)
          return true;
        if (candidate.parent_index < 0 || candidate.parent_index == candidate.feature_index)
          return false;
        if (relation_candidate_creates_cycle(candidate, state))
          return false;

        const auto parent_it = state.find(candidate.parent_index);
        if (parent_it == state.end())
          return false;

        const auto &parent = parent_it->second;
        if (candidate.cat == "adduct")
          return parent.is_default;
        if (candidate.cat == "loss")
        {
          if (parent.is_default)
            return true;
          if (parent.cat != "loss")
            return false;
          std::unordered_set<int> visited;
          return relation_chain_reaches_root(candidate.parent_index, state, visited);
        }
        return false;
      }

      double neutral_mass_from_base_ion(const nta::api::NTA_FEATURE_ROW &ft)
      {
        constexpr double proton = 1.007276;
        if (ft.polarity == 1)
          return ft.mz - proton;
        return ft.mz + proton;
      }

      double theoretical_mz_from_adduct(double neutral_mass, const ADDUCT &adduct)
      {
        return (neutral_mass * adduct.multiplicity) + adduct.mass_distance;
      }

      double ppm_error(double observed, double theoretical)
      {
        if (theoretical == 0.0)
          return std::numeric_limits<double>::infinity();
        return std::abs(observed - theoretical) / std::abs(theoretical) * 1e6;
      }

      std::vector<std::string> split_string(const std::string &value, char delim)
      {
        std::vector<std::string> out;
        std::stringstream ss(value);
        std::string item;
        while (std::getline(ss, item, delim))
        {
          if (!item.empty())
            out.push_back(item);
        }
        return out;
      }

      std::string trim_copy(const std::string &value)
      {
        size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
          ++start;

        size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
          --end;

        return value.substr(start, end - start);
      }

      struct ISOTOPE_ELEMENT_SPEC
      {
        std::vector<std::string> elements;
        std::unordered_map<std::string, std::pair<int, int>> ranges;
      };

      ISOTOPE_ELEMENT_SPEC parse_isotope_element_specs(const std::vector<std::string> &specs)
      {
        ISOTOPE_ELEMENT_SPEC parsed;
        std::unordered_set<std::string> seen;

        for (const auto &raw_spec : specs)
        {
          const std::string spec = trim_copy(raw_spec);
          if (spec.empty())
            continue;

          const size_t colon_pos = spec.find(':');
          const std::string element = (colon_pos == std::string::npos) ? spec : spec.substr(0, colon_pos);
          if (seen.insert(element).second)
            parsed.elements.push_back(element);

          if (colon_pos == std::string::npos)
            continue;

          const std::string range = spec.substr(colon_pos + 1);
          const size_t dash_pos = range.find('-');
          if (dash_pos == std::string::npos)
            continue;

          const int min_n = std::stoi(range.substr(0, dash_pos));
          const int max_n = std::stoi(range.substr(dash_pos + 1));
          parsed.ranges[element] = {min_n, max_n};
        }

        return parsed;
      }

      int isotope_complexity(const std::string &element_label)
      {
        if (element_label.empty())
          return 0;
        return static_cast<int>(split_string(element_label, '/').size());
      }

      double isotope_priority_score(const std::string &element_label)
      {
        static const std::unordered_map<std::string, double> priorities{
            {"13C", 1.00},
            {"37Cl", 0.98},
            {"81Br", 0.98},
            {"34S", 0.92},
            {"33S", 0.82},
            {"15N", 0.78},
            {"18O", 0.62},
            {"17O", 0.40},
            {"2H", 0.35},
            {"29Si", 0.70},
            {"30Si", 0.62},
            {"25Mg", 0.45},
            {"26Mg", 0.48},
            {"41K", 0.40},
            {"44Ca", 0.32},
            {"54Fe", 0.30},
            {"57Fe", 0.34},
            {"65Cu", 0.28},
            {"66Zn", 0.30},
            {"68Zn", 0.26},
            {"77Se", 0.36},
            {"78Se", 0.42},
            {"80Se", 0.44},
            {"10B", 0.24},
            {"36S", 0.18}};

        const std::vector<std::string> tokens = split_string(element_label, '/');
        if (tokens.empty())
          return 0.0;

        double score = 0.0;
        for (const auto &token : tokens)
        {
          const auto it = priorities.find(token);
          score += (it != priorities.end()) ? it->second : 0.2;
        }
        return score / static_cast<double>(tokens.size());
      }

      double isotope_mass_delta(const std::string &element_label)
      {
        static const std::unordered_map<std::string, double> deltas{
            {"13C", 1.0033548378},
            {"2H", 1.0062767},
            {"10B", 0.996809},
            {"15N", 0.9970349},
            {"17O", 1.004217},
            {"18O", 2.004246},
            {"25Mg", 0.999711},
            {"26Mg", 1.995796},
            {"29Si", 0.999568},
            {"30Si", 1.996844},
            {"33S", 0.999388},
            {"34S", 1.995796},
            {"36S", 3.995010},
            {"37Cl", 1.997050},
            {"81Br", 1.997953},
            {"41K", 1.998119},
            {"44Ca", 3.998159},
            {"54Fe", -1.004391},
            {"57Fe", 2.995294},
            {"65Cu", 1.998204},
            {"66Zn", 1.999059},
            {"68Zn", 3.995796},
            {"77Se", 0.997953},
            {"78Se", 1.996004},
            {"80Se", 3.995010}};
        double total = 0.0;
        for (const auto &token : split_string(element_label, '/'))
        {
          const auto it = deltas.find(token);
          if (it != deltas.end())
            total += it->second;
        }
        return total;
      }

      std::string extract_isotope_element(const std::string &legacy_label)
      {
        const auto tokens = split_string(legacy_label, ' ');
        if (tokens.size() >= 4)
          return tokens[2];
        return "";
      }

      std::string extract_isotope_type(const std::string &legacy_label)
      {
        const auto tokens = split_string(legacy_label, ' ');
        if (!tokens.empty())
          return tokens.back();
        return "";
      }
    }

    // MARK: ISOTOPE_COMBINATIONS Implementation
    ISOTOPE_COMBINATIONS::ISOTOPE_COMBINATIONS(ISOTOPE_SET &isotopes, const int &max_number_elements)
    {
      std::set<std::vector<std::string>> combinations_set;

      for (const ISOTOPE &iso : isotopes.data)
      {
        isotopes_str.push_back(iso.isotope);
        abundances.push_back(iso.abundance);
        abundances_monoisotopic.push_back(iso.abundance_monoisotopic);
        min.push_back(iso.min);
        max.push_back(iso.max);
      }

      for (const std::string &iso : isotopes_str)
      {
        std::vector<std::string> iso_vec(1, iso);
        combinations_set.insert(iso_vec);
      }

      for (int n = 1; n <= max_number_elements; n++)
      {
        std::set<std::vector<std::string>> new_combinations_set;

        for (std::vector<std::string> combination : std::vector<std::vector<std::string>>(combinations_set.begin(), combinations_set.end()))
        {
          if (combination[0] == "2H" || combination[0] == "17O")
            continue;

          if (n > 1 && (combination[0] == "15N" || combination[0] == "33S"))
            continue;

          if (combination.size() >= 2)
            if (combination[1] == "15N" || combination[1] == "33S")
              continue;

          for (const std::string &iso : isotopes_str)
          {
            if (iso == "2H" || iso == "17O")
              continue;

            if (n > 1 && (iso == "15N" || iso == "33S"))
              continue;

            combination.push_back(iso);
            std::stable_sort(combination.begin(), combination.end());
            new_combinations_set.insert(combination);
          }
        }
        combinations_set.insert(new_combinations_set.begin(), new_combinations_set.end());
      }

      std::vector<std::vector<std::string>> tensor_combinations_unordered(combinations_set.begin(), combinations_set.end());
      length = tensor_combinations_unordered.size();

      std::vector<float> isotopes_mass_distances;
      for (const ISOTOPE &iso : isotopes.data)
      {
        isotopes_mass_distances.push_back(iso.mass_distance);
      }

      std::vector<std::vector<float>> tensor_mass_distances_unordered(length);
      std::vector<std::vector<float>> tensor_abundances_unordered(length);
      std::vector<float> mass_distances_unordered(length);

      for (int i = 0; i < length; ++i)
      {
        const std::vector<std::string> &combination = tensor_combinations_unordered[i];
        const int combination_length = combination.size();
        std::vector<float> md(combination_length);
        std::vector<float> ab(combination_length);
        for (int j = 0; j < combination_length; ++j)
        {
          std::string iso = combination[j];
          int idx = std::distance(isotopes_str.begin(), std::find(isotopes_str.begin(), isotopes_str.end(), iso));
          md[j] = isotopes_mass_distances[idx];
          ab[j] = abundances[idx];
          mass_distances_unordered[i] = mass_distances_unordered[i] + isotopes_mass_distances[idx];
        }
        tensor_mass_distances_unordered[i] = md;
        tensor_abundances_unordered[i] = ab;
      }

      std::vector<int> order_idx(length);
      std::iota(order_idx.begin(), order_idx.end(), 0);
      std::stable_sort(order_idx.begin(), order_idx.end(), [&](int i, int j) {
        return mass_distances_unordered[i] < mass_distances_unordered[j];
      });

      tensor_combinations.resize(length);
      tensor_mass_distances.resize(length);
      tensor_abundances.resize(length);
      mass_distances.resize(length);
      step.resize(length);

      for (int i = 0; i < length; i++)
      {
        tensor_combinations[i] = tensor_combinations_unordered[order_idx[i]];
        tensor_mass_distances[i] = tensor_mass_distances_unordered[order_idx[i]];
        tensor_abundances[i] = tensor_abundances_unordered[order_idx[i]];
        mass_distances[i] = mass_distances_unordered[order_idx[i]];
        step[i] = std::round(mass_distances[i]);
      }
    }

    // MARK: ISOTOPE_CHAIN Implementation
    ISOTOPE_CHAIN::ISOTOPE_CHAIN(const int &z, const nta::api::NTA_FEATURE_ROW &mono_ion, float mono_mzr)
    {
      chain.resize(1);
      candidate_indices.resize(1);
      charge.resize(1);
      step.resize(1);
      mz.resize(1);
      rt.resize(1);
      mzr.resize(1);
      isotope.resize(1);
      mass_distance.resize(1);
      theoretical_mass_distance.resize(1);
      mass_distance_error.resize(1);
      time_error.resize(1);
      abundance.resize(1);
      theoretical_abundance_min.resize(1);
      theoretical_abundance_max.resize(1);

      chain[0] = mono_ion;
      candidate_indices[0] = 0;
      charge[0] = z;
      step[0] = 0;
      mz[0] = mono_ion.mz;
      rt[0] = mono_ion.rt;
      mzr[0] = mono_mzr;
      isotope[0] = "";
      mass_distance[0] = 0;
      theoretical_mass_distance[0] = 0;
      mass_distance_error[0] = 0;
      time_error[0] = 0;
      abundance[0] = 1;
      theoretical_abundance_min[0] = 0;
      theoretical_abundance_max[0] = 0;
      number_carbons = 0;
      length = 1;
    }

    // MARK: ADDUCT_SET Implementation
    float ADDUCT_SET::neutralizer(const int &pol)
    {
      if (pol == 1)
      {
        return neutralizers[0].mass_distance;
      }
      return neutralizers[1].mass_distance;
    }

    std::vector<ADDUCT> ADDUCT_SET::adducts(const int &pol)
    {
      std::vector<ADDUCT> out;

      if (pol == 1)
      {
        for (const ADDUCT &a : all_adducts)
        {
          if (a.polarity == 1)
          {
            out.push_back(a);
          }
        }
      }

      if (pol == -1)
      {
        for (const ADDUCT &a : all_adducts)
        {
          if (a.polarity == -1)
          {
            out.push_back(a);
          }
        }
      }

      return out;
    }

    // MARK: FRAGMENT_LOSS_SET Implementation
    std::vector<FRAGMENT_LOSS> FRAGMENT_LOSS_SET::losses(const int &pol)
    {
      std::vector<FRAGMENT_LOSS> out;

      for (const FRAGMENT_LOSS &loss : all_losses)
      {
        // Include if polarity matches or if loss is neutral (polarity = 0)
        if (loss.polarity == 0 || loss.polarity == pol)
        {
          out.push_back(loss);
        }
      }

      return out;
    }

    // MARK: CANDIDATE_CHAIN Implementation
    void CANDIDATE_CHAIN::clear()
    {
      chain.clear();
      indices.clear();
      isotope_theoretical_mass_distance.clear();
      isotope_theoretical_abundance_min.clear();
      isotope_theoretical_abundance_max.clear();
    }

    int CANDIDATE_CHAIN::size() const
    {
      return chain.size();
    }

    void CANDIDATE_CHAIN::sort_by_mz()
    {
      if (chain.size() == 0)
        return;

      std::vector<int> new_order(chain.size());
      std::iota(new_order.begin(), new_order.end(), 0);

      std::sort(new_order.begin(), new_order.end(), [this](int i1, int i2) {
        return chain[i1].mz < chain[i2].mz;
      });

      std::vector<nta::api::NTA_FEATURE_ROW> chain_sorted;
      std::vector<int> indices_sorted;

      for (size_t i = 0; i < chain.size(); i++)
      {
        chain_sorted.push_back(chain[new_order[i]]);
        indices_sorted.push_back(new_order[i]);
      }

      chain = chain_sorted;
      indices = indices_sorted;
    }

    std::vector<float> CANDIDATE_CHAIN::get_chain_mzr(float ppm) const
    {
      if (chain.size() == 0)
        return std::vector<float>();

      std::vector<float> mzr(chain.size());
      const float min_ppm = 10.0f;

      for (size_t i = 0; i < chain.size(); i++)
      {
        float fwhm_mz = chain[i].fwhm_mz;
        float mz = chain[i].mz;
        float fwhm_ppm = (fwhm_mz / mz) * 1e6f;

        // Use fwhm_mz / 2 if it's larger than min_ppm, otherwise use min_ppm
        if (fwhm_ppm >= min_ppm) {
          mzr[i] = fwhm_mz / 2.0f;
        } else {
          mzr[i] = (min_ppm * mz) / 1e6f;
        }
      }
      return mzr;
    }

    float CANDIDATE_CHAIN::get_max_mzr(float ppm) const
    {
      if (chain.size() == 0)
        return 0.0;

      std::vector<float> mzr = this->get_chain_mzr(ppm);
      float max_mzr = *std::max_element(mzr.begin(), mzr.end());
      return max_mzr;
    }

    void CANDIDATE_CHAIN::find_isotopic_candidates(const nta::api::NTA_FEATURE_ROW &ft,
                                                    const nta::api::NTA_FEATURES &fts,
                                                    const int &ft_index,
                                                    const int &maxIsotopes,
                                                    const std::vector<int> *component_indices,
                                                    const std::unordered_set<int> *assigned_features)
    {
      const std::string &feature = ft.feature;
      const int &polarity = ft.polarity;
      const float &mz = ft.mz;
      const float max_mz_chain = (mz + maxIsotopes) * 1.05;

      chain.push_back(ft);
      indices.push_back(ft_index);

      // If component_indices provided, search only within component; otherwise search all features
      const std::vector<int> *search_indices = component_indices;
      std::vector<int> all_indices;
      if (!component_indices)
      {
        all_indices.resize(fts.size());
        std::iota(all_indices.begin(), all_indices.end(), 0);
        search_indices = &all_indices;
      }

      for (int z : *search_indices)
      {
        // Skip if feature is already assigned to another isotope chain
        if (assigned_features && assigned_features->count(z) > 0)
          continue;

        const bool within_max_mz_chain = fts.mz[z] > mz && fts.mz[z] <= max_mz_chain;
        const bool same_polarity = fts.polarity[z] == polarity;
        const bool not_main_ft = fts.feature[z] != feature;

        if (within_max_mz_chain && same_polarity && not_main_ft)
        {
          chain.push_back(fts.get_feature(z));
          indices.push_back(z);
        }
      }
    }

    // Helper function implementation
    bool is_max_gap_reached(const int &current_step, const int &maxGaps, const std::vector<int> &steps)
    {
      if (steps.size() == 0)
        return false;

      int max_step = *std::max_element(steps.begin(), steps.end());

      if (max_step == 0)
        return (current_step - 1) > maxGaps;

      int gaps = current_step - max_step - 1;

      return gaps > maxGaps;
    }

    void CANDIDATE_CHAIN::annotate_isotopes(const ISOTOPE_COMBINATIONS &combinations,
                                             const int &maxIsotopes,
                                             const int &maxCharge,
                                             const int &maxGaps,
                                             float ppm,
                                             bool debug)
    {
      bool is_Mplus = false;
      float mzr = this->get_max_mzr(ppm);
      const int number_candidates = chain.size();
      const nta::api::NTA_FEATURE_ROW &mono_ion = chain[0];

      if (debug)
      {
        DEBUG_LOG("\n=== Starting Isotope Annotation ===" << std::endl);
        DEBUG_LOG("  Monoisotopic ion: " << mono_ion.feature << " (mz=" << mono_ion.mz << ", intensity=" << mono_ion.intensity << ")" << std::endl);
        DEBUG_LOG("  Number of candidates: " << number_candidates << std::endl);
        DEBUG_LOG("  Max mzr: " << mzr << std::endl);
        DEBUG_LOG("  Parameters: maxIsotopes=" << maxIsotopes << ", maxCharge=" << maxCharge << ", maxGaps=" << maxGaps << std::endl);
      }

      std::vector<ISOTOPE_CHAIN> isotopic_chains;
      isotopic_chains.push_back(ISOTOPE_CHAIN(1, mono_ion, mzr));

      if (maxCharge > 1)
      {
        for (int z = 2; z <= maxCharge; z++)
        {
          isotopic_chains.push_back(ISOTOPE_CHAIN(z, mono_ion, mzr));
        }
      }

      const int number_charges = isotopic_chains.size();

      if (debug)
      {
        DEBUG_LOG("\n--- Testing " << number_charges << " charge state(s) ---" << std::endl);
      }

      for (int z = 0; z < number_charges; z++)
      {
        ISOTOPE_CHAIN iso_chain = isotopic_chains[z];
        const int charge = iso_chain.charge[0];
        const int number_steps = maxIsotopes + 1;

        if (debug)
        {
          DEBUG_LOG("\nCharge state z=" << charge << ":" << std::endl);
        }

        for (int s = 1; s < number_steps; ++s)
        {
          if (is_max_gap_reached(s, maxGaps, iso_chain.step))
          {
            if (debug) DEBUG_LOG("  Step " << s << ": Max gap reached, stopping" << std::endl);
            break;
          }

          std::vector<int> which_combinations;

          for (int c = 0; c < combinations.length; ++c)
          {
            if (combinations.step[c] == s)
              which_combinations.push_back(c);
          }

          const int number_combinations = which_combinations.size();

          if (debug)
          {
            DEBUG_LOG("  Step " << s << ": Testing " << number_combinations << " isotope combinations" << std::endl);
          }

          std::vector<float> mass_distances(number_combinations);

          for (int c = 0; c < number_combinations; ++c)
          {
            mass_distances[c] = combinations.mass_distances[which_combinations[c]] / charge;
          }

          const float mass_distance_max = *std::max_element(mass_distances.begin(), mass_distances.end());
          const float mass_distance_min = *std::min_element(mass_distances.begin(), mass_distances.end());

          if (debug)
          {
            DEBUG_LOG("    Mass distance range: " << mass_distance_min << " - " << mass_distance_max << std::endl);
          }

          for (int candidate_idx = 1; candidate_idx < number_candidates; ++candidate_idx)
          {
              const nta::api::NTA_FEATURE_ROW &candidate = chain[candidate_idx];
            const float mz = candidate.mz;
            const float rt = candidate.rt;
            const float intensity = candidate.intensity;

            float candidate_mass_distance = mz - mono_ion.mz;
            float candidate_time_error = std::abs(rt - mono_ion.rt);
            float candidate_mass_distance_min = candidate_mass_distance - mzr;
            float candidate_mass_distance_max = candidate_mass_distance + mzr;

            if (debug)
            {
              DEBUG_LOG("    Candidate " << candidate_idx << " (" << candidate.feature << ", mz=" << mz << "):" << std::endl);
              DEBUG_LOG("      Mass distance: " << candidate_mass_distance << " (range: " << candidate_mass_distance_min << " - " << candidate_mass_distance_max << ")" << std::endl);
              DEBUG_LOG("      Time error: " << candidate_time_error << std::endl);
              DEBUG_LOG("      Rel intensity: " << (intensity / mono_ion.intensity) << std::endl);
            }

            // M-ION Check
            if (s == 1)
            {
              if (candidate_mass_distance_min < 1.007276 &&
                  candidate_mass_distance_max > 1.007276 &&
                  (intensity / mono_ion.intensity) > 5)
              {
                if (debug) DEBUG_LOG("      -> Detected as M+ ion (intensity ratio > 5), stopping isotope search" << std::endl);
                is_Mplus = true;
                break;
              }
            }

            double combination_mass_error = 10;

            if (mass_distance_min - mzr < candidate_mass_distance && mass_distance_max + mzr > candidate_mass_distance)
            {
              if (debug) DEBUG_LOG("      -> Within mass distance window, checking combinations..." << std::endl);
              for (int c = 0; c < number_combinations; c++)
              {
                const float candidate_mass_distance_error = std::abs(mass_distances[c] - candidate_mass_distance);
                const std::vector<std::string> &combination = combinations.tensor_combinations[which_combinations[c]];

                // Build combination string early for debug logging
                std::string concat_combination = combination[0];
                for (size_t e = 1; e < combination.size(); ++e)
                {
                  concat_combination += "/" + combination[e];
                }

                float min_rel_int = 1;
                float max_rel_int = 1;

                std::unordered_map<std::string, int> isotope_map;

                for (size_t e = 0; e < combination.size(); ++e)
                {
                  isotope_map[combination[e]]++;
                }

                for (const auto &pair : isotope_map)
                {
                  std::string iso = pair.first;
                  int iso_n = pair.second;

                  const int iso_idx = std::distance(
                      combinations.isotopes_str.begin(),
                      std::find(combinations.isotopes_str.begin(), combinations.isotopes_str.end(), iso));

                  const float iso_ab = combinations.abundances[iso_idx];
                  const float mono_ab = combinations.abundances_monoisotopic[iso_idx];
                  float min_el_num = combinations.min[iso_idx];
                  float max_el_num = combinations.max[iso_idx];

                  // Special handling for carbon isotopes
                  if (iso_n == 1 && iso == "13C" && s == 1)
                  {
                    iso_chain.number_carbons = intensity / (iso_ab * mono_ion.intensity);
                    min_el_num = iso_chain.number_carbons * 0.8;
                    max_el_num = iso_chain.number_carbons * 1.2;
                  }

                  if (iso == "13C" && s > 1 && iso_chain.number_carbons > 0)
                  {
                    // Use estimated carbon count from M+1 if available
                    min_el_num = iso_chain.number_carbons * 0.8;
                    max_el_num = iso_chain.number_carbons * 1.2;
                  }
                  // else: keep default values (1-100) from ISOTOPE_SET

                  // For halogen isotopes in combination with other isotopes (M+3, M+4, etc.)
                  // Use reasonable element count ranges since we know the halogen is present
                  if ((iso == "37Cl" || iso == "81Br") && isotope_map.size() > 1)
                  {
                    // Assume 1-2 halogens are present (common in environmental contaminants)
                    min_el_num = 1;
                    max_el_num = 2;
                  }

                  if (iso_n == 1)
                  {
                    double min_coef = (min_el_num * std::pow(mono_ab, min_el_num - iso_n) * iso_ab) / std::pow(mono_ab, min_el_num);
                    double max_coef = (max_el_num * std::pow(mono_ab, max_el_num - iso_n) * iso_ab) / std::pow(mono_ab, max_el_num);

                    min_rel_int = min_rel_int * min_coef;
                    max_rel_int = max_rel_int * max_coef;
                  }
                  else
                  {
                    unsigned int fact = 1;
                    for (int a = 1; a <= iso_n; ++a)
                      fact *= a;

                    double min_coef = (std::pow(mono_ab, min_el_num - iso_n) * std::pow(iso_ab, iso_n)) / fact;
                    double max_coef = (std::pow(mono_ab, max_el_num - iso_n) * std::pow(iso_ab, iso_n)) / fact;

                    min_coef = min_coef / std::pow(mono_ab, min_el_num);
                    max_coef = max_coef / std::pow(mono_ab, max_el_num);

                    min_coef = min_coef * min_el_num * (min_el_num - 1);
                    max_coef = max_coef * max_el_num * (max_el_num - 1);

                    for (int t = 2; t <= iso_n - 1; ++t)
                    {
                      min_coef = min_coef * (min_el_num - t);
                      max_coef = max_coef * (max_el_num - t);
                    }

                    min_rel_int = min_rel_int * min_coef;
                    max_rel_int = max_rel_int * max_coef;
                  }
                }

                const float rel_int = intensity / mono_ion.intensity;

                if (candidate_mass_distance_error < combination_mass_error &&
                    candidate_mass_distance_error <= mzr * 1.3 &&
                    rel_int >= min_rel_int * 0.7 &&
                    rel_int <= max_rel_int * 1.3)
                {
                  if (debug)
                  {
                    DEBUG_LOG("      -> MATCH found! Combination: " << concat_combination << std::endl);
                    DEBUG_LOG("         Mass error: " << candidate_mass_distance_error << " (threshold: " << (mzr * 1.3) << ")" << std::endl);
                    DEBUG_LOG("         Rel intensity: " << rel_int << " (range: " << (min_rel_int * 0.7) << " - " << (max_rel_int * 1.3) << ")" << std::endl);
                  }
                  combination_mass_error = candidate_mass_distance_error;

                  bool is_in_chain = false;
                  size_t is_in_chain_idx = 0;
                  for (size_t t = 1; t < iso_chain.chain.size(); ++t)
                  {
                    if (iso_chain.chain[t].feature == candidate.feature)
                    {
                      is_in_chain = true;
                      is_in_chain_idx = t;
                      break;
                    }
                  }

                  if (is_in_chain)
                  {
                    iso_chain.chain[is_in_chain_idx] = candidate;
                    iso_chain.candidate_indices[is_in_chain_idx] = candidate_idx;
                    iso_chain.charge[is_in_chain_idx] = charge;
                    iso_chain.step[is_in_chain_idx] = s;
                    iso_chain.mz[is_in_chain_idx] = mz;
                    iso_chain.rt[is_in_chain_idx] = rt;
                    iso_chain.mzr[is_in_chain_idx] = mzr;
                    iso_chain.isotope[is_in_chain_idx] = concat_combination;
                    iso_chain.mass_distance[is_in_chain_idx] = candidate_mass_distance;
                    iso_chain.theoretical_mass_distance[is_in_chain_idx] = mass_distances[c];
                    iso_chain.mass_distance_error[is_in_chain_idx] = candidate_mass_distance_error;
                    iso_chain.time_error[is_in_chain_idx] = candidate_time_error;
                    iso_chain.abundance[is_in_chain_idx] = rel_int;
                    iso_chain.theoretical_abundance_min[is_in_chain_idx] = min_rel_int;
                    iso_chain.theoretical_abundance_max[is_in_chain_idx] = max_rel_int;
                  }
                  else
                  {
                    iso_chain.chain.push_back(candidate);
                    iso_chain.candidate_indices.push_back(candidate_idx);
                    iso_chain.charge.push_back(charge);
                    iso_chain.step.push_back(s);
                    iso_chain.mz.push_back(mz);
                    iso_chain.rt.push_back(rt);
                    iso_chain.mzr.push_back(mzr);
                    iso_chain.isotope.push_back(concat_combination);
                    iso_chain.mass_distance.push_back(candidate_mass_distance);
                    iso_chain.theoretical_mass_distance.push_back(mass_distances[c]);
                    iso_chain.mass_distance_error.push_back(candidate_mass_distance_error);
                    iso_chain.time_error.push_back(candidate_time_error);
                    iso_chain.abundance.push_back(rel_int);
                    iso_chain.theoretical_abundance_min.push_back(min_rel_int);
                    iso_chain.theoretical_abundance_max.push_back(max_rel_int);
                    iso_chain.length++;
                  }
                }
                else if (debug && candidate_mass_distance_error < mzr * 1.3)
                {
                  DEBUG_LOG("         Combination " << concat_combination << ": mass_error=" << candidate_mass_distance_error
                            << ", rel_int=" << rel_int << " (expected: " << min_rel_int << " - " << max_rel_int << ")" << std::endl);
                }
              }
            }
            else if (debug)
            {
              DEBUG_LOG("      -> Outside mass distance window (" << mass_distance_min << " - " << mass_distance_max << ")" << std::endl);
            }
          }

          if (is_Mplus)
            break;
        }

        if (is_Mplus)
          break;

        isotopic_chains[z] = iso_chain;
      }

      if (!is_Mplus)
      {
        int best_chain = 0;

        for (int z = 0; z < number_charges; z++)
        {
          if (isotopic_chains[z].length > isotopic_chains[best_chain].length)
          {
            best_chain = z;
          }
        }

        ISOTOPE_CHAIN &sel_iso_chain = isotopic_chains[best_chain];

        // Get monoisotopic m/z rounded to integer
        int mono_mz_rounded = std::round(mono_ion.mz);

        // Always assign [M+H]+ or [M-H]- to the monoisotopic ion (first in chain)
        const int charge = sel_iso_chain.charge[0];
        if (chain[0].polarity == 1)
        {
          chain[0].adduct = (charge > 1) ? "[M+H]" + std::to_string(charge) + "+" : "[M+H]+";
        }
        else
        {
          chain[0].adduct = (charge > 1) ? "[M-H]" + std::to_string(charge) + "-" : "[M-H]-";
        }

        // Annotate isotopes if chain has more than just the monoisotopic ion
        if (sel_iso_chain.length > 1)
        {
          for (size_t i = 1; i < sel_iso_chain.chain.size(); i++)
          {
            const int candidate_idx = sel_iso_chain.candidate_indices[i];
            nta::api::NTA_FEATURE_ROW &temp_candidate = chain[candidate_idx];
            isotope_theoretical_mass_distance[candidate_idx] = sel_iso_chain.theoretical_mass_distance[i];
            isotope_theoretical_abundance_min[candidate_idx] = sel_iso_chain.theoretical_abundance_min[i];
            isotope_theoretical_abundance_max[candidate_idx] = sel_iso_chain.theoretical_abundance_max[i];

            // Format: isotope MZXXX EL [M+n] where XXX=monoisotopic mass, EL=element, n=step
            std::ostringstream oss;
            oss << "isotope MZ" << mono_mz_rounded << " " << sel_iso_chain.isotope[i] << " [M+" << sel_iso_chain.step[i] << "]";
            temp_candidate.adduct = oss.str();
          }
        }
      }
    }

    void CANDIDATE_CHAIN::find_adduct_candidates(const nta::api::NTA_FEATURE_ROW &ft,
                            const nta::api::NTA_FEATURES &fts,
                            const int &ft_index,
                            const std::vector<int> *component_indices)
    {
      const std::string &feature = ft.feature;
      const int &polarity = ft.polarity;
      const float &mz = ft.mz;
      const float max_mz_chain = mz + 100;

      chain.push_back(ft);
      indices.push_back(ft_index);

      // If component_indices provided, search only within component; otherwise search all features
      const std::vector<int> *search_indices = component_indices;
      std::vector<int> all_indices;
      if (!component_indices)
      {
        all_indices.resize(fts.size());
        std::iota(all_indices.begin(), all_indices.end(), 0);
        search_indices = &all_indices;
      }

      for (int z : *search_indices)
      {
        const bool within_max_mz_chain = fts.mz[z] > mz && fts.mz[z] <= max_mz_chain;
        const bool same_polarity = fts.polarity[z] == polarity;
        const bool not_main_ft = fts.feature[z] != feature;

        if (within_max_mz_chain && same_polarity && not_main_ft)
        {
          chain.push_back(fts.get_feature(z));
          indices.push_back(z);
        }
      }
    }

    void CANDIDATE_CHAIN::annotate_adducts(float ppm, bool debug)
    {
      ADDUCT_SET all_adducts;
      const int &pol = chain[0].polarity;
      const float neutralizer = all_adducts.neutralizer(pol);
      std::vector<ADDUCT> adducts = all_adducts.adducts(pol);
      const int number_candidates = chain.size();
      const std::vector<float> &mzr = this->get_chain_mzr(ppm);
      const float &mion_mz = chain[0].mz;
      const float &mion_mzr = mzr[0];

      // Find the monoisotopic ion ([M+H]+ or [M-H]-) in the chain to reference its mass
      int mh_index = -1;
      float mh_mz = 0.0f;
      std::string base_adduct = (pol == 1) ? "[M+H]+" : "[M-H]-";

      for (int c = 0; c < number_candidates; ++c)
      {
        if (chain[c].adduct == base_adduct)
        {
          mh_index = c;
          mh_mz = chain[c].mz;
          break;
        }
      }

      for (size_t a = 0; a < adducts.size(); ++a)
      {
        const ADDUCT &adduct = adducts[a];
        const float &adduct_mass_distance = adduct.mass_distance;

        for (int c = 1; c < number_candidates; ++c)
        {
          // Skip if already annotated
          if (chain[c].adduct != chain[0].adduct)
          {
            continue;
          }

          const float &mz = chain[c].mz;
          const float exp_mass_distance = mz - (mion_mz + neutralizer);
          const float mass_error = std::abs(exp_mass_distance - adduct_mass_distance);
          const float mass_error_ppm = (mass_error / mz) * 1e6f;

          if (mass_error < mion_mzr)
          {
            if (debug)
            {
              DEBUG_LOG("      -> Assigning adduct " << adduct.type << " to " << chain[c].feature
                        << " (mass_error=" << mass_error << ", mass_error_ppm=" << mass_error_ppm << ")" << std::endl);
            }

            // If we found the monoisotopic ion in the chain and this is not that ion,
            // format as adduct MZXXX [M+Element] to show the relationship
            if (mh_index >= 0 && adduct.type != base_adduct)
            {
              std::ostringstream oss;
              oss << "adduct MZ" << std::round(mh_mz) << " " << adduct.type;
              chain[c].adduct = oss.str();
            }
            else
            {
              // Use the proper adduct notation from the adduct catalog
              chain[c].adduct = adduct.type;
            }
            break;
          }
        }
      }
    }

    void CANDIDATE_CHAIN::find_fragment_candidates(const nta::api::NTA_FEATURE_ROW &ft,
                            const nta::api::NTA_FEATURES &fts,
                            const int &ft_index,
                            const std::vector<int> *component_indices)
    {
      const std::string &feature = ft.feature;
      const int &polarity = ft.polarity;
      const float &mz = ft.mz;
      // Search for fragments up to 100 Da lighter
      const float min_mz_chain = (mz > 100) ? (mz - 100) : 0;

      chain.push_back(ft);
      indices.push_back(ft_index);

      // If component_indices provided, search only within component; otherwise search all features
      const std::vector<int> *search_indices = component_indices;
      std::vector<int> all_indices;
      if (!component_indices)
      {
        all_indices.resize(fts.size());
        std::iota(all_indices.begin(), all_indices.end(), 0);
        search_indices = &all_indices;
      }

      for (int z : *search_indices)
      {
        const bool within_mz_range = fts.mz[z] < mz && fts.mz[z] >= min_mz_chain;
        const bool same_polarity = fts.polarity[z] == polarity;
        const bool not_main_ft = fts.feature[z] != feature;

        if (within_mz_range && same_polarity && not_main_ft)
        {
          chain.push_back(fts.get_feature(z));
          indices.push_back(z);
        }
      }
    }

    void CANDIDATE_CHAIN::annotate_fragments(float ppm, bool debug)
    {
      FRAGMENT_LOSS_SET all_losses;
      const int &pol = chain[0].polarity;
      std::vector<FRAGMENT_LOSS> losses = all_losses.losses(pol);
      const int number_candidates = chain.size();
      const std::vector<float> &mzr = this->get_chain_mzr(ppm);
      const float &parent_mz = chain[0].mz;
      const float &parent_mzr = mzr[0];

      for (size_t l = 0; l < losses.size(); ++l)
      {
        const FRAGMENT_LOSS &loss = losses[l];
        const float &loss_mass = loss.mass_loss;

        for (int c = 1; c < number_candidates; ++c)
        {
          // Skip if already annotated (not empty)
          if (!chain[c].adduct.empty())
          {
            continue;
          }

          const float &mz = chain[c].mz;
          const float exp_mass_loss = parent_mz - mz;
          const float mass_error = std::abs(exp_mass_loss - loss_mass);
          const float mass_error_ppm = (mass_error / mz) * 1e6f;

          if (mass_error < parent_mzr)
          {
            if (debug)
            {
              DEBUG_LOG("      -> Assigning fragment loss -" << loss.formula << " to " << chain[c].feature
                        << " (mass_error=" << mass_error << ", mass_error_ppm=" << mass_error_ppm << ")" << std::endl);
            }

            // Format as "loss MZXXX -Formula"
            std::ostringstream oss;
            oss << "loss MZ" << std::round(parent_mz) << " -" << loss.formula;
            chain[c].adduct = oss.str();
            break;
          }
        }
      }
    } // namespace streamfind::nta_annotation_detail

    using namespace streamfind::nta_annotation_detail;

    // MARK: annotate_components_impl
    void annotate_components_impl(
      nta::PROJECT_NON_TARGET_ANALYSIS &nta_data,
        int maxIsotopes,
        int maxCharge,
        int maxGaps,
        float ppm,
        const std::vector<std::string> &isotopeElements,
        const std::string &debugComponent,
        const std::string &debugAnalysis)
    {
      ISOTOPE_SET isotopes;
      const std::vector<std::string> default_elements = {"C:1-60", "N:0-10", "O:0-20", "S:0-4", "Cl:0-6", "Br:0-4"};
      const ISOTOPE_ELEMENT_SPEC parsed_specs = parse_isotope_element_specs(isotopeElements.empty() ? default_elements : isotopeElements);
      isotopes.filter(parsed_specs.elements);
      isotopes.set_ranges(parsed_specs.ranges);

      const int max_number_elements = 5;
      std::cerr << "Building combinatorial isotopic chains with length " << max_number_elements << "...";
      ISOTOPE_COMBINATIONS combinations(isotopes, max_number_elements);
      std::cerr << "Done!" << std::endl;

      auto &feature_buffers = nta_data.feature_buffers();
      const auto &analysis_names = nta_data.analysis_names();
      const int number_analyses = static_cast<int>(feature_buffers.size());

      if (number_analyses == 0)
      {
        std::cerr << "No analyses found for annotation!" << std::endl;
        return;
      }

      ADDUCT_SET all_adducts;
      FRAGMENT_LOSS_SET all_losses;

      for (int a = 0; a < number_analyses; a++)
      {
        nta::api::NTA_FEATURES &fts = feature_buffers[a];
        const int number_features = fts.size();
        if (number_features == 0)
          continue;

        bool should_debug = (!debugComponent.empty() && !debugAnalysis.empty() && analysis_names[a] == debugAnalysis);

        fts.sort_by_mz();

        std::unordered_map<std::string, std::vector<int>> component_groups;
        for (int f = 0; f < number_features; f++)
        {
          nta::api::NTA_FEATURE_ROW ft = fts.get_feature(f);
          if (!ft.feature_component.empty())
            component_groups[ft.feature_component].push_back(f);
        }

        std::cerr << "Annotating " << component_groups.size() << " components in analysis " << analysis_names[a] << std::endl;

        int total_isotopes_found = 0;
        int total_adducts_found = 0;
        int total_fragments_found = 0;
        int default_adducts_assigned = 0;

        for (const auto &comp_pair : component_groups)
        {
          const std::string &component_id = comp_pair.first;
          const std::vector<int> &component_indices = comp_pair.second;
          if (component_indices.empty())
            continue;

          bool debug_this_component = (should_debug && component_id == debugComponent);
          if (debug_this_component)
          {
            std::ostringstream log_filename;
            log_filename << "log/debug_annotation_" << debugAnalysis << "_" << debugComponent << ".log";
            std::ostringstream header;
            header << "=== Component Annotation Debug Log ===" << std::endl
                   << "Analysis: " << debugAnalysis << std::endl
                   << "Component: " << debugComponent << std::endl;
            nta::utils::init_debug_log(log_filename.str(), header.str());
          }

          for (int idx : component_indices)
          {
            auto ft = fts.get_feature(idx);
            ft.adduct.clear();
            fts.set_feature(idx, ft);
          }

          std::vector<int> sorted_indices = component_indices;
          std::sort(sorted_indices.begin(), sorted_indices.end(), [&fts](int lhs, int rhs) {
            return fts.mz[lhs] < fts.mz[rhs];
          });

          struct ISOTOPE_CHAIN_ASSIGNMENT
          {
            int anchor_idx = -1;
            std::vector<ANNOTATION_CANDIDATE> children;
            double total_ppm = 0.0;
            double total_rt = 0.0;
          };

          std::unordered_map<int, ANNOTATION_CANDIDATE> final_candidate;
          for (int idx : sorted_indices)
          {
            const auto ft = fts.get_feature(idx);
            ANNOTATION_CANDIDATE fallback;
            fallback.cat = "default";
            fallback.type = (ft.polarity == 1) ? "[M+H]+" : "[M-H]-";
            fallback.parent_feature = ft.feature;
            fallback.element_or_delta = (ft.polarity == 1) ? "H" : "-H";
            fallback.feature_index = idx;
            fallback.parent_index = idx;
            fallback.is_default = true;
            fallback.score = 0.01;
            fallback.priority = candidate_priority("default", fallback.type);
            fallback.label = fallback.type;
            final_candidate[idx] = fallback;
          }

          std::vector<ISOTOPE_CHAIN_ASSIGNMENT> isotope_assignments;
          for (int anchor_idx : sorted_indices)
          {
            const auto anchor = fts.get_feature(anchor_idx);
            CANDIDATE_CHAIN isotope_chain;
            isotope_chain.find_isotopic_candidates(anchor, fts, anchor_idx, maxIsotopes, &component_indices, nullptr);
            if (isotope_chain.size() > 1)
            {
              isotope_chain.annotate_isotopes(combinations, maxIsotopes, maxCharge, maxGaps, ppm, debug_this_component);
              ISOTOPE_CHAIN_ASSIGNMENT assignment;
              assignment.anchor_idx = anchor_idx;
              for (size_t i = 1; i < isotope_chain.chain.size(); ++i)
              {
                const int child_idx = isotope_chain.indices[i];
                const auto &child = isotope_chain.chain[i];
                if (!starts_with(child.adduct, "isotope "))
                  continue;

                ANNOTATION_CANDIDATE candidate;
                candidate.cat = "isotope";
                candidate.type = extract_isotope_type(child.adduct);
                candidate.parent_feature = anchor.feature;
                candidate.element_or_delta = extract_isotope_element(child.adduct);
                candidate.feature_index = child_idx;
                candidate.parent_index = anchor_idx;
                const double theoretical_mz = anchor.mz +
                  (isotope_chain.isotope_theoretical_mass_distance.count(child_idx) > 0 ?
                    isotope_chain.isotope_theoretical_mass_distance.at(child_idx) :
                    isotope_mass_delta(candidate.element_or_delta));
                candidate.mass_error_da = std::abs(child.mz - theoretical_mz);
                candidate.mass_error_ppm = ppm_error(child.mz, theoretical_mz);
                if (candidate.mass_error_ppm > ppm)
                  continue;
                candidate.rt_error = std::abs(child.rt - anchor.rt);
                candidate.rel_intensity = (anchor.intensity > 0.0) ? (child.intensity / anchor.intensity) : 0.0;
                candidate.expected_rel_intensity_min =
                  isotope_chain.isotope_theoretical_abundance_min.count(child_idx) > 0 ?
                  isotope_chain.isotope_theoretical_abundance_min.at(child_idx) : 0.0;
                candidate.expected_rel_intensity_max =
                  isotope_chain.isotope_theoretical_abundance_max.count(child_idx) > 0 ?
                  isotope_chain.isotope_theoretical_abundance_max.at(child_idx) : 1.5;
                candidate.priority = candidate_priority(candidate.cat, candidate.type);
                candidate.score = candidate_score(candidate, ppm);
                if (candidate.score < 0.0)
                  continue;
                candidate.label = make_annotation_label(candidate);
                assignment.total_ppm += candidate.mass_error_ppm;
                assignment.total_rt += candidate.rt_error;
                assignment.children.push_back(candidate);
              }

              if (!assignment.children.empty())
                isotope_assignments.push_back(std::move(assignment));
            }
          }

          std::sort(isotope_assignments.begin(), isotope_assignments.end(), [&fts](const ISOTOPE_CHAIN_ASSIGNMENT &lhs, const ISOTOPE_CHAIN_ASSIGNMENT &rhs) {
            if (lhs.children.size() != rhs.children.size())
              return lhs.children.size() > rhs.children.size();
            if (lhs.total_ppm != rhs.total_ppm)
              return lhs.total_ppm < rhs.total_ppm;
            if (lhs.total_rt != rhs.total_rt)
              return lhs.total_rt < rhs.total_rt;
            return fts.mz[lhs.anchor_idx] < fts.mz[rhs.anchor_idx];
          });

          std::unordered_set<int> isotope_anchor_children;
          for (const auto &assignment : isotope_assignments)
          {
            for (const auto &candidate : assignment.children)
              isotope_anchor_children.insert(candidate.feature_index);
          }

          std::unordered_set<int> isotope_children;
          std::unordered_set<int> isotope_occupied;
          for (const auto &assignment : isotope_assignments)
          {
            if (isotope_anchor_children.count(assignment.anchor_idx) > 0)
              continue;
            if (isotope_occupied.count(assignment.anchor_idx) > 0)
              continue;

            bool conflict = false;
            for (const auto &candidate : assignment.children)
            {
              if (isotope_occupied.count(candidate.feature_index) > 0)
              {
                conflict = true;
                break;
              }
            }
            if (conflict)
              continue;

            isotope_occupied.insert(assignment.anchor_idx);
            for (const auto &candidate : assignment.children)
            {
              isotope_occupied.insert(candidate.feature_index);
              isotope_children.insert(candidate.feature_index);
              final_candidate[candidate.feature_index] = candidate;
            }
          }

          std::vector<int> non_isotope_indices;
          non_isotope_indices.reserve(sorted_indices.size());
          for (int idx : sorted_indices)
          {
            if (isotope_children.count(idx) == 0)
              non_isotope_indices.push_back(idx);
          }

          std::unordered_map<int, std::vector<ANNOTATION_CANDIDATE>> relation_candidates;
          for (int anchor_idx : non_isotope_indices)
          {
            const auto anchor = fts.get_feature(anchor_idx);
            const auto adducts = all_adducts.adducts(anchor.polarity);
            const auto losses = all_losses.losses(anchor.polarity);
            const double neutral_mass = neutral_mass_from_base_ion(anchor);

            for (int idx : non_isotope_indices)
            {
              if (idx == anchor_idx)
                continue;

              const auto child = fts.get_feature(idx);
              const double rt_error = std::abs(child.rt - anchor.rt);
              const double rel_intensity = (anchor.intensity > 0.0) ? (child.intensity / anchor.intensity) : 0.0;

              for (const auto &adduct : adducts)
              {
                if (adduct.type == ((anchor.polarity == 1) ? "[M+H]+" : "[M-H]-"))
                  continue;
                const double theoretical_mz = theoretical_mz_from_adduct(neutral_mass, adduct);
                const double mass_error_ppm_value = ppm_error(child.mz, theoretical_mz);
                if (mass_error_ppm_value > std::max(10.0, static_cast<double>(ppm) * 1.5))
                  continue;

                ANNOTATION_CANDIDATE candidate;
                candidate.cat = "adduct";
                candidate.type = adduct.type;
                candidate.parent_feature = anchor.feature;
                candidate.element_or_delta = adduct.element;
                candidate.feature_index = idx;
                candidate.parent_index = anchor_idx;
                candidate.mass_error_da = std::abs(child.mz - theoretical_mz);
                candidate.mass_error_ppm = mass_error_ppm_value;
                candidate.rt_error = rt_error;
                candidate.rel_intensity = rel_intensity;
                candidate.expected_rel_intensity_min = 0.0;
                candidate.expected_rel_intensity_max = 2.0;
                candidate.priority = candidate_priority(candidate.cat, candidate.type);
                candidate.score = candidate_score(candidate, ppm);
                candidate.label = make_annotation_label(candidate);
                relation_candidates[idx].push_back(candidate);
              }

              for (const auto &loss : losses)
              {
                if (child.mz >= anchor.mz)
                  continue;
                const double theoretical_mz = anchor.mz - loss.mass_loss;
                const double mass_error_ppm_value = ppm_error(child.mz, theoretical_mz);
                if (mass_error_ppm_value > std::max(10.0, static_cast<double>(ppm) * 1.5))
                  continue;

                ANNOTATION_CANDIDATE candidate;
                candidate.cat = "loss";
                candidate.type = "M-" + loss.formula;
                candidate.parent_feature = anchor.feature;
                candidate.element_or_delta = "-" + loss.formula;
                candidate.feature_index = idx;
                candidate.parent_index = anchor_idx;
                candidate.mass_error_da = std::abs(child.mz - theoretical_mz);
                candidate.mass_error_ppm = mass_error_ppm_value;
                candidate.rt_error = rt_error;
                candidate.rel_intensity = rel_intensity;
                candidate.expected_rel_intensity_min = 0.0;
                candidate.expected_rel_intensity_max = 1.0;
                candidate.priority = candidate_priority(candidate.cat, candidate.type);
                candidate.score = candidate_score(candidate, ppm);
                candidate.label = make_annotation_label(candidate);
                relation_candidates[idx].push_back(candidate);
              }
            }
          }

          std::unordered_map<int, ANNOTATION_CANDIDATE> relation_state;
          for (int idx : non_isotope_indices)
            relation_state[idx] = final_candidate[idx];

          std::vector<int> relation_update_order = non_isotope_indices;
          std::sort(relation_update_order.begin(), relation_update_order.end(), [&fts](int lhs, int rhs) {
            if (fts.mz[lhs] != fts.mz[rhs])
              return fts.mz[lhs] > fts.mz[rhs];
            return lhs < rhs;
          });

          bool relation_changed = true;
          const int max_relation_iterations = std::max(1, static_cast<int>(non_isotope_indices.size()));
          for (int iter = 0; iter < max_relation_iterations && relation_changed; ++iter)
          {
            relation_changed = false;
            std::unordered_map<int, ANNOTATION_CANDIDATE> next_state = relation_state;

            for (int feature_idx : relation_update_order)
            {
              auto cand_it = relation_candidates.find(feature_idx);
              if (cand_it == relation_candidates.end())
                continue;
              auto &feature_candidates = cand_it->second;

              auto current_it = relation_state.find(feature_idx);
              ANNOTATION_CANDIDATE best = final_candidate[feature_idx];
              if (current_it != relation_state.end() && relation_candidate_is_valid(current_it->second, next_state))
                best = current_it->second;

              for (const auto &candidate : feature_candidates)
              {
                if (!relation_candidate_is_valid(candidate, next_state))
                  continue;
                if (best.is_default || candidate_better(candidate, best))
                  best = candidate;
              }

              next_state[feature_idx] = best;
              if (!candidate_equals(best, relation_state[feature_idx]))
                relation_changed = true;
            }

            relation_state.swap(next_state);
          }

          for (const auto &[feature_idx, candidate] : relation_state)
            final_candidate[feature_idx] = candidate;

          std::unordered_set<int> reserved_targets;
          for (const auto &entry : final_candidate)
          {
            if (!entry.second.is_default)
              reserved_targets.insert(entry.first);
          }

          std::vector<int> derived_anchor_indices;
          for (const auto &entry : final_candidate)
          {
            if (!entry.second.is_default && (entry.second.cat == "adduct" || entry.second.cat == "loss"))
            {
              std::unordered_set<int> visited;
              if (relation_chain_reaches_root(entry.first, final_candidate, visited))
                derived_anchor_indices.push_back(entry.first);
            }
          }

          for (int anchor_idx : derived_anchor_indices)
          {
            const auto anchor = fts.get_feature(anchor_idx);
            std::unordered_set<int> unavailable = reserved_targets;
            unavailable.erase(anchor_idx);

            CANDIDATE_CHAIN isotope_chain;
            isotope_chain.find_isotopic_candidates(anchor, fts, anchor_idx, maxIsotopes, &component_indices, &unavailable);
            if (isotope_chain.size() <= 1)
              continue;

            isotope_chain.annotate_isotopes(combinations, maxIsotopes, maxCharge, maxGaps, ppm, debug_this_component);
            for (size_t i = 1; i < isotope_chain.chain.size(); ++i)
            {
              const int child_idx = isotope_chain.indices[i];
              const auto &child = isotope_chain.chain[i];
              if (!starts_with(child.adduct, "isotope "))
                continue;
              if (reserved_targets.count(child_idx) > 0)
                continue;

              ANNOTATION_CANDIDATE candidate;
              candidate.cat = "isotope";
              candidate.type = extract_isotope_type(child.adduct);
              candidate.parent_feature = anchor.feature;
              candidate.element_or_delta = extract_isotope_element(child.adduct);
              candidate.feature_index = child_idx;
              candidate.parent_index = anchor_idx;
              const double theoretical_mz = anchor.mz +
                (isotope_chain.isotope_theoretical_mass_distance.count(child_idx) > 0 ?
                  isotope_chain.isotope_theoretical_mass_distance.at(child_idx) :
                  isotope_mass_delta(candidate.element_or_delta));
              candidate.mass_error_da = std::abs(child.mz - theoretical_mz);
              candidate.mass_error_ppm = ppm_error(child.mz, theoretical_mz);
              if (candidate.mass_error_ppm > ppm)
                continue;
              candidate.rt_error = std::abs(child.rt - anchor.rt);
              candidate.rel_intensity = (anchor.intensity > 0.0) ? (child.intensity / anchor.intensity) : 0.0;
              candidate.expected_rel_intensity_min =
                isotope_chain.isotope_theoretical_abundance_min.count(child_idx) > 0 ?
                isotope_chain.isotope_theoretical_abundance_min.at(child_idx) : 0.0;
              candidate.expected_rel_intensity_max =
                isotope_chain.isotope_theoretical_abundance_max.count(child_idx) > 0 ?
                isotope_chain.isotope_theoretical_abundance_max.at(child_idx) : 1.5;
              candidate.priority = candidate_priority(candidate.cat, candidate.type);
              candidate.score = candidate_score(candidate, ppm);
              if (candidate.score < 0.0)
                continue;
              candidate.label = make_annotation_label(candidate);
              final_candidate[child_idx] = candidate;
              reserved_targets.insert(child_idx);
            }
          }

          for (int idx : sorted_indices)
          {
            auto ft = fts.get_feature(idx);
            const auto it = final_candidate.find(idx);
            if (it == final_candidate.end() || it->second.is_default)
            {
              ft.adduct = (ft.polarity == 1) ? "[M+H]+" : "[M-H]-";
              ft.annotation_category.clear();
              ft.annotation_type.clear();
              ft.annotation_parent_feature.clear();
              ft.annotation_element.clear();
              ft.annotation_mass_error_da = 0.0;
              ft.annotation_mass_error_ppm = 0.0;
              ft.annotation_rt_error = 0.0;
              ft.annotation_rel_intensity = 0.0;
              ft.annotation_expected_rel_intensity_min = 0.0;
              ft.annotation_expected_rel_intensity_max = 0.0;
              ft.annotation_score = 0.0;
              default_adducts_assigned++;
            }
            else
            {
              ft.adduct = make_annotation_summary(it->second);
              ft.annotation_category = it->second.cat;
              ft.annotation_type = it->second.type;
              ft.annotation_parent_feature = it->second.parent_feature;
              ft.annotation_element = it->second.element_or_delta;
              ft.annotation_mass_error_da = it->second.mass_error_da;
              ft.annotation_mass_error_ppm = it->second.mass_error_ppm;
              ft.annotation_rt_error = it->second.rt_error;
              ft.annotation_rel_intensity = it->second.rel_intensity;
              ft.annotation_expected_rel_intensity_min = it->second.expected_rel_intensity_min;
              ft.annotation_expected_rel_intensity_max = it->second.expected_rel_intensity_max;
              ft.annotation_score = it->second.score;
              if (it->second.cat == "isotope")
                total_isotopes_found++;
              else if (it->second.cat == "adduct")
                total_adducts_found++;
              else if (it->second.cat == "loss")
                total_fragments_found++;
            }
            fts.set_feature(idx, ft);
          }

          if (debug_this_component)
          {
            DEBUG_LOG("\n=== Final Annotations for Component " << component_id << " ===" << std::endl);
            for (const int idx : sorted_indices)
            {
              const auto ft = fts.get_feature(idx);
              DEBUG_LOG("  " << ft.feature << " mz=" << ft.mz << " rt=" << ft.rt << " adduct=\"" << ft.adduct << "\"" << std::endl);
            }
          }
        }

        std::cerr << "Annotating isotopes... Done! Found " << total_isotopes_found << " isotopes." << std::endl;
        std::cerr << "Annotating fragments... Done! Found " << total_fragments_found << " fragments." << std::endl;
        std::cerr << "Annotating adducts... Done! Found " << total_adducts_found << " adducts." << std::endl;
        std::cerr << "Assigning default adducts to remaining features... Done! Assigned " << default_adducts_assigned << " default adducts." << std::endl;
      }
    }

  } // namespace annotation
} // namespace nta
