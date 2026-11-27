#pragma once

#include <optional>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>
#include <arrow/record_batch.h>

#include "streamfind/export.hpp"
#include "streamfind/plugin_abi.h"
#include "streamfind/project.hpp"

namespace streamfind::sdk {

/**
 * Generic project-table access supplied to a plugin capability.
 *
 * Implementations keep the active project connection and transaction outside
 * the plugin. Result-table scans and publications use typed batches; JSON is
 * reserved for control/query responses and declared JSON documents.
 */
class STREAMFIND_SDK_API PluginProjectAccess {
public:
    /**
     * Non-owning view of one typed result batch. The view and all returned
     * string views are valid only for the duration of the batch callback.
     */
    class BatchView {
    public:
        explicit BatchView(std::shared_ptr<arrow::RecordBatch> batch);

        std::uint64_t row_count() const noexcept;
        std::uint32_t column_count() const noexcept;
        /** Access the immutable Arrow batch for types beyond the convenience accessors. */
        const std::shared_ptr<arrow::RecordBatch> &record_batch() const noexcept;
        std::uint32_t column_index(std::string_view name) const;
        bool is_null(std::uint32_t column, std::uint64_t row) const;
        streamfind_plugin_column_type type(std::uint32_t column) const;
        std::string_view text(std::uint32_t column, std::uint64_t row) const;
        std::int64_t integer(std::uint32_t column, std::uint64_t row) const;
        double real(std::uint32_t column, std::uint64_t row) const;
        bool boolean(std::uint32_t column, std::uint64_t row) const;
        bool is_list(std::uint32_t column) const;
        std::uint64_t list_size(std::uint32_t column, std::uint64_t row) const;
        bool list_is_null(std::uint32_t column, std::uint64_t row, std::uint64_t item) const;
        streamfind_plugin_column_type list_child_type(std::uint32_t column) const;
        std::string_view list_text(std::uint32_t column, std::uint64_t row,
                                   std::uint64_t item) const;
        std::int64_t list_integer(std::uint32_t column, std::uint64_t row,
                                  std::uint64_t item) const;
        double list_real(std::uint32_t column, std::uint64_t row,
                         std::uint64_t item) const;

    private:
        std::shared_ptr<arrow::Array> checked_column(std::uint32_t column) const;
        std::shared_ptr<arrow::Array> list_values(std::uint32_t column) const;
        std::int64_t list_value_offset(std::uint32_t column, std::uint64_t row) const;
        void check_row(std::uint64_t row) const;

        std::shared_ptr<arrow::RecordBatch> batch_;
        mutable std::vector<std::string> timestamp_text_;
    };

    using BatchCallback = std::function<void(const BatchView &)>;

    virtual ~PluginProjectAccess() = default;

    virtual std::string_view operation_instance() const noexcept = 0;
    virtual bool is_cancelled() const noexcept = 0;

    virtual void require_table(const std::string &result_ref) = 0;
    // Streams projected columns in the published result's row order; it does not sort rows.
    virtual void read_typed_batches(
        const std::string &result_ref,
        const std::vector<std::string> &column_names,
        const BatchCallback &callback) = 0;
    virtual std::uint64_t count_rows(const std::string &result_ref) = 0;
    virtual void emit_table_batch(
        const std::string &output_contract_id,
        const std::shared_ptr<arrow::RecordBatch> &batch) = 0;
    virtual void emit_result(const std::string &output_contract_id,
                             const std::string &payload) = 0;
    virtual void report_progress(double fraction, std::string_view message) = 0;
};

}  // namespace streamfind::sdk
