#pragma once

#include "streamfind/export.hpp"
#include "streamfind/plugin_abi.h"
#include "streamfind/sdk/plugin_project_access.hpp"

namespace streamfind::sdk {

class STREAMFIND_SDK_API PluginHostAccess final : public PluginProjectAccess {
public:
    PluginHostAccess(const streamfind_plugin_host_api &host, void *execution_context);

    Json query(const std::string &sql) override;
    Json read(const std::string &table_name,
              const std::vector<std::string> &column_names,
              const std::string &order_by) override;
    void clear_table(const std::string &table_name) override;
    void append(const std::string &table_name,
                const std::vector<std::string> &column_names,
                const std::vector<std::vector<std::optional<std::string>>> &rows) override;
    void update_composite(
        const std::string &table_name,
        const std::vector<std::string> &key_columns,
        const std::vector<std::string> &update_columns,
        const std::vector<std::vector<std::optional<std::string>>> &rows) override;
    void delete_rows(const std::string &table_name, const std::string &key_column,
                     const std::vector<std::vector<std::optional<std::string>>> &rows) override;
    void require_table(const std::string &table_name) override;

private:
    const streamfind_plugin_host_api &host_;
    void *execution_context_;
};

}  // namespace streamfind::sdk
