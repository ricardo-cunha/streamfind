// suspect_screening.cpp
// Suspect screening implementations for NTA_DATA

#include "methods/nta_suspect_screening.hpp"
#include "utils/openbabel_adapter.hpp"
#include "operations/operations.hpp"
#include "methods/nta_processing_methods.hpp"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <limits>

namespace nta
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
      std::string enc = mass_spec::reader::utils::encode_little_endian_from_float(tmp, 4);
      return mass_spec::reader::utils::encode_base64(enc);
    }

    std::vector<float> decode_floats(const std::string &encoded)
    {
      if (encoded.empty())
        return {};
      std::string decoded = mass_spec::reader::utils::decode_base64(encoded);
      return mass_spec::reader::utils::decode_little_endian_to_float(decoded, 4);
    }

    std::vector<SuspectQuery> normalize_suspects(const std::vector<SuspectQuery> &suspects)
    {
      if (!sf::obabel::openbabel_available())
        return suspects;

      std::vector<SuspectQuery> normalized = suspects;
      for (auto &sus : normalized)
      {
        if (sus.SMILES.empty() && sus.InChI.empty())
          continue;

        const sf::obabel::NormalizedStructure structure =
          sf::obabel::normalize_structure(sus.SMILES, sus.InChI);
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
      row.database_id = suspect.database_id;
      row.db_ms2_size = suspect.db_ms2_size;
      row.db_ms2_mz = suspect.db_ms2_mz;
      row.db_ms2_intensity = suspect.db_ms2_intensity;
      row.db_ms2_formula = suspect.db_ms2_formula;
      row.db_ms2_smiles = suspect.db_ms2_smiles;
      row.exp_ms2_size = suspect.exp_ms2_size;
      row.exp_ms2_mz = suspect.exp_ms2_mz;
      row.exp_ms2_intensity = suspect.exp_ms2_intensity;
      return row;
    }

    void screening_impl(
        PROJECT_NON_TARGET_ANALYSIS &nta_data,
        const std::vector<std::string> &analyses,
        const std::vector<SuspectQuery> &suspects,
        double ppm,
        double sec,
        double ppmMS2,
        double mzrMS2,
        double minCosineSimilarity,
        int minSharedFragments,
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

      bool use_mass = false;
      for (const auto &sus : normalized_suspects)
      {
        if (sus.has_mass)
        {
          use_mass = true;
          break;
        }
      }

      struct FeatureRef
      {
        size_t analysis_idx;
        int feature_idx;
        std::string assigned_name;
      };

      std::vector<FeatureRef> matched;

      for (size_t a = 0; a < analysis_names.size(); ++a)
      {
        const std::string &analysis = analysis_names[a];
        if (!analyses_set.empty() && analyses_set.find(analysis) == analyses_set.end())
          continue;

        const nta::api::NTA_FEATURES &fts = feature_buffers[a];
        for (int i = 0; i < fts.size(); ++i)
        {
          if (!filtered && fts.filtered[i])
            continue;

          std::string assigned;
          for (const auto &sus : normalized_suspects)
          {
            if (use_mass)
            {
              if (!sus.has_mass)
                continue;
              double expected_mz = sus.mass + (static_cast<double>(fts.polarity[i]) * 1.007276);
              if (!within_ppm(fts.mz[i], expected_mz, ppm))
                continue;
            }

            assigned = sus.name;
          }

          if (!assigned.empty())
          {
            matched.push_back({a, i, assigned});
          }
        }
      }

      if (matched.empty())
        return;

      auto find_suspect = [&](const std::string &feature_name) -> const SuspectQuery *
      {
        for (const auto &sus : normalized_suspects)
        {
          if (feature_name.find(sus.name) != std::string::npos)
            return &sus;
        }
        return nullptr;
      };

      for (const auto &ref : matched)
      {
        const SuspectQuery *sus = find_suspect(ref.assigned_name);
        if (!sus)
          continue;

        const nta::api::NTA_FEATURES &fts = feature_buffers[ref.analysis_idx];
        const int i = ref.feature_idx;
        const size_t idx = static_cast<size_t>(i);

        api::NTA_SUSPECT_ROW row;
        row.analysis = analysis_names[ref.analysis_idx];
        row.feature = get_or_default(fts.feature, idx, std::string());
        row.candidate_rank = 1;
        row.name = ref.assigned_name;
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
        row.database_id = sus->database_id;

        row.db_mass = std::numeric_limits<double>::quiet_NaN();
        if (use_mass && sus->has_mass)
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
        if (row.db_rt > 0.0 && std::isfinite(row.exp_rt))
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

        if (write_internal_standards)
        {
          internal_standard_buffers[ref.analysis_idx].append(suspect_to_internal_standard(row));
        }
        else
        {
          suspect_buffers[ref.analysis_idx].append(row);
        }
      }
    }

    void suspect_screening_impl(
        PROJECT_NON_TARGET_ANALYSIS &nta_data,
        const std::vector<std::string> &analyses,
        const std::vector<SuspectQuery> &suspects,
        double ppm,
        double sec,
        double ppmMS2,
        double mzrMS2,
        double minCosineSimilarity,
        int minSharedFragments,
        bool filtered)
    {
      screening_impl(nta_data, analyses, suspects, ppm, sec, ppmMS2, mzrMS2, minCosineSimilarity, minSharedFragments, filtered, false);
    }

    void find_internal_standards_impl(
        PROJECT_NON_TARGET_ANALYSIS &nta_data,
        const std::vector<std::string> &analyses,
        const std::vector<SuspectQuery> &suspects,
        double ppm,
        double sec,
        double ppmMS2,
        double mzrMS2,
        double minCosineSimilarity,
        int minSharedFragments,
        bool filtered)
    {
      screening_impl(nta_data, analyses, suspects, ppm, sec, ppmMS2, mzrMS2, minCosineSimilarity, minSharedFragments, filtered, true);
    }
  } // namespace suspect_screening
} // namespace nta
