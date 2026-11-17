#include "streamfind/sdk/plugin_host_access.hpp"
#include "streamfind/sdk/plugin_data_service.hpp"

#include <arrow/api.h>
#include <arrow/c/bridge.h>

#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::sdk {
namespace detail {

streamfind_plugin_column_type plugin_type(const arrow::DataType &type) {
    switch (type.id()) {
    case arrow::Type::BOOL: return STREAMFIND_PLUGIN_COLUMN_BOOL;
    case arrow::Type::INT64: return STREAMFIND_PLUGIN_COLUMN_INT64;
    case arrow::Type::DOUBLE: return STREAMFIND_PLUGIN_COLUMN_FLOAT64;
    case arrow::Type::TIMESTAMP: return STREAMFIND_PLUGIN_COLUMN_TIMESTAMP;
    case arrow::Type::DECIMAL128:
    case arrow::Type::DECIMAL256: return STREAMFIND_PLUGIN_COLUMN_DECIMAL;
    case arrow::Type::BINARY:
    case arrow::Type::LARGE_BINARY: return STREAMFIND_PLUGIN_COLUMN_BINARY;
    case arrow::Type::STRING:
    case arrow::Type::LARGE_STRING: return STREAMFIND_PLUGIN_COLUMN_UTF8;
    default:
        throw std::invalid_argument("Arrow type requires direct RecordBatch access: " + type.ToString());
    }
}

struct TypedReadBatch {
    PluginProjectAccess::BatchCallback callback;
    std::exception_ptr exception;
};

streamfind_plugin_status consume_typed_batch(
    void *context, struct ArrowArray *array, struct ArrowSchema *schema) {
    auto &result = *static_cast<TypedReadBatch *>(context);
    try {
        auto imported = arrow::ImportRecordBatch(array, schema);
        if (!imported.ok()) throw std::runtime_error(imported.status().ToString());
        const PluginProjectAccess::BatchView view(*imported);
        result.callback(view);
        return STREAMFIND_PLUGIN_OK;
    } catch (...) {
        result.exception = std::current_exception();
        return STREAMFIND_PLUGIN_ERROR;
    }
}

void release_arrow(struct ArrowArray &array, struct ArrowSchema &schema) noexcept {
    if (array.release != nullptr) array.release(&array);
    if (schema.release != nullptr) schema.release(&schema);
}

}  // namespace detail

PluginProjectAccess::BatchView::BatchView(std::shared_ptr<arrow::RecordBatch> batch)
    : batch_(std::move(batch)), timestamp_text_(batch_ ? batch_->num_rows() : 0) {
    if (!batch_) throw std::invalid_argument("Arrow result batch is null");
}

std::uint64_t PluginProjectAccess::BatchView::row_count() const noexcept {
    return static_cast<std::uint64_t>(batch_->num_rows());
}

std::uint32_t PluginProjectAccess::BatchView::column_count() const noexcept {
    return static_cast<std::uint32_t>(batch_->num_columns());
}

const std::shared_ptr<arrow::RecordBatch> &PluginProjectAccess::BatchView::record_batch() const noexcept {
    return batch_;
}

std::shared_ptr<arrow::Array> PluginProjectAccess::BatchView::checked_column(
    std::uint32_t column) const {
    if (column >= static_cast<std::uint32_t>(batch_->num_columns()))
        throw std::out_of_range("typed result column index is out of range");
    return batch_->column(static_cast<int>(column));
}

void PluginProjectAccess::BatchView::check_row(std::uint64_t row) const {
    if (row >= row_count()) throw std::out_of_range("typed result row index is out of range");
}

std::uint32_t PluginProjectAccess::BatchView::column_index(std::string_view name) const {
    for (int column = 0; column < batch_->num_columns(); ++column)
        if (batch_->schema()->field(column)->name() == name)
            return static_cast<std::uint32_t>(column);
    throw std::out_of_range("typed result batch has no column named " + std::string(name));
}

bool PluginProjectAccess::BatchView::is_null(std::uint32_t column, std::uint64_t row) const {
    check_row(row);
    return checked_column(column)->IsNull(static_cast<std::int64_t>(row));
}

streamfind_plugin_column_type PluginProjectAccess::BatchView::type(std::uint32_t column) const {
    return detail::plugin_type(*checked_column(column)->type());
}

std::string_view PluginProjectAccess::BatchView::text(
    std::uint32_t column, std::uint64_t row) const {
    check_row(row);
    auto array = checked_column(column);
    if (array->IsNull(static_cast<std::int64_t>(row))) return {};
    switch (array->type_id()) {
    case arrow::Type::STRING: {
        const auto value = std::static_pointer_cast<arrow::StringArray>(array)->GetView(row);
        return {value.data(), value.size()};
    }
    case arrow::Type::LARGE_STRING: {
        const auto value = std::static_pointer_cast<arrow::LargeStringArray>(array)->GetView(row);
        return {value.data(), value.size()};
    }
    case arrow::Type::BINARY: {
        const auto value = std::static_pointer_cast<arrow::BinaryArray>(array)->GetView(row);
        return {value.data(), value.size()};
    }
    case arrow::Type::LARGE_BINARY: {
        const auto value = std::static_pointer_cast<arrow::LargeBinaryArray>(array)->GetView(row);
        return {value.data(), value.size()};
    }
    case arrow::Type::TIMESTAMP:
    case arrow::Type::DECIMAL128:
    case arrow::Type::DECIMAL256:
        timestamp_text_.at(static_cast<std::size_t>(row)) = array->GetScalar(row).ValueOrDie()->ToString();
        return timestamp_text_.at(static_cast<std::size_t>(row));
    default:
        throw std::invalid_argument("typed result column is not text-like");
    }
}

std::int64_t PluginProjectAccess::BatchView::integer(
    std::uint32_t column, std::uint64_t row) const {
    check_row(row);
    auto array = checked_column(column);
    if (array->IsNull(row)) throw std::invalid_argument("typed result integer is null");
    if (array->type_id() != arrow::Type::INT64)
        throw std::invalid_argument("typed result column is not int64");
    return std::static_pointer_cast<arrow::Int64Array>(array)->Value(row);
}

double PluginProjectAccess::BatchView::real(
    std::uint32_t column, std::uint64_t row) const {
    check_row(row);
    auto array = checked_column(column);
    if (array->IsNull(row)) throw std::invalid_argument("typed result real is null");
    if (array->type_id() != arrow::Type::DOUBLE)
        throw std::invalid_argument("typed result column is not float64");
    return std::static_pointer_cast<arrow::DoubleArray>(array)->Value(row);
}

bool PluginProjectAccess::BatchView::boolean(
    std::uint32_t column, std::uint64_t row) const {
    check_row(row);
    auto array = checked_column(column);
    if (array->IsNull(row)) throw std::invalid_argument("typed result boolean is null");
    if (array->type_id() != arrow::Type::BOOL)
        throw std::invalid_argument("typed result column is not boolean");
    return std::static_pointer_cast<arrow::BooleanArray>(array)->Value(row);
}

bool PluginProjectAccess::BatchView::is_list(std::uint32_t column) const {
    const auto id = checked_column(column)->type_id();
    return id == arrow::Type::LIST || id == arrow::Type::LARGE_LIST;
}

std::uint64_t PluginProjectAccess::BatchView::list_size(
    std::uint32_t column, std::uint64_t row) const {
    check_row(row);
    if (is_null(column, row)) return 0;
    auto array = checked_column(column);
    if (array->type_id() == arrow::Type::LIST)
        return static_cast<std::uint64_t>(std::static_pointer_cast<arrow::ListArray>(array)->value_length(row));
    if (array->type_id() == arrow::Type::LARGE_LIST)
        return static_cast<std::uint64_t>(std::static_pointer_cast<arrow::LargeListArray>(array)->value_length(row));
    throw std::invalid_argument("typed result column is not a list");
}

std::shared_ptr<arrow::Array> PluginProjectAccess::BatchView::list_values(
    std::uint32_t column) const {
    auto array = checked_column(column);
    if (array->type_id() == arrow::Type::LIST)
        return std::static_pointer_cast<arrow::ListArray>(array)->values();
    if (array->type_id() == arrow::Type::LARGE_LIST)
        return std::static_pointer_cast<arrow::LargeListArray>(array)->values();
    throw std::invalid_argument("typed result column is not a list");
}

std::int64_t PluginProjectAccess::BatchView::list_value_offset(
    std::uint32_t column, std::uint64_t row) const {
    auto array = checked_column(column);
    if (array->type_id() == arrow::Type::LIST)
        return std::static_pointer_cast<arrow::ListArray>(array)->value_offset(row);
    if (array->type_id() == arrow::Type::LARGE_LIST) {
        const auto offset = std::static_pointer_cast<arrow::LargeListArray>(array)->value_offset(row);
        if (offset > std::numeric_limits<std::int64_t>::max())
            throw std::overflow_error("typed result list offset exceeds int64");
        return static_cast<std::int64_t>(offset);
    }
    throw std::invalid_argument("typed result column is not a list");
}

bool PluginProjectAccess::BatchView::list_is_null(
    std::uint32_t column, std::uint64_t row, std::uint64_t item) const {
    const auto size = list_size(column, row);
    if (item >= size) throw std::out_of_range("typed result list item index is out of range");
    return list_values(column)->IsNull(list_value_offset(column, row) + static_cast<std::int64_t>(item));
}

streamfind_plugin_column_type PluginProjectAccess::BatchView::list_child_type(
    std::uint32_t column) const {
    auto array = checked_column(column);
    if (array->type_id() != arrow::Type::LIST && array->type_id() != arrow::Type::LARGE_LIST)
        throw std::invalid_argument("typed result column is not a list");
    return detail::plugin_type(*array->type()->field(0)->type());
}

std::string_view PluginProjectAccess::BatchView::list_text(
    std::uint32_t column, std::uint64_t row, std::uint64_t item) const {
    if (list_is_null(column, row, item)) throw std::invalid_argument("typed result list item is null");
    auto values = list_values(column);
    const auto index = list_value_offset(column, row) + static_cast<std::int64_t>(item);
    switch (values->type_id()) {
    case arrow::Type::STRING: {
        const auto value = std::static_pointer_cast<arrow::StringArray>(values)->GetView(index);
        return {value.data(), value.size()};
    }
    case arrow::Type::LARGE_STRING: {
        const auto value = std::static_pointer_cast<arrow::LargeStringArray>(values)->GetView(index);
        return {value.data(), value.size()};
    }
    default: throw std::invalid_argument("typed result list child is not UTF-8");
    }
}

std::int64_t PluginProjectAccess::BatchView::list_integer(
    std::uint32_t column, std::uint64_t row, std::uint64_t item) const {
    if (list_is_null(column, row, item)) throw std::invalid_argument("typed result list item is null");
    auto values = list_values(column);
    if (values->type_id() != arrow::Type::INT64)
        throw std::invalid_argument("typed result list child is not int64");
    return std::static_pointer_cast<arrow::Int64Array>(values)->Value(
        list_value_offset(column, row) + static_cast<std::int64_t>(item));
}

double PluginProjectAccess::BatchView::list_real(
    std::uint32_t column, std::uint64_t row, std::uint64_t item) const {
    if (list_is_null(column, row, item)) throw std::invalid_argument("typed result list item is null");
    auto values = list_values(column);
    if (values->type_id() != arrow::Type::DOUBLE)
        throw std::invalid_argument("typed result list child is not float64");
    return std::static_pointer_cast<arrow::DoubleArray>(values)->Value(
        list_value_offset(column, row) + static_cast<std::int64_t>(item));
}

PluginHostAccess::PluginHostAccess(
    const streamfind_plugin_host_api &host, void *execution_context)
    : host_(host), execution_context_(execution_context) {
    if (execution_context_ == nullptr || host_.has_table == nullptr ||
        host_.read_batch == nullptr || host_.count_rows == nullptr ||
        host_.emit_table_batch == nullptr || host_.emit_result == nullptr)
        throw std::invalid_argument("incomplete plugin host API");
}

std::string_view PluginHostAccess::operation_instance() const noexcept {
    return static_cast<const PluginDataServiceContext *>(execution_context_)->operation_instance;
}

bool PluginHostAccess::is_cancelled() const noexcept {
    return host_.is_cancelled != nullptr && host_.is_cancelled(execution_context_, host_.user_data) != 0;
}

void PluginHostAccess::read_typed_batches(const std::string &table_name,
    const std::vector<std::string> &column_names, const std::string &order_by,
    const BatchCallback &callback) {
    if (table_name.empty() || column_names.empty() || order_by.empty() || !callback)
        throw std::invalid_argument("invalid dynamic batch read request");
    std::vector<const char *> names;
    names.reserve(column_names.size());
    for (const auto &name : column_names) {
        if (name.empty()) throw std::invalid_argument("empty dynamic project read column");
        names.push_back(name.c_str());
    }
    if (is_cancelled()) return;
    detail::TypedReadBatch batch{callback, {}};
    const auto status = host_.read_batch(execution_context_, table_name.data(),
        static_cast<std::uint32_t>(table_name.size()), names.data(),
        static_cast<std::uint32_t>(names.size()), 0,
        std::numeric_limits<std::uint64_t>::max(), &detail::consume_typed_batch,
        &batch, host_.user_data);
    if (batch.exception) std::rethrow_exception(batch.exception);
    if (status != STREAMFIND_PLUGIN_OK)
        throw std::runtime_error("plugin read_batch failed with status " + std::to_string(status));
}

std::uint64_t PluginHostAccess::count_rows(const std::string &table_name) {
    std::uint64_t row_count = 0;
    const auto status = host_.count_rows(execution_context_, table_name.data(),
        static_cast<uint32_t>(table_name.size()), &row_count, host_.user_data);
    if (status != STREAMFIND_PLUGIN_OK)
        throw std::runtime_error("plugin count_rows failed with status " + std::to_string(status));
    return row_count;
}

void PluginHostAccess::emit_table_batch(const std::string &output_contract_id,
    const std::shared_ptr<arrow::RecordBatch> &batch) {
    if (output_contract_id.empty() || !batch)
        throw std::invalid_argument("invalid output Arrow batch");
    struct ArrowArray array{};
    struct ArrowSchema schema{};
    const auto exported = arrow::ExportRecordBatch(*batch, &array, &schema);
    if (!exported.ok()) throw std::runtime_error(exported.ToString());
    const auto status = host_.emit_table_batch(execution_context_, output_contract_id.data(),
        static_cast<uint32_t>(output_contract_id.size()), &array, &schema, host_.user_data);
    detail::release_arrow(array, schema);
    if (status != STREAMFIND_PLUGIN_OK)
        throw std::runtime_error("workflow output batch emission failed with status " + std::to_string(status));
}

void PluginHostAccess::emit_result(const std::string &output_contract_id, const std::string &payload) {
    if (output_contract_id.empty()) throw std::invalid_argument("empty output contract id");
    const auto status = host_.emit_result(execution_context_, output_contract_id.data(),
        static_cast<uint32_t>(output_contract_id.size()), payload.data(),
        static_cast<std::uint64_t>(payload.size()), host_.user_data);
    if (status != STREAMFIND_PLUGIN_OK)
        throw std::runtime_error("workflow result emission failed with status " + std::to_string(status));
}

void PluginHostAccess::report_progress(double fraction, std::string_view message) {
    if (host_.report_progress == nullptr) return;
    const auto status = host_.report_progress(execution_context_, fraction, message.data(),
        static_cast<uint32_t>(message.size()), nullptr);
    if (status != STREAMFIND_PLUGIN_OK) throw std::runtime_error("plugin progress reporting failed");
}

void PluginHostAccess::require_table(const std::string &table_name) {
    uint8_t exists = 0;
    const auto status = host_.has_table(execution_context_, table_name.data(),
        static_cast<uint32_t>(table_name.size()), &exists, host_.user_data);
    if (status != STREAMFIND_PLUGIN_OK || exists == 0)
        throw std::runtime_error("plugin table is unavailable: " + table_name);
}

}  // namespace streamfind::sdk
