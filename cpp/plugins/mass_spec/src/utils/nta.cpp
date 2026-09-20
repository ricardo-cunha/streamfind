#include "utils/nta.hpp"
#include "readers/reader.hpp"

#include <algorithm>
#include <chrono>
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
            "analysis", "feature", "feature_component", "feature_group", "adduct", "rt", "mz", "mass", "intensity", "noise", "sn", "area", "trace_count",
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
            "string", "string", "string", "string", "string", "real", "real", "real", "real", "real", "real", "real", "integer",
            "real", "real", "real", "real", "real", "real", "real", "real", "real", "real", "real", "real", "real", "real", "real",
            "integer", "real", "integer", "boolean", "string", "boolean", "real", "integer", "string", "string", "string", "string", "string",
            "integer", "string", "string", "integer", "string", "string", "string", "string", "string", "string", "real", "real", "real", "real",
            "real", "real", "real", "real", "real", "real", "real", "real", "string", "real", "real", "real", "boolean", "boolean", "timestamp"};
        return types;
    }

    nlohmann::json feature_row(const NTA_FEATURE_ROW &r)
    {
        return nlohmann::json{{"analysis", r.analysis}, {"feature", r.feature}, {"feature_component", r.feature_component}, {"feature_group", r.feature_group}, {"adduct", r.adduct},
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
