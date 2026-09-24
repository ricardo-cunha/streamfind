#include "utils/nta.hpp"
#include "utils/tools_resolver.hpp"
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <fstream>

#include "readers/reader.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <cmath>
#include <ctime>
#include <limits>
#include <numeric>

namespace streamfind::mass_spec::nta::utils
{
    std::ofstream debug_log;
    void init_debug_log(const std::string &name, const std::string &header)
    {
        debug_log.open(name, std::ios::trunc);
        if (debug_log)
            debug_log << header << '\n';
    }
    void close_debug_log()
    {
        if (debug_log.is_open())
            debug_log.close();
    }
    float mean(const std::vector<float> &v) { return v.empty() ? 0.f : std::accumulate(v.begin(), v.end(), 0.f) / v.size(); }
    float standard_deviation(const std::vector<float> &v, float m)
    {
        if (v.empty())
            return 0.f;
        float s = 0;
        for (float x : v)
            s += (x - m) * (x - m);
        return std::sqrt(s / v.size());
    }
    float quantile(std::vector<float> v, float q)
    {
        if (v.empty())
            return 0;
        q = std::clamp(q, 0.f, 1.f);
        auto i = static_cast<size_t>((v.size() - 1) * q);
        std::nth_element(v.begin(), v.begin() + i, v.end());
        return v[i];
    }
    std::string encode_floats_base64(const std::vector<float> &input, int precision)
    {
        return ::mass_spec::reader::utils::encode_base64(::mass_spec::reader::utils::encode_little_endian_from_float(input, precision));
    }
    std::vector<size_t> get_sort_indices_float(const std::vector<float> &v)
    {
        std::vector<size_t> i(v.size());
        std::iota(i.begin(), i.end(), 0);
        std::sort(i.begin(), i.end(), [&](auto a, auto b)
                  { return v[a] < v[b]; });
        return i;
    }
    static void reorder(std::vector<float> &v, const std::vector<size_t> &i)
    {
        std::vector<float> out;
        out.reserve(i.size());
        for (auto x : i)
            out.push_back(v[x]);
        v = std::move(out);
    }
    static void reorder(std::vector<int> &v, const std::vector<size_t> &i)
    {
        std::vector<int> out;
        out.reserve(i.size());
        for (auto x : i)
            out.push_back(v[x]);
        v = std::move(out);
    }
    void reorder_multiple_vectors(const std::vector<size_t> &i, std::vector<float> &a, std::vector<float> &b, std::vector<float> &c)
    {
        reorder(a, i);
        reorder(b, i);
        reorder(c, i);
    }
    void reorder_multiple_vectors(const std::vector<size_t> &i, std::vector<float> &a, std::vector<float> &b, std::vector<float> &c, std::vector<float> &d)
    {
        reorder(a, i);
        reorder(b, i);
        reorder(c, i);
        reorder(d, i);
    }
    void reorder_multiple_vectors(const std::vector<size_t> &i, std::vector<float> &a, std::vector<float> &b, std::vector<float> &c, std::vector<float> &d, std::vector<int> &e)
    {
        reorder(a, i);
        reorder(b, i);
        reorder(c, i);
        reorder(d, i);
        reorder(e, i);
    }
    std::vector<size_t> filter_above_threshold(const std::vector<float> &v, const std::vector<float> &t)
    {
        std::vector<size_t> out;
        for (size_t i = 0; i < std::min(v.size(), t.size()); ++i)
            if (v[i] > t[i])
                out.push_back(i);
        return out;
    }
    std::vector<int> cluster_by_threshold_float(const std::vector<float> &v, const std::vector<float> &t)
    {
        std::vector<int> out(v.size());
        for (size_t i = 1; i < v.size(); ++i)
            out[i] = out[i - 1] + (v[i] - v[i - 1] > t[std::min(i, t.size() - 1)]);
        return out;
    }
    std::vector<float> calculate_baseline(const std::vector<float> &v, int w)
    {
        std::vector<float> out(v.size());
        for (size_t i = 0; i < v.size(); ++i)
        {
            auto a = i > static_cast<size_t>(w) ? i - w : 0, b = std::min(v.size() - 1, i + static_cast<size_t>(w));
            out[i] = *std::min_element(v.begin() + a, v.begin() + b + 1);
        }
        if (v.size() > 2)
            for (size_t i = 1; i + 1 < v.size(); ++i)
                out[i] = (out[i - 1] + out[i] + out[i + 1]) / 3;
        return out;
    }
    std::vector<float> smooth_intensity_savitzky_golay(const std::vector<float> &v, int window, int)
    {
        std::vector<float> out(v.size());
        auto half = static_cast<size_t>(window / 2);
        for (size_t i = 0; i < v.size(); ++i)
        {
            auto a = i > half ? i - half : 0, b = std::min(v.size() - 1, i + half);
            float s = 0;
            for (auto j = a; j <= b; ++j)
                s += v[j];
            out[i] = s / (b - a + 1);
        }
        return out;
    }
    void calculate_derivatives(const std::vector<float> &v, std::vector<float> &d1, std::vector<float> &d2)
    {
        d1.clear();
        d2.clear();
        for (size_t i = 0; i + 1 < v.size(); ++i)
            d1.push_back(v[i + 1] - v[i]);
        for (size_t i = 0; i + 1 < d1.size(); ++i)
            d2.push_back(d1[i + 1] - d1[i]);
    }
    float gaussian_function_with_baseline(float A, float mu, float sigma, float base, float x) { return base + A * std::exp(-(x - mu) * (x - mu) / (2 * sigma * sigma)); }
    void fit_gaussian(const std::vector<float> &x, const std::vector<float> &y, float &A, float &mu, float &sigma, float &base)
    {
        const float alpha = .01f, beta1 = .9f, beta2 = .999f, epsilon = 1e-8f;
        float mA = 0, vA = 0, mMu = 0, vMu = 0, mS = 0, vS = 0, mB = 0, vB = 0;
        for (int iter = 1; iter <= 500; ++iter)
        {
            float gA = 0, gMu = 0, gS = 0, gB = 0;
            for (size_t i = 0; i < x.size(); ++i)
            {
                float e = std::exp(-(x[i] - mu) * (x[i] - mu) / (2 * sigma * sigma)), err = y[i] - (base + A * e);
                gA += -2 * err * e;
                gMu += -2 * err * A * e * (x[i] - mu) / (sigma * sigma);
                gS += -2 * err * A * e * (x[i] - mu) * (x[i] - mu) / (sigma * sigma * sigma);
                gB += -2 * err;
            }
            auto update = [&](float g, float &m, float &v, float &p)
            {m=beta1*m+(1-beta1)*g;v=beta2*v+(1-beta2)*g*g;float mh=m/(1-std::pow(beta1,iter)),vh=v/(1-std::pow(beta2,iter));p-=alpha*mh/(std::sqrt(vh)+epsilon); };
            update(gA, mA, vA, A);
            A = std::max(.1f, A);
            update(gMu, mMu, vMu, mu);
            update(gS, mS, vS, sigma);
            sigma = std::clamp(sigma, .1f, 100.f);
            update(gB, mB, vB, base);
            base = std::max(0.f, base);
        }
    }
    float calculate_gaussian_rsquared(const std::vector<float> &x, const std::vector<float> &y, float A, float mu, float sigma, float base)
    {
        if (y.empty())
            return 0;
        auto m = mean(y);
        float total = 0, res = 0;
        for (size_t i = 0; i < y.size(); ++i)
        {
            auto p = gaussian_function_with_baseline(A, mu, sigma, base, x[i]);
            res += (y[i] - p) * (y[i] - p);
            total += (y[i] - m) * (y[i] - m);
        }
        return total ? 1 - res / total : 0;
    }
    float calculate_area(const std::vector<float> &x, const std::vector<float> &y)
    {
        float a = 0;
        for (size_t i = 1; i < x.size() && i < y.size(); ++i)
            a += (x[i] - x[i - 1]) * (y[i] + y[i - 1]) / 2;
        return std::max(0.f, a);
    }
    float calculate_jaggedness(const std::vector<float> &v)
    {
        if (v.size() < 3)
            return 0;
        auto m = *std::max_element(v.begin(), v.end());
        if (!m)
            return 0;
        float a = 0;
        for (size_t i = 1; i + 1 < v.size(); ++i)
            a += std::abs(v[i] - (v[i - 1] + v[i + 1]) / 2);
        return a / ((v.size() - 2) * m);
    }
    float calculate_sharpness(const std::vector<float> &x, const std::vector<float> &y, float area)
    {
        if (x.empty() || !area)
            return 0;
        return *std::max_element(y.begin(), y.end()) / ((x.back() - x.front()) * std::sqrt(std::abs(area)));
    }
    float calculate_asymmetry(const std::vector<float> &x, const std::vector<float> &y)
    {
        if (x.size() < 3)
            return 1;
        auto i = std::distance(y.begin(), std::max_element(y.begin(), y.end()));
        auto base = std::min(y.front(), y.back()), level = base + (*std::max_element(y.begin(), y.end()) - base) * .1f;
        size_t l = 0, r = y.size() - 1;
        for (size_t j = i; j > 0; --j)
            if (y[j] <= level)
            {
                l = j;
                break;
            }
        for (size_t j = i; j < y.size(); ++j)
            if (y[j] <= level)
            {
                r = j;
                break;
            }
        return l >= i || r <= i ? 1 : (x[r] - x[i]) / (x[i] - x[l]);
    }
    int calculate_modality(const std::vector<float> &v, float p)
    {
        if (v.size() < 3)
            return 1;
        auto m = *std::max_element(v.begin(), v.end());
        int n = 0;
        for (size_t i = 1; i + 1 < v.size(); ++i)
            if (v[i] > v[i - 1] && v[i] > v[i + 1] && v[i] >= m * p)
                ++n;
        return std::max(1, n);
    }
    float calculate_theoretical_plates(float rt, float width) { return width && rt ? 5.54f * std::pow(rt / width, 2) : 0; }

    std::string utc_now()
    {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        char buffer[20]{};
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &utc);
        return buffer;
    }

    std::string text(const nlohmann::json &row, const char *column)
    {
        const auto it = row.find(column);
        if (it == row.end() || it->is_null()) return {};
        return it->is_string() ? it->get<std::string>() : it->dump();
    }

    int integer(const nlohmann::json &row, const char *column)
    {
        const auto it = row.find(column);
        if (it == row.end() || it->is_null()) return 0;
        if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
        if (it->is_number_integer()) return it->get<int>();
        if (it->is_number()) return static_cast<int>(it->get<double>());
        const auto value = it->get<std::string>();
        return value.empty() ? 0 : std::stoi(value);
    }

    double real(const nlohmann::json &row, const char *column)
    {
        const auto value = text(row, column);
        return value.empty() ? 0.0 : std::stod(value);
    }

    const std::vector<std::string> &feature_columns()
    {
        static const std::vector<std::string> columns = {
            "analysis", "replicate", "blank", "feature", "feature_component", "feature_group", "adduct", "rt", "mz", "mass", "intensity", "noise", "sn", "area", "trace_count",
            "rtmin", "rtmax", "width", "mzmin", "mzmax", "ppm", "fwhm_rt", "fwhm_mz", "gaussian_A", "gaussian_mu", "gaussian_sigma", "gaussian_r2",
            "jaggedness", "sharpness", "asymmetry", "modality", "plates", "polarity", "filtered", "filter", "filled", "correction", "eic_size", "eic_rt",
            "eic_mz", "eic_intensity", "eic_baseline", "eic_smoothed", "ms1_size", "ms1_mz", "ms1_intensity", "ms2_size", "ms2_mz", "ms2_intensity",
            "annotation_category", "annotation_type", "annotation_parent_feature", "annotation_element", "annotation_mass_error_da", "annotation_mass_error_ppm",
            "annotation_rt_error", "annotation_rel_intensity", "annotation_expected_rel_intensity_min", "annotation_expected_rel_intensity_max", "annotation_score",
            "component_size", "component_rt_center", "component_rt_spread", "component_density", "component_mean_correlation", "component_best_partner",
            "component_max_correlation", "component_mean_correlation_to_component", "component_membership_score", "component_is_core", "component_bridge_flag", "created_at"};
        return columns;
    }

    const std::vector<std::string> &feature_types()
    {
        static const std::vector<std::string> types = {
            "string", "string", "string", "string", "string", "string", "string", "real", "real", "real", "real", "real", "real", "real", "integer",
            "real", "real", "real", "real", "real", "real", "real", "real", "real", "real", "real", "real", "real", "real", "real",
            "integer", "real", "integer", "boolean", "string", "boolean", "real", "integer", "string", "string", "string", "string", "string",
            "integer", "string", "string", "integer", "string", "string", "string", "string", "string", "string", "real", "real", "real", "real",
            "real", "real", "real", "real", "real", "real", "real", "real", "string", "real", "real", "real", "boolean", "boolean", "timestamp"};
        return types;
    }

    nlohmann::json feature_row(const NTA_FEATURE_ROW &r)
    {
        return nlohmann::json{{"analysis", r.analysis}, {"replicate", r.replicate}, {"blank", r.blank}, {"feature", r.feature}, {"feature_component", r.feature_component}, {"feature_group", r.feature_group}, {"adduct", r.adduct},
            {"rt", r.rt}, {"mz", r.mz}, {"mass", r.mass}, {"intensity", r.intensity}, {"noise", r.noise}, {"sn", r.sn}, {"area", r.area}, {"trace_count", r.eic_size},
            {"rtmin", r.rtmin}, {"rtmax", r.rtmax}, {"width", r.width}, {"mzmin", r.mzmin}, {"mzmax", r.mzmax}, {"ppm", r.ppm}, {"fwhm_rt", r.fwhm_rt}, {"fwhm_mz", r.fwhm_mz},
            {"gaussian_A", r.gaussian_A}, {"gaussian_mu", r.gaussian_mu}, {"gaussian_sigma", r.gaussian_sigma}, {"gaussian_r2", r.gaussian_r2}, {"jaggedness", r.jaggedness},
            {"sharpness", r.sharpness}, {"asymmetry", r.asymmetry}, {"modality", r.modality}, {"plates", r.plates}, {"polarity", r.polarity}, {"filtered", r.filtered}, {"filter", r.filter},
            {"filled", r.filled}, {"correction", r.correction}, {"eic_size", r.eic_size}, {"eic_rt", r.eic_rt}, {"eic_mz", r.eic_mz}, {"eic_intensity", r.eic_intensity},
            {"eic_baseline", r.eic_baseline}, {"eic_smoothed", r.eic_smoothed}, {"ms1_size", r.ms1_size}, {"ms1_mz", r.ms1_mz}, {"ms1_intensity", r.ms1_intensity},
            {"ms2_size", r.ms2_size}, {"ms2_mz", r.ms2_mz}, {"ms2_intensity", r.ms2_intensity}, {"annotation_category", r.annotation_category}, {"annotation_type", r.annotation_type},
            {"annotation_parent_feature", r.annotation_parent_feature}, {"annotation_element", r.annotation_element}, {"annotation_mass_error_da", r.annotation_mass_error_da},
            {"annotation_mass_error_ppm", r.annotation_mass_error_ppm}, {"annotation_rt_error", r.annotation_rt_error}, {"annotation_rel_intensity", r.annotation_rel_intensity},
            {"annotation_expected_rel_intensity_min", r.annotation_expected_rel_intensity_min}, {"annotation_expected_rel_intensity_max", r.annotation_expected_rel_intensity_max},
            {"annotation_score", r.annotation_score}, {"component_size", r.component_size}, {"component_rt_center", r.component_rt_center}, {"component_rt_spread", r.component_rt_spread},
            {"component_density", r.component_density}, {"component_mean_correlation", r.component_mean_correlation}, {"component_best_partner", r.component_best_partner},
            {"component_max_correlation", r.component_max_correlation}, {"component_mean_correlation_to_component", r.component_mean_correlation_to_component},
            {"component_membership_score", r.component_membership_score}, {"component_is_core", r.component_is_core}, {"component_bridge_flag", r.component_bridge_flag}, {"created_at", utc_now()}};
    }

    const std::vector<std::string> &suspects_columns()
    {
        static const std::vector<std::string> columns = {"analysis", "feature", "feature_group", "candidate_rank", "name", "polarity", "db_mass", "exp_mass", "error_mass", "db_rt", "exp_rt", "error_rt", "intensity", "area", "id_level", "score", "shared_fragments", "cosine_similarity", "formula", "smiles", "inchi", "inchikey", "xlogp", "database_id", "db_ms2_size", "db_ms2_mz", "db_ms2_intensity", "db_ms2_formula", "db_ms2_smiles", "exp_ms2_size", "exp_ms2_mz", "exp_ms2_intensity"};
        return columns;
    }
    const std::vector<std::string> &internal_standards_columns()
    {
        static const std::vector<std::string> columns = {"analysis", "feature", "feature_group", "feature_component", "adduct", "candidate_rank", "name", "polarity", "db_mass", "exp_mass", "error_mass", "db_rt", "exp_rt", "error_rt", "intensity", "area", "id_level", "score", "shared_fragments", "cosine_similarity", "formula", "smiles", "inchi", "inchikey", "xlogp", "database_id", "db_ms2_size", "db_ms2_mz", "db_ms2_intensity", "db_ms2_formula", "db_ms2_smiles", "exp_ms2_size", "exp_ms2_mz", "exp_ms2_intensity"};
        return columns;
    }
    const std::vector<std::string> &transformation_products_columns()
    {
        static const std::vector<std::string> columns = {"analysis", "feature_group", "precursor_feature_group", "main_precursor_feature_group", "assignment_rank", "name", "formula", "mass", "smiles", "inchi", "inchikey", "xlogp", "transformation", "precursor_name", "precursor_formula", "precursor_mass", "precursor_smiles", "precursor_inchi", "precursor_inchikey", "precursor_xlogp", "main_precursor_name", "main_precursor_formula", "main_precursor_mass", "main_precursor_smiles", "main_precursor_inchi", "main_precursor_inchikey", "main_precursor_xlogp", "cosine_similarity", "main_precursor_cosine_similarity", "rt_plausibility", "main_precursor_rt_plausibility", "assignment_score", "network_level", "assignment_status", "created_at"};
        return columns;
    }

}

namespace streamfind::mass_spec::nta::utils::detail
{
        std::string sql(const std::string &value)
        {
            std::string out = "'";
            for (char c : value)
                out += c == '\'' ? "''" : std::string(1, c);
            return out + "'";
        }

        const std::string &input_table(const Json &parameters, const char *port)
        {
            return parameters.at("_inputs").at(port).at("physical_table").get_ref<const std::string &>();
        }

        // Port of merge_NTA_FEATURE_SPECTRA: cluster a per-feature spectrum by m/z,
        // merge cross-scan representatives and apply a presence filter. Matches the
        // former R implementation (bindings/r/src/core/nta/nta.cpp).
        MZ_INTENSITY merge_nta_feature_spectra(const ::mass_spec::spectra::MASS_SPEC_TARGETS_SPECTRA &spectra,
                                               float mzClust, float presence)
        {
            MZ_INTENSITY out;
            const size_t n = spectra.mz.size();
            if (n == 0)
                return out;

            std::vector<size_t> idx(n);
            std::iota(idx.begin(), idx.end(), 0);
            std::sort(idx.begin(), idx.end(), [&](size_t i, size_t j)
                      { return spectra.mz[i] < spectra.mz[j]; });

            std::vector<float> sorted_mz(n), sorted_intensity(n), sorted_rt(n);
            std::vector<float> sorted_pre_ce(n, std::numeric_limits<float>::quiet_NaN());
            for (size_t k = 0; k < n; ++k)
            {
                const size_t src = idx[k];
                sorted_mz[k] = spectra.mz[src];
                sorted_intensity[k] = spectra.intensity[src];
                if (spectra.rt.size() > src)
                    sorted_rt[k] = spectra.rt[src];
                if (spectra.pre_ce.size() > src)
                    sorted_pre_ce[k] = spectra.pre_ce[src];
            }

            size_t total_unique_rt = 0;
            if (!sorted_rt.empty())
            {
                std::vector<float> tmp = sorted_rt;
                std::sort(tmp.begin(), tmp.end());
                total_unique_rt = static_cast<size_t>(std::unique(tmp.begin(), tmp.end()) - tmp.begin());
            }

            std::vector<float> all_finite_pre_ce;
            all_finite_pre_ce.reserve(n);
            for (float v : sorted_pre_ce)
                if (std::isfinite(v))
                    all_finite_pre_ce.push_back(v);
            std::sort(all_finite_pre_ce.begin(), all_finite_pre_ce.end());
            all_finite_pre_ce.erase(std::unique(all_finite_pre_ce.begin(), all_finite_pre_ce.end()), all_finite_pre_ce.end());
            const size_t total_unique_pre_ce = all_finite_pre_ce.size();

            const float mz_tol = std::max(mzClust, 0.0f);
            const float presence_thresh = std::clamp(presence, 0.0f, 1.0f);

            std::vector<float> new_mz, new_intensity;
            new_mz.reserve(n);
            new_intensity.reserve(n);

            size_t start = 0;
            while (start < n)
            {
                size_t end = start + 1;
                while (end < n && (sorted_mz[end] - sorted_mz[end - 1]) <= mz_tol)
                    ++end;

                std::map<float, std::vector<size_t>> rt_groups;
                for (size_t i = start; i < end; ++i)
                    rt_groups[sorted_rt[i]].push_back(i);

                if (rt_groups.size() <= 1)
                {
                    for (size_t i = start; i < end; ++i)
                    {
                        new_mz.push_back(sorted_mz[i]);
                        new_intensity.push_back(sorted_intensity[i]);
                    }
                    start = end;
                    continue;
                }

                std::vector<float> reps_mz, reps_int, reps_pre_ce;
                reps_mz.reserve(rt_groups.size());
                reps_int.reserve(rt_groups.size());
                reps_pre_ce.reserve(rt_groups.size());
                for (auto &[rt_val, indices] : rt_groups)
                {
                    size_t best = indices[0];
                    float best_int = sorted_intensity[best];
                    for (size_t ji : indices)
                        if (sorted_intensity[ji] > best_int)
                        {
                            best = ji;
                            best_int = sorted_intensity[ji];
                        }
                    reps_mz.push_back(sorted_mz[best]);
                    reps_int.push_back(best_int);
                    if (std::isfinite(sorted_pre_ce[best]))
                        reps_pre_ce.push_back(sorted_pre_ce[best]);
                }

                bool pass = true;
                if (presence_thresh > 0.0f && total_unique_rt > 0)
                {
                    float required = presence_thresh * static_cast<float>(total_unique_rt);
                    if (total_unique_pre_ce > 0 && !reps_pre_ce.empty())
                    {
                        std::sort(reps_pre_ce.begin(), reps_pre_ce.end());
                        size_t uniq_ce = static_cast<size_t>(std::unique(reps_pre_ce.begin(), reps_pre_ce.end()) - reps_pre_ce.begin());
                        if (uniq_ce < total_unique_pre_ce)
                            required *= static_cast<float>(uniq_ce) / static_cast<float>(total_unique_pre_ce);
                    }
                    if (static_cast<float>(reps_mz.size()) < required)
                        pass = false;
                }

                if (!pass)
                {
                    start = end;
                    continue;
                }

                size_t best_rep = 0;
                float best_rep_int = reps_int[0];
                for (size_t i = 1; i < reps_mz.size(); ++i)
                    if (reps_int[i] > best_rep_int)
                    {
                        best_rep = i;
                        best_rep_int = reps_int[i];
                    }
                new_mz.push_back(reps_mz[best_rep]);
                new_intensity.push_back(best_rep_int);

                start = end;
            }

            out.mz = std::move(new_mz);
            out.intensity = std::move(new_intensity);
            return out;
        }

        std::string encode_float_array(const std::vector<float> &values)
        {
            return ::mass_spec::reader::utils::encode_base64(
                ::mass_spec::reader::utils::encode_little_endian_from_float(values, 4));
        }

        // Load every persisted feature for the selected analyses from MASS_SPEC_NTA_FEATURES
        // and bundle them per analysis, alongside the analysis names/paths/headers needed to
        // open the raw data through the reader, and the blank/replicate metadata used by the
        // processing algorithms.
        ::streamfind::mass_spec::nta::NtaProjectData load_analysis_features(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
        {
            std::vector<std::string> names, paths, blanks, replicates;
            std::vector<int> indices;
            std::vector<::mass_spec::reader::MASS_SPEC_SPECTRA_HEADERS> headers;
            const auto wanted = parameters.value("analysis_names", Json::array());
            // query_json stringifies every column; parse the analysis index tolerantly.
            auto analysis_index_of = [&](const Json &row)
            {
                auto it = row.find("analysis_index");
                if (it == row.end() || it->is_null())
                    return 0;
                const auto v = it->get<std::string>();
                return v.empty() ? 0 : std::stoi(v);
            };
            const bool has_analysis_input = parameters.contains("_inputs") &&
                parameters.at("_inputs").contains("analysesTable");
            const auto selected_name = [&](const std::string &name, const int index)
            {
                if (wanted.empty()) return true;
                for (const auto &x : wanted)
                {
                    if (x.is_string() && x.get<std::string>() == name) return true;
                    if (x.is_number_integer() && x.get<int>() == index) return true;
                }
                return false;
            };
            if (has_analysis_input)
            {
                for (const auto &row : access.query("SELECT analysis,file_path,analysis_index,blank,replicate FROM " + input_table(parameters, "analysesTable") + " ORDER BY analysis"))
                {
                    const auto name = row.at("analysis").get<std::string>();
                    if (!selected_name(name, analysis_index_of(row))) continue;
                    ::mass_spec::reader::MASS_SPEC_FILE file(row.at("file_path").get<std::string>());
                    file.select_analysis(analysis_index_of(row));
                    names.push_back(name);
                    paths.push_back(row.at("file_path").get<std::string>());
                    indices.push_back(analysis_index_of(row));
                    blanks.push_back(row.value("blank", ""));
                    replicates.push_back(row.value("replicate", ""));
                    headers.push_back(file.get_spectra_headers());
                }
            }
            else
            {
                // Components and annotation consume only ntaFeaturesTable.
                // Derive their analysis context from that bound artifact.
                std::set<std::string> seen_names;
                int feature_analysis_index = 0;
                for (const auto &row : access.query("SELECT analysis FROM " + input_table(parameters, "ntaFeaturesTable") + " ORDER BY analysis"))
                {
                    const auto name = row.at("analysis").get<std::string>();
                    if (!seen_names.insert(name).second) continue;
                    const auto selected = selected_name(name, feature_analysis_index++);
                    if (!selected) continue;
                    names.push_back(name);
                    paths.emplace_back();
                    indices.push_back(0);
                    blanks.emplace_back();
                    replicates.emplace_back();
                }
                headers.resize(names.size());
            }
            ::streamfind::mass_spec::nta::NtaProjectData data(std::move(names), std::move(paths), std::move(headers));
            data.set_analysis_indices(std::move(indices));
            data.set_blank_names(std::move(blanks));
            data.set_replicate_names(std::move(replicates));
            auto &buffers = data.feature_buffers();
            for (size_t i = 0; i < buffers.size(); ++i)
                buffers[i].analysis = data.analysis_names()[i];
            std::vector<std::string> feature_replicates(data.analysis_names().size());
            std::vector<std::string> feature_blanks(data.analysis_names().size());
            // NULL-tolerant row accessors (query_json stringifies all column values).
            auto s = [&](const Json &row, const char *col)
            {
                auto it = row.find(col);
                return (it != row.end() && !it->is_null()) ? it->get<std::string>() : std::string();
            };
            auto d = [&](const Json &row, const char *col)
            { auto v = s(row, col); return v.empty() ? 0.0 : std::stod(v); };
            auto i = [&](const Json &row, const char *col)
            { auto v = s(row, col); return v.empty() ? 0 : std::stoi(v); };
            auto b = [&](const Json &row, const char *col)
            {
                auto v = s(row, col);
                return v == "true" || v == "TRUE" || v == "1";
            };
            for (const auto &row : access.query("SELECT analysis, replicate, blank, feature, feature_component, feature_group, adduct, rt, mz, mass, intensity, noise, sn, area, rtmin, rtmax, width, mzmin, mzmax, ppm, fwhm_rt, fwhm_mz, gaussian_A, gaussian_mu, gaussian_sigma, gaussian_r2, jaggedness, sharpness, asymmetry, modality, plates, polarity, filtered, filter, filled, correction, eic_size, eic_rt, eic_mz, eic_intensity, eic_baseline, eic_smoothed, ms1_size, ms1_mz, ms1_intensity, ms2_size, ms2_mz, ms2_intensity, annotation_category, annotation_type, annotation_parent_feature, annotation_element, annotation_mass_error_da, annotation_mass_error_ppm, annotation_rt_error, annotation_rel_intensity, annotation_expected_rel_intensity_min, annotation_expected_rel_intensity_max, annotation_score, component_size, component_rt_center, component_rt_spread, component_density, component_mean_correlation, component_best_partner, component_max_correlation, component_mean_correlation_to_component, component_membership_score, component_is_core, component_bridge_flag FROM " + input_table(parameters, "ntaFeaturesTable") + " ORDER BY analysis"))
            {
                const auto an = row.at("analysis").get<std::string>();
                const auto it = std::find(data.analysis_names().begin(), data.analysis_names().end(), an);
                if (it == data.analysis_names().end())
                    continue;
                const auto analysis_position = static_cast<size_t>(it - data.analysis_names().begin());
                if (!has_analysis_input)
                {
                    feature_replicates[analysis_position] = s(row, "replicate");
                    feature_blanks[analysis_position] = s(row, "blank");
                }
                ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW r;
                r.analysis = an;
                r.replicate = s(row, "replicate");
                r.blank = s(row, "blank");
                r.feature = s(row, "feature");
                r.feature_component = s(row, "feature_component");
                r.feature_group = s(row, "feature_group");
                r.adduct = s(row, "adduct");
                r.rt = d(row, "rt");
                r.mz = d(row, "mz");
                r.mass = d(row, "mass");
                r.intensity = d(row, "intensity");
                r.noise = d(row, "noise");
                r.sn = d(row, "sn");
                r.area = d(row, "area");
                r.rtmin = d(row, "rtmin");
                r.rtmax = d(row, "rtmax");
                r.width = d(row, "width");
                r.mzmin = d(row, "mzmin");
                r.mzmax = d(row, "mzmax");
                r.ppm = d(row, "ppm");
                r.fwhm_rt = d(row, "fwhm_rt");
                r.fwhm_mz = d(row, "fwhm_mz");
                r.gaussian_A = d(row, "gaussian_A");
                r.gaussian_mu = d(row, "gaussian_mu");
                r.gaussian_sigma = d(row, "gaussian_sigma");
                r.gaussian_r2 = d(row, "gaussian_r2");
                r.jaggedness = d(row, "jaggedness");
                r.sharpness = d(row, "sharpness");
                r.asymmetry = d(row, "asymmetry");
                r.modality = i(row, "modality");
                r.plates = d(row, "plates");
                r.polarity = i(row, "polarity");
                r.filtered = b(row, "filtered");
                r.filter = s(row, "filter");
                r.filled = b(row, "filled");
                r.correction = d(row, "correction");
                r.eic_size = i(row, "eic_size");
                r.eic_rt = s(row, "eic_rt");
                r.eic_mz = s(row, "eic_mz");
                r.eic_intensity = s(row, "eic_intensity");
                r.eic_baseline = s(row, "eic_baseline");
                r.eic_smoothed = s(row, "eic_smoothed");
                r.ms1_size = i(row, "ms1_size");
                r.ms1_mz = s(row, "ms1_mz");
                r.ms1_intensity = s(row, "ms1_intensity");
                r.ms2_size = i(row, "ms2_size");
                r.ms2_mz = s(row, "ms2_mz");
                r.ms2_intensity = s(row, "ms2_intensity");
                r.annotation_category = s(row, "annotation_category");
                r.annotation_type = s(row, "annotation_type");
                r.annotation_parent_feature = s(row, "annotation_parent_feature");
                r.annotation_element = s(row, "annotation_element");
                r.annotation_mass_error_da = d(row, "annotation_mass_error_da");
                r.annotation_mass_error_ppm = d(row, "annotation_mass_error_ppm");
                r.annotation_rt_error = d(row, "annotation_rt_error");
                r.annotation_rel_intensity = d(row, "annotation_rel_intensity");
                r.annotation_expected_rel_intensity_min = d(row, "annotation_expected_rel_intensity_min");
                r.annotation_expected_rel_intensity_max = d(row, "annotation_expected_rel_intensity_max");
                r.annotation_score = d(row, "annotation_score");
                r.component_size = i(row, "component_size");
                r.component_rt_center = d(row, "component_rt_center");
                r.component_rt_spread = d(row, "component_rt_spread");
                r.component_density = d(row, "component_density");
                r.component_mean_correlation = d(row, "component_mean_correlation");
                r.component_best_partner = s(row, "component_best_partner");
                r.component_max_correlation = d(row, "component_max_correlation");
                r.component_mean_correlation_to_component = d(row, "component_mean_correlation_to_component");
                r.component_membership_score = d(row, "component_membership_score");
                r.component_is_core = b(row, "component_is_core");
                r.component_bridge_flag = b(row, "component_bridge_flag");
                buffers[static_cast<size_t>(it - data.analysis_names().begin())].append_feature(r);
            }
            if (!has_analysis_input)
            {
                data.set_replicate_names(std::move(feature_replicates));
                data.set_blank_names(std::move(feature_blanks));
            }
            return data;
        }

        // Map the JSON `targets` array (suspects/internal standards) into SuspectQuery objects.
        std::vector<::streamfind::mass_spec::nta::suspect_screening::SuspectQuery> parse_suspect_targets(const Json &parameters)
        {
            std::vector<::streamfind::mass_spec::nta::suspect_screening::SuspectQuery> out;
            const auto targets = parameters.value("targets", Json::array());
            for (const auto &t : targets)
            {
                ::streamfind::mass_spec::nta::suspect_screening::SuspectQuery q;
                q.name = t.value("id", t.value("name", ""));
                if (t.contains("mass"))
                {
                    q.has_mass = true;
                    q.mass = t.at("mass").get<double>();
                }
                else if (t.contains("mz"))
                {
                    q.has_mass = true;
                    q.mass = t.at("mz").get<double>();
                }
                q.rt = t.value("rt", 0.0);
                q.formula = t.value("formula", "");
                q.SMILES = t.value("SMILES", t.value("smiles", ""));
                q.InChI = t.value("InChI", t.value("inchi", ""));
                q.InChIKey = t.value("InChIKey", t.value("inchikey", ""));
                q.database_id = t.value("database_id", "");
                q.score = t.value("score", 0.0);
                if (t.contains("xLogP"))
                {
                    q.has_xLogP = true;
                    q.xLogP = t.at("xLogP").get<double>();
                }
                // Preserve both modes from the CSV adapter; older callers may provide
                // the compact `fragments_mz`/`fragments_intensity` positive aliases.
                const auto positive_mz = t.value("fragments_mz_positive",
                                                 t.value("fragments_mz_pos", t.value("fragments_mz", Json::array())));
                const auto positive_int = t.value("fragments_intensity_positive",
                                                  t.value("fragments_intensity_pos", t.value("fragments_intensity", Json::array())));
                const auto negative_mz = t.value("fragments_mz_negative",
                                                 t.value("fragments_mz_neg", Json::array()));
                const auto negative_int = t.value("fragments_intensity_negative",
                                                  t.value("fragments_intensity_neg", Json::array()));
                for (const auto &v : positive_mz)
                    q.fragments_mz_pos.push_back(v.get<double>());
                for (const auto &v : positive_int)
                    q.fragments_intensity_pos.push_back(v.get<double>());
                for (const auto &v : negative_mz)
                    q.fragments_mz_neg.push_back(v.get<double>());
                for (const auto &v : negative_int)
                    q.fragments_intensity_neg.push_back(v.get<double>());
                out.push_back(std::move(q));
            }
            return out;
        }

        bool excluded_feature(const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &r, bool filtered)
        {
            return r.filtered && !filtered;
        }

        bool already_had(const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &r, int level)
        {
            if (level == 1)
                return r.ms1_size > 0 && !r.ms1_mz.empty() && !r.ms1_intensity.empty();
            return r.ms2_size > 0 && !r.ms2_mz.empty() && !r.ms2_intensity.empty();
        }

        // Non-finite doubles are stored as SQL NULL so DuckDB accepts them; loading maps
        // NULL back to NaN to preserve the R NA "disabled" semantics of the filter steps.
        // ---------------------------------------------------------------------------
        // Batched Appender persistence helpers.
        //
        // Each row is a vector of optional strings aligned to `*_columns()`. A cell is
        // SQL NULL when `std::nullopt`, otherwise the already-stringified scalar. The
        // core `Project::append_rows` reflects the DuckDB column types and appends each
        // cell with the matching typed duckdb_append_* call, so numbers stay numeric.
        // The column lists omit `created_at`: it is left to the table DEFAULT so the
        // persisted `created_at` stays CURRENT_TIMESTAMP exactly as before.
        // ---------------------------------------------------------------------------
        std::optional<std::string> str_cell(const std::string &v) { return v; }
        std::optional<std::string> inum_cell(int v) { return std::to_string(v); }
        // Feature numeric values remain present, including non-finite values.
        std::optional<std::string> fnum_cell(double v) { return std::to_string(v); }
        // Mirror `dn()` used by suspects/internal standards: non-finite becomes NULL.
        std::optional<std::string> dnum_cell(double v)
        {
            return std::isfinite(v) ? std::optional<std::string>(std::to_string(v)) : std::nullopt;
        }
        std::optional<std::string> bool_cell(bool v) { return v ? "true" : "false"; }

        const std::vector<std::string> &features_columns()
        {
            static const std::vector<std::string> cols = {
                "analysis", "feature", "feature_component", "feature_group", "adduct",
                "rt", "mz", "mass", "intensity", "noise", "sn", "area", "trace_count",
                "rtmin", "rtmax", "width", "mzmin", "mzmax", "ppm", "fwhm_rt", "fwhm_mz",
                "gaussian_A", "gaussian_mu", "gaussian_sigma", "gaussian_r2", "jaggedness", "sharpness", "asymmetry",
                "modality", "plates", "polarity", "filtered", "filter", "filled", "correction",
                "eic_size", "eic_rt", "eic_mz", "eic_intensity", "eic_baseline", "eic_smoothed",
                "ms1_size", "ms1_mz", "ms1_intensity", "ms2_size", "ms2_mz", "ms2_intensity",
                "annotation_category", "annotation_type", "annotation_parent_feature", "annotation_element",
                "annotation_mass_error_da", "annotation_mass_error_ppm", "annotation_rt_error",
                "annotation_rel_intensity", "annotation_expected_rel_intensity_min", "annotation_expected_rel_intensity_max", "annotation_score",
                "component_size", "component_rt_center", "component_rt_spread", "component_density",
                "component_mean_correlation", "component_best_partner", "component_max_correlation",
                "component_mean_correlation_to_component", "component_membership_score", "component_is_core", "component_bridge_flag"};
            return cols;
        }

        std::vector<std::optional<std::string>> feature_cells(const ::streamfind::mass_spec::nta::api::NTA_FEATURE_ROW &r)
        {
            return {
                str_cell(r.analysis), str_cell(r.feature), str_cell(r.feature_component), str_cell(r.feature_group), str_cell(r.adduct),
                fnum_cell(r.rt), fnum_cell(r.mz), fnum_cell(r.mass), fnum_cell(r.intensity), fnum_cell(r.noise), fnum_cell(r.sn), fnum_cell(r.area),
                // Preserve the existing trace_count binding used by the table contract.
                inum_cell(r.eic_size),
                fnum_cell(r.rtmin), fnum_cell(r.rtmax), fnum_cell(r.width), fnum_cell(r.mzmin), fnum_cell(r.mzmax), fnum_cell(r.ppm),
                fnum_cell(r.fwhm_rt), fnum_cell(r.fwhm_mz), fnum_cell(r.gaussian_A), fnum_cell(r.gaussian_mu), fnum_cell(r.gaussian_sigma), fnum_cell(r.gaussian_r2),
                fnum_cell(r.jaggedness), fnum_cell(r.sharpness), fnum_cell(r.asymmetry),
                inum_cell(r.modality), fnum_cell(r.plates), inum_cell(r.polarity),
                bool_cell(r.filtered), str_cell(r.filter), bool_cell(r.filled), fnum_cell(r.correction),
                inum_cell(r.eic_size), str_cell(r.eic_rt), str_cell(r.eic_mz), str_cell(r.eic_intensity), str_cell(r.eic_baseline), str_cell(r.eic_smoothed),
                inum_cell(r.ms1_size), str_cell(r.ms1_mz), str_cell(r.ms1_intensity), inum_cell(r.ms2_size), str_cell(r.ms2_mz), str_cell(r.ms2_intensity),
                str_cell(r.annotation_category), str_cell(r.annotation_type), str_cell(r.annotation_parent_feature), str_cell(r.annotation_element),
                fnum_cell(r.annotation_mass_error_da), fnum_cell(r.annotation_mass_error_ppm), fnum_cell(r.annotation_rt_error),
                fnum_cell(r.annotation_rel_intensity), fnum_cell(r.annotation_expected_rel_intensity_min), fnum_cell(r.annotation_expected_rel_intensity_max), fnum_cell(r.annotation_score),
                inum_cell(r.component_size), fnum_cell(r.component_rt_center), fnum_cell(r.component_rt_spread), fnum_cell(r.component_density),
                fnum_cell(r.component_mean_correlation), str_cell(r.component_best_partner), fnum_cell(r.component_max_correlation),
                fnum_cell(r.component_mean_correlation_to_component), fnum_cell(r.component_membership_score),
                bool_cell(r.component_is_core), bool_cell(r.component_bridge_flag)};
        }

        const std::vector<std::string> &suspects_columns()
        {
            static const std::vector<std::string> cols = {
                "analysis", "feature", "feature_group", "candidate_rank", "name", "polarity",
                "db_mass", "exp_mass", "error_mass", "db_rt", "exp_rt", "error_rt", "intensity", "area",
                "id_level", "score", "shared_fragments", "cosine_similarity", "formula", "SMILES", "InChI", "InChIKey",
                "xLogP", "database_id", "db_ms2_size", "db_ms2_mz", "db_ms2_intensity", "db_ms2_formula", "db_ms2_smiles",
                "exp_ms2_size", "exp_ms2_mz", "exp_ms2_intensity"};
            return cols;
        }

        std::vector<std::optional<std::string>> suspect_cells(const ::streamfind::mass_spec::nta::api::NTA_SUSPECT_ROW &r)
        {
            return {
                str_cell(r.analysis), str_cell(r.feature), str_cell(r.feature_group),
                inum_cell(r.candidate_rank), str_cell(r.name), inum_cell(r.polarity),
                dnum_cell(r.db_mass), dnum_cell(r.exp_mass), dnum_cell(r.error_mass), dnum_cell(r.db_rt), dnum_cell(r.exp_rt), dnum_cell(r.error_rt),
                dnum_cell(r.intensity), dnum_cell(r.area), inum_cell(r.id_level), dnum_cell(r.score), inum_cell(r.shared_fragments), dnum_cell(r.cosine_similarity),
                str_cell(r.formula), str_cell(r.SMILES), str_cell(r.InChI), str_cell(r.InChIKey),
                dnum_cell(r.xLogP), str_cell(r.database_id), inum_cell(r.db_ms2_size), str_cell(r.db_ms2_mz), str_cell(r.db_ms2_intensity),
                str_cell(r.db_ms2_formula), str_cell(r.db_ms2_smiles), inum_cell(r.exp_ms2_size), str_cell(r.exp_ms2_mz), str_cell(r.exp_ms2_intensity)};
        }

        const std::vector<std::string> &internal_standards_columns()
        {
            static const std::vector<std::string> cols = {
                "analysis", "feature", "feature_group", "feature_component", "adduct",
                "candidate_rank", "name", "polarity", "db_mass", "exp_mass", "error_mass", "db_rt", "exp_rt", "error_rt",
                "intensity", "area", "id_level", "score", "shared_fragments", "cosine_similarity",
                "formula", "SMILES", "InChI", "InChIKey", "xLogP", "database_id",
                "db_ms2_size", "db_ms2_mz", "db_ms2_intensity", "db_ms2_formula", "db_ms2_smiles",
                "exp_ms2_size", "exp_ms2_mz", "exp_ms2_intensity"};
            return cols;
        }

        std::vector<std::optional<std::string>> internal_standard_cells(const ::streamfind::mass_spec::nta::api::NTA_INTERNAL_STANDARD_ROW &r)
        {
            return {
                str_cell(r.analysis), str_cell(r.feature), str_cell(r.feature_group), str_cell(r.feature_component), str_cell(r.adduct),
                inum_cell(r.candidate_rank), str_cell(r.name), inum_cell(r.polarity),
                dnum_cell(r.db_mass), dnum_cell(r.exp_mass), dnum_cell(r.error_mass), dnum_cell(r.db_rt), dnum_cell(r.exp_rt), dnum_cell(r.error_rt),
                dnum_cell(r.intensity), dnum_cell(r.area), inum_cell(r.id_level), dnum_cell(r.score), inum_cell(r.shared_fragments), dnum_cell(r.cosine_similarity),
                str_cell(r.formula), str_cell(r.SMILES), str_cell(r.InChI), str_cell(r.InChIKey), dnum_cell(r.xLogP), str_cell(r.database_id),
                inum_cell(r.db_ms2_size), str_cell(r.db_ms2_mz), str_cell(r.db_ms2_intensity), str_cell(r.db_ms2_formula), str_cell(r.db_ms2_smiles),
                inum_cell(r.exp_ms2_size), str_cell(r.exp_ms2_mz), str_cell(r.exp_ms2_intensity)};
        }

        void emit_features(streamfind::sdk::PluginProjectAccess &access, ::streamfind::mass_spec::nta::NtaProjectData &data)
        {
            Json rows = Json::array();
            for (const auto &buffer : data.feature_buffers())
                for (int fi = 0; fi < buffer.size(); ++fi)
                    rows.push_back(::streamfind::mass_spec::nta::utils::feature_row(buffer.get_feature(fi)));
            access.emit_table_rows("ntaFeaturesTable", ::streamfind::mass_spec::nta::utils::feature_columns(),
                                   ::streamfind::mass_spec::nta::utils::feature_types(), rows);
        }

        Json suspect_json(const ::streamfind::mass_spec::nta::api::NTA_SUSPECT_ROW &r)
        {
            return Json{{"analysis",r.analysis},{"feature",r.feature},{"feature_group",r.feature_group},{"candidate_rank",r.candidate_rank},{"name",r.name},{"polarity",r.polarity},{"db_mass",r.db_mass},{"exp_mass",r.exp_mass},{"error_mass",r.error_mass},{"db_rt",r.db_rt},{"exp_rt",r.exp_rt},{"error_rt",r.error_rt},{"intensity",r.intensity},{"area",r.area},{"id_level",r.id_level},{"score",r.score},{"shared_fragments",r.shared_fragments},{"cosine_similarity",r.cosine_similarity},{"formula",r.formula},{"SMILES",r.SMILES},{"InChI",r.InChI},{"InChIKey",r.InChIKey},{"xLogP",r.xLogP},{"database_id",r.database_id},{"db_ms2_size",r.db_ms2_size},{"db_ms2_mz",r.db_ms2_mz},{"db_ms2_intensity",r.db_ms2_intensity},{"db_ms2_formula",r.db_ms2_formula},{"db_ms2_smiles",r.db_ms2_smiles},{"exp_ms2_size",r.exp_ms2_size},{"exp_ms2_mz",r.exp_ms2_mz},{"exp_ms2_intensity",r.exp_ms2_intensity}};
        }

        void emit_suspects(streamfind::sdk::PluginProjectAccess &access, ::streamfind::mass_spec::nta::NtaProjectData &data)
        {
            Json rows = Json::array();
            for (const auto &buffer : data.suspect_buffers()) for (int i=0; i<buffer.size(); ++i) rows.push_back(suspect_json(buffer.get_suspect(i)));
            access.emit_table_rows("suspectsTable", suspects_columns(), {"string","string","string","integer","string","integer","real","real","real","real","real","real","real","real","integer","real","integer","real","string","string","string","string","real","string","integer","string","string","string","string","integer","string","string"}, rows);
        }

        Json internal_standard_json(const ::streamfind::mass_spec::nta::api::NTA_INTERNAL_STANDARD_ROW &r)
        {
            ::streamfind::mass_spec::nta::api::NTA_SUSPECT_ROW s;
            s.analysis=r.analysis; s.feature=r.feature; s.feature_group=r.feature_group; s.candidate_rank=r.candidate_rank; s.name=r.name; s.polarity=r.polarity;
            s.db_mass=r.db_mass; s.exp_mass=r.exp_mass; s.error_mass=r.error_mass; s.db_rt=r.db_rt; s.exp_rt=r.exp_rt; s.error_rt=r.error_rt;
            s.intensity=r.intensity; s.area=r.area; s.id_level=r.id_level; s.score=r.score; s.shared_fragments=r.shared_fragments; s.cosine_similarity=r.cosine_similarity;
            s.formula=r.formula; s.SMILES=r.SMILES; s.InChI=r.InChI; s.InChIKey=r.InChIKey; s.xLogP=r.xLogP; s.database_id=r.database_id;
            s.db_ms2_size=r.db_ms2_size; s.db_ms2_mz=r.db_ms2_mz; s.db_ms2_intensity=r.db_ms2_intensity; s.db_ms2_formula=r.db_ms2_formula; s.db_ms2_smiles=r.db_ms2_smiles;
            s.exp_ms2_size=r.exp_ms2_size; s.exp_ms2_mz=r.exp_ms2_mz; s.exp_ms2_intensity=r.exp_ms2_intensity;
            auto out = suspect_json(s); out["feature_component"] = r.feature_component; out["adduct"] = r.adduct; return out;
        }

        void emit_internal_standards(streamfind::sdk::PluginProjectAccess &access, ::streamfind::mass_spec::nta::NtaProjectData &data)
        {
            Json rows = Json::array();
            for (const auto &buffer : data.internal_standard_buffers()) for (int i=0; i<buffer.size(); ++i) rows.push_back(internal_standard_json(buffer.get_internal_standard(i)));
            access.emit_table_rows("internalStandardsTable", internal_standards_columns(), {"string","string","string","string","string","integer","string","integer","real","real","real","real","real","real","real","real","integer","real","integer","real","string","string","string","string","real","string","integer","string","string","string","string","integer","string","string"}, rows);
        }

        // NULL-tolerant accessors (query_json stringifies all column values).
        static std::string col_s(const Json &row, const char *col)
        {
            auto it = row.find(col);
            return (it != row.end() && !it->is_null()) ? it->get<std::string>() : std::string();
        }
        static double col_d(const Json &row, const char *col)
        {
            auto v = col_s(row, col);
            return v.empty() ? std::numeric_limits<double>::quiet_NaN() : std::stod(v);
        }
        static int col_i(const Json &row, const char *col)
        {
            auto v = col_s(row, col);
            return v.empty() ? 0 : std::stoi(v);
        }

        void load_suspects(streamfind::sdk::PluginProjectAccess &access, ::streamfind::mass_spec::nta::NtaProjectData &data, const Json &parameters)
        {
            auto &buffers = data.suspect_buffers();
            for (auto &b : buffers)
                b = ::streamfind::mass_spec::nta::api::NTA_SUSPECTS();
            for (const auto &row : access.query("SELECT analysis,feature,feature_group,candidate_rank,name,polarity,db_mass,exp_mass,error_mass,db_rt,exp_rt,error_rt,intensity,area,id_level,score,shared_fragments,cosine_similarity,formula,SMILES,InChI,InChIKey,xLogP,database_id,db_ms2_size,db_ms2_mz,db_ms2_intensity,db_ms2_formula,db_ms2_smiles,exp_ms2_size,exp_ms2_mz,exp_ms2_intensity FROM " + input_table(parameters, "suspectsTable") + " ORDER BY analysis"))
            {
                const auto an = row.at("analysis").get<std::string>();
                const auto it = std::find(data.analysis_names().begin(), data.analysis_names().end(), an);
                if (it == data.analysis_names().end())
                    continue;
                ::streamfind::mass_spec::nta::api::NTA_SUSPECT_ROW r;
                r.analysis = an;
                r.feature = col_s(row, "feature");
                r.feature_group = col_s(row, "feature_group");
                r.candidate_rank = col_i(row, "candidate_rank");
                r.name = col_s(row, "name");
                r.polarity = col_i(row, "polarity");
                r.db_mass = col_d(row, "db_mass");
                r.exp_mass = col_d(row, "exp_mass");
                r.error_mass = col_d(row, "error_mass");
                r.db_rt = col_d(row, "db_rt");
                r.exp_rt = col_d(row, "exp_rt");
                r.error_rt = col_d(row, "error_rt");
                r.intensity = col_d(row, "intensity");
                r.area = col_d(row, "area");
                r.id_level = col_i(row, "id_level");
                r.score = col_d(row, "score");
                r.shared_fragments = col_i(row, "shared_fragments");
                r.cosine_similarity = col_d(row, "cosine_similarity");
                r.formula = col_s(row, "formula");
                r.SMILES = col_s(row, "SMILES");
                r.InChI = col_s(row, "InChI");
                r.InChIKey = col_s(row, "InChIKey");
                r.xLogP = col_d(row, "xLogP");
                r.database_id = col_s(row, "database_id");
                r.db_ms2_size = col_i(row, "db_ms2_size");
                r.db_ms2_mz = col_s(row, "db_ms2_mz");
                r.db_ms2_intensity = col_s(row, "db_ms2_intensity");
                r.db_ms2_formula = col_s(row, "db_ms2_formula");
                r.db_ms2_smiles = col_s(row, "db_ms2_smiles");
                r.exp_ms2_size = col_i(row, "exp_ms2_size");
                r.exp_ms2_mz = col_s(row, "exp_ms2_mz");
                r.exp_ms2_intensity = col_s(row, "exp_ms2_intensity");
                buffers[static_cast<size_t>(it - data.analysis_names().begin())].append(r);
            }
        }

        void load_internal_standards(streamfind::sdk::PluginProjectAccess &access, ::streamfind::mass_spec::nta::NtaProjectData &data, const Json &parameters)
        {
            auto &buffers = data.internal_standard_buffers();
            for (auto &b : buffers)
                b = ::streamfind::mass_spec::nta::api::NTA_INTERNAL_STANDARDS();
            for (const auto &row : access.query("SELECT analysis,feature,feature_group,feature_component,adduct,candidate_rank,name,polarity,db_mass,exp_mass,error_mass,db_rt,exp_rt,error_rt,intensity,area,id_level,score,shared_fragments,cosine_similarity,formula,SMILES,InChI,InChIKey,xLogP,database_id,db_ms2_size,db_ms2_mz,db_ms2_intensity,db_ms2_formula,db_ms2_smiles,exp_ms2_size,exp_ms2_mz,exp_ms2_intensity FROM " + input_table(parameters, "internalStandardsTable") + " ORDER BY analysis"))
            {
                const auto an = row.at("analysis").get<std::string>();
                const auto it = std::find(data.analysis_names().begin(), data.analysis_names().end(), an);
                if (it == data.analysis_names().end())
                    continue;
                ::streamfind::mass_spec::nta::api::NTA_INTERNAL_STANDARD_ROW r;
                r.analysis = an;
                r.feature = col_s(row, "feature");
                r.feature_group = col_s(row, "feature_group");
                r.feature_component = col_s(row, "feature_component");
                r.adduct = col_s(row, "adduct");
                r.candidate_rank = col_i(row, "candidate_rank");
                r.name = col_s(row, "name");
                r.polarity = col_i(row, "polarity");
                r.db_mass = col_d(row, "db_mass");
                r.exp_mass = col_d(row, "exp_mass");
                r.error_mass = col_d(row, "error_mass");
                r.db_rt = col_d(row, "db_rt");
                r.exp_rt = col_d(row, "exp_rt");
                r.error_rt = col_d(row, "error_rt");
                r.intensity = col_d(row, "intensity");
                r.area = col_d(row, "area");
                r.id_level = col_i(row, "id_level");
                r.score = col_d(row, "score");
                r.shared_fragments = col_i(row, "shared_fragments");
                r.cosine_similarity = col_d(row, "cosine_similarity");
                r.formula = col_s(row, "formula");
                r.SMILES = col_s(row, "SMILES");
                r.InChI = col_s(row, "InChI");
                r.InChIKey = col_s(row, "InChIKey");
                r.xLogP = col_d(row, "xLogP");
                r.database_id = col_s(row, "database_id");
                r.db_ms2_size = col_i(row, "db_ms2_size");
                r.db_ms2_mz = col_s(row, "db_ms2_mz");
                r.db_ms2_intensity = col_s(row, "db_ms2_intensity");
                r.db_ms2_formula = col_s(row, "db_ms2_formula");
                r.db_ms2_smiles = col_s(row, "db_ms2_smiles");
                r.exp_ms2_size = col_i(row, "exp_ms2_size");
                r.exp_ms2_mz = col_s(row, "exp_ms2_mz");
                r.exp_ms2_intensity = col_s(row, "exp_ms2_intensity");
                buffers[static_cast<size_t>(it - data.analysis_names().begin())].append(r);
            }
        }

        // Map the JSON `transformation_products` parameter (R data.frame columns:
        // name, transformation, precursor_*/main_precursor_* plus optional product
        // formula/mass/SMILES/InChI/InChIKey/xLogP) into model rows. Mirrors the R
        // wrapper's required-column checks (16 mandatory columns, at least one product
        // structure identifier); absent numbers become NaN like R NA values.
        std::vector<::streamfind::mass_spec::nta::api::NTA_TRANSFORMATION_PRODUCT_ROW> parse_transformation_products(const Json &parameters)
        {
            std::vector<::streamfind::mass_spec::nta::api::NTA_TRANSFORMATION_PRODUCT_ROW> out;
            const auto rows = parameters.value("transformation_products", Json::array());
            if (!rows.is_array())
                throw Error(ErrorCode::InvalidArgument, "transformation_products must be an array");
            if (rows.empty())
                return out;
            static const char *required_cols[] = {
                "name", "transformation",
                "precursor_name", "precursor_formula", "precursor_mass",
                "precursor_SMILES", "precursor_InChI", "precursor_InChIKey", "precursor_xLogP",
                "main_precursor_name", "main_precursor_formula", "main_precursor_mass",
                "main_precursor_SMILES", "main_precursor_InChI", "main_precursor_InChIKey", "main_precursor_xLogP"};
            const double nan = std::numeric_limits<double>::quiet_NaN();
            auto str = [](const Json &o, const char *key)
            {
                auto it = o.find(key);
                return (it != o.end() && !it->is_null()) ? it->get<std::string>() : std::string();
            };
            auto num = [&](const Json &o, const char *key)
            {
                auto it = o.find(key);
                return (it != o.end() && !it->is_null()) ? it->get<double>() : nan;
            };
            for (const auto &t : rows)
            {
                if (!t.is_object())
                    throw Error(ErrorCode::InvalidArgument, "transformation_products entries must be objects");
                for (const char *col : required_cols)
                {
                    if (!t.contains(col))
                        throw Error(ErrorCode::InvalidArgument, std::string("transformation_products rows require column '") + col + "'");
                }
                const bool has_structure = t.contains("SMILES") || t.contains("InChI") || t.contains("InChIKey");
                if (!has_structure)
                    throw Error(ErrorCode::InvalidArgument, "transformation_products rows require at least one of SMILES, InChI, or InChIKey");
                ::streamfind::mass_spec::nta::api::NTA_TRANSFORMATION_PRODUCT_ROW r;
                r.name = str(t, "name");
                r.formula = str(t, "formula");
                r.mass = num(t, "mass");
                r.SMILES = str(t, "SMILES");
                r.InChI = str(t, "InChI");
                r.InChIKey = str(t, "InChIKey");
                r.xLogP = num(t, "xLogP");
                r.transformation = str(t, "transformation");
                r.precursor_name = str(t, "precursor_name");
                r.precursor_formula = str(t, "precursor_formula");
                r.precursor_mass = num(t, "precursor_mass");
                r.precursor_SMILES = str(t, "precursor_SMILES");
                r.precursor_InChI = str(t, "precursor_InChI");
                r.precursor_InChIKey = str(t, "precursor_InChIKey");
                r.precursor_xLogP = num(t, "precursor_xLogP");
                r.main_precursor_name = str(t, "main_precursor_name");
                r.main_precursor_formula = str(t, "main_precursor_formula");
                r.main_precursor_mass = num(t, "main_precursor_mass");
                r.main_precursor_SMILES = str(t, "main_precursor_SMILES");
                r.main_precursor_InChI = str(t, "main_precursor_InChI");
                r.main_precursor_InChIKey = str(t, "main_precursor_InChIKey");
                r.main_precursor_xLogP = num(t, "main_precursor_xLogP");
                out.push_back(std::move(r));
            }
            return out;
        }

        // Resolve the per-row analysis index for transformation-product output rows:
        // the analysis whose suspect buffer contains the resolved product feature
        // group (falling back to the resolved parent groups, then to the buffer
        // holding the most suspects). Shared by the suspects append and the
        // transformation-products persistence so both paths agree on the analysis.
        std::vector<int> transformation_product_analysis_indices(::streamfind::mass_spec::nta::NtaProjectData &data,
                                                                 const ::streamfind::mass_spec::nta::api::NTA_TRANSFORMATION_PRODUCTS &products)
        {
            auto &buffers = data.suspect_buffers();
            std::vector<int> out;
            out.reserve(static_cast<size_t>(products.size()));
            auto analysis_of_group = [&](const std::string &fg) -> int
            {
                if (fg.empty())
                    return -1;
                for (size_t a = 0; a < buffers.size(); ++a)
                    for (int i = 0; i < buffers[a].size(); ++i)
                        if (buffers[a].get_suspect(i).feature_group == fg)
                            return static_cast<int>(a);
                return -1;
            };
            int fallback_analysis = 0;
            int fallback_count = -1;
            for (size_t a = 0; a < buffers.size(); ++a)
                if (buffers[a].size() > fallback_count)
                {
                    fallback_count = buffers[a].size();
                    fallback_analysis = static_cast<int>(a);
                }
            for (int i = 0; i < products.size(); ++i)
            {
                const auto row = products.get_transformation_product(i);
                int a = analysis_of_group(row.feature_group);
                if (a < 0)
                    a = analysis_of_group(row.resolved_direct_parent_feature_group);
                if (a < 0)
                    a = analysis_of_group(row.resolved_main_parent_feature_group);
                if (a < 0)
                    a = fallback_analysis;
                out.push_back(a);
            }
            return out;
        }

        // Append assign_transformation_products output rows to the per-analysis suspect
        // buffers. Each output row is placed in the analysis whose suspect buffer
        // contains the resolved product feature group (falling back to the resolved
        // parent groups, then to the buffer holding the most suspects). Context fields
        // (polarity, RT, intensity, experimental MS2) are carried over from a
        // representative suspect of the target analysis when one exists. The `feature`
        // cell is synthesized per row so the SUSPECTS primary key
        // (analysis, feature) stays unique.
        void append_transformation_products_to_suspects(::streamfind::mass_spec::nta::NtaProjectData &data,
                                                        const ::streamfind::mass_spec::nta::api::NTA_TRANSFORMATION_PRODUCTS &products)
        {
            auto &buffers = data.suspect_buffers();
            const auto &names = data.analysis_names();
            const double nan = std::numeric_limits<double>::quiet_NaN();
            const auto assigned = transformation_product_analysis_indices(data, products);

            for (int i = 0; i < products.size(); ++i)
            {
                const auto row = products.get_transformation_product(i);
                const int a = assigned[static_cast<size_t>(i)];

                int rep = -1;
                for (int s = 0; s < buffers[a].size(); ++s)
                {
                    const auto cand = buffers[a].get_suspect(s);
                    if (cand.feature_group == row.feature_group)
                    {
                        rep = s;
                        break;
                    }
                }
                if (rep < 0 && buffers[a].size() > 0)
                    rep = 0;

                const auto rep_suspect = rep >= 0 ? buffers[a].get_suspect(rep) : ::streamfind::mass_spec::nta::api::NTA_SUSPECT_ROW();

                ::streamfind::mass_spec::nta::api::NTA_SUSPECT_ROW s;
                s.analysis = names[static_cast<size_t>(a)];
                s.feature = rep >= 0 ? rep_suspect.feature + "_transform_" + std::to_string(i)
                                     : "transform_" + std::to_string(i);
                s.feature_group = row.feature_group;
                s.candidate_rank = row.assignment_rank;
                s.name = row.name;
                s.polarity = rep_suspect.polarity;
                s.db_mass = row.mass;
                s.exp_mass = rep_suspect.exp_mass;
                s.error_mass = nan;
                s.db_rt = nan;
                s.exp_rt = rep_suspect.exp_rt;
                s.error_rt = nan;
                s.intensity = rep_suspect.intensity;
                s.area = rep_suspect.area;
                s.id_level = 0;
                s.score = row.assignment_score;
                s.shared_fragments = 0;
                s.cosine_similarity = row.cosine_similarity;
                s.formula = row.formula;
                s.SMILES = row.SMILES;
                s.InChI = row.InChI;
                s.InChIKey = row.InChIKey;
                s.xLogP = row.xLogP;
                s.database_id = "";
                s.db_ms2_size = 0;
                s.db_ms2_mz = "";
                s.db_ms2_intensity = "";
                s.db_ms2_formula = "";
                s.db_ms2_smiles = "";
                s.exp_ms2_size = rep_suspect.exp_ms2_size;
                s.exp_ms2_mz = rep_suspect.exp_ms2_mz;
                s.exp_ms2_intensity = rep_suspect.exp_ms2_intensity;
                buffers[static_cast<size_t>(a)].append(s);
            }
        }

        const std::vector<std::string> &transformation_products_columns()
        {
            static const std::vector<std::string> cols = {
                "analysis", "feature_group", "precursor_feature_group", "main_precursor_feature_group",
                "assignment_rank", "name", "formula", "mass", "SMILES", "InChI", "InChIKey", "xLogP", "transformation",
                "precursor_name", "precursor_formula", "precursor_mass", "precursor_SMILES", "precursor_InChI", "precursor_InChIKey", "precursor_xLogP",
                "main_precursor_name", "main_precursor_formula", "main_precursor_mass", "main_precursor_SMILES", "main_precursor_InChI", "main_precursor_InChIKey", "main_precursor_xLogP",
                "cosine_similarity", "main_precursor_cosine_similarity", "rt_plausibility", "main_precursor_rt_plausibility",
                "assignment_score", "network_level", "assignment_status"};
            return cols;
        }

        std::vector<std::optional<std::string>> transformation_product_cells(const std::string &analysis,
                                                                             const ::streamfind::mass_spec::nta::api::NTA_TRANSFORMATION_PRODUCT_ROW &r)
        {
            return {
                str_cell(analysis), str_cell(r.feature_group), str_cell(r.precursor_feature_group), str_cell(r.main_precursor_feature_group),
                inum_cell(r.assignment_rank), str_cell(r.name), str_cell(r.formula), dnum_cell(r.mass), str_cell(r.SMILES), str_cell(r.InChI), str_cell(r.InChIKey), dnum_cell(r.xLogP), str_cell(r.transformation),
                str_cell(r.precursor_name), str_cell(r.precursor_formula), dnum_cell(r.precursor_mass), str_cell(r.precursor_SMILES), str_cell(r.precursor_InChI), str_cell(r.precursor_InChIKey), dnum_cell(r.precursor_xLogP),
                str_cell(r.main_precursor_name), str_cell(r.main_precursor_formula), dnum_cell(r.main_precursor_mass), str_cell(r.main_precursor_SMILES), str_cell(r.main_precursor_InChI), str_cell(r.main_precursor_InChIKey), dnum_cell(r.main_precursor_xLogP),
                dnum_cell(r.cosine_similarity), dnum_cell(r.main_precursor_cosine_similarity), dnum_cell(r.rt_plausibility), dnum_cell(r.main_precursor_rt_plausibility),
                dnum_cell(r.assignment_score), inum_cell(r.network_level), str_cell(r.assignment_status)};
        }

        void emit_transformation_products(streamfind::sdk::PluginProjectAccess &access, ::streamfind::mass_spec::nta::NtaProjectData &data,
                                           const ::streamfind::mass_spec::nta::api::NTA_TRANSFORMATION_PRODUCTS &products)
        {
            const auto assigned = transformation_product_analysis_indices(data, products);
            const auto &names = data.analysis_names();
            Json rows = Json::array();
            auto columns = transformation_products_columns();
            columns.push_back("created_at");
            for (int i = 0; i < products.size(); ++i)
            {
                const auto cells = transformation_product_cells(names[static_cast<size_t>(assigned[static_cast<size_t>(i)])], products.get_transformation_product(i));
                Json row = Json::object();
                for (size_t c=0; c<cells.size(); ++c) if (cells[c]) row[columns[c]] = *cells[c];
                row["created_at"] = ::streamfind::mass_spec::nta::utils::utc_now();
                rows.push_back(std::move(row));
            }
            access.emit_table_rows("transformationProductsTable", columns,
                                   {"string","string","string","string","integer","string","string","real","string","string","string","real","string","string","string","real","string","string","string","real","string","string","real","string","string","string","real","real","real","real","real","integer","string","timestamp"}, rows);
        }

        // Write the MetFrag LocalCSV database from the JSON `database` parameter rows
        // (name, formula, mass, SMILES, InChI, InChIKey, xLogP), mirroring the R
        // binding's write_local_metfrag_database. The runner's normalize_localcsv_database
        // step maps the user-friendly columns onto MetFrag's required identifiers.
        std::string write_local_metfrag_database(const Json &database, const std::string &run_dir)
        {
            if (!database.is_array() || database.empty())
                throw Error(ErrorCode::InvalidArgument, "Local MetFrag database must contain at least one row.");
            auto str = [](const Json &o, const char *key)
            {
                auto it = o.find(key);
                return (it != o.end() && !it->is_null()) ? it->get<std::string>() : std::string();
            };
            auto num = [](const Json &o, const char *key)
            {
                auto it = o.find(key);
                return (it != o.end() && !it->is_null()) ? it->get<double>()
                                                         : std::numeric_limits<double>::quiet_NaN();
            };
            auto csv_escape = [](const std::string &value)
            {
                if (value.find_first_of(",\"\r\n") == std::string::npos)
                    return value;
                std::string out;
                out.reserve(value.size() + 2);
                out.push_back('"');
                for (char c : value)
                {
                    if (c == '"')
                        out += "\"\"";
                    else
                        out.push_back(c);
                }
                out.push_back('"');
                return out;
            };
            auto write_num = [](double value)
            {
                if (std::isnan(value))
                    return std::string();
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(10) << value;
                return oss.str();
            };

            const std::string out_path = (std::filesystem::path(run_dir) / "metfrag_local_database.csv").string();
            std::ofstream out(out_path);
            if (!out.is_open())
                throw Error(ErrorCode::InvalidArgument, "Cannot write local MetFrag database to: " + out_path);
            out << "name,formula,mass,rt,SMILES,InChI,InChIKey,xLogP\n";
            for (const auto &t : database)
            {
                if (!t.is_object())
                    throw Error(ErrorCode::InvalidArgument, "database entries must be objects");
                out << csv_escape(str(t, "name")) << ','
                    << csv_escape(str(t, "formula")) << ','
                    << csv_escape(write_num(num(t, "mass"))) << ','
                    << ',' // rt is not part of the wire schema; kept empty like the R passthrough
                    << csv_escape(str(t, "SMILES")) << ','
                    << csv_escape(str(t, "InChI")) << ','
                    << csv_escape(str(t, "InChIKey")) << ','
                    << csv_escape(write_num(num(t, "xLogP"))) << '\n';
            }
            return out_path;
        }

        // Normalize the MetFrag database_type exactly like R's
        // .normalize_metfrag_database_type (case-insensitive match against the R
        // choices) plus the run() mapping "Local" -> "LocalCSV".
        std::string normalize_metfrag_database_type(const std::string &database_type)
        {
            static const std::vector<std::string> r_types = {"KEGG", "PubChem", "ExtendedPubChem", "Local"};
            std::string lowered = database_type;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            for (const auto &t : r_types)
            {
                std::string lt = t;
                std::transform(lt.begin(), lt.end(), lt.begin(), [](unsigned char c)
                               { return static_cast<char>(std::tolower(c)); });
                if (lt == lowered)
                    return t == "Local" ? std::string("LocalCSV") : t;
            }
            throw Error(ErrorCode::WorkflowValidation,
                        "database_type must be one of: KEGG, PubChem, ExtendedPubChem, Local.");
        }

}
