#pragma once

#include <optional>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "streamfind/export.hpp"
#include "streamfind/plugin_abi.h"
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
    using JsonBatchCallback = std::function<void(const Json &)>;

    virtual ~PluginProjectAccess() = default;

    virtual const std::filesystem::path &database_path() const noexcept = 0;
    virtual std::string_view operation_instance() const noexcept = 0;
    virtual bool is_cancelled() const noexcept = 0;

    virtual Json query(const std::string &sql) = 0;
    virtual void require_table(const std::string &table_name) = 0;
    virtual Json read(
        const std::string &table_name,
        const std::vector<std::string> &column_names,
        const std::string &order_by) = 0;
    virtual void read_batches(
        const std::string &table_name,
        const std::vector<std::string> &column_names,
        const std::string &order_by,
        const JsonBatchCallback &callback) = 0;
    virtual std::uint64_t count_rows(const std::string &table_name) = 0;
    virtual void clear_table(const std::string &table_name) = 0;
    virtual void append(
        const std::string &table_name,
        const std::vector<std::string> &column_names,
        const std::vector<std::vector<std::optional<std::string>>> &rows) = 0;
    virtual void emit_table_rows(
        const std::string &output_contract_id,
        const std::vector<std::string> &column_names,
        const std::vector<std::string> &column_types,
        const nlohmann::json &rows) = 0;
    virtual void emit_table_batch(
        const std::string &output_contract_id,
        const std::vector<streamfind_plugin_batch_column> &columns,
        std::uint64_t row_count) = 0;
    virtual void emit_result(const std::string &output_contract_id,
                             const std::string &payload) = 0;
    virtual void update_composite(
        const std::string &table_name,
        const std::vector<std::string> &key_columns,
        const std::vector<std::string> &update_columns,
        const std::vector<std::vector<std::optional<std::string>>> &rows) = 0;
    virtual void delete_rows(
        const std::string &table_name,
        const std::string &key_column,
        const std::vector<std::vector<std::optional<std::string>>> &rows) = 0;
    virtual void report_progress(double fraction, std::string_view message) = 0;
};

}  // namespace streamfind::sdk
