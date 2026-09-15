#include "operations/operations.hpp"
#include <string>
#include <vector>

namespace streamfind::mass_spec::operations
{

    namespace nta_detail
    {
        struct TableSpec
        {
            const char *table;
            std::vector<const char *> mass_columns;
            std::vector<const char *> mz_columns;
            std::vector<const char *> rt_columns;
            bool has_polarity;
            const char *order_by;
        };

        Json query_table(sdk::PluginProjectAccess &access, const Json &parameters, const TableSpec &spec)
        {
            access.require_table(spec.table);
            const auto targets = streamfind::mass_spec::detail::normalize_targets_for_operation(parameters);
            std::vector<std::string> filters;
            auto add_or = [](const std::vector<std::string> &items)
            {
                std::string result = "(";
                for (std::size_t i = 0; i < items.size(); ++i)
                    result += (i ? " OR " : "") + items[i];
                return result + ")";
            };
            std::vector<std::string> target_filters;
            for (const auto &target : targets)
            {
                std::vector<std::string> match;
                if (!target.analyses.empty())
                {
                    std::vector<std::string> conditions;
                    for (const auto &analysis : target.analyses)
                        conditions.push_back("analysis = " + streamfind::mass_spec::detail::sql(analysis));
                    match.push_back(add_or(conditions));
                }
                for (auto &condition : streamfind::mass_spec::detail::target_sql_conditions(target, spec.mass_columns, spec.mz_columns, spec.rt_columns, spec.has_polarity))
                    match.push_back(std::move(condition));
                if (!match.empty())
                    target_filters.push_back(add_or(match));
            }
            if (!target_filters.empty())
                filters.push_back(add_or(target_filters));
            std::string query = "SELECT * FROM " + std::string(spec.table) + " WHERE 1=1";
            for (const auto &filter : filters)
                query += " AND " + filter;
            return access.query(query + " ORDER BY " + spec.order_by);
        }
    }

    Json get_features(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        access.require_table("MASS_SPEC_NTA_FEATURES");
        const auto targets = streamfind::mass_spec::detail::normalize_targets_for_operation(parameters);
        std::vector<std::string> filters;
        if (!parameters.value("filtered", false))
            filters.push_back("filtered = FALSE");
        auto add_or = [](const std::vector<std::string> &items)
        {
            std::string result = "(";
            for (std::size_t i = 0; i < items.size(); ++i)
                result += (i ? " OR " : "") + items[i];
            return result + ")";
        };
        std::vector<std::string> target_filters;
        for (const auto &target : targets)
        {
            std::vector<std::string> match;
            if (!target.analyses.empty())
            {
                std::vector<std::string> conditions;
                for (const auto &analysis : target.analyses)
                    conditions.push_back("analysis = " + streamfind::mass_spec::detail::sql(analysis));
                match.push_back(add_or(conditions));
            }
            for (auto &condition : streamfind::mass_spec::detail::target_sql_conditions(target, {"mass"}, {"mz"}, {"rt"}, true))
                match.push_back(std::move(condition));
            if (!match.empty())
                target_filters.push_back(add_or(match));
        }
        if (!target_filters.empty())
            filters.push_back(add_or(target_filters));
        std::string query = "SELECT * FROM MASS_SPEC_NTA_FEATURES WHERE 1=1";
        for (const auto &filter : filters)
            query += " AND " + filter;
        return access.query(query + " ORDER BY analysis, rt, feature");
    }

    Json get_internal_standards(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return nta_detail::query_table(access, parameters, {"MASS_SPEC_NTA_INTERNAL_STANDARDS", {"db_mass", "exp_mass"}, {}, {"db_rt", "exp_rt"}, true, "analysis"});
    }

    Json get_suspects(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return nta_detail::query_table(access, parameters, {"MASS_SPEC_NTA_SUSPECTS", {"db_mass", "exp_mass"}, {}, {"db_rt", "exp_rt"}, true, "analysis"});
    }

    Json get_transformation_products(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return nta_detail::query_table(access, parameters, {"MASS_SPEC_NTA_TRANSFORMATION_PRODUCTS", {"mass"}, {}, {}, false, "analysis"});
    }

} // namespace streamfind::mass_spec::operations
