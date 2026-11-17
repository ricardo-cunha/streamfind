#pragma once

#include "streamfind/export.hpp"
#include "streamfind/plugin_abi.h"
#include "streamfind/sdk/plugin_project_access.hpp"

namespace streamfind::sdk {

class STREAMFIND_SDK_API PluginHostAccess final : public PluginProjectAccess {
public:
    PluginHostAccess(const streamfind_plugin_host_api &host, void *execution_context);

    std::string_view operation_instance() const noexcept override;
    bool is_cancelled() const noexcept override;

    void read_typed_batches(const std::string &table_name,
                            const std::vector<std::string> &column_names,
                            const std::string &order_by,
                            const BatchCallback &callback) override;
    std::uint64_t count_rows(const std::string &table_name) override;
    void emit_table_batch(const std::string &output_contract_id,
                          const std::shared_ptr<arrow::RecordBatch> &batch) override;
    void emit_result(const std::string &output_contract_id,
                     const std::string &payload) override;
    void report_progress(double fraction, std::string_view message) override;
    void require_table(const std::string &table_name) override;

private:
    const streamfind_plugin_host_api &host_;
    void *execution_context_;
};

}  // namespace streamfind::sdk
