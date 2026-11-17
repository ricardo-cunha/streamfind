#include "utils/quantification.hpp"

#include <iostream>

using Json = nlohmann::json;

namespace streamfind::mass_spec::quantification::test_detail
{
Json targets()
{
    return Json::array({
        {{"target_id", "caffeine"}, {"role", "analyte"}, {"internal_standard_id", "caffeine_d9"},
         {"rt", 1020.0}, {"rt_tolerance", 30.0}, {"response_metric", "area"},
         {"normalization", "internal_standard"}, {"quantifier_signal_type", "MS"},
         {"quantifier_polarity", 1}, {"quantifier_precursor_mz", 195.1},
         {"quantifier_product_mz", 138.1}, {"qualifier_signal_type", "MS"},
         {"qualifier_polarity", 1}, {"qualifier_precursor_mz", 195.1},
         {"qualifier_product_mz", 110.1}},
        {{"target_id", "caffeine_d9"}, {"role", "internal_standard"}, {"rt", 1018.0},
         {"rt_tolerance", 30.0}, {"response_metric", "area"}, {"normalization", "none"},
         {"quantifier_signal_type", "MS"}, {"quantifier_polarity", 1},
         {"quantifier_precursor_mz", 204.1}, {"quantifier_product_mz", 144.1}}});
}
}

int main()
{
    const auto target_rows = streamfind::mass_spec::quantification::test_detail::targets();
    if (!streamfind::mass_spec::quantification::utils::validate_method(target_rows).empty())
        return 2;

    const Json assignments = Json::array({
        {{"analysis", "cal_5ugL"}, {"target_id", "caffeine"}, {"sample_type", "calibrator"},
         {"calibration_level", 5}, {"nominal_concentration", 5.0}},
        {{"analysis", "unknown_1"}, {"target_id", "caffeine"}, {"sample_type", "unknown"}}});
    if (!streamfind::mass_spec::quantification::utils::validate_assignments(assignments).empty())
        return 3;

    auto invalid = target_rows;
    invalid[0]["quantifier_product_mz"] = 0.0;
    if (streamfind::mass_spec::quantification::utils::validate_method(invalid).empty())
        return 4;
    invalid = assignments;
    invalid[1]["sample_type"] = "calibrator";
    invalid[1]["nominal_concentration"] = -1.0;
    if (streamfind::mass_spec::quantification::utils::validate_assignments(invalid).empty())
        return 5;
    invalid = target_rows;
    invalid[0]["internal_standard_id"] = "caffeine";
    if (streamfind::mass_spec::quantification::utils::validate_method(invalid).empty())
        return 6;

    std::cout << "quantification validation contract passed\n";
    return 0;
}
