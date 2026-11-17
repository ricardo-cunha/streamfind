#include "streamfind/sdk/plugin_data_service.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <future>

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "streamfind/data_proxy_protocol.hpp"
#include <arrow/c/bridge.h>

namespace streamfind::sdk::detail {

PluginDataServiceContext *context_from(void *execution_context) {
    return static_cast<PluginDataServiceContext *>(execution_context);
}

streamfind_plugin_status consume_arrow_batch(
    const std::shared_ptr<arrow::RecordBatch> &batch,
    streamfind_plugin_consume_batch_fn consume,
    void *consumer_context) {
    struct ArrowArray array{};
    struct ArrowSchema schema{};
    const auto exported = arrow::ExportRecordBatch(*batch, &array, &schema);
    if (!exported.ok()) throw std::runtime_error(exported.ToString());
    const auto status = consume(consumer_context, &array, &schema);
    if (array.release != nullptr) array.release(&array);
    if (schema.release != nullptr) schema.release(&schema);
    return status;
}
bool allowed_table(const PluginDataServiceContext &context, const std::string &table) {
    return context.allowed_tables.empty() ||
           std::find(context.allowed_tables.begin(), context.allowed_tables.end(), table) != context.allowed_tables.end();
}

bool allowed_column(const PluginDataServiceContext &context, const std::string &table,
                    const std::string &column, bool writing) {
    const auto &scopes = writing ? context.writable_columns : context.readable_columns;
    const auto found = scopes.find(table);
    return found != scopes.end() && found->second.find(column) != found->second.end();
}

Json proxy_call(PluginDataServiceContext &context, const std::string &method, Json payload) {
    if (!context.data_proxy_request)
        throw std::invalid_argument("service data proxy is not configured");
    static std::atomic_uint64_t sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    payload["method"] = method;
    const auto response_json = context.data_proxy_request(data_proxy::request(
        "plugin-data-" + std::to_string(timestamp) + "-" +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)),
        context.data_proxy_session_id, context.data_proxy_run_id,
        context.data_proxy_operation_id,
        context.operation_instance, {}, std::move(payload)));
    const auto response = data_proxy::Envelope::from_json(response_json);
    if (!response.error_code.empty())
        throw std::runtime_error(response.error_code + ": " + response.error_message);
    if (!response.payload.is_object())
        throw std::runtime_error("data-proxy plugin response payload is not an object");
    return response.payload;
}

}  // namespace streamfind::sdk::detail

namespace streamfind::sdk {

namespace detail {

void report_data_service_error(PluginDataServiceContext &context, std::string_view message) {
    if (context.report_error) context.report_error(message);
}

}  // namespace detail

streamfind_plugin_status plugin_has_table(
    void *execution_context,
    const char *table_name,
    uint32_t table_name_size,
    uint8_t *exists,
    void *) {
    if (execution_context == nullptr || table_name == nullptr || exists == nullptr || table_name_size == 0) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    }
    auto *context = detail::context_from(execution_context);
    if (!context->data_proxy_request) {
        return STREAMFIND_PLUGIN_ERROR;
    }
    try {
        const std::string table(table_name, table_name_size);
        if (!detail::allowed_table(*context, table)) return STREAMFIND_PLUGIN_NOT_ALLOWED;
        *exists = detail::proxy_call(*context, "has_table", Json{{"table", table}}).value("exists", false) ? 1 : 0;
        return STREAMFIND_PLUGIN_OK;
    } catch (const std::exception &error) {
        detail::report_data_service_error(*context,
            std::string("plugin data-service failure: ") + error.what());
        return STREAMFIND_PLUGIN_ERROR;
    } catch (...) {
        detail::report_data_service_error(*context, "unknown plugin data-service failure");
        return STREAMFIND_PLUGIN_ERROR;
    }
}

streamfind_plugin_status plugin_read_batch(
    void *execution_context,
    const char *table_name,
    uint32_t table_name_size,
    const char *const *requested_columns,
    uint32_t column_count,
    uint64_t offset,
    uint64_t limit,
    streamfind_plugin_consume_batch_fn consume,
    void *consumer_context,
    void *) {
    if (execution_context == nullptr) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    if (table_name == nullptr || table_name_size == 0 || requested_columns == nullptr ||
        column_count == 0 || consume == nullptr) {
        detail::report_data_service_error(*context, "read_batch received an invalid host request");
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    }
    if (!context->data_proxy_request) return STREAMFIND_PLUGIN_ERROR;
    try {
        const std::string table(table_name, table_name_size);
        const auto invalid_request = [&](std::string message) {
            detail::report_data_service_error(*context, std::move(message));
            return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
        };
        if (!detail::allowed_table(*context, table)) {
            detail::report_data_service_error(*context,
                "read_batch denied access to result table '" + table + "'");
            return STREAMFIND_PLUGIN_NOT_ALLOWED;
        }
        if (table.rfind("result:", 0) != 0)
            throw std::invalid_argument("worker table inputs must reference a published result file");
        std::vector<std::string> source_column_names;
        for (uint32_t index = 0; index < column_count; ++index) {
            if (requested_columns[index] == nullptr || requested_columns[index][0] == '\0')
                return invalid_request("read_batch request column has no name");
            const std::string name(requested_columns[index]);
            // Strip table alias prefix for the allowed_column check.
            std::string bare_name = name;
            if (const auto dot = bare_name.find('.'); dot != std::string::npos)
                bare_name = bare_name.substr(dot + 1);
            if (!detail::allowed_column(*context, table, bare_name, false)) {
                detail::report_data_service_error(*context,
                    "read_batch denied access to column '" + bare_name + "' in result table '" + table + "'");
                return STREAMFIND_PLUGIN_NOT_ALLOWED;
            }
            source_column_names.push_back(std::move(bare_name));
        }
        const auto batch_limit = limit;
        std::uint64_t proxy_offset = offset;
        if (!context->receive_result_input_handle)
            throw std::runtime_error("worker result input handle receiver is not configured");
        const auto result_id = table.substr(std::string("result:").size());
        // Receive the duplicated handle while the service is servicing this
        // request. Waiting until after the HTTP response can deadlock when the
        // service's pipe write waits for the worker to read the broker message.
        auto handle_future = std::async(std::launch::async, [context] {
            return context->receive_result_input_handle();
        });
        const auto grant = detail::proxy_call(*context, "lease_result_input",
            Json{{"result_id", result_id}});
        (void)grant;
        const auto native_handle = handle_future.get();
        data_proxy::ArrowFileBatchReader reader(native_handle);
        std::uint64_t emitted = 0;
        for (;;) {
            if (context->cancelled != nullptr && context->cancelled->load())
                return STREAMFIND_PLUGIN_CANCELLED;
            const auto current_limit = std::min<std::uint64_t>(4096, batch_limit - emitted);
            const auto batch = reader.read_typed(proxy_offset, current_limit, source_column_names);
            if (!batch || batch->num_rows() == 0) break;
            const auto status = detail::consume_arrow_batch(
                batch, consume, consumer_context);
            if (status != STREAMFIND_PLUGIN_OK) return status;
            const auto count = static_cast<std::uint64_t>(batch->num_rows());
            proxy_offset += count;
            emitted += count;
            if (emitted >= batch_limit) break;
        }
        return STREAMFIND_PLUGIN_OK;
    } catch (const std::exception &error) {
        detail::report_data_service_error(*context,
                                          std::string("plugin read_batch failure: ") + error.what());
        return STREAMFIND_PLUGIN_ERROR;
    } catch (...) {
        detail::report_data_service_error(*context, "unknown plugin data-service failure");
        return STREAMFIND_PLUGIN_ERROR;
    }
}

streamfind_plugin_status plugin_count_rows(
    void *execution_context,
    const char *table_name,
    uint32_t table_name_size,
    uint64_t *row_count,
    void *) {
    if (execution_context == nullptr || table_name == nullptr || table_name_size == 0 || row_count == nullptr)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    if (!context->data_proxy_request) return STREAMFIND_PLUGIN_ERROR;
    try {
        const std::string table(table_name, table_name_size);
        if (!detail::allowed_table(*context, table)) return STREAMFIND_PLUGIN_NOT_ALLOWED;
        *row_count = detail::proxy_call(*context, "count_rows", Json{{"table", table}})
            .value("row_count", std::uint64_t{0});
        return STREAMFIND_PLUGIN_OK;
    } catch (...) {
        detail::report_data_service_error(*context, "unknown plugin row-count failure");
        return STREAMFIND_PLUGIN_ERROR;
    }
}

streamfind_plugin_status plugin_emit_table_batch(
    void *execution_context,
    const char *output_contract_id,
    uint32_t output_contract_id_size,
    struct ArrowArray *array,
    struct ArrowSchema *schema,
    void *) {
    if (execution_context == nullptr || output_contract_id == nullptr ||
        output_contract_id_size == 0 || array == nullptr || schema == nullptr)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    if (!context->emit_table_batch) return STREAMFIND_PLUGIN_NOT_ALLOWED;
    const auto status = context->emit_table_batch(execution_context,
        output_contract_id, output_contract_id_size, array, schema, nullptr);
    if (array->release != nullptr) array->release(array);
    if (schema->release != nullptr) schema->release(schema);
    return status;
}
streamfind_plugin_status plugin_emit_result(
    void *execution_context,
    const char *output_contract_id,
    uint32_t output_contract_id_size,
    const char *payload,
    uint64_t payload_size,
    void *) {
    if (execution_context == nullptr || output_contract_id == nullptr ||
        output_contract_id_size == 0 || payload == nullptr)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    if (!context->emit_result)
        return STREAMFIND_PLUGIN_NOT_ALLOWED;
    return context->emit_result(
        execution_context, output_contract_id, output_contract_id_size,
        payload, payload_size, nullptr);
}

streamfind_plugin_status plugin_report_progress(
    void *execution_context,
    double fraction,
    const char *message,
    uint32_t message_size,
    void *) {
    if (execution_context == nullptr) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    }
    auto *context = detail::context_from(execution_context);
    if (context->progress) {
        const auto normalized = std::isfinite(fraction)
            ? std::clamp(fraction, 0.0, 1.0)
            : 0.0;
        context->progress(normalized, message == nullptr ? std::string_view{} :
                                                       std::string_view(message, message_size));
    }
    return STREAMFIND_PLUGIN_OK;
}

uint8_t plugin_is_cancelled(void *execution_context, void *) {
    if (execution_context == nullptr) {
        return 1;
    }
    const auto *context = detail::context_from(execution_context);
    if (context->cancellation_requested && context->cancellation_requested()) return 1;
    return context->cancelled != nullptr && context->cancelled->load() ? 1 : 0;
}

}  // namespace streamfind::sdk
