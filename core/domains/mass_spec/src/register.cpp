#include "streamfind/catalogue.hpp"
#include "streamfind/catalogue_binding.hpp"
#include "streamfind/mass_spec/mass_spec.hpp"
#include "streamfind/mass_spec/processing_methods_chromatograms.hpp"
#include "streamfind/mass_spec/processing_methods_nta.hpp"
#include "streamfind/mass_spec/register.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>

namespace streamfind::mass_spec {

namespace detail {

const char *case_insensitive_equal(const std::string &a, const char *b) {
    if (a.size() != std::char_traits<char>::length(b)) return nullptr;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return nullptr;
    return b;
}

Json columnar(Json rows, const Json &schema) {
    Json columns = Json::object();
    const auto properties = schema.value("properties", Json::object());
    for (auto it = properties.begin(); it != properties.end(); ++it)
        columns[it.key()] = Json::array();
    for (const auto &row : rows) {
        for (auto it = columns.begin(); it != columns.end(); ++it)
            it.value().push_back(row.contains(it.key()) ? row.at(it.key()) : Json(nullptr));
    }
    return {{"row_count", rows.size()}, {"columns", std::move(columns)}};
}

// Per-method VALUE validation for the NTA processing methods. Runs on the
// RESOLVED parameters: optional parameters may be ABSENT (disabled), and the
// schema-level type checks have already run. Throws
// Error(ErrorCode::WorkflowValidation, "<id>: invalid parameters: <reason>")
// on the first failing check, mirroring the executor guard conditions in
// processing_methods_nta.cpp.
MethodValidator nta_validator(const std::string &id) {
    const auto require = [id](bool ok, const std::string &reason) {
        if (!ok) throw Error(ErrorCode::WorkflowValidation, id + ": invalid parameters: " + reason);
    };
    const auto real_at_least = [=](const Json &p, const char *key, double lo, const std::string &reason) {
        if (p.contains(key) && p.at(key).get<double>() < lo) require(false, reason);
    };
    const auto real_above = [=](const Json &p, const char *key, double lo, const std::string &reason) {
        if (p.contains(key) && !(p.at(key).get<double>() > lo)) require(false, reason);
    };
    const auto real_in = [=](const Json &p, const char *key, double lo, double hi, const std::string &reason) {
        if (!p.contains(key)) return;
        const double value = p.at(key).get<double>();
        if (value < lo || value > hi) require(false, reason);
    };
    const auto int_at_least = [=](const Json &p, const char *key, int lo, const std::string &reason) {
        if (p.contains(key) && p.at(key).get<int>() < lo) require(false, reason);
    };
    const auto pair_array = [=](const Json &p, const char *key, const std::string &reason) {
        if (!p.contains(key)) return;
        const Json &value = p.at(key);
        if (!value.is_array() || value.size() != 2) require(false, reason);
    };
    const auto validate_targets = [=](const Json &p) {
        if (!p.contains("targets")) return;
        const Json &targets = p.at("targets");
        if (!targets.is_array()) require(false, "targets must be an array");
        for (const auto &target : targets) {
            if (!target.is_object()) require(false, "targets entries must be objects");
            const bool has_id = target.contains("id") && target.at("id").is_string();
            const bool has_name = target.contains("name") && target.at("name").is_string();
            if (!has_id && !has_name) require(false, "targets entries require an id or name");
            const bool has_identifier =
                (target.contains("mass") && target.at("mass").is_number()) ||
                (target.contains("mz") && target.at("mz").is_number()) ||
                (target.contains("formula") && target.at("formula").is_string()) ||
                (target.contains("SMILES") && target.at("SMILES").is_string()) ||
                (target.contains("InChI") && target.at("InChI").is_string());
            if (!has_identifier) require(false, "targets entries require mass, mz, formula, SMILES, or InChI");
            const char *fragment_pairs[][2] = {
                {"fragments_mz_pos", "fragments_intensity_pos"},
                {"fragments_mz_neg", "fragments_intensity_neg"}
            };
            for (const auto &pair : fragment_pairs) {
                const bool has_mz = target.contains(pair[0]);
                const bool has_intensity = target.contains(pair[1]);
                if (!has_mz && !has_intensity) continue;
                if (!has_mz || !has_intensity) require(false, std::string(pair[0]) + " and " + pair[1] + " must be provided together");
                if (!target.at(pair[0]).is_array() || !target.at(pair[1]).is_array())
                    require(false, std::string(pair[0]) + " and " + pair[1] + " must be arrays");
                if (target.at(pair[0]).size() != target.at(pair[1]).size())
                    require(false, std::string(pair[0]) + " and " + pair[1] + " must have equal lengths");
            }
        }
    };

    if (id == "mass_spec.find_features") {
        return [=](const Json &p) {
            real_above(p, "ppm_threshold", 0.0, "ppm_threshold must be > 0");
            real_at_least(p, "noise_threshold", 0.0, "noise_threshold must be >= 0");
            real_above(p, "min_snr", 0.0, "min_snr must be > 0");
            int_at_least(p, "min_traces", 1, "min_traces must be >= 1");
            real_above(p, "baseline_window", 0.0, "baseline_window must be > 0");
            real_above(p, "max_feature_width", 0.0, "max_feature_width must be > 0");
            real_in(p, "base_quantile", 0.0, 1.0, "base_quantile must be in [0,1]");
            if (p.contains("rt_windows_min") || p.contains("rt_windows_max")) {
                if (!p.contains("rt_windows_min") || !p.contains("rt_windows_max"))
                    require(false, "rt_windows_min and rt_windows_max must be provided together");
                const Json &mins = p.at("rt_windows_min");
                const Json &maxs = p.at("rt_windows_max");
                if (!mins.is_array() || !maxs.is_array())
                    require(false, "rt_windows_min and rt_windows_max must be arrays");
                if (mins.size() != maxs.size())
                    require(false, "rt_windows_min and rt_windows_max must have equal lengths");
                for (size_t i = 0; i < mins.size(); ++i)
                    if (mins[i].get<double>() > maxs[i].get<double>())
                        require(false, "rt window min must not exceed its max");
            }
        };
    }
    if (id == "mass_spec.load_features_ms1") {
        return [=](const Json &p) {
            pair_array(p, "rt_window", "rt_window must be a two-element array");
            pair_array(p, "mz_window", "mz_window must be a two-element array");
            real_at_least(p, "min_traces_intensity", 0.0, "min_traces_intensity must be >= 0");
            real_at_least(p, "mz_clust", 0.0, "mz_clust must be >= 0");
            real_in(p, "presence", 0.0, 1.0, "presence must be in [0,1]");
        };
    }
    if (id == "mass_spec.load_features_ms2") {
        return [=](const Json &p) {
            real_at_least(p, "min_traces_intensity", 0.0, "min_traces_intensity must be >= 0");
            real_above(p, "isolation_window", 0.0, "isolation_window must be > 0");
            real_at_least(p, "mz_clust", 0.0, "mz_clust must be >= 0");
            real_in(p, "presence", 0.0, 1.0, "presence must be in [0,1]");
        };
    }
    if (id == "mass_spec.subtract_blank") {
        return [=](const Json &p) {
            real_at_least(p, "blank_threshold", 0.0, "blank_threshold must be >= 0");
            real_at_least(p, "rt_expand", 0.0, "rt_expand must be >= 0");
            real_at_least(p, "mz_expand", 0.0, "mz_expand must be >= 0");
        };
    }
    if (id == "mass_spec.filter_features") {
        return [=](const Json &p) {
            const char *reals[] = {
                "min_sn", "min_intensity", "min_area", "min_width", "max_width", "max_ppm",
                "min_fwhm_rt", "max_fwhm_rt", "min_fwhm_mz", "max_fwhm_mz",
                "min_gaussian_a", "min_gaussian_mu", "max_gaussian_mu",
                "min_gaussian_sigma", "max_gaussian_sigma", "min_gaussian_r2",
                "max_jaggedness", "min_sharpness", "min_asymmetry", "max_asymmetry", "min_plates"
            };
            for (const char *key : reals) real_at_least(p, key, 0.0, std::string(key) + " must be >= 0");
            real_in(p, "min_rel_presence_replicate", 0.0, 1.0, "min_rel_presence_replicate must be in [0,1]");
            const char *ints[] = {"max_modality", "min_size_eic", "min_size_ms1", "min_size_ms2"};
            for (const char *key : ints) int_at_least(p, key, 0, std::string(key) + " must be >= 0");
        };
    }
    if (id == "mass_spec.filter_features_ms2") {
        return [=](const Json &p) {
            int_at_least(p, "top", 0, "top must be >= 0");
            real_at_least(p, "min_intensity_ms2", 0.0, "min_intensity_ms2 must be >= 0");
            real_at_least(p, "rel_min_intensity", 0.0, "rel_min_intensity must be >= 0");
            real_at_least(p, "mz_clust", 0.0, "mz_clust must be >= 0");
            real_in(p, "blank_presence_threshold", 0.0, 1.0, "blank_presence_threshold must be in [0,1]");
            real_in(p, "global_presence_threshold", 0.0, 1.0, "global_presence_threshold must be in [0,1]");
        };
    }
    if (id == "mass_spec.group_features") {
        return [=](const Json &p) {
            if (p.contains("method")) {
                const std::string method = p.at("method").get<std::string>();
                if (!method.empty() && method != "internal_standards" && method != "obi_warp")
                    require(false, "method must be internal_standards or obi_warp");
            }
            real_at_least(p, "rt_deviation", 0.0, "rt_deviation must be >= 0");
            real_at_least(p, "ppm", 0.0, "ppm must be >= 0");
            int_at_least(p, "min_samples", 1, "min_samples must be >= 1");
            real_above(p, "bin_size", 0.0, "bin_size must be > 0");
        };
    }
    if (id == "mass_spec.fill_features") {
        return [=](const Json &p) {
            real_at_least(p, "rt_expand", 0.0, "rt_expand must be >= 0");
            real_at_least(p, "mz_expand", 0.0, "mz_expand must be >= 0");
            real_above(p, "max_peak_width", 0.0, "max_peak_width must be > 0");
            real_at_least(p, "min_traces_intensity", 0.0, "min_traces_intensity must be >= 0");
            int_at_least(p, "min_number_traces", 1, "min_number_traces must be >= 1");
            real_at_least(p, "min_intensity_ms1", 0.0, "min_intensity_ms1 must be >= 0");
            real_at_least(p, "rt_apex_deviation", 0.0, "rt_apex_deviation must be >= 0");
            real_at_least(p, "min_signal_to_noise_ratio", 0.0, "min_signal_to_noise_ratio must be >= 0");
            real_at_least(p, "min_gaussian_fit", 0.0, "min_gaussian_fit must be >= 0");
        };
    }
    if (id == "mass_spec.create_components") {
        return [=](const Json &p) {
            real_in(p, "min_correlation", -1.0, 1.0, "min_correlation must be in [-1,1]");
            pair_array(p, "rt_window", "rt_window must be a two-element array");
        };
    }
    if (id == "mass_spec.annotate_components") {
        return [=](const Json &p) {
            int_at_least(p, "max_isotopes", 1, "max_isotopes must be >= 1");
            int_at_least(p, "max_charge", 1, "max_charge must be >= 1");
            int_at_least(p, "max_gaps", 0, "max_gaps must be >= 0");
            real_at_least(p, "ppm", 0.0, "ppm must be >= 0");
        };
    }
    if (id == "mass_spec.suspect_screening" || id == "mass_spec.find_internal_standards") {
        return [=](const Json &p) {
            real_at_least(p, "ppm", 0.0, "ppm must be >= 0");
            real_at_least(p, "sec", 0.0, "sec must be >= 0");
            real_at_least(p, "ppm_ms2", 0.0, "ppm_ms2 must be >= 0");
            real_at_least(p, "mzr_ms2", 0.0, "mzr_ms2 must be >= 0");
            real_in(p, "min_cosine_similarity", 0.0, 1.0, "min_cosine_similarity must be in [0,1]");
            int_at_least(p, "min_shared_fragments", 0, "min_shared_fragments must be >= 0");
            validate_targets(p);
        };
    }
    if (id == "mass_spec.correct_matrix_suppression") {
        return [=](const Json &p) {
            real_at_least(p, "mp_rt_window", 0.0, "mp_rt_window must be >= 0");
        };
    }
    if (id == "mass_spec.filter_suspects" || id == "mass_spec.filter_internal_standards") {
        return [=](const Json &p) {
            real_at_least(p, "min_score", 0.0, "min_score must be >= 0");
            real_at_least(p, "max_error_rt", 0.0, "max_error_rt must be >= 0");
            real_at_least(p, "max_error_mass", 0.0, "max_error_mass must be >= 0");
            int_at_least(p, "min_shared_fragments", 0, "min_shared_fragments must be >= 0");
            real_in(p, "min_cosine_similarity", 0.0, 1.0, "min_cosine_similarity must be in [0,1]");
            if (p.contains("id_levels")) {
                const Json &levels = p.at("id_levels");
                if (!levels.is_array()) require(false, "id_levels must be an array");
                for (const auto &level : levels)
                    if (level.get<int>() < 1) require(false, "id_levels entries must be >= 1");
            }
        };
    }
    // Targets-like structural check for table parameters that carry "rows"
    // (transformation_products / metfrag database): a non-empty array of
    // objects with at least an id/name and one of mass/mz/formula/SMILES/InChI.
    const auto validate_rows = [=](const Json &p, const char *key) {
        if (!p.contains(key)) return;
        const Json &rows = p.at(key);
        if (!rows.is_array()) require(false, std::string(key) + " must be an array");
        for (const auto &row : rows) {
            if (!row.is_object()) require(false, std::string(key) + " entries must be objects");
            const bool has_id = row.contains("id") && row.at("id").is_string();
            const bool has_name = row.contains("name") && row.at("name").is_string();
            if (!has_id && !has_name) require(false, std::string(key) + " entries require an id or name");
            const bool has_identifier =
                (row.contains("mass") && row.at("mass").is_number()) ||
                (row.contains("mz") && row.at("mz").is_number()) ||
                (row.contains("formula") && row.at("formula").is_string()) ||
                (row.contains("SMILES") && row.at("SMILES").is_string()) ||
                (row.contains("InChI") && row.at("InChI").is_string());
            if (!has_identifier) require(false, std::string(key) + " entries require mass, mz, formula, SMILES, or InChI");
        }
    };
    if (id == "mass_spec.assign_transformation_products") {
        return [=](const Json &p) {
            real_at_least(p, "mzr_ms2", 0.0, "mzr_ms2 must be >= 0");
            if (p.contains("chromatographic_phase")) {
                const std::string phase = p.at("chromatographic_phase").get<std::string>();
                if (phase != "reverse_phase" && phase != "hilic")
                    require(false, "chromatographic_phase must be reverse_phase or hilic");
            }
            validate_rows(p, "transformation_products");
        };
    }
    if (id == "mass_spec.metfrag_screening") {
        return [=](const Json &p) {
            real_at_least(p, "ppm", 0.0, "ppm must be >= 0");
            real_at_least(p, "sec", 0.0, "sec must be >= 0");
            real_at_least(p, "ppm_ms2", 0.0, "ppm_ms2 must be >= 0");
            real_at_least(p, "mzr_ms2", 0.0, "mzr_ms2 must be >= 0");
            int_at_least(p, "top_n", 1, "top_n must be >= 1");
            int_at_least(p, "maximum_tree_depth", 1, "maximum_tree_depth must be >= 1");
            int_at_least(p, "number_threads", 1, "number_threads must be >= 1");
            if (p.contains("database_type")) {
                // R .normalize_metfrag_database_type accepts these choices,
                // case-insensitively; "Local" is mapped to "LocalCSV" at run().
                const std::string database_type = p.at("database_type").get<std::string>();
                const char *r_types[] = {"KEGG", "PubChem", "ExtendedPubChem", "Local"};
                bool ok = false;
                for (const char *t : r_types)
                    if (case_insensitive_equal(database_type, t)) ok = true;
                if (!ok) require(false, "database_type must be one of: KEGG, PubChem, ExtendedPubChem, Local");
            }
            if (p.contains("score_types") || p.contains("score_weights")) {
                if (!p.contains("score_types") || !p.contains("score_weights"))
                    require(false, "score_types and score_weights must be provided together");
                const Json &types = p.at("score_types");
                const Json &weights = p.at("score_weights");
                if (!types.is_array() || !weights.is_array())
                    require(false, "score_types and score_weights must be arrays");
                if (types.size() != weights.size())
                    require(false, "score_types and score_weights must have equal lengths");
            }
            validate_rows(p, "database");
        };
    }
    return [](const Json &) {};
}

}

void register_methods(MethodRegistry &registry) {
    const auto resolver = [](const std::string &id) -> MethodExecutor {
        if (id == "mass_spec.load_chromatograms") return processing::load_chromatograms;
        if (id == "mass_spec.filter_chromatograms_retention_time") return processing::filter_chromatograms_retention_time;
        if (id == "mass_spec.find_features") return processing_methods::find_features;
        if (id == "mass_spec.load_features_ms1") return processing_methods::load_features_ms1;
        if (id == "mass_spec.load_features_ms2") return processing_methods::load_features_ms2;
        if (id == "mass_spec.subtract_blank") return processing_methods::subtract_blank;
        if (id == "mass_spec.filter_features") return processing_methods::filter_features;
        if (id == "mass_spec.filter_features_ms2") return processing_methods::filter_features_ms2;
        if (id == "mass_spec.group_features") return processing_methods::group_features;
        if (id == "mass_spec.fill_features") return processing_methods::fill_features;
        if (id == "mass_spec.create_components") return processing_methods::create_components;
        if (id == "mass_spec.annotate_components") return processing_methods::annotate_components;
        if (id == "mass_spec.suspect_screening") return processing_methods::suspect_screening;
        if (id == "mass_spec.find_internal_standards") return processing_methods::find_internal_standards;
        if (id == "mass_spec.filter_suspects") return processing_methods::filter_suspects;
        if (id == "mass_spec.filter_internal_standards") return processing_methods::filter_internal_standards;
        if (id == "mass_spec.correct_matrix_suppression") return processing_methods::correct_matrix_suppression;
        if (id == "mass_spec.assign_transformation_products") return processing_methods::assign_transformation_products;
        if (id == "mass_spec.metfrag_screening") return processing_methods::metfrag_screening;
        return {};
    };
    const auto chromatograms = [&resolver](const std::string &id) -> MethodExecutor {
        if (id == "mass_spec.load_chromatograms" ||
            id == "mass_spec.filter_chromatograms_retention_time") return resolver(id);
        return {};
    };
    const auto nta = [&resolver](const std::string &id) -> MethodExecutor {
        if (id == "mass_spec.load_chromatograms" ||
            id == "mass_spec.filter_chromatograms_retention_time") return {};
        return resolver(id);
    };
    const auto validator = [](const std::string &id) { return detail::nta_validator(id); };
    streamfind::catalogue::register_methods("mass_spec", registry, chromatograms, validator);
    streamfind::catalogue::register_methods("mass_spec", registry, nta, validator);
}

void register_operations(OperationRegistry &registry) {
    const auto resolver = [](const std::string &id, const Json &result_schema) -> OperationExecutor {
        return [id, result_schema](streamfind::Project &project, const Json &parameters) {
            auto domain = mass_spec::Project(project);
            Json result;
            if (id == "mass_spec.add_analyses") result = domain.add_analyses(parameters);
            else if (id == "mass_spec.remove_analyses") result = domain.remove_analyses(parameters);
            else if (id == "mass_spec.get_analyses_info") result = domain.get_analyses_info(parameters);
            else if (id == "mass_spec.get_analysis_names") result = domain.get_analysis_names(parameters);
            else if (id == "mass_spec.get_replicate_names") result = domain.get_replicate_names(parameters);
            else if (id == "mass_spec.get_blank_names") result = domain.get_blank_names(parameters);
            else if (id == "mass_spec.get_concentrations") result = domain.get_concentrations(parameters);
            else if (id == "mass_spec.set_replicate_names") result = domain.set_replicate_names(parameters);
            else if (id == "mass_spec.set_blank_names") result = domain.set_blank_names(parameters);
            else if (id == "mass_spec.set_concentrations") result = domain.set_concentrations(parameters);
            else if (id == "mass_spec.get_spectra_headers") result = domain.get_spectra_headers(parameters);
            else if (id == "mass_spec.get_chromatograms_headers") result = domain.get_chromatograms_headers(parameters);
            else if (id == "mass_spec.get_spectra_tic") result = domain.get_spectra_tic(parameters);
            else if (id == "mass_spec.get_raw_spectra_eic") result = domain.get_raw_spectra_eic(parameters);
            else if (id == "mass_spec.get_raw_spectra_ms1") result = domain.get_raw_spectra_ms1(parameters);
            else if (id == "mass_spec.get_raw_spectra_ms2") result = domain.get_raw_spectra_ms2(parameters);
            else if (id == "mass_spec.get_raw_spectra") result = domain.get_raw_spectra(parameters);
            else if (id == "mass_spec.get_chromatograms") result = domain.get_chromatograms(parameters);
            else if (id == "mass_spec.get_raw_chromatograms") result = domain.get_raw_chromatograms(parameters);
            else if (id == "mass_spec.get_features") result = domain.get_features(parameters);
            else if (id == "mass_spec.get_suspects") result = domain.get_suspects(parameters);
            else if (id == "mass_spec.get_internal_standards") result = domain.get_internal_standards(parameters);
            else if (id == "mass_spec.get_transformation_products") result = domain.get_transformation_products(parameters);
            else result = domain.get_analyses_info(parameters);
            return result_schema.value("type", "") == "table" ? detail::columnar(std::move(result), result_schema) : result;
        };
    };
    const auto base = [&resolver](const std::string &id, const Json &schema) -> OperationExecutor {
        if (id.rfind("mass_spec.get_chromatograms", 0) == 0 ||
            id.rfind("mass_spec.get_raw_chromatograms", 0) == 0 ||
            id.rfind("mass_spec.load_chromatograms", 0) == 0 ||
            id.rfind("mass_spec.filter_chromatograms", 0) == 0) return {};
        if (id.rfind("mass_spec.get_features", 0) == 0 ||
            id.rfind("mass_spec.get_suspects", 0) == 0 ||
            id.rfind("mass_spec.get_internal_standards", 0) == 0 ||
            id.rfind("mass_spec.get_transformation_products", 0) == 0) return {};
        return resolver(id, schema);
    };
    const auto chromatograms = [&resolver](const std::string &id, const Json &schema) -> OperationExecutor {
        if (id.rfind("mass_spec.get_chromatograms", 0) == 0 ||
            id.rfind("mass_spec.get_raw_chromatograms", 0) == 0 ||
            id.rfind("mass_spec.load_chromatograms", 0) == 0 ||
            id.rfind("mass_spec.filter_chromatograms", 0) == 0) return resolver(id, schema);
        return {};
    };
    const auto nta = [&resolver](const std::string &id, const Json &schema) -> OperationExecutor {
        if (id.rfind("mass_spec.get_features", 0) == 0 ||
            id.rfind("mass_spec.get_suspects", 0) == 0 ||
            id.rfind("mass_spec.get_internal_standards", 0) == 0 ||
            id.rfind("mass_spec.get_transformation_products", 0) == 0) return resolver(id, schema);
        return {};
    };
    streamfind::catalogue::register_operations("mass_spec", registry, base);
    streamfind::catalogue::register_operations("mass_spec", registry, chromatograms);
    streamfind::catalogue::register_operations("mass_spec", registry, nta);
}

}
