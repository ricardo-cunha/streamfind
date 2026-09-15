#include "methods/nta_processing_methods.hpp"

#include "methods/nta_deconvolution.hpp"
#include "readers/reader.hpp"
#include "methods/nta_annotation.hpp"
#include "methods/nta_blank_subtraction.hpp"
#include "methods/nta_componentization.hpp"
#include "methods/nta_correction_algorithms.hpp"
#include "methods/nta_filters.hpp"
#include "methods/nta_gap_filling.hpp"
#include "methods/nta_alignment.hpp"
#include "methods/nta_suspect_screening.hpp"
#include "methods/nta_assign_transformation_products.hpp"
#include "methods/nta_metfrag_runner.hpp"
#include "utils/tools_resolver.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace streamfind::mass_spec::processing_methods
{

    namespace detail
    {
        std::string sql(const std::string &value)
        {
            std::string out = "'";
            for (char c : value)
                out += c == '\'' ? "''" : std::string(1, c);
            return out + "'";
        }

        struct MZ_INTENSITY
        {
            std::vector<float> mz;
            std::vector<float> intensity;
        };

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
        nta::PROJECT_NON_TARGET_ANALYSIS load_analysis_features(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
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
            for (const auto &row : access.query("SELECT analysis,file_path,analysis_index,blank,replicate FROM MASS_SPEC_ANALYSES ORDER BY analysis"))
            {
                const auto name = row.at("analysis").get<std::string>();
                bool selected = wanted.empty();
                for (const auto &x : wanted)
                    selected = selected || x.get<std::string>() == name;
                if (!selected)
                    continue;
                ::mass_spec::reader::MASS_SPEC_FILE file(row.at("file_path").get<std::string>());
                file.select_analysis(analysis_index_of(row));
                names.push_back(name);
                paths.push_back(row.at("file_path").get<std::string>());
                indices.push_back(analysis_index_of(row));
                blanks.push_back(row.value("blank", ""));
                replicates.push_back(row.value("replicate", ""));
                headers.push_back(file.get_spectra_headers());
            }
            nta::PROJECT_NON_TARGET_ANALYSIS data(std::move(names), std::move(paths), std::move(headers));
            data.set_analysis_indices(std::move(indices));
            data.set_blank_names(std::move(blanks));
            data.set_replicate_names(std::move(replicates));
            auto &buffers = data.feature_buffers();
            for (size_t i = 0; i < buffers.size(); ++i)
                buffers[i].analysis = data.analysis_names()[i];
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
            for (const auto &row : access.query("SELECT analysis, feature, feature_component, feature_group, adduct, rt, mz, mass, intensity, noise, sn, area, rtmin, rtmax, width, mzmin, mzmax, ppm, fwhm_rt, fwhm_mz, gaussian_A, gaussian_mu, gaussian_sigma, gaussian_r2, jaggedness, sharpness, asymmetry, modality, plates, polarity, filtered, filter, filled, correction, eic_size, eic_rt, eic_mz, eic_intensity, eic_baseline, eic_smoothed, ms1_size, ms1_mz, ms1_intensity, ms2_size, ms2_mz, ms2_intensity, annotation_category, annotation_type, annotation_parent_feature, annotation_element, annotation_mass_error_da, annotation_mass_error_ppm, annotation_rt_error, annotation_rel_intensity, annotation_expected_rel_intensity_min, annotation_expected_rel_intensity_max, annotation_score, component_size, component_rt_center, component_rt_spread, component_density, component_mean_correlation, component_best_partner, component_max_correlation, component_mean_correlation_to_component, component_membership_score, component_is_core, component_bridge_flag FROM MASS_SPEC_NTA_FEATURES ORDER BY analysis"))
            {
                const auto an = row.at("analysis").get<std::string>();
                const auto it = std::find(data.analysis_names().begin(), data.analysis_names().end(), an);
                if (it == data.analysis_names().end())
                    continue;
                nta::api::NTA_FEATURE_ROW r;
                r.analysis = an;
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
            return data;
        }

        // Map the JSON `targets` array (suspects/internal standards) into SuspectQuery objects.
        std::vector<nta::suspect_screening::SuspectQuery> parse_suspect_targets(const Json &parameters)
        {
            std::vector<nta::suspect_screening::SuspectQuery> out;
            const auto targets = parameters.value("targets", Json::array());
            for (const auto &t : targets)
            {
                nta::suspect_screening::SuspectQuery q;
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

        bool excluded_feature(const nta::api::NTA_FEATURE_ROW &r, bool filtered)
        {
            return r.filtered && !filtered;
        }

        bool already_had(const nta::api::NTA_FEATURE_ROW &r, int level)
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

        std::vector<std::optional<std::string>> feature_cells(const nta::api::NTA_FEATURE_ROW &r)
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

        std::vector<std::optional<std::string>> suspect_cells(const nta::api::NTA_SUSPECT_ROW &r)
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

        std::vector<std::optional<std::string>> internal_standard_cells(const nta::api::NTA_INTERNAL_STANDARD_ROW &r)
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

        void persist_features(streamfind::sdk::PluginProjectAccess &access, nta::PROJECT_NON_TARGET_ANALYSIS &data)
        {
            access.require_table("MASS_SPEC_NTA_FEATURES");
            access.clear_table("MASS_SPEC_NTA_FEATURES");
            std::vector<std::vector<std::optional<std::string>>> rows;
            for (const auto &buffer : data.feature_buffers())
                for (int fi = 0; fi < buffer.size(); ++fi)
                    rows.push_back(feature_cells(buffer.get_feature(fi)));
            access.append("MASS_SPEC_NTA_FEATURES", features_columns(), rows);
        }

        void persist_suspects(streamfind::sdk::PluginProjectAccess &access, nta::PROJECT_NON_TARGET_ANALYSIS &data)
        {
            access.require_table("MASS_SPEC_NTA_SUSPECTS");
            access.clear_table("MASS_SPEC_NTA_SUSPECTS");
            std::vector<std::vector<std::optional<std::string>>> rows;
            for (const auto &buffer : data.suspect_buffers())
                for (int s = 0; s < buffer.size(); ++s)
                    rows.push_back(suspect_cells(buffer.get_suspect(s)));
            access.append("MASS_SPEC_NTA_SUSPECTS", suspects_columns(), rows);
        }

        void persist_internal_standards(streamfind::sdk::PluginProjectAccess &access, nta::PROJECT_NON_TARGET_ANALYSIS &data)
        {
            access.require_table("MASS_SPEC_NTA_INTERNAL_STANDARDS");
            access.clear_table("MASS_SPEC_NTA_INTERNAL_STANDARDS");
            std::vector<std::vector<std::optional<std::string>>> rows;
            for (const auto &buffer : data.internal_standard_buffers())
                for (int s = 0; s < buffer.size(); ++s)
                    rows.push_back(internal_standard_cells(buffer.get_internal_standard(s)));
            access.append("MASS_SPEC_NTA_INTERNAL_STANDARDS", internal_standards_columns(), rows);
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

        void load_suspects(streamfind::sdk::PluginProjectAccess &access, nta::PROJECT_NON_TARGET_ANALYSIS &data)
        {
            access.require_table("MASS_SPEC_NTA_FEATURES");
            auto &buffers = data.suspect_buffers();
            for (auto &b : buffers)
                b = nta::api::NTA_SUSPECTS();
            for (const auto &row : access.query("SELECT analysis,feature,feature_group,candidate_rank,name,polarity,db_mass,exp_mass,error_mass,db_rt,exp_rt,error_rt,intensity,area,id_level,score,shared_fragments,cosine_similarity,formula,SMILES,InChI,InChIKey,xLogP,database_id,db_ms2_size,db_ms2_mz,db_ms2_intensity,db_ms2_formula,db_ms2_smiles,exp_ms2_size,exp_ms2_mz,exp_ms2_intensity FROM MASS_SPEC_NTA_SUSPECTS ORDER BY analysis"))
            {
                const auto an = row.at("analysis").get<std::string>();
                const auto it = std::find(data.analysis_names().begin(), data.analysis_names().end(), an);
                if (it == data.analysis_names().end())
                    continue;
                nta::api::NTA_SUSPECT_ROW r;
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

        void load_internal_standards(streamfind::sdk::PluginProjectAccess &access, nta::PROJECT_NON_TARGET_ANALYSIS &data)
        {
            access.require_table("MASS_SPEC_NTA_FEATURES");
            auto &buffers = data.internal_standard_buffers();
            for (auto &b : buffers)
                b = nta::api::NTA_INTERNAL_STANDARDS();
            for (const auto &row : access.query("SELECT analysis,feature,feature_group,feature_component,adduct,candidate_rank,name,polarity,db_mass,exp_mass,error_mass,db_rt,exp_rt,error_rt,intensity,area,id_level,score,shared_fragments,cosine_similarity,formula,SMILES,InChI,InChIKey,xLogP,database_id,db_ms2_size,db_ms2_mz,db_ms2_intensity,db_ms2_formula,db_ms2_smiles,exp_ms2_size,exp_ms2_mz,exp_ms2_intensity FROM MASS_SPEC_NTA_INTERNAL_STANDARDS ORDER BY analysis"))
            {
                const auto an = row.at("analysis").get<std::string>();
                const auto it = std::find(data.analysis_names().begin(), data.analysis_names().end(), an);
                if (it == data.analysis_names().end())
                    continue;
                nta::api::NTA_INTERNAL_STANDARD_ROW r;
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
        std::vector<nta::api::NTA_TRANSFORMATION_PRODUCT_ROW> parse_transformation_products(const Json &parameters)
        {
            std::vector<nta::api::NTA_TRANSFORMATION_PRODUCT_ROW> out;
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
                nta::api::NTA_TRANSFORMATION_PRODUCT_ROW r;
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
        std::vector<int> transformation_product_analysis_indices(nta::PROJECT_NON_TARGET_ANALYSIS &data,
                                                                 const nta::api::NTA_TRANSFORMATION_PRODUCTS &products)
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
        void append_transformation_products_to_suspects(nta::PROJECT_NON_TARGET_ANALYSIS &data,
                                                        const nta::api::NTA_TRANSFORMATION_PRODUCTS &products)
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

                const auto rep_suspect = rep >= 0 ? buffers[a].get_suspect(rep) : nta::api::NTA_SUSPECT_ROW();

                nta::api::NTA_SUSPECT_ROW s;
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
                                                                             const nta::api::NTA_TRANSFORMATION_PRODUCT_ROW &r)
        {
            return {
                str_cell(analysis), str_cell(r.feature_group), str_cell(r.precursor_feature_group), str_cell(r.main_precursor_feature_group),
                inum_cell(r.assignment_rank), str_cell(r.name), str_cell(r.formula), dnum_cell(r.mass), str_cell(r.SMILES), str_cell(r.InChI), str_cell(r.InChIKey), dnum_cell(r.xLogP), str_cell(r.transformation),
                str_cell(r.precursor_name), str_cell(r.precursor_formula), dnum_cell(r.precursor_mass), str_cell(r.precursor_SMILES), str_cell(r.precursor_InChI), str_cell(r.precursor_InChIKey), dnum_cell(r.precursor_xLogP),
                str_cell(r.main_precursor_name), str_cell(r.main_precursor_formula), dnum_cell(r.main_precursor_mass), str_cell(r.main_precursor_SMILES), str_cell(r.main_precursor_InChI), str_cell(r.main_precursor_InChIKey), dnum_cell(r.main_precursor_xLogP),
                dnum_cell(r.cosine_similarity), dnum_cell(r.main_precursor_cosine_similarity), dnum_cell(r.rt_plausibility), dnum_cell(r.main_precursor_rt_plausibility),
                dnum_cell(r.assignment_score), inum_cell(r.network_level), str_cell(r.assignment_status)};
        }

        void persist_transformation_products(streamfind::sdk::PluginProjectAccess &access, nta::PROJECT_NON_TARGET_ANALYSIS &data,
                                             const nta::api::NTA_TRANSFORMATION_PRODUCTS &products)
        {
            access.require_table("MASS_SPEC_NTA_TRANSFORMATION_PRODUCTS");
            access.clear_table("MASS_SPEC_NTA_TRANSFORMATION_PRODUCTS");
            const auto assigned = transformation_product_analysis_indices(data, products);
            const auto &names = data.analysis_names();
            std::vector<std::vector<std::optional<std::string>>> rows;
            rows.reserve(static_cast<size_t>(products.size()));
            for (int i = 0; i < products.size(); ++i)
                rows.push_back(transformation_product_cells(names[static_cast<size_t>(assigned[static_cast<size_t>(i)])], products.get_transformation_product(i)));
            access.append("MASS_SPEC_NTA_TRANSFORMATION_PRODUCTS", transformation_products_columns(), rows);
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

    } // namespace detail

    Json find_features_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        access.require_table("MASS_SPEC_NTA_FEATURES");
        const auto minimums = parameters.value("rt_windows_min", Json::array()), maximums = parameters.value("rt_windows_max", Json::array());
        // Empty RT windows (R default) mean the full retention-time range.
        if (!minimums.empty() || !maximums.empty())
        {
            if (minimums.size() != maximums.size())
                throw Error(ErrorCode::InvalidArgument, "rt_windows_min and rt_windows_max must have equal lengths.");
        }
        const float ppm = parameters.value("ppm_threshold", 15.0), noise = parameters.value("noise_threshold", 250.0), snr = parameters.value("min_snr", 3.0);
        const int traces = parameters.value("min_traces", 3);
        const float baseline = parameters.value("baseline_window", 200.0), width = parameters.value("max_width", parameters.value("max_feature_width", 100.0)), quantile = parameters.value("base_quantile", .1);
        if (ppm <= 0 || noise < 0 || snr < 0 || traces < 1 || baseline <= 0 || width <= 0 || quantile <= 0 || quantile >= 1)
            throw Error(ErrorCode::InvalidArgument, "invalid feature detector parameters");

        access.clear_table("MASS_SPEC_NTA_FEATURES");
        std::vector<std::string> names, paths;
        std::vector<int> indices;
        std::vector<::mass_spec::reader::MASS_SPEC_SPECTRA_HEADERS> headers;
        const auto wanted = parameters.value("analysis_names", Json::array());
        for (const auto &row : access.query("SELECT analysis,file_path,analysis_index FROM MASS_SPEC_ANALYSES ORDER BY analysis"))
        {
            const auto name = row.at("analysis").get<std::string>();
            bool selected = wanted.empty();
            for (const auto &x : wanted)
                selected = selected || x.get<std::string>() == name;
            if (!selected)
                continue;
            int index = 0;
            if (auto it = row.find("analysis_index"); it != row.end() && !it->is_null())
            {
                const auto text = it->get<std::string>();
                index = text.empty() ? 0 : std::stoi(text);
            }
            ::mass_spec::reader::MASS_SPEC_FILE file(row.at("file_path").get<std::string>());
            file.select_analysis(index);
            names.push_back(name);
            paths.push_back(row.at("file_path").get<std::string>());
            indices.push_back(index);
            headers.push_back(file.get_spectra_headers());
        }
        nta::PROJECT_NON_TARGET_ANALYSIS data(std::move(names), std::move(paths), std::move(headers));
        data.set_analysis_indices(std::move(indices));
        std::vector<float> mins, maxs;
        for (const auto &v : minimums)
            mins.push_back(v.get<float>());
        for (const auto &v : maximums)
            maxs.push_back(v.get<float>());
        nta::deconvolution::find_features_impl(data, mins, maxs, ppm, noise, snr, traces, baseline, width, quantile, "", 0, -1);
        std::vector<std::vector<std::optional<std::string>>> feature_rows;
        for (const auto &buffer : data.feature_buffers())
            for (int fi = 0; fi < buffer.size(); ++fi)
                feature_rows.push_back(detail::feature_cells(buffer.get_feature(fi)));
        access.append("MASS_SPEC_NTA_FEATURES", detail::features_columns(), feature_rows);
        return Json{{"status", "finished"}, {"info", "Features detected."}};
    }

    Json load_features_ms1_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const bool filtered = parameters.value("filtered", false);
        const auto rt_window = parameters.value("rt_window", Json::array({-2.0, 2.0}));
        const auto mz_window = parameters.value("mz_window", Json::array({-1.0, 6.0}));
        const float min_traces = parameters.value("min_traces_intensity", 250.0);
        const float mz_clust = parameters.value("mz_clust", 0.005);
        const float presence = parameters.value("presence", 0.8);
        if (min_traces < 0 || mz_clust < 0 || presence < 0 || presence > 1)
            throw Error(ErrorCode::InvalidArgument, "invalid MS1 spectrum loading parameters");
        const float rt_lo = rt_window.size() >= 1 ? rt_window[0].get<float>() : 0.0f;
        const float rt_hi = rt_window.size() >= 2 ? rt_window[1].get<float>() : 0.0f;
        const float mz_lo = mz_window.size() >= 1 ? mz_window[0].get<float>() : 0.0f;
        const float mz_hi = mz_window.size() >= 2 ? mz_window[1].get<float>() : 0.0f;
        auto data = detail::load_analysis_features(access, parameters);
        auto &buffers = data.feature_buffers();
        for (size_t i = 0; i < buffers.size(); ++i)
        {
            ::mass_spec::spectra::MASS_SPEC_TARGETS targets;
            int counter = 0;
            for (int j = 0; j < buffers[i].size(); ++j)
            {
                const auto ft = buffers[i].get_feature(j);
                if (detail::excluded_feature(ft, filtered))
                    continue;
                if (detail::already_had(ft, 1))
                    continue;
                targets.index.push_back(counter++);
                targets.id.push_back(ft.feature);
                targets.level.push_back(1);
                targets.polarity.push_back(ft.polarity);
                targets.precursor.push_back(false);
                targets.mzmin.push_back(static_cast<float>(ft.mzmin) + mz_lo);
                targets.mzmax.push_back(static_cast<float>(ft.mzmax) + mz_hi);
                targets.rtmin.push_back(static_cast<float>(ft.rtmin) + rt_lo);
                targets.rtmax.push_back(static_cast<float>(ft.rtmax) + rt_hi);
                targets.mz.push_back(static_cast<float>(ft.mz));
                targets.mass.push_back(static_cast<float>(ft.mass));
                targets.rt.push_back(static_cast<float>(ft.rt));
                targets.mobility.push_back(0.0f);
                targets.mobilitymin.push_back(0.0f);
                targets.mobilitymax.push_back(0.0f);
            }
            if (targets.id.empty())
                continue;
            if (!std::filesystem::exists(data.file_paths()[i]))
                continue;
            std::cerr << "[load_features_ms1] " << i + 1 << "/" << buffers.size()
                      << " targets=" << targets.id.size() << std::endl;
            ::mass_spec::reader::MASS_SPEC_FILE file(data.file_paths()[i]);
            file.select_analysis(data.analysis_index_at(i));
            auto spectra = file.get_spectra_targets(targets, data.spectra_headers_at(i), min_traces, 0.0f);
            std::cerr << "[load_features_ms1] extracted " << spectra.id.size()
                      << " spectrum points" << std::endl;
            std::vector<std::vector<std::optional<std::string>>> updates;
            size_t updated_for_analysis = 0;
            for (int j = 0; j < buffers[i].size(); ++j)
            {
                auto ft = buffers[i].get_feature(j);
                if (detail::excluded_feature(ft, filtered))
                    continue;
                if (detail::already_had(ft, 1))
                    continue;
                ::mass_spec::spectra::MASS_SPEC_TARGETS_SPECTRA sub;
                for (size_t k = 0; k < spectra.id.size(); ++k)
                {
                    if (spectra.id[k] != ft.feature)
                        continue;
                    sub.mz.push_back(spectra.mz[k]);
                    sub.intensity.push_back(spectra.intensity[k]);
                    if (k < spectra.rt.size())
                        sub.rt.push_back(spectra.rt[k]);
                    if (k < spectra.pre_ce.size())
                        sub.pre_ce.push_back(spectra.pre_ce[k]);
                }
                auto merged = detail::merge_nta_feature_spectra(sub, mz_clust, presence);
                if (merged.mz.empty())
                    continue;
                ft.ms1_size = static_cast<int>(merged.mz.size());
                ft.ms1_mz = detail::encode_float_array(merged.mz);
                ft.ms1_intensity = detail::encode_float_array(merged.intensity);
                buffers[i].set_feature(j, ft);
                updates.push_back({ft.analysis, ft.feature, std::to_string(ft.ms1_size),
                                   ft.ms1_mz, ft.ms1_intensity});
                ++updated_for_analysis;
            }
            access.update_composite("MASS_SPEC_NTA_FEATURES", {"analysis", "feature"},
                                    {"ms1_size", "ms1_mz", "ms1_intensity"}, updates);
            std::cerr << "[load_features_ms1] updated " << updated_for_analysis
                      << " features" << std::endl;
        }
        return Json{{"status", "finished"}, {"info", "MS1 spectra loaded."}};
    }

    Json load_features_ms2_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const bool filtered = parameters.value("filtered", false);
        const float min_traces = parameters.value("min_traces_intensity", 10.0);
        const float isolation_window = parameters.value("isolation_window", 1.3);
        const float mz_clust = parameters.value("mz_clust", 0.005);
        const float presence = parameters.value("presence", 0.8);
        if (min_traces < 0 || isolation_window < 0 || mz_clust < 0 || presence < 0 || presence > 1)
            throw Error(ErrorCode::InvalidArgument, "invalid MS2 spectrum loading parameters");
        auto data = detail::load_analysis_features(access, parameters);
        auto &buffers = data.feature_buffers();
        for (size_t i = 0; i < buffers.size(); ++i)
        {
            ::mass_spec::spectra::MASS_SPEC_TARGETS targets;
            int counter = 0;
            for (int j = 0; j < buffers[i].size(); ++j)
            {
                const auto ft = buffers[i].get_feature(j);
                if (detail::excluded_feature(ft, filtered))
                    continue;
                if (detail::already_had(ft, 2))
                    continue;
                targets.index.push_back(counter++);
                targets.id.push_back(ft.feature);
                targets.level.push_back(2);
                targets.polarity.push_back(ft.polarity);
                targets.precursor.push_back(true);
                targets.mzmin.push_back(static_cast<float>(ft.mz) - isolation_window / 2.0f);
                targets.mzmax.push_back(static_cast<float>(ft.mz) + isolation_window / 2.0f);
                targets.rtmin.push_back(static_cast<float>(ft.rtmin));
                targets.rtmax.push_back(static_cast<float>(ft.rtmax));
                targets.mz.push_back(static_cast<float>(ft.mz));
                targets.mass.push_back(static_cast<float>(ft.mass));
                targets.rt.push_back(static_cast<float>(ft.rt));
                targets.mobility.push_back(0.0f);
                targets.mobilitymin.push_back(0.0f);
                targets.mobilitymax.push_back(0.0f);
            }
            if (targets.id.empty())
                continue;
            if (!std::filesystem::exists(data.file_paths()[i]))
                continue;
            ::mass_spec::reader::MASS_SPEC_FILE file(data.file_paths()[i]);
            file.select_analysis(data.analysis_index_at(i));
            auto spectra = file.get_spectra_targets(targets, data.spectra_headers_at(i), 0.0f, min_traces);
            std::cerr << "[load_features_ms2] " << i + 1 << "/" << buffers.size()
                      << " targets=" << targets.id.size() << std::endl;
            std::cerr << "[load_features_ms2] extracted " << spectra.id.size()
                      << " spectrum points" << std::endl;
            std::vector<std::vector<std::optional<std::string>>> updates;
            size_t updated_for_analysis = 0;
            for (int j = 0; j < buffers[i].size(); ++j)
            {
                auto ft = buffers[i].get_feature(j);
                if (detail::excluded_feature(ft, filtered))
                    continue;
                if (detail::already_had(ft, 2))
                    continue;
                ::mass_spec::spectra::MASS_SPEC_TARGETS_SPECTRA sub;
                for (size_t k = 0; k < spectra.id.size(); ++k)
                {
                    if (spectra.id[k] != ft.feature)
                        continue;
                    sub.mz.push_back(spectra.mz[k]);
                    sub.intensity.push_back(spectra.intensity[k]);
                    if (k < spectra.rt.size())
                        sub.rt.push_back(spectra.rt[k]);
                    if (k < spectra.pre_ce.size())
                        sub.pre_ce.push_back(spectra.pre_ce[k]);
                }
                auto merged = detail::merge_nta_feature_spectra(sub, mz_clust, presence);
                if (merged.mz.empty())
                    continue;
                ft.ms2_size = static_cast<int>(merged.mz.size());
                ft.ms2_mz = detail::encode_float_array(merged.mz);
                ft.ms2_intensity = detail::encode_float_array(merged.intensity);
                buffers[i].set_feature(j, ft);
                updates.push_back({ft.analysis, ft.feature, std::to_string(ft.ms2_size),
                                   ft.ms2_mz, ft.ms2_intensity});
                ++updated_for_analysis;
            }
            access.update_composite("MASS_SPEC_NTA_FEATURES", {"analysis", "feature"},
                                    {"ms2_size", "ms2_mz", "ms2_intensity"}, updates);
            std::cerr << "[load_features_ms2] updated " << updated_for_analysis
                      << " features" << std::endl;
        }
        return Json{{"status", "finished"}, {"info", "MS2 spectra loaded."}};
    }

    Json subtract_blank_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const float blank_threshold = parameters.value("blank_threshold", 5.0);
        const float rt_expand = parameters.value("rt_expand", 10.0);
        const float mz_expand = parameters.value("mz_expand", 0.005);
        const float min_traces_intensity = parameters.value("min_traces_intensity", 0.0);
        if (blank_threshold < 0 || rt_expand < 0 || mz_expand < 0 || min_traces_intensity < 0)
            throw Error(ErrorCode::InvalidArgument, "invalid blank subtraction parameters");
        auto data = detail::load_analysis_features(access, parameters);
        nta::blank_subtraction::subtract_blank_impl(data, blank_threshold, rt_expand, mz_expand, min_traces_intensity);
        detail::persist_features(access, data);
        return Json{{"status", "finished"}, {"info", "Blank subtraction completed."}};
    }

    Json filter_features_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        // Optional numeric filters: null/absent (R NA) disable the filter via NaN.
        auto opt_real = [&](const char *key) -> double
        {
            auto it = parameters.find(key);
            if (it == parameters.end() || it->is_null())
                return std::numeric_limits<double>::quiet_NaN();
            return it->get<double>();
        };
        auto opt_int = [&](const char *key) -> int
        {
            auto it = parameters.find(key);
            if (it == parameters.end() || it->is_null())
                return 0;
            return it->get<int>();
        };
        auto has = [&](const char *key) -> bool
        {
            auto it = parameters.find(key);
            return it != parameters.end() && !it->is_null();
        };

        const double minSN = opt_real("min_sn");
        const double minIntensity = opt_real("min_intensity");
        const double minArea = opt_real("min_area");
        const double minWidth = opt_real("min_width");
        const double maxWidth = opt_real("max_width");
        const double maxPPM = opt_real("max_ppm");
        const double minFwhmRT = opt_real("min_fwhm_rt");
        const double maxFwhmRT = opt_real("max_fwhm_rt");
        const double minFwhmMZ = opt_real("min_fwhm_mz");
        const double maxFwhmMZ = opt_real("max_fwhm_mz");
        const double minGaussianA = opt_real("min_gaussian_a");
        const double minGaussianMu = opt_real("min_gaussian_mu");
        const double maxGaussianMu = opt_real("max_gaussian_mu");
        const double minGaussianSigma = opt_real("min_gaussian_sigma");
        const double maxGaussianSigma = opt_real("max_gaussian_sigma");
        const double minGaussianR2 = opt_real("min_gaussian_r2");
        const double maxJaggedness = opt_real("max_jaggedness");
        const double minSharpness = opt_real("min_sharpness");
        const double minAsymmetry = opt_real("min_asymmetry");
        const double maxAsymmetry = opt_real("max_asymmetry");
        const double minPlates = opt_real("min_plates");
        const double minRelPresenceReplicate = opt_real("min_rel_presence_replicate");
        const int maxModality = opt_int("max_modality");
        const bool hasMaxModality = has("max_modality");
        const int minSizeEIC = opt_int("min_size_eic");
        const bool hasMinSizeEIC = has("min_size_eic");
        const int minSizeMS1 = opt_int("min_size_ms1");
        const bool hasMinSizeMS1 = has("min_size_ms1");
        const int minSizeMS2 = opt_int("min_size_ms2");
        const bool hasMinSizeMS2 = has("min_size_ms2");

        // only_filled is tri-state: true=keep only filled, false=keep only non-filled,
        // null/absent=disabled (matches R onlyFilled=NA).
        bool hasOnlyFilled = false, onlyFilledValue = false;
        if (auto it = parameters.find("only_filled"); it != parameters.end() && !it->is_null())
        {
            hasOnlyFilled = true;
            onlyFilledValue = it->get<bool>();
        }
        const bool removeFilled = parameters.value("remove_filled", false);
        const bool removeIsotopes = parameters.value("remove_isotopes", false);
        const bool removeAdducts = parameters.value("remove_adducts", false);
        const bool removeLosses = parameters.value("remove_losses", false);

        auto data = detail::load_analysis_features(access, parameters);
        nta::filter_features::filter_features_impl(
            data,
            minSN, minIntensity, minArea, minWidth, maxWidth, maxPPM,
            minFwhmRT, maxFwhmRT, minFwhmMZ, maxFwhmMZ,
            minGaussianA, minGaussianMu, maxGaussianMu, minGaussianSigma, maxGaussianSigma, minGaussianR2,
            maxJaggedness, minSharpness, minAsymmetry, maxAsymmetry,
            maxModality, hasMaxModality, minPlates,
            hasOnlyFilled, onlyFilledValue, removeFilled,
            minSizeEIC, hasMinSizeEIC, minSizeMS1, hasMinSizeMS1, minSizeMS2, hasMinSizeMS2,
            minRelPresenceReplicate,
            removeIsotopes, removeAdducts, removeLosses);
        detail::persist_features(access, data);
        return Json{{"status", "finished"}, {"info", "Features filtered."}};
    }

    Json filter_features_ms2_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const int top = parameters.value("top", 0);
        const float min_intensity_ms2 = parameters.value("min_intensity_ms2", NAN);
        const float rel_min_intensity = parameters.value("rel_min_intensity", NAN);
        const bool blank_clean = parameters.value("blank_clean", false);
        const float mz_clust = parameters.value("mz_clust", 0.005);
        const float blank_presence_threshold = parameters.value("blank_presence_threshold", 0.8);
        const float global_presence_threshold = parameters.value("global_presence_threshold", 0.1);
        if (top < 0 || mz_clust < 0 || blank_presence_threshold < 0 || blank_presence_threshold > 1 ||
            global_presence_threshold < 0 || global_presence_threshold > 1)
            throw Error(ErrorCode::InvalidArgument, "invalid MS2 feature filtering parameters");
        auto data = detail::load_analysis_features(access, parameters);
        nta::filter_features_ms2::filter_features_ms2_impl(data, top, min_intensity_ms2, rel_min_intensity,
                                                           blank_clean, mz_clust, blank_presence_threshold, global_presence_threshold);
        detail::persist_features(access, data);
        return Json{{"status", "finished"}, {"info", "MS2 peak lists filtered."}};
    }

    Json group_features_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto method = parameters.value("method", std::string("internal_standards"));
        const float rt_deviation = parameters.value("rt_deviation", 5.0);
        const float ppm = parameters.value("ppm", 10.0);
        const int min_samples = parameters.value("min_samples", 1);
        const float bin_size = parameters.value("bin_size", 5.0);
        if (method.empty() || rt_deviation < 0 || ppm < 0 || min_samples < 1 || bin_size <= 0)
            throw Error(ErrorCode::InvalidArgument, "invalid feature grouping parameters");
        auto data = detail::load_analysis_features(access, parameters);
        if (method == "internal_standards")
            detail::load_internal_standards(access, data);
        nta::alignment::group_features_impl(data, method, rt_deviation, ppm, min_samples, bin_size);
        detail::persist_features(access, data);
        return Json{{"status", "finished"}, {"info", "Features grouped."}};
    }

    Json fill_features_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const bool within_replicate = parameters.value("within_replicate", false);
        const bool filtered = parameters.value("filtered", false);
        const float rt_expand = parameters.value("rt_expand", 10.0);
        const float mz_expand = parameters.value("mz_expand", 0.01);
        const float max_peak_width = parameters.value("max_peak_width", 30.0);
        const float min_traces_intensity = parameters.value("min_traces_intensity", 1000.0);
        const int min_number_traces = parameters.value("min_number_traces", 5);
        const float min_intensity_ms1 = parameters.value("min_intensity", parameters.value("min_intensity_ms1", 5000.0));
        const float rt_apex_deviation = parameters.value("rt_apex_deviation", 5.0);
        const float min_signal_to_noise_ratio = parameters.value("min_signal_to_noise_ratio", 3.0);
        const float min_gaussian_fit = parameters.value("min_gaussian_fit", 0.2);
        if (rt_expand < 0 || mz_expand < 0 || max_peak_width <= 0 || min_traces_intensity < 0 ||
            min_number_traces < 1 || min_intensity_ms1 < 0 || rt_apex_deviation < 0 ||
            min_signal_to_noise_ratio < 0 || min_gaussian_fit < 0 || min_gaussian_fit > 1)
            throw Error(ErrorCode::InvalidArgument, "invalid gap filling parameters");
        auto data = detail::load_analysis_features(access, parameters);
        nta::gap_filling::fill_features_impl(data, within_replicate, filtered, rt_expand, mz_expand,
                                             max_peak_width, min_traces_intensity, min_number_traces, min_intensity_ms1,
                                             rt_apex_deviation, min_signal_to_noise_ratio, min_gaussian_fit);
        detail::persist_features(access, data);
        return Json{{"status", "finished"}, {"info", "Feature gaps filled."}};
    }

    Json create_components_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const float min_correlation = parameters.value("min_correlation", 0.8);
        std::vector<float> rt_window;
        const auto rt_window_param = parameters.value("rt_window", Json::array());
        for (const auto &v : rt_window_param)
            rt_window.push_back(v.get<float>());
        if (rt_window.empty())
            rt_window = {0.0f, 0.0f};
        if (min_correlation < 0 || min_correlation > 1)
            throw Error(ErrorCode::InvalidArgument, "invalid componentization parameters");
        auto data = detail::load_analysis_features(access, parameters);
        nta::componentization::create_components_impl(data, rt_window, min_correlation);
        detail::persist_features(access, data);
        return Json{{"status", "finished"}, {"info", "Components created."}};
    }

    Json annotate_components_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const int max_isotopes = parameters.value("max_isotopes", 5);
        const int max_charge = parameters.value("max_charge", 1);
        const int max_gaps = parameters.value("max_gaps", 1);
        const float ppm = parameters.value("ppm", 10.0);
        std::vector<std::string> isotope_elements;
        const auto isotope_elements_param = parameters.value("isotope_elements", Json::array({Json("C:1-60"), Json("N:0-10"), Json("O:0-20"), Json("S:0-4"), Json("Cl:0-6"), Json("Br:0-4")}));
        for (const auto &v : isotope_elements_param)
            isotope_elements.push_back(v.get<std::string>());
        if (max_isotopes < 1 || max_charge < 1 || max_gaps < 0 || ppm < 0)
            throw Error(ErrorCode::InvalidArgument, "invalid annotation parameters");
        auto data = detail::load_analysis_features(access, parameters);
        nta::annotation::annotate_components_impl(data, max_isotopes, max_charge, max_gaps, ppm, isotope_elements);
        detail::persist_features(access, data);
        return Json{{"status", "finished"}, {"info", "Components annotated."}};
    }

    Json suspect_screening_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const double ppm = parameters.value("ppm", 5.0);
        const double sec = parameters.value("sec", 10.0);
        const double ppm_ms2 = parameters.value("ppm_ms2", 10.0);
        const double mzr_ms2 = parameters.value("mzr_ms2", 0.008);
        const double min_cosine_similarity = parameters.value("min_cosine_similarity", 0.7);
        const int min_shared_fragments = parameters.value("min_shared_fragments", 3);
        const bool filtered = parameters.value("filtered", true);
        if (ppm < 0 || sec < 0 || ppm_ms2 < 0 || mzr_ms2 < 0 || min_cosine_similarity < 0 ||
            min_cosine_similarity > 1 || min_shared_fragments < 0)
            throw Error(ErrorCode::InvalidArgument, "invalid suspect screening parameters");
        auto data = detail::load_analysis_features(access, parameters);
        const auto suspects = detail::parse_suspect_targets(parameters);
        nta::suspect_screening::suspect_screening_impl(data, data.analysis_names(), suspects,
                                                       ppm, sec, ppm_ms2, mzr_ms2, min_cosine_similarity, min_shared_fragments, filtered);
        detail::persist_features(access, data);
        detail::persist_suspects(access, data);
        return Json{{"status", "finished"}, {"info", "Suspect screening completed."}};
    }

    Json filter_suspects_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        std::vector<std::string> names;
        for (const auto &v : parameters.value("names", Json::array()))
            names.push_back(v.get<std::string>());
        auto opt_real = [&](const char *key) -> double
        {
            auto it = parameters.find(key);
            return (it != parameters.end() && !it->is_null()) ? it->get<double>() : std::numeric_limits<double>::quiet_NaN();
        };
        const double min_score = opt_real("min_score");
        const double max_error_rt = opt_real("max_error_rt");
        const double max_error_mass = opt_real("max_error_mass");
        std::vector<int> id_levels;
        for (const auto &v : parameters.value("id_levels", Json::array()))
            id_levels.push_back(v.get<int>());
        const int min_shared_fragments = parameters.value("min_shared_fragments", 0);
        const double min_cosine_similarity = opt_real("min_cosine_similarity");
        if (min_shared_fragments < 0)
            throw Error(ErrorCode::InvalidArgument, "invalid suspect filtering parameters");
        auto data = detail::load_analysis_features(access, parameters);
        detail::load_suspects(access, data);
        nta::filter_suspects::filter_suspects_impl(data, names, min_score, max_error_rt, max_error_mass,
                                                   id_levels, min_shared_fragments, min_cosine_similarity);
        detail::persist_suspects(access, data);
        return Json{{"status", "finished"}, {"info", "Suspects filtered."}};
    }

    Json find_internal_standards_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const double ppm = parameters.value("ppm", 5.0);
        const double sec = parameters.value("sec", 10.0);
        const double ppm_ms2 = parameters.value("ppm_ms2", 10.0);
        const double mzr_ms2 = parameters.value("mzr_ms2", 0.008);
        const double min_cosine_similarity = parameters.value("min_cosine_similarity", 0.7);
        const int min_shared_fragments = parameters.value("min_shared_fragments", 3);
        const bool filtered = parameters.value("filtered", true);
        if (ppm < 0 || sec < 0 || ppm_ms2 < 0 || mzr_ms2 < 0 || min_cosine_similarity < 0 ||
            min_cosine_similarity > 1 || min_shared_fragments < 0)
            throw Error(ErrorCode::InvalidArgument, "invalid internal standard parameters");
        auto data = detail::load_analysis_features(access, parameters);
        const auto suspects = detail::parse_suspect_targets(parameters);
        nta::suspect_screening::find_internal_standards_impl(data, data.analysis_names(), suspects,
                                                             ppm, sec, ppm_ms2, mzr_ms2, min_cosine_similarity, min_shared_fragments, filtered);
        detail::persist_features(access, data);
        detail::persist_internal_standards(access, data);
        return Json{{"status", "finished"}, {"info", "Internal standards found."}};
    }

    Json filter_internal_standards_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        std::vector<std::string> names;
        for (const auto &v : parameters.value("names", Json::array()))
            names.push_back(v.get<std::string>());
        auto opt_real = [&](const char *key) -> double
        {
            auto it = parameters.find(key);
            return (it != parameters.end() && !it->is_null()) ? it->get<double>() : std::numeric_limits<double>::quiet_NaN();
        };
        const double min_score = opt_real("min_score");
        const double max_error_rt = opt_real("max_error_rt");
        const double max_error_mass = opt_real("max_error_mass");
        std::vector<int> id_levels;
        for (const auto &v : parameters.value("id_levels", Json::array()))
            id_levels.push_back(v.get<int>());
        const int min_shared_fragments = parameters.value("min_shared_fragments", 0);
        const double min_cosine_similarity = opt_real("min_cosine_similarity");
        if (min_shared_fragments < 0)
            throw Error(ErrorCode::InvalidArgument, "invalid internal standard filtering parameters");
        auto data = detail::load_analysis_features(access, parameters);
        detail::load_internal_standards(access, data);
        nta::filter_internal_standards::filter_internal_standards_impl(data, names, min_score, max_error_rt, max_error_mass,
                                                                       id_levels, min_shared_fragments, min_cosine_similarity);
        detail::persist_internal_standards(access, data);
        return Json{{"status", "finished"}, {"info", "Internal standards filtered."}};
    }

    Json correct_matrix_suppression_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const float mp_rt_window = parameters.value("mp_rt_window", 10.0);
        std::string ref_blank_replicate = parameters.value("ref_blank_replicate", std::string(""));
        if (ref_blank_replicate == "NA" || ref_blank_replicate == "NA_character_")
            ref_blank_replicate.clear();
        if (mp_rt_window <= 0)
            throw Error(ErrorCode::InvalidArgument, "invalid matrix suppression correction parameters");
        auto data = detail::load_analysis_features(access, parameters);
        detail::load_internal_standards(access, data);
        nta::correction_algorithms::correct_matrix_suppression_impl(data, mp_rt_window, ref_blank_replicate);
        detail::persist_features(access, data);
        return Json{{"status", "finished"}, {"info", "Matrix suppression corrected."}};
    }

    Json assign_transformation_products_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        // R defaults (catalogue carries no defaults; the executor mirrors the R method).
        const std::string phase = parameters.value("chromatographic_phase", std::string("reverse_phase"));
        const double mzr_ms2 = parameters.value("mzr_ms2", 0.008);
        if (phase != "reverse_phase" && phase != "hilic")
            throw Error(ErrorCode::InvalidArgument, "invalid assign_transformation_products parameters: chromatographic_phase must be reverse_phase or hilic");
        if (mzr_ms2 < 0)
            throw Error(ErrorCode::InvalidArgument, "invalid assign_transformation_products parameters: mzr_ms2 must be >= 0");

        // Operate on the current suspects buffer (from suspect_screening): load the
        // per-analysis features/analyses plus the persisted suspects, run the
        // algorithm over the combined suspect rows, append the combination-scored
        // rows back into the suspect buffers, and persist through the suspects path.
        auto data = detail::load_analysis_features(access, parameters);
        detail::load_suspects(access, data);
        const auto tp_rows = detail::parse_transformation_products(parameters);

        std::vector<nta::api::NTA_SUSPECT_ROW> suspects;
        for (const auto &buffer : data.suspect_buffers())
            for (int i = 0; i < buffer.size(); ++i)
                suspects.push_back(buffer.get_suspect(i));

        const auto products = nta::assign_transformation_products::assign_transformation_products_impl(
            suspects, tp_rows, phase, mzr_ms2);
        detail::append_transformation_products_to_suspects(data, products);
        detail::persist_transformation_products(access, data, products);
        detail::persist_suspects(access, data);
        return Json{{"status", "finished"}, {"info", "Transformation products assigned."}};
    }

    Json metfrag_screening_with_access(streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        // Tool resolution is discovery-only. Installation must be an explicit user action.
        const auto tool = streamfind::mass_spec::tools::resolve_metfrag();
        if (!tool)
            throw Error(ErrorCode::MethodExecution,
                        "MetFrag screening requires Java 21 and MetFragCL. Install them explicitly with "
                        "streamfind-cli tools install java and streamfind-cli tools install metfrag, then retry. "
                        "Expected managed locations are %USERPROFILE%\\.streamfind\\tools on Windows "
                        "or $HOME/.streamfind/tools on Linux/macOS.");

        const std::string database_type = detail::normalize_metfrag_database_type(
            parameters.value("database_type", std::string("PubChem")));
        // R method defaults.
        const double ppm = parameters.value("ppm", 5.0);
        const double sec = parameters.value("sec", 10.0);
        const double ppm_ms2 = parameters.value("ppm_ms2", 10.0);
        const double mzr_ms2 = parameters.value("mzr_ms2", 0.008);
        const int top_n = parameters.value("top_n", 5);
        std::vector<std::string> score_types;
        for (const auto &v : parameters.value("score_types", Json::array({Json("FragmenterScore")})))
            score_types.push_back(v.get<std::string>());
        std::vector<double> score_weights;
        for (const auto &v : parameters.value("score_weights", Json::array({Json(1.0)})))
            score_weights.push_back(v.get<double>());
        std::vector<std::string> pre_processing_candidate_filter;
        for (const auto &v : parameters.value("pre_processing_candidate_filter",
                                              Json::array({Json("UnconnectedCompoundFilter"), Json("IsotopeFilter")})))
            pre_processing_candidate_filter.push_back(v.get<std::string>());
        std::vector<std::string> post_processing_candidate_filter;
        for (const auto &v : parameters.value("post_processing_candidate_filter", Json::array({Json("InChIKeyFilter")})))
            post_processing_candidate_filter.push_back(v.get<std::string>());
        const int maximum_tree_depth = parameters.value("maximum_tree_depth", 3);
        const int number_threads = parameters.value("number_threads", 1);
        const bool use_smiles = parameters.value("use_smiles", true);
        const bool filtered = parameters.value("filtered", false);
        // `debug` is accepted for schema parity; the runner keeps the inspectable
        // PSV output for features with candidates regardless (R never forwards it).

        if (ppm < 0 || sec < 0 || ppm_ms2 < 0 || mzr_ms2 < 0)
            throw Error(ErrorCode::InvalidArgument, "invalid metfrag_screening parameters: ppm, sec, ppm_ms2, and mzr_ms2 must be >= 0");
        if (top_n < 1)
            throw Error(ErrorCode::InvalidArgument, "invalid metfrag_screening parameters: top_n must be >= 1");
        if (maximum_tree_depth < 1)
            throw Error(ErrorCode::InvalidArgument, "invalid metfrag_screening parameters: maximum_tree_depth must be >= 1");
        if (number_threads < 1)
            throw Error(ErrorCode::InvalidArgument, "invalid metfrag_screening parameters: number_threads must be >= 1");
        if (score_types.size() != score_weights.size())
            throw Error(ErrorCode::InvalidArgument, "invalid metfrag_screening parameters: score_types and score_weights must have the same length");

        auto data = detail::load_analysis_features(access, parameters);
        nta::metfrag_runner::MetFragParams p;
        p.metfrag_path = tool->second; // MetFragCL.jar
        p.java_path = tool->first;     // java executable
        p.database_type = database_type;
        p.ppm = ppm;
        p.sec = sec;
        p.ppmMS2 = ppm_ms2;
        p.mzrMS2 = mzr_ms2;
        p.top_n = top_n;
        p.score_types = std::move(score_types);
        p.score_weights = std::move(score_weights);
        p.pre_processing_candidate_filter = std::move(pre_processing_candidate_filter);
        p.post_processing_candidate_filter = std::move(post_processing_candidate_filter);
        p.candidate_writer = {"CSV", "FragmentSmilesPSV"};
        p.maximum_tree_depth = maximum_tree_depth;
        p.number_threads = number_threads;
        p.use_smiles = use_smiles;
        p.filtered = filtered;
        p.run_dir = nta::metfrag_runner::resolve_run_dir(p);
        if (p.database_type == "LocalCSV")
        {
            std::filesystem::create_directories(p.run_dir);
            p.database_path = detail::write_local_metfrag_database(
                parameters.value("database", Json::array()), p.run_dir);
        }
        nta::metfrag_runner::metfrag_screening_impl(data, data.analysis_names(), p);
        detail::persist_suspects(access, data);
        return Json{{"status", "finished"}, {"info", "MetFrag screening completed."}};
    }

} // namespace streamfind::mass_spec::processing_methods
