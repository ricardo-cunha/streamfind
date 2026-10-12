#include "operations/nta/nta_suspect_screening.hpp"
// suspect_screening.cpp
// Suspect screening implementations for NTA_DATA

#include "utils/nta.hpp"
#include "streamfind/core/vendors/openbabel.hpp"
#include "operations/base.hpp"
#include "fixedEnvelopes.h"
#include "element_tables.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <limits>

namespace streamfind::mass_spec::nta
{
  namespace suspect_screening
  {
    double ppm_tol(double value, double ppm)
    {
      return std::abs(value) * ppm / 1e6;
    }

    bool within_ppm(double value, double target, double ppm)
    {
      double tol = ppm_tol(target, ppm);
      return std::abs(value - target) <= tol;
    }

    bool within_sec(double value, double target, double sec)
    {
      return std::abs(value - target) <= sec;
    }

    std::string encode_floats(const std::vector<double> &input)
    {
      if (input.empty())
        return "";
      std::vector<float> tmp;
      tmp.reserve(input.size());
      for (double v : input)
        tmp.push_back(static_cast<float>(v));
      std::string enc = ::mass_spec::reader::utils::encode_little_endian_from_float(tmp, 4);
      return ::mass_spec::reader::utils::encode_base64(enc);
    }

    std::vector<float> decode_floats(const std::string &encoded)
    {
      if (encoded.empty())
        return {};
      std::string decoded = ::mass_spec::reader::utils::decode_base64(encoded);
      return ::mass_spec::reader::utils::decode_little_endian_to_float(decoded, 4);
    }

    template <typename T>
    T get_or_default(const std::vector<T> &vec, size_t idx, const T &def);

    struct IsotopeAwareFormula
    {
      std::string natural_formula;
      double fixed_mass_delta = 0.0;
      bool has_fixed_isotopes = false;
    };

    int isotope_table_index(const std::string &symbol, int mass_number)
    {
      for (size_t i = 0; i < IsoSpec::isospec_number_of_isotopic_entries; ++i)
      {
        if (symbol == IsoSpec::elem_table_symbol[i] &&
            static_cast<int>(std::llround(IsoSpec::elem_table_massNo[i])) == mass_number)
          return static_cast<int>(i);
      }
      return -1;
    }

    int most_abundant_isotope_table_index(const std::string &symbol)
    {
      int best = -1;
      for (size_t i = 0; i < IsoSpec::isospec_number_of_isotopic_entries; ++i)
      {
        if (symbol != IsoSpec::elem_table_symbol[i] || IsoSpec::elem_table_Radioactive[i] ||
            IsoSpec::elem_table_probability[i] <= 0.0)
          continue;
        if (best < 0 || IsoSpec::elem_table_probability[i] > IsoSpec::elem_table_probability[best])
          best = static_cast<int>(i);
      }
      return best;
    }

    IsotopeAwareFormula parse_isotope_formula(const std::string &formula)
    {
      IsotopeAwareFormula result;
      std::string normalized;
      for (size_t i = 0; i < formula.size();)
      {
        if (formula[i] == '[')
        {
          const size_t close = formula.find(']', i + 1);
          if (close == std::string::npos)
            return {};
          const std::string isotope_token = formula.substr(i + 1, close - i - 1);
          size_t symbol_start = 0;
          while (symbol_start < isotope_token.size() &&
                 std::isdigit(static_cast<unsigned char>(isotope_token[symbol_start])))
            ++symbol_start;
          if (symbol_start == 0 || symbol_start == isotope_token.size())
            return {};
          const int mass_number = std::stoi(isotope_token.substr(0, symbol_start));
          const std::string symbol = isotope_token.substr(symbol_start);
          const int isotope_index = isotope_table_index(symbol, mass_number);
          const int base_index = most_abundant_isotope_table_index(symbol);
          if (isotope_index < 0 || base_index < 0)
            return {};

          size_t cursor = close + 1;
          const size_t count_start = cursor;
          while (cursor < formula.size() && std::isdigit(static_cast<unsigned char>(formula[cursor])))
            ++cursor;
          const int count = count_start == cursor ? 1 : std::stoi(formula.substr(count_start, cursor - count_start));
          if (count <= 0)
            return {};

          normalized += symbol;
          if (count > 1)
            normalized += std::to_string(count);

          result.fixed_mass_delta += static_cast<double>(count) *
              (IsoSpec::elem_table_mass[isotope_index] - IsoSpec::elem_table_mass[base_index]);
          result.has_fixed_isotopes = true;
          i = cursor;
          continue;
        }

        // Accept the conventional shorthand D/T as labelled hydrogen while
        // retaining the explicit isotope information internally.
        if (formula[i] == 'D' || formula[i] == 'T')
        {
          const int mass_number = formula[i] == 'D' ? 2 : 3;
          const int isotope_index = isotope_table_index("H", mass_number);
          const int base_index = most_abundant_isotope_table_index("H");
          if (isotope_index < 0 || base_index < 0)
            return {};
          ++i;
          const size_t count_start = i;
          while (i < formula.size() && std::isdigit(static_cast<unsigned char>(formula[i])))
            ++i;
          const int count = count_start == i ? 1 : std::stoi(formula.substr(count_start, i - count_start));
          if (count <= 0)
            return {};
          normalized += "H";
          if (count > 1)
            normalized += std::to_string(count);
          result.fixed_mass_delta += static_cast<double>(count) *
              (IsoSpec::elem_table_mass[isotope_index] - IsoSpec::elem_table_mass[base_index]);
          result.has_fixed_isotopes = true;
          continue;
        }

        if (formula[i] < 'A' || formula[i] > 'Z')
          return {};
        normalized.push_back(formula[i++]);
        if (i < formula.size() && formula[i] >= 'a' && formula[i] <= 'z')
          normalized.push_back(formula[i++]);
        const size_t count_start = i;
        while (i < formula.size() && formula[i] >= '0' && formula[i] <= '9')
          normalized.push_back(formula[i++]);
        if (i == count_start)
          normalized.push_back('1');
      }
      result.natural_formula = normalized;
      return result;
    }

    IsotopeMatch matches_isotope_pattern(const SuspectQuery &suspect,
                                 const ::streamfind::mass_spec::nta::api::NTA_FEATURES &features,
                                 size_t feature_index,
                                 double ppm)
    {
      if (suspect.formula.empty())
        return {};

      const auto experimental_mz = decode_floats(get_or_default(features.ms1_mz, feature_index, std::string()));
      const auto experimental_intensity = decode_floats(get_or_default(features.ms1_intensity, feature_index, std::string()));
      if (experimental_mz.empty() || experimental_mz.size() != experimental_intensity.size())
        return {};

      try
      {
        const IsotopeAwareFormula parsed_formula = parse_isotope_formula(suspect.formula);
        if (parsed_formula.natural_formula.empty())
          return {};
        IsoSpec::Iso isotope_model(parsed_formula.natural_formula);
        auto envelope = IsoSpec::FixedEnvelope::FromTotalProb(isotope_model, 0.999, true, true);
        envelope.sort_by_mass();

        constexpr std::size_t max_isotope_peaks = 12;
        constexpr double minimum_probability = 1e-5;
        const double ion_offset = static_cast<double>(features.polarity[feature_index]) * 1.007276466621;
        std::vector<double> theoretical_mz;
        std::vector<double> theoretical_probability;
        for (size_t i = 0; i < envelope.confs_no() && theoretical_mz.size() < max_isotope_peaks; ++i)
        {
          if (envelope.prob(i) < minimum_probability)
            continue;
          theoretical_mz.push_back(envelope.mass(i) + parsed_formula.fixed_mass_delta + ion_offset);
          theoretical_probability.push_back(envelope.prob(i));
        }
        IsotopeMatch result;
        result.theoretical_peaks = static_cast<int>(theoretical_mz.size());
        result.evaluated = true;
        if (theoretical_mz.size() < 2)
          return result;

        std::vector<bool> used(experimental_mz.size(), false);
        std::vector<double> observed(theoretical_mz.size(), 0.0);
        int matched_peaks = 0;
        for (size_t theoretical_index = 0; theoretical_index < theoretical_mz.size(); ++theoretical_index)
        {
          const double tolerance = ppm_tol(theoretical_mz[theoretical_index], ppm);
          int best_index = -1;
          double best_error = std::numeric_limits<double>::max();
          for (size_t experimental_index = 0; experimental_index < experimental_mz.size(); ++experimental_index)
          {
            if (used[experimental_index])
              continue;
            const double error = std::abs(experimental_mz[experimental_index] - theoretical_mz[theoretical_index]);
            if (error <= tolerance && error < best_error)
            {
              best_index = static_cast<int>(experimental_index);
              best_error = error;
            }
          }
          if (best_index >= 0)
          {
            used[static_cast<size_t>(best_index)] = true;
            observed[theoretical_index] = experimental_intensity[static_cast<size_t>(best_index)];
            matched_peaks++;
          }
        }
        result.matched_peaks = matched_peaks;

        const double max_theoretical = *std::max_element(theoretical_probability.begin(), theoretical_probability.end());
        const double max_observed = *std::max_element(observed.begin(), observed.end());
        if (max_theoretical <= 0.0 || max_observed <= 0.0)
          return result;

        double dot = 0.0;
        double theoretical_norm = 0.0;
        double observed_norm = 0.0;
        for (size_t i = 0; i < theoretical_probability.size(); ++i)
        {
          const double expected = theoretical_probability[i] / max_theoretical;
          const double observed_value = observed[i] / max_observed;
          dot += expected * observed_value;
          theoretical_norm += expected * expected;
          observed_norm += observed_value * observed_value;
        }
        const double cosine = dot / std::sqrt(theoretical_norm * observed_norm);
        result.similarity = std::isfinite(cosine) ? cosine : 0.0;
        return result;
      }
      catch (const std::exception &)
      {
        return {};
      }
    }

    std::vector<SuspectQuery> normalize_suspects(const std::vector<SuspectQuery> &suspects)
    {
      if (!streamfind::core::vendors::openbabel::openbabel_available())
        return suspects;

      const auto contains_isotope_label = [](const SuspectQuery &suspect)
      {
        // Open Babel normalizes ordinary structures well, but isotope-labelled
        // standards must retain the mass/formula supplied by the target file.
        // In particular, deuterated CSV targets use [2H] in SMILES/formulas and
        // /i...D... in InChI; normalizing those can discard the label or produce
        // a non-finite derived mass.
        const auto has_bracket_isotope = [](const std::string &value)
        {
          for (size_t i = 0; i + 2 < value.size(); ++i)
          {
            if (value[i] == '[' && std::isdigit(static_cast<unsigned char>(value[i + 1])))
              return true;
          }
          return false;
        };
        return has_bracket_isotope(suspect.formula) ||
               has_bracket_isotope(suspect.SMILES) ||
               suspect.formula.find('D') != std::string::npos ||
               suspect.formula.find('T') != std::string::npos ||
               suspect.InChI.find("/i") != std::string::npos;
      };

      std::vector<SuspectQuery> normalized = suspects;
      for (auto &sus : normalized)
      {
        if ((sus.SMILES.empty() && sus.InChI.empty()) || contains_isotope_label(sus))
        {
          // Keep labelled descriptors authoritative. If the target did not
          // provide a mass, derive it from the natural formula plus the fixed
          // isotope mass deltas instead of asking Open Babel to parse a
          // bracket-isotope formula through its ordinary formula boundary.
          if (!sus.has_mass && !sus.formula.empty())
          {
            const IsotopeAwareFormula parsed_formula = parse_isotope_formula(sus.formula);
            if (!parsed_formula.natural_formula.empty())
            {
              try
              {
                const auto natural_mass = streamfind::core::vendors::openbabel::mass_from_formula(parsed_formula.natural_formula);
                if (natural_mass.ok && std::isfinite(natural_mass.exact_mass))
                {
                  sus.mass = natural_mass.exact_mass + parsed_formula.fixed_mass_delta;
                  sus.has_mass = true;
                }
              }
              catch (const std::exception &)
              {
                // Leave the target mass unset when the natural formula is
                // incomplete; screening can still use the supplied structure.
              }
            }
          }
          continue;
        }

        streamfind::core::vendors::openbabel::NormalizedStructure structure;
        try
        {
          structure = streamfind::core::vendors::openbabel::normalize_structure(sus.SMILES, sus.InChI);
        }
        catch (const std::exception &)
        {
          // A single isotope-labelled target must not abort screening for the
          // remaining targets. Keep its supplied mass/formula/identifiers.
          continue;
        }
        if (!structure.ok)
          continue;

        // Replace user-supplied structure-derived fields with the normalized
        // Open Babel values so mass, formula, identifiers, and logP stay in sync.
        sus.SMILES = structure.canonical_smiles;
        sus.formula = structure.formula;
        sus.InChI = structure.inchi;
        sus.InChIKey = structure.inchikey;
        sus.mass = structure.exact_mass;
        sus.has_mass = true;
        sus.xLogP = structure.xlogp;
        sus.has_xLogP = structure.has_xlogp;
      }
      return normalized;
    }

    template <typename T>
    T get_or_default(const std::vector<T> &vec, size_t idx, const T &def)
    {
      if (idx < vec.size())
        return vec[idx];
      return def;
    }

    api::NTA_INTERNAL_STANDARD_ROW suspect_to_internal_standard(const api::NTA_SUSPECT_ROW &suspect)
    {
      api::NTA_INTERNAL_STANDARD_ROW row;
      row.created_at = suspect.created_at;
      row.analysis = suspect.analysis;
      row.feature = suspect.feature;
      row.feature_group = suspect.feature_group;
      row.candidate_rank = suspect.candidate_rank;
      row.name = suspect.name;
      row.polarity = suspect.polarity;
      row.db_mass = suspect.db_mass;
      row.exp_mass = suspect.exp_mass;
      row.error_mass = suspect.error_mass;
      row.db_rt = suspect.db_rt;
      row.exp_rt = suspect.exp_rt;
      row.error_rt = suspect.error_rt;
      row.intensity = suspect.intensity;
      row.area = suspect.area;
      row.id_level = suspect.id_level;
      row.score = suspect.score;
      row.shared_fragments = suspect.shared_fragments;
      row.cosine_similarity = suspect.cosine_similarity;
      row.formula = suspect.formula;
      row.SMILES = suspect.SMILES;
      row.InChI = suspect.InChI;
      row.InChIKey = suspect.InChIKey;
      row.xLogP = suspect.xLogP;

      row.db_ms2_size = suspect.db_ms2_size;
      row.db_ms2_mz = suspect.db_ms2_mz;
      row.db_ms2_intensity = suspect.db_ms2_intensity;
      row.db_ms2_formula = suspect.db_ms2_formula;
      row.db_ms2_smiles = suspect.db_ms2_smiles;
      row.exp_ms2_size = suspect.exp_ms2_size;
      row.exp_ms2_mz = suspect.exp_ms2_mz;
      row.exp_ms2_intensity = suspect.exp_ms2_intensity;
      row.isotope_theoretical_peaks = suspect.isotope_theoretical_peaks;
      row.isotope_matched_peaks = suspect.isotope_matched_peaks;
      row.isotope_similarity = suspect.isotope_similarity;
      row.isotope_match = suspect.isotope_match;
      return row;
    }

    void screening_impl(
        NtaProjectData &nta_data,
        const std::vector<std::string> &analyses,
        const std::vector<SuspectQuery> &suspects,
        double ppm,
        double sec,
        double ppmMS2,
        double mzrMS2,
        double minCosineSimilarity,
        int minSharedFragments,
        double isotopePpm,
        bool filtered,
        bool write_internal_standards)
    {
      const std::vector<SuspectQuery> normalized_suspects = normalize_suspects(suspects);

      const auto &analysis_names = nta_data.analysis_names();
      const auto &feature_buffers = nta_data.feature_buffers();
      auto &suspect_buffers = nta_data.suspect_buffers();
      auto &internal_standard_buffers = nta_data.internal_standard_buffers();

      for (size_t i = 0; i < analysis_names.size(); ++i)
      {
        suspect_buffers[i] = api::NTA_SUSPECTS();
        internal_standard_buffers[i] = api::NTA_INTERNAL_STANDARDS();
      }

      if (normalized_suspects.empty() || analysis_names.empty())
        return;

      std::unordered_set<std::string> analyses_set;
      if (!analyses.empty())
      {
        analyses_set.insert(analyses.begin(), analyses.end());
      }

      struct FeatureRef
      {
        size_t analysis_idx;
        int feature_idx;
        size_t suspect_idx;
      };

      std::vector<FeatureRef> matched;

      for (size_t a = 0; a < analysis_names.size(); ++a)
      {
        const std::string &analysis = analysis_names[a];
        if (!analyses_set.empty() && analyses_set.find(analysis) == analyses_set.end())
          continue;

        const ::streamfind::mass_spec::nta::api::NTA_FEATURES &fts = feature_buffers[a];
        for (int i = 0; i < fts.size(); ++i)
        {
          if (!filtered && fts.filtered[i])
            continue;

          size_t best_suspect = normalized_suspects.size();
          double best_mass_error = std::numeric_limits<double>::infinity();
          int best_isotope_matches = -1;
          double best_isotope_similarity = -1.0;
          double best_score = -std::numeric_limits<double>::infinity();
          bool best_rt_match = false;
          for (size_t suspect_idx = 0; suspect_idx < normalized_suspects.size(); ++suspect_idx)
          {
            const auto &sus = normalized_suspects[suspect_idx];
            if (!sus.has_mass)
              continue;
            double expected_mz = sus.mass + (static_cast<double>(fts.polarity[i]) * 1.007276);
            if (!within_ppm(fts.mz[i], expected_mz, ppm))
              continue;

            const IsotopeMatch isotope = matches_isotope_pattern(sus, fts, static_cast<size_t>(i), isotopePpm);
            const double mass_error = std::abs(fts.mz[i] - expected_mz) / std::max(std::abs(expected_mz), 1e-12) * 1e6;
            const bool rt_match = sus.has_rt && within_sec(fts.rt[i], sus.rt, sec);
            const bool better = best_suspect == normalized_suspects.size() ||
                                mass_error < best_mass_error ||
                                (mass_error == best_mass_error && rt_match && !best_rt_match) ||
                                (mass_error == best_mass_error && rt_match == best_rt_match && isotope.matched_peaks > best_isotope_matches) ||
                                (mass_error == best_mass_error && rt_match == best_rt_match && isotope.matched_peaks == best_isotope_matches && isotope.similarity > best_isotope_similarity) ||
                                (mass_error == best_mass_error && rt_match == best_rt_match && isotope.matched_peaks == best_isotope_matches && isotope.similarity == best_isotope_similarity && sus.score > best_score) ||
                                (mass_error == best_mass_error && rt_match == best_rt_match && isotope.matched_peaks == best_isotope_matches && isotope.similarity == best_isotope_similarity && sus.score == best_score && sus.name < normalized_suspects[best_suspect].name);
            if (better)
            {
              best_suspect = suspect_idx;
              best_mass_error = mass_error;
              best_rt_match = rt_match;
              best_isotope_matches = isotope.matched_peaks;
              best_isotope_similarity = isotope.similarity;
              best_score = sus.score;
            }
          }

          if (best_suspect != normalized_suspects.size())
            matched.push_back({a, i, best_suspect});
        }
      }

      if (matched.empty())
        return;

      for (const auto &ref : matched)
      {
        const SuspectQuery *sus = &normalized_suspects[ref.suspect_idx];

        const ::streamfind::mass_spec::nta::api::NTA_FEATURES &fts = feature_buffers[ref.analysis_idx];
        const int i = ref.feature_idx;
        const size_t idx = static_cast<size_t>(i);

        api::NTA_SUSPECT_ROW row;
        row.analysis = analysis_names[ref.analysis_idx];
        row.feature = get_or_default(fts.feature, idx, std::string());
        row.feature_group = get_or_default(fts.feature_group, idx, std::string());
        row.candidate_rank = 1;
        row.name = sus->name;
        row.polarity = get_or_default(fts.polarity, idx, 0);
        row.exp_mass = static_cast<double>(get_or_default(fts.mass, idx, 0.0f));
        row.exp_rt = static_cast<double>(get_or_default(fts.rt, idx, 0.0f));
        row.intensity = static_cast<double>(get_or_default(fts.intensity, idx, 0.0f));
        row.area = static_cast<double>(get_or_default(fts.area, idx, 0.0f));
        row.id_level = 4;
        row.shared_fragments = 0;
        row.cosine_similarity = 0.0;
        row.score = sus->score;
        row.formula = sus->formula;
        row.SMILES = sus->SMILES;
        row.InChI = sus->InChI;
        row.InChIKey = sus->InChIKey;
        row.xLogP = sus->has_xLogP ? sus->xLogP : std::numeric_limits<double>::quiet_NaN();


        row.db_mass = std::numeric_limits<double>::quiet_NaN();
        if (sus->has_mass)
        {
          row.db_mass = sus->mass;
        }

        row.error_mass = std::numeric_limits<double>::quiet_NaN();
        if (std::isfinite(row.db_mass) && std::isfinite(row.exp_mass) && row.exp_mass != 0.0)
        {
          double err = ((row.exp_mass - row.db_mass) / row.exp_mass) * 1e6;
          row.error_mass = std::round(err * 10.0) / 10.0;
        }

        row.db_rt = sus->rt;
        row.error_rt = std::numeric_limits<double>::quiet_NaN();
        bool rt_matched = false;
        if (sus->has_rt && std::isfinite(row.exp_rt))
        {
          double err_rt = row.exp_rt - row.db_rt;
          row.error_rt = std::round(err_rt * 10.0) / 10.0;
          rt_matched = within_sec(row.exp_rt, row.db_rt, sec);
        }

        row.db_ms2_size = 0;
        row.db_ms2_mz = "";
        row.db_ms2_intensity = "";
        row.db_ms2_formula = "";
        row.db_ms2_smiles = "";
        row.exp_ms2_size = get_or_default(fts.ms2_size, idx, 0);
        row.exp_ms2_mz = get_or_default(fts.ms2_mz, idx, std::string());
        row.exp_ms2_intensity = get_or_default(fts.ms2_intensity, idx, std::string());
        const IsotopeMatch isotope = matches_isotope_pattern(*sus, fts, idx, isotopePpm);
        row.isotope_theoretical_peaks = isotope.theoretical_peaks;
        row.isotope_matched_peaks = isotope.matched_peaks;
        row.isotope_similarity = isotope.similarity;
        row.isotope_match = isotope.evaluated && isotope.matched_peaks > 0;

        const std::vector<double> *sus_mz = nullptr;
        const std::vector<double> *sus_int = nullptr;
        if (row.polarity > 0)
        {
          sus_mz = &sus->fragments_mz_pos;
          sus_int = &sus->fragments_intensity_pos;
        }
        else if (row.polarity < 0)
        {
          sus_mz = &sus->fragments_mz_neg;
          sus_int = &sus->fragments_intensity_neg;
        }

        const bool can_check_ms2 = (sus_mz && !sus_mz->empty() &&
                                    !row.exp_ms2_mz.empty() &&
                                    !row.exp_ms2_intensity.empty());

        bool ms2_matched = false;
        if (can_check_ms2)
        {
          row.db_ms2_size = static_cast<int>(sus_mz->size());
          row.db_ms2_mz = encode_floats(*sus_mz);
          row.db_ms2_intensity = encode_floats(*sus_int);
          row.db_ms2_formula = "";
          row.db_ms2_smiles = "";

          std::vector<float> exp_mz = decode_floats(row.exp_ms2_mz);
          std::vector<float> exp_int = decode_floats(row.exp_ms2_intensity);
          if (!exp_mz.empty() && exp_mz.size() == exp_int.size())
          {
            std::vector<int> exp_idx(sus_mz->size(), -1);
            for (size_t z = 0; z < sus_mz->size(); ++z)
            {
              double mz = (*sus_mz)[z];
              double tol = mz * ppmMS2 / 1e6;
              if (tol < mzrMS2)
                tol = mzrMS2;
              double mzmin = mz - tol;
              double mzmax = mz + tol;
              int best_idx = -1;
              double best_err = 0.0;
              for (size_t k = 0; k < exp_mz.size(); ++k)
              {
                if (exp_mz[k] < mzmin || exp_mz[k] > mzmax)
                  continue;
                double err = std::abs(exp_mz[k] - mz);
                if (best_idx == -1 || err < best_err)
                {
                  best_idx = static_cast<int>(k);
                  best_err = err;
                }
              }
              exp_idx[z] = best_idx;
            }

            int shared = 0;
            for (int idx_match : exp_idx)
            {
              if (idx_match >= 0)
                shared++;
            }

            row.shared_fragments = shared;
            double cosine = 0.0;
            if (shared > 0)
            {
              std::vector<double> intensity_db;
              std::vector<double> intensity_exp;
              intensity_db.reserve(shared);
              intensity_exp.reserve(shared);
              for (size_t z = 0; z < exp_idx.size(); ++z)
              {
                int idx_match = exp_idx[z];
                if (idx_match < 0)
                  continue;
                intensity_db.push_back(get_or_default(*sus_int, z, 0.0));
                intensity_exp.push_back(exp_int[idx_match]);
              }
              double max_db = *std::max_element(intensity_db.begin(), intensity_db.end());
              double max_exp = *std::max_element(intensity_exp.begin(), intensity_exp.end());
              if (max_db > 0.0 && max_exp > 0.0)
              {
                double dot = 0.0;
                double mag_db = 0.0;
                double mag_exp = 0.0;
                for (size_t k = 0; k < intensity_db.size(); ++k)
                {
                  double dbi = intensity_db[k] / max_db;
                  double exi = intensity_exp[k] / max_exp;
                  dot += dbi * exi;
                  mag_db += dbi * dbi;
                  mag_exp += exi * exi;
                }
                if (mag_db > 0.0 && mag_exp > 0.0)
                {
                  cosine = dot / (std::sqrt(mag_db) * std::sqrt(mag_exp));
                }
              }
            }

            row.cosine_similarity = std::round(cosine * 10000.0) / 10000.0;
            if (row.shared_fragments >= minSharedFragments || row.cosine_similarity >= minCosineSimilarity)
            {
              ms2_matched = true;
            }
          }
        }

        if (rt_matched && ms2_matched)
        {
          row.id_level = 1;
        }
        else if (ms2_matched)
        {
          row.id_level = 2;
        }
        else if (rt_matched)
        {
          row.id_level = 3;
        }
        else
        {
          row.id_level = 4;
        }

        // Internal-standard matches use the standard suspect result contract
        // while retaining the dedicated table needed by correction methods.
        if (!write_internal_standards)
          suspect_buffers[ref.analysis_idx].append(row);
        else
        {
          auto internal_standard = suspect_to_internal_standard(row);
          internal_standard.feature_component = get_or_default(fts.feature_component, idx, std::string());
          internal_standard.adduct = get_or_default(fts.adduct, idx, std::string());
          internal_standard_buffers[ref.analysis_idx].append(internal_standard);
        }
      }
    }

    void suspect_screening_impl(
        NtaProjectData &nta_data,
        const std::vector<std::string> &analyses,
        const std::vector<SuspectQuery> &suspects,
        double ppm,
        double sec,
        double ppmMS2,
        double mzrMS2,
        double minCosineSimilarity,
        int minSharedFragments,
        double isotopePpm,
        bool filtered)
    {
      screening_impl(nta_data, analyses, suspects, ppm, sec, ppmMS2, mzrMS2, minCosineSimilarity, minSharedFragments, isotopePpm, filtered, false);
    }

    void find_internal_standards_impl(
        NtaProjectData &nta_data,
        const std::vector<std::string> &analyses,
        const std::vector<SuspectQuery> &suspects,
        double ppm,
        double sec,
        double ppmMS2,
        double mzrMS2,
        double minCosineSimilarity,
        int minSharedFragments,
        double isotopePpm,
        bool filtered)
    {
      screening_impl(nta_data, analyses, suspects, ppm, sec, ppmMS2, mzrMS2, minCosineSimilarity, minSharedFragments, isotopePpm, filtered, true);
    }

    void convert_suspects_to_internal_standards(NtaProjectData &nta_data)
    {
      auto &internal_buffers = nta_data.internal_standard_buffers();
      const auto &feature_buffers = nta_data.feature_buffers();
      const auto &analysis_names = nta_data.analysis_names();
      for (std::size_t analysis_index = 0; analysis_index < internal_buffers.size(); ++analysis_index)
      {
        internal_buffers[analysis_index] = api::NTA_INTERNAL_STANDARDS();
        if (analysis_index >= nta_data.suspect_buffers().size())
          continue;
        const auto &suspects = nta_data.suspect_buffers()[analysis_index];
        for (int suspect_index = 0; suspect_index < suspects.size(); ++suspect_index)
        {
          const auto suspect = suspects.get_suspect(suspect_index);
          auto internal_standard = suspect_to_internal_standard(suspect);
          internal_standard.analysis = analysis_index < analysis_names.size()
              ? analysis_names[analysis_index] : suspect.analysis;
          if (analysis_index < feature_buffers.size())
          {
            const auto &features = feature_buffers[analysis_index];
            for (int feature_index = 0; feature_index < features.size(); ++feature_index)
            {
              const auto feature = features.get_feature(feature_index);
              if (feature.feature != suspect.feature)
                continue;
              internal_standard.feature_group = feature.feature_group;
              internal_standard.feature_component = feature.feature_component;
              internal_standard.adduct = feature.adduct;
              break;
            }
          }
          internal_buffers[analysis_index].append(internal_standard);
        }
      }
    }
  } // namespace suspect_screening
} // namespace nta

#include "utils/nta.hpp"
namespace streamfind::mass_spec::nta::suspect_screening
{
using Json = nlohmann::json;
    Json run(::streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const double ppm = parameters.value("ppm", 5.0);
        const double sec = parameters.value("sec", 10.0);
        const double ppm_ms2 = parameters.value("ppm_ms2", 10.0);
        const double mzr_ms2 = parameters.value("mzr_ms2", 0.008);
        const double min_cosine_similarity = parameters.value("min_cosine_similarity", 0.7);
        const int min_shared_fragments = parameters.value("min_shared_fragments", 3);
        const double isotope_ppm = parameters.value("isotope_ppm", ppm);
        const bool filtered = parameters.value("filtered", true);
        if (ppm < 0 || sec < 0 || ppm_ms2 < 0 || mzr_ms2 < 0 || min_cosine_similarity < 0 ||
            min_cosine_similarity > 1 || min_shared_fragments < 0 || isotope_ppm < 0)
            throw Error(ErrorCode::InvalidArgument, "invalid suspect screening parameters");
        auto data = utils::detail::load_analysis_features(access, parameters);
        const auto suspects = utils::detail::parse_suspect_targets(access, parameters, false);
        ::streamfind::mass_spec::nta::suspect_screening::suspect_screening_impl(data, data.analysis_names(), suspects,
                                                       ppm, sec, ppm_ms2, mzr_ms2, min_cosine_similarity, min_shared_fragments,
                                                       isotope_ppm, filtered);
        utils::detail::emit_suspects(access, data);
        return Json{{"status", "finished"}, {"info", "Suspect screening completed."}};
    }
}

#include "utils/nta.hpp"
namespace streamfind::mass_spec::nta::find_internal_standards
{
using Json = nlohmann::json;
    Json run(::streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const double ppm = parameters.value("ppm", 5.0);
        const double sec = parameters.value("sec", 10.0);
        const double ppm_ms2 = parameters.value("ppm_ms2", 10.0);
        const double mzr_ms2 = parameters.value("mzr_ms2", 0.008);
        const double min_cosine_similarity = parameters.value("min_cosine_similarity", 0.7);
        const int min_shared_fragments = parameters.value("min_shared_fragments", 3);
        const double isotope_ppm = parameters.value("isotope_ppm", ppm);
        const bool filtered = parameters.value("filtered", true);
        if (ppm < 0 || sec < 0 || ppm_ms2 < 0 || mzr_ms2 < 0 || min_cosine_similarity < 0 ||
            min_cosine_similarity > 1 || min_shared_fragments < 0 || isotope_ppm < 0)
            throw Error(ErrorCode::InvalidArgument, "invalid internal standard parameters");
        auto data = utils::detail::load_analysis_features(access, parameters);
        const auto suspects = utils::detail::parse_suspect_targets(access, parameters, false);
        ::streamfind::mass_spec::nta::suspect_screening::find_internal_standards_impl(data, data.analysis_names(), suspects,
                                                             ppm, sec, ppm_ms2, mzr_ms2, min_cosine_similarity, min_shared_fragments, isotope_ppm, filtered);
        utils::detail::emit_internal_standards(access, data);
        return Json{{"status", "finished"}, {"info", "Internal standards found."}};
    }
}

#include "utils/nta.hpp"
namespace streamfind::mass_spec::nta::enrich_internal_standards
{
using Json = nlohmann::json;

    Json run(::streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        auto data = utils::detail::load_analysis_features(access, parameters);
        utils::detail::load_internal_standards(access, data, parameters);

        auto &standards = data.internal_standard_buffers();
        const auto &features = data.feature_buffers();
        for (std::size_t analysis_index = 0; analysis_index < standards.size(); ++analysis_index)
        {
            auto &standard_buffer = standards[analysis_index];
            const auto &feature_buffer = features[analysis_index];
            for (int standard_index = 0; standard_index < standard_buffer.size(); ++standard_index)
            {
                const std::string &feature_name = standard_buffer.feature[static_cast<std::size_t>(standard_index)];
                for (int feature_index = 0; feature_index < feature_buffer.size(); ++feature_index)
                {
                    if (feature_buffer.feature[static_cast<std::size_t>(feature_index)] != feature_name)
                        continue;
                    const auto index = static_cast<std::size_t>(standard_index);
                    const auto feature = static_cast<std::size_t>(feature_index);
                    standard_buffer.feature_group[index] = feature < feature_buffer.feature_group.size() ? feature_buffer.feature_group[feature] : std::string();
                    standard_buffer.feature_component[index] = feature < feature_buffer.feature_component.size() ? feature_buffer.feature_component[feature] : std::string();
                    standard_buffer.adduct[index] = feature < feature_buffer.adduct.size() ? feature_buffer.adduct[feature] : std::string();
                    break;
                }
            }
        }

        utils::detail::emit_internal_standards(access, data);
        return Json{{"status", "finished"}, {"info", "Internal-standard feature context enriched."}};
    }
}
