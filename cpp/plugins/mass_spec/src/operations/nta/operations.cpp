#include "operations/nta/operations.hpp"

#include "utils/base.hpp"
#include "utils/nta.hpp"

#include <algorithm>

namespace streamfind::mass_spec::nta
{
using Json = nlohmann::json;

Json get_features(sdk::PluginProjectAccess &access, const Json &parameters)
{
    auto rows = base::utils::input_rows(access, parameters, "ntaFeaturesTable", utils::feature_columns(), "analysis");
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
