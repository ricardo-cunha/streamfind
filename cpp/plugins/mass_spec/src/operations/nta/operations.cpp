#include "operations/nta/operations.hpp"
#include "operations/nta/nta_load_features_ms1.hpp"
#include "operations/nta/nta_load_features_ms2.hpp"
#include "operations/nta/nta_blank_subtraction.hpp"
#include "operations/nta/nta_filters.hpp"
#include "operations/nta/nta_alignment.hpp"
#include "operations/nta/nta_gap_filling.hpp"
#include "operations/nta/nta_componentization.hpp"
#include "operations/nta/nta_annotation.hpp"
#include "operations/nta/nta_suspect_screening.hpp"
#include "operations/nta/nta_correction_algorithms.hpp"
#include "operations/nta/nta_assign_transformation_products.hpp"
#include "operations/nta/nta_metfrag_runner.hpp"


#include "utils/base.hpp"
#include "utils/nta.hpp"

#include <algorithm>

namespace streamfind::mass_spec::nta
{
using Json = nlohmann::json;

Json get_features(sdk::PluginProjectAccess &access, const Json &parameters)
{
    auto rows = base::utils::input_rows(access, parameters, "featuresTable", utils::feature_columns(), "analysis");
    if (!parameters.value("filtered", false))
        rows.erase(std::remove_if(rows.begin(), rows.end(), [](const Json &row) { return utils::integer(row, "filtered") != 0; }), rows.end());
    return base::utils::filter_target_rows(rows, parameters, "mass", "mz", "rt", "polarity");
}

Json get_suspects(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto rows = base::utils::input_rows(access, parameters, "suspectsTable", utils::suspects_columns(), "analysis");
    return base::utils::filter_target_rows(rows, parameters, "exp_mass", nullptr, "exp_rt", "polarity");
}

Json get_internal_standards(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto rows = base::utils::input_rows(access, parameters, "internalStandardsTable", utils::internal_standards_columns(), "analysis");
    return base::utils::filter_target_rows(rows, parameters, "exp_mass", nullptr, "exp_rt", "polarity");
}

Json get_transformation_products(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto rows = base::utils::input_rows(access, parameters, "transformationProductsTable", utils::transformation_products_columns(), "analysis");
    return base::utils::filter_target_rows(rows, parameters, "mass", nullptr, nullptr, nullptr);
}
}
