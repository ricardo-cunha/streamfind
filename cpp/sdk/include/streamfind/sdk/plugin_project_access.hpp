#pragma once

#include <optional>
#include <string>
#include <vector>

#include "streamfind/export.hpp"
#include "streamfind/project.hpp"

namespace streamfind::sdk {

/**
 * Generic project-table access supplied to a plugin capability.
 *
 * Implementations keep the active project connection and transaction outside
 * the plugin. The current string-row representation is an internal bridge;
 * typed column batches are the next ABI-facing refinement.
 */
class STREAMFIND_SDK_API PluginProjectAccess {
public:
    virtual ~PluginProjectAccess() = default;

    virtual Json query(const std::string &sql) = 0;
    virtual void require_table(const std::string &table_name) = 0;
    virtual Json read(
        const std::string &table_name,
        const std::vector<std::string> &column_names,
        const std::string &order_by) = 0;
    virtual void clear_table(const std::string &table_name) = 0;
    virtual void append(
        const std::string &table_name,
        const std::vector<std::string> &column_names,
        const std::vector<std::vector<std::optional<std::string>>> &rows) = 0;
    virtual void update_composite(
        const std::string &table_name,
        const std::vector<std::string> &key_columns,
        const std::vector<std::string> &update_columns,
        const std::vector<std::vector<std::optional<std::string>>> &rows) = 0;
    virtual void delete_rows(
        const std::string &table_name,
        const std::string &key_column,
        const std::vector<std::vector<std::optional<std::string>>> &rows) = 0;
};

}  // namespace streamfind::sdk
