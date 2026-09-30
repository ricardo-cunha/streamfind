#pragma once

#include "streamfind/export.hpp"
#include "streamfind/plugin_abi.h"
#include "streamfind/sdk/plugin_project_access.hpp"

namespace streamfind::sdk {

class STREAMFIND_SDK_API PluginHostAccess final : public PluginProjectAccess {
public:
    PluginHostAccess(const streamfind_plugin_host_api &host, void *execution_context);

    const std::filesystem::path &database_path() const noexcept override;
    std::string_view operation_instance() const noexcept override;

    Json query(const std::string &sql) override;
    Json read(const std::string &table_name,
              const std::vector<std::string> &column_names,
              const std::string &order_by) override;
    void clear_table(const std::string &table_name) override;
    void append(const std::string &table_name,
                const std::vector<std::string> &column_names,
                const std::vector<std::vector<std::optional<std::string>>> &rows) override;
    void emit_table_rows(
        const std::string &output_contract_id,
        const std::vector<std::string> &column_names,
        const std::vector<std::string> &column_types,
        const nlohmann::json &rows) override;
    void emit_table_batch(const std::string &output_contract_id,
                          const std::vector<streamfind_plugin_batch_column> &columns,
                          std::uint64_t row_count) override;
    void emit_result(const std::string &output_contract_id,
                     const std::string &payload) override;
    void update_composite(
        const std::string &table_name,
        const std::vector<std::string> &key_columns,
        const std::vector<std::string> &update_columns,
        const std::vector<std::vector<std::optional<std::string>>> &rows) override;
    void delete_rows(const std::string &table_name, const std::string &key_column,
                     const std::vector<std::vector<std::optional<std::string>>> &rows) override;
    void report_progress(double fraction, std::string_view message) override;
    void require_table(const std::string &table_name) override;

private:
    streamfind_plugin_column_type column_type(const std::string &table_name,
                                              const std::string &column_name) const;
    const streamfind_plugin_host_api &host_;
    void *execution_context_;
};

}  // namespace streamfind::sdk
