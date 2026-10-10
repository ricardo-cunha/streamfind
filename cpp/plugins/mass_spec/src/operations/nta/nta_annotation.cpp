#include "operations/nta/nta_annotation.hpp"
#include "utils/nta.hpp"
#include "streamfind/core/vendors/openbabel.hpp"
#include "element_tables.h"
#include "utils/nta.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <limits>
#include <cctype>
#include <stdexcept>

namespace streamfind::mass_spec::nta
{
  namespace annotation
  {
    double isotope_combination_chemical_preference(const std::vector<std::string> &combination)
    {
      int carbon_isotopes = 0;
      int halogen_isotopes = 0;
      for (const auto &isotope : combination)
      {
        if (isotope == "13C")
          ++carbon_isotopes;
        else if (isotope == "37Cl" || isotope == "81Br")
          ++halogen_isotopes;
      }

      // Organic compounds are the default domain. Prefer a carbon-supported
      // explanation when the isotope-mass match is effectively tied, while
      // retaining halogen candidates for the complete-envelope comparison.
      return (0.04 * static_cast<double>(carbon_isotopes)) -
             (0.10 * static_cast<double>(halogen_isotopes));
    }

    bool isotope_combination_is_carbon_only(const std::vector<std::string> &combination)
    {
      bool has_carbon = false;
      for (const auto &isotope : combination)
      {
        if (isotope == "13C")
          has_carbon = true;
        if (isotope == "37Cl" || isotope == "81Br")
          return false;
      }
      return has_carbon;
    }

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

      bool relation_candidate_is_valid_for_root(
          const ANNOTATION_CANDIDATE &candidate,
          const std::unordered_map<int, ANNOTATION_CANDIDATE> &state,
          int root_index)
      {
        if (!relation_candidate_is_valid(candidate, state))
          return false;

        if (candidate.parent_index == root_index)
          return true;

        const auto parent_it = state.find(candidate.parent_index);
        if (parent_it == state.end() || parent_it->second.is_default)
          return false;

        // Only an already-selected loss chain may continue below the chosen
        // molecular root. Adducts and dimers must attach directly to that
        // root; otherwise an unselected default feature can become an
        // accidental parent merely because it happens to precede the target
        // in the local update order.
        return candidate.cat == "loss" && parent_it->second.cat == "loss";
      }

      double neutral_mass_from_base_ion(const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &ft)
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

      std::string lowercase_copy(const std::string &value)
      {
        std::string out = value;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char character) {
          return static_cast<char>(std::tolower(character));
        });
        return out;
      }

      std::string adduct_modification_token(const ADDUCT &adduct)
      {
        const auto open = adduct.type.find('[');
        const auto close = adduct.type.find(']');
        if (open == std::string::npos || close == std::string::npos || close <= open + 1)
          return adduct.type;

        std::string expression = adduct.type.substr(open + 1, close - open - 1);
        if (starts_with(expression, "M"))
          expression.erase(0, 1);
        return expression;
      }

      std::string loss_modification_token(const FRAGMENT_LOSS &loss)
      {
        return loss.expression.empty() ? "-" + loss.formula : loss.expression;
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
          if (min_n < 0 || max_n < min_n)
            throw std::invalid_argument("isotope element ranges must satisfy 0 <= min <= max: " + spec);
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
        static const ISOTOPE_SET isotopes;
        const std::vector<std::string> tokens = split_string(element_label, '/');
        if (tokens.empty())
          return 0.0;

        double score = 0.0;
        for (const auto &token : tokens)
        {
          const auto it = std::find_if(isotopes.data.begin(), isotopes.data.end(), [&](const ISOTOPE &isotope) {
            return isotope.isotope == token;
          });
          if (it == isotopes.data.end())
          {
            score += 0.0;
            continue;
          }

          // Derive the priority from the current IsoSpec abundance rather
          // than a second handwritten isotope-property table. The chemical
          // carbon/halogen preference is applied separately by the envelope
          // scorer.
          const double relative_abundance = std::max(0.0, static_cast<double>(it->abundance));
          score += std::min(1.0, std::sqrt(relative_abundance));
        }
        return score / static_cast<double>(tokens.size());
      }

      double isotope_mass_delta(const std::string &element_label)
      {
        double total = 0.0;
        static const ISOTOPE_SET isotopes;
        for (const auto &token : split_string(element_label, '/'))
        {
          const auto it = std::find_if(isotopes.data.begin(), isotopes.data.end(), [&](const ISOTOPE &isotope) {
            return isotope.isotope == token;
          });
          if (it != isotopes.data.end())
            total += it->mass_distance;
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
    ISOTOPE_SET::ISOTOPE_SET()
    {
      struct ElementBase
      {
        std::string symbol;
        double mass = 0.0;
        double probability = 0.0;
      };

      std::vector<ElementBase> bases;
      for (size_t i = 0; i < IsoSpec::isospec_number_of_isotopic_entries; ++i)
      {
        const std::string symbol = IsoSpec::elem_table_symbol[i];
        if (symbol.empty() || IsoSpec::elem_table_Radioactive[i] || IsoSpec::elem_table_probability[i] <= 0.0)
          continue;
        auto base = std::find_if(bases.begin(), bases.end(), [&](const ElementBase &candidate) {
          return candidate.symbol == symbol;
        });
        if (base == bases.end())
        {
          bases.push_back({symbol, IsoSpec::elem_table_mass[i], IsoSpec::elem_table_probability[i]});
        }
        else if (IsoSpec::elem_table_probability[i] > base->probability)
        {
          base->mass = IsoSpec::elem_table_mass[i];
          base->probability = IsoSpec::elem_table_probability[i];
        }
      }

      for (size_t i = 0; i < IsoSpec::isospec_number_of_isotopic_entries; ++i)
      {
        const std::string symbol = IsoSpec::elem_table_symbol[i];
        if (symbol.empty() || IsoSpec::elem_table_Radioactive[i] || IsoSpec::elem_table_probability[i] <= 0.0)
          continue;
        const auto base = std::find_if(bases.begin(), bases.end(), [&](const ElementBase &candidate) {
          return candidate.symbol == symbol;
        });
        if (base == bases.end() || std::abs(IsoSpec::elem_table_mass[i] - base->mass) < 1e-9)
          continue;

        const int mass_number = static_cast<int>(std::llround(IsoSpec::elem_table_massNo[i]));
        const std::string isotope = std::to_string(mass_number) + symbol;
        data.emplace_back(
            symbol,
            isotope,
            static_cast<float>(IsoSpec::elem_table_mass[i] - base->mass),
            static_cast<float>(IsoSpec::elem_table_probability[i] / base->probability),
            static_cast<float>(base->probability),
            0,
            100);
      }
    }

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
      combinations_by_step.resize(max_number_elements + 1);

      for (int i = 0; i < length; i++)
      {
        tensor_combinations[i] = tensor_combinations_unordered[order_idx[i]];
        tensor_mass_distances[i] = tensor_mass_distances_unordered[order_idx[i]];
        tensor_abundances[i] = tensor_abundances_unordered[order_idx[i]];
        mass_distances[i] = mass_distances_unordered[order_idx[i]];
        step[i] = std::round(mass_distances[i]);
        if (step[i] >= 0 && step[i] < static_cast<int>(combinations_by_step.size()))
          combinations_by_step[step[i]].push_back(i);
      }
    }

    // MARK: ISOTOPE_CHAIN Implementation
    ISOTOPE_CHAIN::ISOTOPE_CHAIN(const int &z, const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &mono_ion, float mono_mzr)
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
    namespace modification_mass_detail
    {
      constexpr double electron_mass_da = 0.000548579909065;

      std::string expand_formula_coefficient(const std::string &formula)
      {
        if (formula.empty() || !std::isdigit(static_cast<unsigned char>(formula.front())))
          return formula;

        std::size_t separator = 0;
        while (separator < formula.size() && std::isdigit(static_cast<unsigned char>(formula[separator])))
          ++separator;
        const int coefficient = std::stoi(formula.substr(0, separator));
        if (coefficient <= 0 || separator == formula.size())
          throw std::invalid_argument("invalid modification formula: " + formula);

        std::string expanded;
        std::size_t cursor = separator;
        while (cursor < formula.size())
        {
          if (!std::isupper(static_cast<unsigned char>(formula[cursor])))
            throw std::invalid_argument("invalid modification formula: " + formula);
          const std::size_t element_start = cursor++;
          if (cursor < formula.size() && std::islower(static_cast<unsigned char>(formula[cursor])))
            ++cursor;
          const std::string element = formula.substr(element_start, cursor - element_start);
          const std::size_t count_start = cursor;
          while (cursor < formula.size() && std::isdigit(static_cast<unsigned char>(formula[cursor])))
            ++cursor;
          const int count = count_start == cursor ? 1 : std::stoi(formula.substr(count_start, cursor - count_start));
          if (count <= 0)
            throw std::invalid_argument("invalid modification formula: " + formula);
          expanded += element;
          const int expanded_count = coefficient * count;
          if (expanded_count != 1)
            expanded += std::to_string(expanded_count);
        }
        return expanded;
      }

      std::string adduct_formula(const std::string &element)
      {
        if (element == "ACN+H")
          return "C2H4N";
        if (element == "CH3OH+H")
          return "CH5O";
        if (element == "CH3COO")
          return "C2H3O2";
        if (element == "FA-H")
          return "CHO2";
        if (element == "2FA-H")
          return "CHO2";
        if (element == "2-H")
          return "H";
        if (element == "2H")
          return "H";
        if (element == "2Na")
          return "Na";
        if (element == "2K")
          return "K";
        if (element == "2NH4")
          return "NH4";
        if (element == "2Cl")
          return "Cl";
        if (!element.empty() && element.front() == '-')
          return expand_formula_coefficient(element.substr(1));
        return expand_formula_coefficient(element);
      }

      double formula_mass(const std::string &formula)
      {
        const auto result = ::streamfind::core::vendors::openbabel::mass_from_formula(formula);
        if (!result.ok)
          throw std::runtime_error("Open Babel could not calculate mass for formula " + formula + ": " + result.error);
        return result.exact_mass;
      }

      double adduct_mass(const ADDUCT &adduct)
      {
        const std::string formula = adduct_formula(adduct.element);
        const double neutral_mass = formula_mass(formula);
        const bool subtract = (!adduct.element.empty() && adduct.element.front() == '-') || adduct.element == "2-H";
        if (subtract)
          return -(neutral_mass - electron_mass_da);
        return neutral_mass + (adduct.polarity < 0 ? electron_mass_da : -electron_mass_da);
      }
    }

    const std::vector<std::string> &default_adduct_expressions()
    {
      static const std::vector<std::string> expressions{
          "+H", "+Na", "+K", "+NH4", "+ACN+H", "+CH3OH+H",
          "2M+H", "2M+Na", "2M+K", "2M+NH4",
          "-H", "+Cl", "+Br", "+CHO2", "+CH3COO", "+FA-H",
          "2M-H", "2M+Cl", "2M+FA-H"};
      return expressions;
    }

    const std::vector<std::string> &default_loss_expressions()
    {
      static const std::vector<std::string> expressions{
          "-H2O", "-CO2", "-NH3", "-CO", "-CH3", "-CH2O2",
          "-HCl", "-HF", "-SO2", "-SO3", "-H2SO4", "-CH3OH",
          "-C2H4", "-C2H2", "-NO", "-NO2", "-HNO2", "-HNO3",
          "-CH2", "-C2H6O", "-HPO3", "-H3PO4"};
      return expressions;
    }

    ADDUCT make_default_adduct(const std::string &expression)
    {
      const bool dimer = expression.rfind("2M", 0) == 0;
      const std::string suffix = dimer ? expression.substr(2) : expression;
      if (suffix.empty())
        throw std::invalid_argument("invalid adduct expression: " + expression);

      const bool negative = expression == "-H" || expression == "+Cl" ||
          expression == "+Br" || expression == "+CHO2" || expression == "+CH3COO" ||
          expression == "+FA-H" || expression == "2M-H" || expression == "2M+Cl" ||
          expression == "2M+FA-H";
      const int polarity = negative ? -1 : 1;
      const int multiplicity = dimer ? 2 : 1;
      const std::string adduct_component = suffix.front() == '+' ? suffix.substr(1) : suffix;
      const std::string element = dimer ? "2" + adduct_component : adduct_component;

      const std::string ion_sign = polarity > 0 ? "+" : "-";
      const std::string type = "[" + (dimer ? expression : "M" + expression) + "]" + ion_sign;
      return ADDUCT(element, polarity, "adduct", type, 0.0f, 1, multiplicity);
    }

    FRAGMENT_LOSS make_default_loss(const std::string &expression)
    {
      if (expression.size() < 2 || expression.front() != '-')
        throw std::invalid_argument("invalid loss expression: " + expression);
      const std::string formula = modification_mass_detail::expand_formula_coefficient(expression.substr(1));
      const int polarity = expression == "-NH3" ? 1 : (expression == "-CH2O2" ? -1 : 0);
      return FRAGMENT_LOSS(expression.substr(1), formula, 0.0f, polarity, expression);
    }

    ADDUCT_SET::ADDUCT_SET()
    {
      for (const auto &expression : default_adduct_expressions())
        all_adducts.push_back(make_default_adduct(expression));
      for (ADDUCT &adduct : neutralizers)
      {
        adduct.formula = "H";
        const double proton_mass = modification_mass_detail::formula_mass(adduct.formula) - modification_mass_detail::electron_mass_da;
        adduct.mass_distance = static_cast<float>(-adduct.polarity * proton_mass);
      }
      for (ADDUCT &adduct : all_adducts)
      {
        adduct.formula = modification_mass_detail::adduct_formula(adduct.element);
        adduct.mass_distance = static_cast<float>(modification_mass_detail::adduct_mass(adduct));
      }
    }

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

      for (const ADDUCT &a : all_adducts)
      {
        if (a.polarity == pol)
        {
          out.push_back(a);
        }
      }

      return out;
    }

    // MARK: FRAGMENT_LOSS_SET Implementation
    FRAGMENT_LOSS_SET::FRAGMENT_LOSS_SET()
    {
      for (const auto &expression : default_loss_expressions())
        all_losses.push_back(make_default_loss(expression));
      for (FRAGMENT_LOSS &loss : all_losses)
        loss.mass_loss = static_cast<float>(modification_mass_detail::formula_mass(loss.formula));
    }

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

      std::vector<::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW> chain_sorted;
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

    void CANDIDATE_CHAIN::find_isotopic_candidates(const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &ft,
                                                    const ::streamfind::mass_spec::nta::api::NTA_FEATURES &fts,
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
      const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &mono_ion = chain[0];

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

          if (s < static_cast<int>(combinations.combinations_by_step.size()))
            which_combinations = combinations.combinations_by_step[s];

          const int number_combinations = which_combinations.size();

          // The bounded combination table currently covers isotope steps up
          // to max_number_elements. A larger max_isotopes value is valid and
          // simply has no generated combinations for later steps; do not call
          // min/max_element on an empty step bucket.
          if (number_combinations == 0)
            continue;

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
              const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &candidate = chain[candidate_idx];
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
                double combination_preference = -std::numeric_limits<double>::infinity();

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

                // In a carbon-rich organic envelope, M+2 and later peaks can
                // receive meaningful contributions from multiple light
                // isotopes. Keep a carbon-only explanation eligible when the
                // M+1-derived carbon estimate supports it, rather than letting
                // a single high M+2 peak force a halogen label. The complete
                // envelope score remains responsible for the final confidence.
                const bool carbon_rich = iso_chain.number_carbons >= 40.0;
                const bool carbon_only = isotope_combination_is_carbon_only(combination);
                const double intensity_upper_bound =
                    (carbon_rich && carbon_only && s >= 2)
                        ? std::max(static_cast<double>(max_rel_int * 1.3),
                                   std::min(2.0, static_cast<double>(max_rel_int * 2.0)))
                        : static_cast<double>(max_rel_int * 1.3);

                const double mass_tie_tolerance = std::max(0.0005, mzr * 1.0e-6);
                const double chemical_preference = isotope_combination_chemical_preference(combination);
                const bool mass_is_better = candidate_mass_distance_error < (combination_mass_error - mass_tie_tolerance);
                const bool chemically_preferred_tie =
                    std::abs(candidate_mass_distance_error - combination_mass_error) <= mass_tie_tolerance &&
                    chemical_preference > combination_preference;

                if ((mass_is_better || chemically_preferred_tie) &&
                    candidate_mass_distance_error <= mzr * 1.3 &&
                    rel_int >= min_rel_int * 0.7 &&
                    rel_int <= intensity_upper_bound)
                {
                  if (debug)
                  {
                    DEBUG_LOG("      -> MATCH found! Combination: " << concat_combination << std::endl);
                    DEBUG_LOG("         Mass error: " << candidate_mass_distance_error << " (threshold: " << (mzr * 1.3) << ")" << std::endl);
                    DEBUG_LOG("         Rel intensity: " << rel_int << " (range: " << (min_rel_int * 0.7) << " - " << (max_rel_int * 1.3) << ")" << std::endl);
                  }
                  combination_mass_error = candidate_mass_distance_error;
                  combination_preference = chemical_preference;

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
            ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &temp_candidate = chain[candidate_idx];
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

    void CANDIDATE_CHAIN::find_adduct_candidates(const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &ft,
                            const ::streamfind::mass_spec::nta::api::NTA_FEATURES &fts,
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

    void CANDIDATE_CHAIN::find_fragment_candidates(const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &ft,
                            const ::streamfind::mass_spec::nta::api::NTA_FEATURES &fts,
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

    struct ROOTED_RELATION_SOLUTION
    {
      std::unordered_map<int, ANNOTATION_CANDIDATE> state;
      double score = -std::numeric_limits<double>::infinity();
      std::size_t selected_edges = 0;
      int root_index = -1;
    };

    struct ROOTED_RELATION_ASSIGNMENT
    {
      ROOTED_RELATION_SOLUTION selected;
      ROOTED_RELATION_SOLUTION alternative;
      std::size_t candidate_edge_count = 0;
      std::size_t ambiguous_edge_count = 0;
    };

    double rooted_relation_edge_score(const ANNOTATION_CANDIDATE &candidate,
                                      int root_index)
    {
      double score = candidate.score;
      if (candidate.cat == "adduct")
      {
        // A positive modification attached directly to the molecular root is
        // the preferred orientation when it competes with a reverse loss.
        score += candidate.parent_index == root_index ? 0.08 : -0.10;
        if (candidate.is_dimer)
          score -= 0.06;
      }
      else if (candidate.cat == "loss")
      {
        // Losses remain valid, but require stronger evidence than a direct
        // positive modification and become progressively less preferable in
        // a chain. This is a soft orientation prior, not a hard exclusion.
        score -= candidate.parent_index == root_index ? 0.04 : 0.08;
      }
      return score;
    }

    ROOTED_RELATION_ASSIGNMENT solve_rooted_relation_graph(
        const std::vector<int> &relation_order,
        const std::unordered_map<int, std::vector<ANNOTATION_CANDIDATE>> &candidate_edges,
        const std::unordered_map<int, ANNOTATION_CANDIDATE> &default_state,
        const std::unordered_map<int, double> &root_priors,
        const std::vector<float> &feature_mz)
    {
      ROOTED_RELATION_ASSIGNMENT result;
      for (const auto &[feature_index, candidates] : candidate_edges)
        result.candidate_edge_count += candidates.size();

      const auto better_solution = [&feature_mz](const ROOTED_RELATION_SOLUTION &lhs,
                                                  const ROOTED_RELATION_SOLUTION &rhs) {
        constexpr double epsilon = 1e-12;
        if (lhs.score > rhs.score + epsilon)
          return true;
        if (rhs.score > lhs.score + epsilon)
          return false;
        if (lhs.selected_edges != rhs.selected_edges)
          return lhs.selected_edges > rhs.selected_edges;
        if (lhs.root_index < 0)
          return false;
        if (rhs.root_index < 0)
          return true;
        if (feature_mz[lhs.root_index] != feature_mz[rhs.root_index])
          return feature_mz[lhs.root_index] < feature_mz[rhs.root_index];
        return lhs.root_index < rhs.root_index;
      };

      constexpr std::size_t relation_beam_width = 24;
      for (const int root_index : relation_order)
      {
        std::vector<ROOTED_RELATION_SOLUTION> beam(1);
        beam.front().root_index = root_index;
        const auto root_prior_it = root_priors.find(root_index);
        beam.front().score = root_prior_it == root_priors.end() ? 0.0 : root_prior_it->second;
        beam.front().state = default_state;

        for (const int feature_index : relation_order)
        {
          if (feature_index == root_index)
            continue;

          const auto edge_it = candidate_edges.find(feature_index);
          std::vector<ROOTED_RELATION_SOLUTION> next_beam;
          const std::size_t branch_count = edge_it == candidate_edges.end() ? 1 : edge_it->second.size() + 1;
          next_beam.reserve(beam.size() * branch_count);

          for (const auto &solution : beam)
          {
            // Keep the unmodified explanation. A mass-compatible relation is
            // not authoritative unless it wins in the component objective.
            next_beam.push_back(solution);
            if (edge_it == candidate_edges.end())
              continue;

            for (const auto &candidate : edge_it->second)
            {
              if (!relation_candidate_is_valid_for_root(candidate, solution.state, root_index))
                continue;

              ROOTED_RELATION_SOLUTION branched = solution;
              branched.state[feature_index] = candidate;
              branched.score += rooted_relation_edge_score(candidate, root_index);
              ++branched.selected_edges;
              next_beam.push_back(std::move(branched));
            }
          }

          std::sort(next_beam.begin(), next_beam.end(), better_solution);
          if (next_beam.size() > relation_beam_width)
            next_beam.resize(relation_beam_width);
          beam = std::move(next_beam);
        }

        if (beam.empty())
          continue;
        const ROOTED_RELATION_SOLUTION solution = beam.front();
        if (result.selected.root_index < 0 || better_solution(solution, result.selected))
        {
          if (result.selected.root_index >= 0 &&
              (result.alternative.root_index < 0 || better_solution(result.selected, result.alternative)))
            result.alternative = result.selected;
          result.selected = solution;
        }
        else if (result.alternative.root_index < 0 || better_solution(solution, result.alternative))
        {
          result.alternative = solution;
        }
      }

      if (result.selected.root_index >= 0)
      {
        for (const int feature_index : relation_order)
        {
          const auto edge_it = candidate_edges.find(feature_index);
          if (edge_it == candidate_edges.end())
            continue;

          const auto selected_it = result.selected.state.find(feature_index);
          const bool selected_edge = selected_it != result.selected.state.end() && !selected_it->second.is_default;
          double selected_score = selected_edge ? rooted_relation_edge_score(selected_it->second, result.selected.root_index) : 0.0;
          for (const auto &candidate : edge_it->second)
          {
            if (!relation_candidate_is_valid_for_root(candidate, result.selected.state, result.selected.root_index))
              continue;
            const double score = rooted_relation_edge_score(candidate, result.selected.root_index);
            if ((!selected_edge && score > 0.0) ||
                (selected_edge && !candidate_equals(candidate, selected_it->second) && std::abs(score - selected_score) < 0.05))
            {
              ++result.ambiguous_edge_count;
              break;
            }
          }
        }
      }
      return result;
    }

    // MARK: annotate_components_impl
    void annotate_components_impl(
      ::streamfind::mass_spec::nta::NtaProjectData &nta_data,
        int maxIsotopes,
        int maxCharge,
        int maxGaps,
        float ppm,
        const std::vector<std::string> &isotopeElements,
        const std::vector<std::string> &modifications,
        bool useDefaultModifications,
        const std::string &debugComponent,
        const std::string &debugAnalysis,
        sdk::DebugSession *debug)
    {
      ISOTOPE_SET isotopes;
      const std::vector<std::string> default_elements = {"C:1-80", "N:0-10", "O:0-20", "S:0-4", "Cl:0-6", "Br:0-4"};
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
      std::unordered_set<std::string> selected_modifications;
      std::unordered_map<std::string, std::string> modification_labels;
      for (const auto &raw_modification : modifications)
      {
        const auto modification = lowercase_copy(trim_copy(raw_modification));
        if (modification.empty())
          throw std::invalid_argument("modifications cannot contain empty entries");
        if (!selected_modifications.insert(modification).second)
          throw std::invalid_argument("duplicate annotation modification: " + raw_modification);
        modification_labels.emplace(modification, trim_copy(raw_modification));
      }

      if (!useDefaultModifications)
      {
        std::unordered_set<std::string> built_in_modifications;
        for (const auto &adduct : all_adducts.all_adducts)
          built_in_modifications.insert(lowercase_copy(adduct_modification_token(adduct)));
        for (const auto &loss : all_losses.all_losses)
          built_in_modifications.insert(lowercase_copy(loss_modification_token(loss)));

        for (const auto &modification : selected_modifications)
        {
          if (built_in_modifications.count(modification) > 0)
            continue;
          if (modification.size() < 2 || (modification.front() != '+' && modification.front() != '-') || modification.find('m') != std::string::npos)
            continue;

          const std::string expression = modification_labels.at(modification);
          const std::string formula_expression = expression.substr(1);
          const std::string formula = modification_mass_detail::expand_formula_coefficient(formula_expression);
          if (formula.empty())
            throw std::invalid_argument("annotation modification requires a formula: " + expression);
          modification_mass_detail::formula_mass(formula);

          if (expression.front() == '+')
          {
            ADDUCT custom(formula_expression, 1, "adduct", "[M" + expression + "]+", 0.0f, 1, 1);
            custom.formula = formula;
            custom.mass_distance = static_cast<float>(modification_mass_detail::adduct_mass(custom));
            all_adducts.all_adducts.push_back(std::move(custom));
          }
          else
          {
            FRAGMENT_LOSS custom("custom", formula, 0.0f, 0, expression);
            custom.mass_loss = static_cast<float>(modification_mass_detail::formula_mass(formula));
            all_losses.all_losses.push_back(std::move(custom));
          }
        }
      }

      if (!useDefaultModifications)
      {
        std::unordered_set<std::string> supported_modifications;
        for (const auto &adduct : all_adducts.all_adducts)
          supported_modifications.insert(lowercase_copy(adduct_modification_token(adduct)));
        for (const auto &loss : all_losses.all_losses)
          supported_modifications.insert(lowercase_copy(loss_modification_token(loss)));

        for (const auto &modification : selected_modifications)
        {
          if (supported_modifications.count(modification) == 0)
            throw std::invalid_argument("unknown annotation modification: " + modification);
        }
      }

      for (int a = 0; a < number_analyses; a++)
      {
        ::streamfind::mass_spec::nta::api::NTA_FEATURES &fts = feature_buffers[a];
        const int number_features = fts.size();
        if (number_features == 0)
          continue;

        bool should_debug = (!debugComponent.empty() && !debugAnalysis.empty() && analysis_names[a] == debugAnalysis);

        fts.sort_by_mz();

        std::unordered_map<std::string, std::vector<int>> component_groups;
        for (int f = 0; f < number_features; f++)
        {
          ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW ft = fts.get_feature(f);
          if (!ft.feature_component.empty())
            component_groups[ft.feature_component].push_back(f);
        }

        std::cerr << "Annotating " << component_groups.size() << " components in analysis " << analysis_names[a] << std::endl;

        int total_isotopes_found = 0;
        int total_adducts_found = 0;
        int total_fragments_found = 0;
        int default_adducts_assigned = 0;

        std::vector<std::string> component_ids;
        component_ids.reserve(component_groups.size());
        for (const auto &comp_pair : component_groups)
          component_ids.push_back(comp_pair.first);
        std::sort(component_ids.begin(), component_ids.end());

        for (const auto &component_id : component_ids)
        {
          const std::vector<int> &component_indices = component_groups.at(component_id);
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
            ::streamfind::mass_spec::nta::utils::init_debug_log(
                debug != nullptr ? debug->path().string() : log_filename.str(), header.str(), true);
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
            double total_score = 0.0;
            double envelope_score = 0.0;
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
                assignment.total_score += candidate.score;
                assignment.children.push_back(candidate);
              }

              if (!assignment.children.empty())
              {
                const double child_count = static_cast<double>(assignment.children.size());
                const double mean_score = assignment.total_score / child_count;
                // Reward complete, coherent envelopes while keeping the score
                // bounded as components grow. The child-count term prevents a
                // single high-scoring isotope from outranking a well-supported
                // multi-isotope chain.
                assignment.envelope_score = assignment.total_score +
                  (0.15 * std::log1p(child_count)) + (0.10 * mean_score);
                isotope_assignments.push_back(std::move(assignment));
              }
            }
          }

          std::sort(isotope_assignments.begin(), isotope_assignments.end(), [&fts](const ISOTOPE_CHAIN_ASSIGNMENT &lhs, const ISOTOPE_CHAIN_ASSIGNMENT &rhs) {
            if (lhs.children.size() != rhs.children.size())
              return lhs.children.size() > rhs.children.size();
            if (lhs.envelope_score != rhs.envelope_score)
              return lhs.envelope_score > rhs.envelope_score;
            if (lhs.total_ppm != rhs.total_ppm)
              return lhs.total_ppm < rhs.total_ppm;
            if (lhs.total_rt != rhs.total_rt)
              return lhs.total_rt < rhs.total_rt;
            return fts.mz[lhs.anchor_idx] < fts.mz[rhs.anchor_idx];
          });

          constexpr std::size_t max_isotope_anchor_hypotheses = 64;
          if (isotope_assignments.size() > max_isotope_anchor_hypotheses)
            isotope_assignments.resize(max_isotope_anchor_hypotheses);

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
            auto adducts = all_adducts.adducts(anchor.polarity);
            auto losses = all_losses.losses(anchor.polarity);
            if (!useDefaultModifications)
            {
              adducts.erase(std::remove_if(adducts.begin(), adducts.end(), [&](const ADDUCT &adduct) {
                return selected_modifications.count(lowercase_copy(adduct_modification_token(adduct))) == 0;
              }), adducts.end());
              losses.erase(std::remove_if(losses.begin(), losses.end(), [&](const FRAGMENT_LOSS &loss) {
                return selected_modifications.count(lowercase_copy(loss_modification_token(loss))) == 0;
              }), losses.end());
            }
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
                candidate.is_dimer = adduct.multiplicity > 1;
                candidate.relation_id = adduct_modification_token(adduct);
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
                candidate.type = "M" + loss_modification_token(loss);
                candidate.parent_feature = anchor.feature;
                candidate.element_or_delta = loss_modification_token(loss);
                candidate.feature_index = idx;
                candidate.parent_index = anchor_idx;
                candidate.relation_id = loss_modification_token(loss);
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

          std::vector<int> relation_update_order = non_isotope_indices;
          std::sort(relation_update_order.begin(), relation_update_order.end(), [&fts](int lhs, int rhs) {
            if (fts.mz[lhs] != fts.mz[rhs])
              return fts.mz[lhs] > fts.mz[rhs];
            return lhs < rhs;
          });

          for (auto &[feature_idx, candidates] : relation_candidates)
          {
            std::sort(candidates.begin(), candidates.end(), [](const ANNOTATION_CANDIDATE &lhs, const ANNOTATION_CANDIDATE &rhs) {
              if (candidate_better(lhs, rhs))
                return true;
              if (candidate_better(rhs, lhs))
                return false;
              if (lhs.cat != rhs.cat)
                return lhs.cat < rhs.cat;
          if (lhs.relation_id != rhs.relation_id)
            return lhs.relation_id < rhs.relation_id;
              if (lhs.type != rhs.type)
                return lhs.type < rhs.type;
              if (lhs.element_or_delta != rhs.element_or_delta)
                return lhs.element_or_delta < rhs.element_or_delta;
              return lhs.parent_index < rhs.parent_index;
            });
          }

          if (debug_this_component)
          {
            std::size_t candidate_edge_count = 0;
            for (const auto &[feature_idx, candidates] : relation_candidates)
              candidate_edge_count += candidates.size();

            DEBUG_LOG("\n=== Candidate molecular edges for Component " << component_id
                      << " (" << candidate_edge_count << ") ===" << std::endl);
            for (const int feature_idx : non_isotope_indices)
            {
              const auto candidates_it = relation_candidates.find(feature_idx);
              if (candidates_it == relation_candidates.end())
                continue;

              const auto child = fts.get_feature(feature_idx);
              for (const auto &candidate : candidates_it->second)
              {
                const auto parent = fts.get_feature(candidate.parent_index);
                DEBUG_LOG("  parent=" << parent.feature
                          << " child=" << child.feature
                          << " category=" << candidate.cat
                          << " direction=parent_to_child"
                          << " relation_id=" << candidate.relation_id
                          << " rule=" << candidate.element_or_delta
                          << " type=" << candidate.type
                          << " mass_error_ppm=" << candidate.mass_error_ppm
                          << " rt_error=" << candidate.rt_error
                          << " intensity_ratio=" << candidate.rel_intensity
                          << " score=" << candidate.score << std::endl);
              }
            }
          }

          std::unordered_map<int, double> root_priors;
          const auto root_prior = [&fts, &non_isotope_indices](int root_index) {
            double min_mz = std::numeric_limits<double>::infinity();
            double max_mz = -std::numeric_limits<double>::infinity();
            double max_log_intensity = 0.0;
            for (const int idx : non_isotope_indices)
            {
              min_mz = std::min(min_mz, static_cast<double>(fts.mz[idx]));
              max_mz = std::max(max_mz, static_cast<double>(fts.mz[idx]));
              max_log_intensity = std::max(max_log_intensity, std::log1p(std::max(0.0, static_cast<double>(fts.intensity[idx]))));
            }

            const double mz_span = std::max(1e-9, max_mz - min_mz);
            const double low_mz_support = 1.0 - (static_cast<double>(fts.mz[root_index]) - min_mz) / mz_span;
            const double log_intensity = std::log1p(std::max(0.0, static_cast<double>(fts.intensity[root_index])));
            const double intensity_support = max_log_intensity > 0.0 ? log_intensity / max_log_intensity : 0.0;

            // A root prior is deliberately weak. It breaks otherwise equal
            // interpretations in favor of a lower-m/z, well-supported base
            // feature, but a coherent modification/loss explanation still
            // dominates when its candidate score is materially better.
            return 0.04 * low_mz_support + 0.04 * intensity_support;
          };

          for (const int root_index : relation_update_order)
            root_priors[root_index] = root_prior(root_index);

          const ROOTED_RELATION_ASSIGNMENT relation_assignment = solve_rooted_relation_graph(
              relation_update_order, relation_candidates, final_candidate, root_priors, fts.mz);
          const auto &selected_solution = relation_assignment.selected;
          const auto &alternative_solution = relation_assignment.alternative;

          for (const auto &[feature_idx, candidate] : selected_solution.state)
            final_candidate[feature_idx] = candidate;

          if (debug_this_component && selected_solution.root_index >= 0)
          {
            const auto root = fts.get_feature(selected_solution.root_index);
            DEBUG_LOG("\n=== Selected molecular root: " << root.feature
                      << " mz=" << root.mz
                      << " score=" << selected_solution.score
                      << " edges=" << selected_solution.selected_edges
                      << " candidate_edges=" << relation_assignment.candidate_edge_count
                      << " ambiguous_edges=" << relation_assignment.ambiguous_edge_count
                      << " ===" << std::endl);
            if (alternative_solution.root_index >= 0)
            {
              const auto alternative_root = fts.get_feature(alternative_solution.root_index);
              const double score_gap = selected_solution.score - alternative_solution.score;
              DEBUG_LOG("  Alternative root: " << alternative_root.feature
                        << " mz=" << alternative_root.mz
                        << " score=" << alternative_solution.score
                        << " score_gap=" << score_gap
                        << (score_gap < 0.05 ? " [AMBIGUOUS]" : "") << std::endl);
            }
          }

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

#include "utils/nta.hpp"
namespace streamfind::mass_spec::nta::annotate_components
{
using Json = nlohmann::json;
    Json run(::streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const int max_isotopes = parameters.value("max_isotopes", 8);
        const int max_charge = parameters.value("max_charge", 1);
        const int max_gaps = parameters.value("max_gaps", 1);
        const float ppm = parameters.value("ppm", 10.0);
        std::vector<std::string> isotope_elements;
        const auto isotope_elements_param = parameters.value("isotope_elements", Json::array({Json("C:1-80"), Json("N:0-10"), Json("O:0-20"), Json("S:0-4"), Json("Cl:0-6"), Json("Br:0-4")}));
        for (const auto &v : isotope_elements_param)
        {
            if (!v.is_string())
                throw Error(ErrorCode::InvalidArgument, "isotope_elements entries must be strings");
            isotope_elements.push_back(v.get<std::string>());
        }
        std::vector<std::string> modifications;
        const bool use_default_modifications = !parameters.contains("annotation_modifications");
        if (!use_default_modifications)
        {
            const auto &modifications_param = parameters.at("annotation_modifications");
            if (!modifications_param.is_array())
                throw Error(ErrorCode::InvalidArgument, "annotation_modifications must be an array");
            for (const auto &v : modifications_param)
            {
                if (!v.is_string())
                    throw Error(ErrorCode::InvalidArgument, "annotation_modifications entries must be strings");
                modifications.push_back(v.get<std::string>());
            }
        }
        if (max_isotopes < 1 || max_charge < 1 || max_gaps < 0 || ppm < 0)
            throw Error(ErrorCode::InvalidArgument, "invalid annotation parameters");
        // Annotation is intentionally all-analysis, like find_features.
        // Ignore stale selection values from older workflow revisions.
        auto all_analysis_parameters = parameters;
        all_analysis_parameters.erase("analysis_names");
        auto data = utils::detail::load_analysis_features(access, all_analysis_parameters);
        const auto debug_component = parameters.contains("debug_component") && parameters.at("debug_component").is_string()
            ? parameters.at("debug_component").get<std::string>() : std::string{};
        const auto debug_analysis = parameters.contains("debug_analysis") && parameters.at("debug_analysis").is_string()
            ? parameters.at("debug_analysis").get<std::string>() : std::string{};
        auto debug = sdk::DebugSession::open(
            access.database_path(), "mass_spec.annotate_components", access.operation_instance(),
            sdk::DebugOptions{debug_analysis, 0.0, -1, !debug_component.empty() && !debug_analysis.empty(), false});
        ::streamfind::mass_spec::nta::annotation::annotate_components_impl(
            data, max_isotopes, max_charge, max_gaps, ppm, isotope_elements,
            modifications, use_default_modifications,
            debug_component, debug_analysis, &debug);
        utils::detail::emit_features(access, data);
        return Json{{"status", "finished"}, {"info", "Components annotated."}};
    }
}
