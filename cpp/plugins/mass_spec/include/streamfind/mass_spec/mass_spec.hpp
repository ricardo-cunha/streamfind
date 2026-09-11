#pragma once

#include "streamfind/project.hpp"
#include "streamfind/project_table_store.hpp"
#include "streamfind/export.hpp"
#include "streamfind/mass_spec/reader.hpp"

namespace streamfind::mass_spec {

struct TargetRange {
    std::string id;
    std::vector<std::string> analyses;
    std::vector<int> polarities;
    std::vector<int> levels;
    double mass_min = 0.0;
    double mass_max = 0.0;
    double mz_min = 0.0;
    double mz_max = 0.0;
    double rt_min = 0.0;
    double rt_max = 0.0;
};

struct TargetQuery {
    std::vector<TargetRange> targets;
    double ppm = 20.0;
    double rt_tolerance = 60.0;
    int charge = 1;
};

inline constexpr const char *analyses_table_name = "MASS_SPEC_ANALYSES";
inline constexpr const char *spectra_headers_table_name = "MASS_SPEC_SPECTRA_HEADERS";
inline constexpr const char *chromatograms_headers_table_name = "MASS_SPEC_CHROMATOGRAMS_HEADERS";

STREAMFIND_DOMAIN_API void install_base_schema(streamfind::ProjectTableStore &tables);

class Project {
public:
    explicit Project(streamfind::Project &project);
    void create_schema();
    Json add_analyses(const Json &parameters);
    Json remove_analyses(const Json &parameters);
    Json get_analyses_info(const Json &parameters = Json::object());
    Json get_analysis_names(const Json &parameters = Json::object());
    Json get_replicate_names(const Json &parameters = Json::object());
    Json get_blank_names(const Json &parameters = Json::object());
    Json get_concentrations(const Json &parameters = Json::object());
    Json set_replicate_names(const Json &parameters);
    Json set_blank_names(const Json &parameters);
    Json set_concentrations(const Json &parameters);
    Json get_spectra_headers(const Json &parameters = Json::object());
    Json get_chromatograms_headers(const Json &parameters = Json::object());
    Json get_spectra_tic(const Json &parameters = Json::object());
    Json get_raw_spectra(const Json &parameters = Json::object());
    Json get_raw_spectra_eic(const Json &parameters = Json::object());
    Json get_raw_spectra_ms1(const Json &parameters = Json::object());
    Json get_raw_spectra_ms2(const Json &parameters = Json::object());
    Json get_chromatograms(const Json &parameters = Json::object());
    Json get_raw_chromatograms(const Json &parameters = Json::object());
    Json get_features(const Json &parameters = Json::object());
    Json get_suspects(const Json &parameters = Json::object());
    Json get_internal_standards(const Json &parameters = Json::object());
    Json get_transformation_products(const Json &parameters = Json::object());

private:
    streamfind::Project &project_;
};

STREAMFIND_DOMAIN_API void register_methods(MethodRegistry &registry);

}