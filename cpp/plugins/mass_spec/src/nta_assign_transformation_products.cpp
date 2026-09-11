// nta_assign_transformation_products.cpp
// AssignTransformationProducts algorithm port.
// Ported operation-faithfully from bindings/r/src/core/nta/nta_assign_transformation_products.cpp:
// scoring formulas, tolerances, ranking, and filters are copied verbatim; only the
// plumbing (model structs, JSON marshalling, persistence) is adapted.

#include "streamfind/mass_spec/nta_assign_transformation_products.hpp"
#include "streamfind/mass_spec/nta.hpp"
#include "streamfind/mass_spec/reader.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace nta::assign_transformation_products
{
  struct NORMALIZED_ROW
  {
    nta::api::NTA_TRANSFORMATION_PRODUCT_ROW row;
  };

  struct CANDIDATE
  {
    nta::api::NTA_TRANSFORMATION_PRODUCT_ROW row;
    double score = -std::numeric_limits<double>::infinity();
  };

  std::string trim_copy(const std::string &value)
  {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
  }

  std::string upper_copy(std::string value)
  {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
      return static_cast<char>(std::toupper(c));
    });
    return value;
  }

  std::string collapse_spaces(std::string value)
  {
    std::string out;
    out.reserve(value.size());
    bool prev_space = false;
    for (char ch : value)
    {
      const bool is_space = std::isspace(static_cast<unsigned char>(ch)) != 0;
      if (is_space)
      {
        if (!prev_space)
        {
          out.push_back(' ');
        }
      }
      else
      {
        out.push_back(ch);
      }
      prev_space = is_space;
    }
    return trim_copy(out);
  }

  std::string normalize_structure_key(const std::string &inchikey,
                                      const std::string &inchi,
                                      const std::string &smiles)
  {
    const auto ik = upper_copy(trim_copy(inchikey));
    if (!ik.empty()) return ik;
    const auto ic = trim_copy(inchi);
    if (!ic.empty()) return ic;
    const auto smi = collapse_spaces(trim_copy(smiles));
    if (!smi.empty()) return smi;
    return "";
  }

  std::string normalize_transformation_tag(const std::string &value)
  {
    std::string tag = collapse_spaces(trim_copy(value));
    if (tag.empty()) return "";
    const auto pos = tag.find(':');
    if (pos != std::string::npos)
    {
      tag = trim_copy(tag.substr(pos + 1));
    }
    const std::string suffix = " transformation";
    if (tag.size() > suffix.size())
    {
      const auto lower = upper_copy(tag);
      const auto lower_suffix = upper_copy(suffix);
      if (lower.compare(lower.size() - lower_suffix.size(), lower_suffix.size(), lower_suffix) == 0)
      {
        tag = trim_copy(tag.substr(0, tag.size() - suffix.size()));
      }
    }
    return tag;
  }

  std::vector<double> decode_encoded(const std::string &encoded)
  {
    std::vector<double> out;
    if (encoded.empty()) return out;
    const std::string raw = mass_spec::reader::utils::decode_base64(encoded);
    const auto fv = mass_spec::reader::utils::decode_little_endian_to_float(raw, 4);
    out.reserve(fv.size());
    for (float f : fv) out.push_back(static_cast<double>(f));
    return out;
  }

  double cosine_similarity(const std::vector<double> &mz1, const std::vector<double> &int1,
                           const std::vector<double> &mz2, const std::vector<double> &int2,
                           double tol)
  {
    if (mz1.empty() || mz2.empty()) return std::numeric_limits<double>::quiet_NaN();

    std::vector<double> i1;
    std::vector<double> i2;
    i1.reserve(mz2.size());
    i2.reserve(mz2.size());
    for (std::size_t j = 0; j < mz2.size(); ++j)
    {
      double best = 0.0;
      for (std::size_t k = 0; k < mz1.size(); ++k)
      {
        if (std::abs(mz1[k] - mz2[j]) <= tol && int1[k] > best) best = int1[k];
      }
      i1.push_back(best);
      i2.push_back(int2[j]);
    }

    double dot = 0.0;
    double mag1 = 0.0;
    double mag2 = 0.0;
    for (std::size_t j = 0; j < i1.size(); ++j)
    {
      dot += i1[j] * i2[j];
      mag1 += i1[j] * i1[j];
      mag2 += i2[j] * i2[j];
    }
    if (mag1 <= 0.0 || mag2 <= 0.0) return 0.0;
    return dot / (std::sqrt(mag1) * std::sqrt(mag2));
  }

  double mean_rt(const std::vector<double> &v)
  {
    double sum = 0.0;
    int n = 0;
    for (double value : v)
    {
      if (!std::isnan(value))
      {
        sum += value;
        ++n;
      }
    }
    return n > 0 ? sum / n : std::numeric_limits<double>::quiet_NaN();
  }

  double rt_plausibility(double prod_logp, double prec_logp,
                         double prod_rt, double prec_rt,
                         const std::string &phase)
  {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (std::isnan(prod_logp) || std::isnan(prec_logp) ||
        std::isnan(prod_rt) || std::isnan(prec_rt))
    {
      return nan;
    }

    const double dl = prod_logp - prec_logp;
    const double dr = prod_rt - prec_rt;
    const double sl = (dl > 0.0) ? 1.0 : (dl < 0.0) ? -1.0 : 0.0;
    const double sr = (dr > 0.0) ? 1.0 : (dr < 0.0) ? -1.0 : 0.0;
    if (phase == "reverse_phase") return sl * sr;
    if (phase == "hilic") return -sl * sr;
    return nan;
  }

  double clamp01(double value)
  {
    if (std::isnan(value)) return 0.0;
    return std::max(0.0, std::min(1.0, value));
  }

  double bool_score(bool value)
  {
    return value ? 1.0 : 0.0;
  }

  double formula_mass(const std::string &formula)
  {
    static const std::unordered_map<std::string, double> masses = {
        {"H", 1.00782503223}, {"C", 12.0}, {"N", 14.00307400443}, {"O", 15.99491461957},
        {"F", 18.99840316273}, {"P", 30.97376199842}, {"S", 31.9720711744}, {"CL", 34.968852682},
        {"BR", 78.9183376}, {"I", 126.9044719}};

    double total = 0.0;
    for (std::size_t i = 0; i < formula.size();)
    {
      if (!std::isalpha(static_cast<unsigned char>(formula[i]))) return std::numeric_limits<double>::quiet_NaN();
      std::string symbol(1, static_cast<char>(std::toupper(static_cast<unsigned char>(formula[i]))));
      ++i;
      if (i < formula.size() && std::islower(static_cast<unsigned char>(formula[i])))
      {
        symbol.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(formula[i]))));
        ++i;
      }
      const auto it = masses.find(upper_copy(symbol));
      if (it == masses.end()) return std::numeric_limits<double>::quiet_NaN();
      int count = 0;
      while (i < formula.size() && std::isdigit(static_cast<unsigned char>(formula[i])))
      {
        count = count * 10 + (formula[i] - '0');
        ++i;
      }
      total += it->second * static_cast<double>(count == 0 ? 1 : count);
    }
    return total;
  }

  double expected_transformation_delta(const std::string &tag)
  {
    const std::string key = upper_copy(trim_copy(tag));
    if (key.empty() || key == "MAIN_PRECURSOR") return 0.0;
    static const std::unordered_map<std::string, std::string> known = {
        {"HYDROXYLATION", "+O"},
        {"DIHYDROXYLATION", "+O2"},
        {"OXIDATION", "+O"},
        {"REDUCTION", "+H2"},
        {"METHYLATION", "+CH2"},
        {"DEMETHYLATION", "-CH2"},
        {"ETHYLATION", "+C2H4"},
        {"DEETHYLATION", "-C2H4"},
        {"SULFATION", "+SO3"},
        {"GLUCURONIDATION", "+C6H8O6"},
        {"ACETYLATION", "+C2H2O"},
        {"DEAMINATION", "-NH"},
        {"DEHYDRATION", "-H2O"},
        {"DECHLORINATION", "-Cl"},
        {"DEBROMINATION", "-Br"}};

    std::string expr = key;
    const auto it = known.find(expr);
    if (it != known.end()) expr = upper_copy(it->second);

    double total = 0.0;
    std::size_t pos = 0;
    bool matched = false;
    while (pos < expr.size())
    {
      while (pos < expr.size() && (expr[pos] == ' ' || expr[pos] == ',' || expr[pos] == ';')) ++pos;
      if (pos >= expr.size()) break;
      char sign = expr[pos];
      if (sign != '+' && sign != '-') return std::numeric_limits<double>::quiet_NaN();
      ++pos;
      std::size_t start = pos;
      while (pos < expr.size() && std::isalnum(static_cast<unsigned char>(expr[pos]))) ++pos;
      if (start == pos) return std::numeric_limits<double>::quiet_NaN();
      const double mass = formula_mass(expr.substr(start, pos - start));
      if (std::isnan(mass)) return std::numeric_limits<double>::quiet_NaN();
      total += (sign == '+') ? mass : -mass;
      matched = true;
    }
    return matched ? total : std::numeric_limits<double>::quiet_NaN();
  }

  double combine_score(double cos_sim,
                       double main_cos_sim,
                       double rt_plaus,
                       double main_rt_plaus,
                       bool direct_present,
                       bool main_present,
                       bool main_consistent,
                       bool transformation_valid,
                       double delta_error)
  {
    const double delta_score = std::isnan(delta_error) ? 0.0 : std::max(0.0, 1.0 - (delta_error / 0.02));
    return 5.0 * clamp01(cos_sim) +
           2.5 * clamp01(main_cos_sim) +
           1.0 * std::max(0.0, rt_plaus) +
           0.5 * std::max(0.0, main_rt_plaus) +
           0.75 * bool_score(direct_present) +
           0.5 * bool_score(main_present) +
           1.25 * bool_score(main_consistent) +
           0.5 * bool_score(transformation_valid) +
           1.5 * delta_score;
  }

  NORMALIZED_ROW normalize_row(const nta::api::NTA_TRANSFORMATION_PRODUCT_ROW &input)
  {
    NORMALIZED_ROW out;
    out.row = input;
    out.row.name = trim_copy(input.name);
    out.row.formula = trim_copy(input.formula);
    out.row.SMILES = collapse_spaces(trim_copy(input.SMILES));
    out.row.InChI = trim_copy(input.InChI);
    out.row.InChIKey = upper_copy(trim_copy(input.InChIKey));
    out.row.transformation = normalize_transformation_tag(input.transformation);
    out.row.precursor_name = trim_copy(input.precursor_name);
    out.row.precursor_formula = trim_copy(input.precursor_formula);
    out.row.precursor_SMILES = collapse_spaces(trim_copy(input.precursor_SMILES));
    out.row.precursor_InChI = trim_copy(input.precursor_InChI);
    out.row.precursor_InChIKey = upper_copy(trim_copy(input.precursor_InChIKey));
    out.row.main_precursor_name = trim_copy(input.main_precursor_name);
    out.row.main_precursor_formula = trim_copy(input.main_precursor_formula);
    out.row.main_precursor_SMILES = collapse_spaces(trim_copy(input.main_precursor_SMILES));
    out.row.main_precursor_InChI = trim_copy(input.main_precursor_InChI);
    out.row.main_precursor_InChIKey = upper_copy(trim_copy(input.main_precursor_InChIKey));
    // Structure keys are derived internally from normalized InChIKey/InChI/SMILES.
    out.row.product_structure_key = normalize_structure_key(out.row.InChIKey, out.row.InChI, out.row.SMILES);
    out.row.precursor_structure_key = normalize_structure_key(out.row.precursor_InChIKey, out.row.precursor_InChI, out.row.precursor_SMILES);
    out.row.main_precursor_structure_key = normalize_structure_key(out.row.main_precursor_InChIKey, out.row.main_precursor_InChI, out.row.main_precursor_SMILES);
    return out;
  }

  nta::api::NTA_TRANSFORMATION_PRODUCTS assign_transformation_products_impl(
      const std::vector<nta::api::NTA_SUSPECT_ROW> &suspects,
      const std::vector<nta::api::NTA_TRANSFORMATION_PRODUCT_ROW> &tp_rows,
      const std::string &chromatographic_phase,
      double mzrMS2)
  {
    nta::api::NTA_TRANSFORMATION_PRODUCTS out;
    if (tp_rows.empty()) return out;

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<std::string> empty_fg = {""};

    std::unordered_map<std::string, std::vector<std::string>> fg_map;
    std::unordered_map<std::string, std::vector<std::size_t>> sf_idx;
    for (std::size_t i = 0; i < suspects.size(); ++i)
    {
      const auto &s = suspects[i];
      const auto key = normalize_structure_key(s.InChIKey, s.InChI, s.SMILES);
      if (key.empty() || s.feature_group.empty()) continue;
      auto &groups = fg_map[key];
      if (std::find(groups.begin(), groups.end(), s.feature_group) == groups.end())
      {
        groups.push_back(s.feature_group);
      }
      sf_idx[key + '\0' + s.feature_group].push_back(i);
    }

    for (const auto &raw_tp : tp_rows)
    {
      const auto normalized = normalize_row(raw_tp).row;

      const auto it_prod = fg_map.find(normalized.product_structure_key);
      const auto it_prec = fg_map.find(normalized.precursor_structure_key);
      const auto it_main = fg_map.find(normalized.main_precursor_structure_key);

      const std::vector<std::string> &prod_fgs = (it_prod != fg_map.end()) ? it_prod->second : empty_fg;
      const std::vector<std::string> &prec_fgs = (it_prec != fg_map.end()) ? it_prec->second : empty_fg;
      const std::vector<std::string> &main_fgs = (it_main != fg_map.end()) ? it_main->second : empty_fg;

      for (const auto &prod_fg : prod_fgs)
      {
        CANDIDATE best;
        bool have_best = false;

        for (const auto &prec_fg : prec_fgs)
        {
          for (const auto &main_fg : main_fgs)
          {
            auto candidate = normalized;
            candidate.feature_group = prod_fg;
            candidate.precursor_feature_group = prec_fg;
            candidate.main_precursor_feature_group = main_fg;

            double cos_sim = nan;
            double main_cos_sim = nan;
            double rt_plaus = nan;
            double main_rt_plaus = nan;

            if (!prod_fg.empty() && !prec_fg.empty())
            {
              const auto it_ps = sf_idx.find(candidate.product_structure_key + '\0' + prod_fg);
              const auto it_qs = sf_idx.find(candidate.precursor_structure_key + '\0' + prec_fg);
              if (it_ps != sf_idx.end() && it_qs != sf_idx.end())
              {
                std::vector<double> prod_rts;
                std::vector<double> prec_rts;
                for (std::size_t idx : it_ps->second) prod_rts.push_back(suspects[idx].exp_rt);
                for (std::size_t idx : it_qs->second) prec_rts.push_back(suspects[idx].exp_rt);
                rt_plaus = rt_plausibility(candidate.xLogP, candidate.precursor_xLogP, mean_rt(prod_rts), mean_rt(prec_rts), chromatographic_phase);

                double best_cos = nan;
                for (std::size_t pi : it_ps->second)
                {
                  if (suspects[pi].exp_ms2_size <= 0) continue;
                  const auto mz1 = decode_encoded(suspects[pi].exp_ms2_mz);
                  const auto in1 = decode_encoded(suspects[pi].exp_ms2_intensity);
                  for (std::size_t qi : it_qs->second)
                  {
                    if (suspects[qi].exp_ms2_size <= 0) continue;
                    const auto mz2 = decode_encoded(suspects[qi].exp_ms2_mz);
                    const auto in2 = decode_encoded(suspects[qi].exp_ms2_intensity);
                    const double value = cosine_similarity(mz1, in1, mz2, in2, mzrMS2);
                    if (!std::isnan(value) && (std::isnan(best_cos) || value > best_cos)) best_cos = value;
                  }
                }
                cos_sim = best_cos;
              }
            }

            if (!prod_fg.empty() && !main_fg.empty() && !candidate.main_precursor_structure_key.empty())
            {
              const auto it_ps = sf_idx.find(candidate.product_structure_key + '\0' + prod_fg);
              const auto it_ms = sf_idx.find(candidate.main_precursor_structure_key + '\0' + main_fg);
              if (it_ps != sf_idx.end() && it_ms != sf_idx.end())
              {
                std::vector<double> prod_rts;
                std::vector<double> main_rts;
                for (std::size_t idx : it_ps->second) prod_rts.push_back(suspects[idx].exp_rt);
                for (std::size_t idx : it_ms->second) main_rts.push_back(suspects[idx].exp_rt);
                main_rt_plaus = rt_plausibility(candidate.xLogP, candidate.main_precursor_xLogP, mean_rt(prod_rts), mean_rt(main_rts), chromatographic_phase);

                double best_cos = nan;
                for (std::size_t pi : it_ps->second)
                {
                  if (suspects[pi].exp_ms2_size <= 0) continue;
                  const auto mz1 = decode_encoded(suspects[pi].exp_ms2_mz);
                  const auto in1 = decode_encoded(suspects[pi].exp_ms2_intensity);
                  for (std::size_t mi : it_ms->second)
                  {
                    if (suspects[mi].exp_ms2_size <= 0) continue;
                    const auto mz2 = decode_encoded(suspects[mi].exp_ms2_mz);
                    const auto in2 = decode_encoded(suspects[mi].exp_ms2_intensity);
                    const double value = cosine_similarity(mz1, in1, mz2, in2, mzrMS2);
                    if (!std::isnan(value) && (std::isnan(best_cos) || value > best_cos)) best_cos = value;
                  }
                }
                main_cos_sim = best_cos;
              }
            }

            candidate.cosine_similarity = cos_sim;
            candidate.main_precursor_cosine_similarity = main_cos_sim;
            candidate.rt_plausibility = rt_plaus;
            candidate.main_precursor_rt_plausibility = main_rt_plaus;
            candidate.transformation_mass_delta_observed =
                (std::isnan(candidate.mass) || std::isnan(candidate.precursor_mass))
                    ? nan
                    : candidate.mass - candidate.precursor_mass;
            candidate.transformation_mass_delta_expected = expected_transformation_delta(candidate.transformation);
            candidate.transformation_mass_delta_error =
                (std::isnan(candidate.transformation_mass_delta_expected) || std::isnan(candidate.transformation_mass_delta_observed))
                    ? nan
                    : std::abs(candidate.transformation_mass_delta_observed - candidate.transformation_mass_delta_expected);
            candidate.transformation_valid =
                candidate.transformation == "main_precursor" ||
                (!std::isnan(candidate.transformation_mass_delta_error) && candidate.transformation_mass_delta_error <= 0.01);

            candidate.resolved_direct_parent_feature_group = prec_fg;
            candidate.resolved_main_parent_feature_group = !main_fg.empty() ? main_fg : prec_fg;
            candidate.is_direct_assignment = !prec_fg.empty();
            candidate.is_main_parent_consistent =
                candidate.transformation == "main_precursor" ||
                candidate.main_precursor_structure_key.empty() ||
                main_fg.empty() ||
                (candidate.resolved_main_parent_feature_group == main_fg);

            candidate.network_level = 0;
            if (candidate.transformation != "main_precursor")
            {
              candidate.network_level = (candidate.resolved_main_parent_feature_group == candidate.resolved_direct_parent_feature_group) ? 1 : 2;
            }

            candidate.assignment_score = combine_score(
                candidate.cosine_similarity,
                candidate.main_precursor_cosine_similarity,
                candidate.rt_plausibility,
                candidate.main_precursor_rt_plausibility,
                candidate.is_direct_assignment,
                !candidate.resolved_main_parent_feature_group.empty(),
                candidate.is_main_parent_consistent,
                candidate.transformation_valid,
                candidate.transformation_mass_delta_error);

            CANDIDATE current;
            current.row = candidate;
            current.score = candidate.assignment_score;
            if (!have_best || current.score > best.score)
            {
              best = current;
              have_best = true;
            }
          }
        }

        if (!have_best)
        {
          auto fallback = normalized;
          fallback.feature_group = prod_fg;
          fallback.resolved_main_parent_feature_group = prod_fg;
          fallback.assignment_status = "unresolved";
          fallback.transformation_mass_delta_expected = expected_transformation_delta(fallback.transformation);
          fallback.transformation_mass_delta_observed = nan;
          fallback.transformation_mass_delta_error = nan;
          out.append(fallback);
          continue;
        }

        best.row.assignment_rank = 1;
        best.row.assignment_status = best.row.is_direct_assignment ? "assigned" : "unresolved";
        if (best.row.transformation == "main_precursor")
        {
          best.row.resolved_direct_parent_feature_group = best.row.feature_group;
          best.row.resolved_main_parent_feature_group = best.row.feature_group;
          best.row.is_direct_assignment = true;
          best.row.is_main_parent_consistent = true;
          best.row.transformation_valid = true;
          best.row.network_level = 0;
        }
        out.append(best.row);
      }
    }

    return out;
  }

} // namespace nta::assign_transformation_products