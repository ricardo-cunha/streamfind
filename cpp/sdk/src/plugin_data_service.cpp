#include "streamfind/sdk/plugin_data_service.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <chrono>
#include <iostream>
#include <limits>

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "streamfind/data_proxy_protocol.hpp"

namespace streamfind::sdk::detail {

PluginDataServiceContext *context_from(void *execution_context) {
    return static_cast<PluginDataServiceContext *>(execution_context);
}

std::optional<std::string> column_value(const streamfind_plugin_batch_column &column, uint64_t row) {
    if (column.validity_bitmap != nullptr &&
        (column.validity_bitmap[row / 8] & static_cast<uint8_t>(1u << (row % 8))) == 0) {
        return std::nullopt;
    }
    if (column.data == nullptr || column.element_size == 0) {
        throw std::invalid_argument("batch column has no data");
    }
    switch (column.type) {
    case STREAMFIND_PLUGIN_COLUMN_UTF8:
    case STREAMFIND_PLUGIN_COLUMN_TIMESTAMP:
    case STREAMFIND_PLUGIN_COLUMN_DECIMAL:
    case STREAMFIND_PLUGIN_COLUMN_BINARY: {
        if (column.element_size != sizeof(streamfind_plugin_string_view)) {
            throw std::invalid_argument("UTF-8 column has invalid element size");
        }
        const auto *values = static_cast<const streamfind_plugin_string_view *>(column.data);
        if (values[row].data == nullptr && values[row].size != 0)
            throw std::invalid_argument("UTF-8 column contains a null value pointer");
        const std::string value(values[row].data == nullptr ? "" : values[row].data, values[row].size);
        if ((column.flags & STREAMFIND_PLUGIN_COLUMN_FLAG_ARRAY) != 0) {
            const auto parsed = Json::parse(value, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array())
                throw std::invalid_argument("array column value is not a JSON array");
        }
        return value;
    }
    case STREAMFIND_PLUGIN_COLUMN_INT64: {
        if (column.element_size != sizeof(int64_t)) {
            throw std::invalid_argument("INT64 column has invalid element size");
        }
        return std::to_string(static_cast<const int64_t *>(column.data)[row]);
    }
    case STREAMFIND_PLUGIN_COLUMN_FLOAT64: {
        if (column.element_size != sizeof(double)) {
            throw std::invalid_argument("FLOAT64 column has invalid element size");
        }
        const double value = static_cast<const double *>(column.data)[row];
        // DuckDB accepts SQL NULL for unavailable numeric values, but not the
        // textual C++ spellings "nan"/"inf" produced by std::to_string.
        // Plugin table emitters use non-finite doubles for missing measurements.
        if (!std::isfinite(value))
            return std::nullopt;
        return std::to_string(value);
    }
    case STREAMFIND_PLUGIN_COLUMN_BOOL: {
        if (column.element_size != sizeof(uint8_t)) {
            throw std::invalid_argument("BOOL column has invalid element size");
        }
        return static_cast<const uint8_t *>(column.data)[row] != 0 ? "true" : "false";
    }
    default:
        throw std::invalid_argument("batch column type is unsupported");
    }
}

std::string quoted_identifier(const char *value, uint32_t size) {
    if (value == nullptr || size == 0) throw std::invalid_argument("empty identifier");
    std::string name(value, size);
    if (!(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_'))
        throw std::invalid_argument("invalid identifier");
    for (const char character : name)
        if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_'))
            throw std::invalid_argument("invalid identifier");
    return "\"" + name + "\"";
}

std::string sql_literal(const std::optional<std::string> &value) {
    if (!value) return "NULL";
    std::string escaped = "'";
    for (const char character : *value) escaped += character == '\'' ? "''" : std::string(1, character);
    return escaped + "'";
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
        context.data_proxy_session_id, context.data_proxy_run_id, {},
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

streamfind_plugin_status plugin_clear_table(
    void *execution_context, const char *table_name, uint32_t table_name_size, void *) {
    if (execution_context == nullptr || table_name == nullptr || table_name_size == 0)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    try {
        const std::string table(table_name, table_name_size);
        if (!detail::allowed_table(*context, table)) return STREAMFIND_PLUGIN_NOT_ALLOWED;
        detail::proxy_call(*context, "execute", Json{{"sql", "DELETE FROM " + detail::quoted_identifier(table_name, table_name_size)}});
        return STREAMFIND_PLUGIN_OK;
    } catch (...) {
            detail::report_data_service_error(*context, "unknown plugin data-service failure");
            return STREAMFIND_PLUGIN_ERROR;
        }
}

streamfind_plugin_status plugin_read_batch(
    void *execution_context,
    const char *table_name,
    uint32_t table_name_size,
    const streamfind_plugin_batch_column *requested_columns,
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
        if (!detail::allowed_table(*context, table)) return STREAMFIND_PLUGIN_NOT_ALLOWED;
        Json requested_names = Json::array();
        for (uint32_t index = 0; index < column_count; ++index) {
            const auto &column = requested_columns[index];
            if (column.name == nullptr || column.name_size == 0)
                return invalid_request("read_batch request column has no name");
            const std::string name(column.name, column.name_size);
            // Strip table alias prefix for the allowed_column check.
            std::string bare_name = name;
            if (const auto dot = bare_name.find('.'); dot != std::string::npos)
                bare_name = bare_name.substr(dot + 1);
            if (!detail::allowed_column(*context, table, bare_name, false)) return STREAMFIND_PLUGIN_NOT_ALLOWED;
            requested_names.push_back(bare_name);
        }
        Json rows;
        {
            rows = Json::array();
            const auto batch_limit = std::min<std::uint64_t>(
                limit == std::numeric_limits<std::uint64_t>::max() ? 16384 : limit, 16384);
            std::uint64_t proxy_offset = offset;
            while (rows.size() < batch_limit || limit == std::numeric_limits<std::uint64_t>::max()) {
                const auto encoded = detail::proxy_call(*context, "read_batch_binary",
                    Json{{"table", table}, {"columns", requested_names},
                         {"offset", proxy_offset}, {"limit", batch_limit}});
                const auto bytes = data_proxy::base64_decode(encoded.value("data", std::string{}));
                const auto batch = data_proxy::arrow_ipc_decode_rows(bytes);
                if (!batch.is_array() || batch.empty()) break;
                for (const auto &row : batch) rows.push_back(row);
                proxy_offset += batch.size();
                if (batch.size() < batch_limit || limit != std::numeric_limits<std::uint64_t>::max()) break;
            }
            if (limit != std::numeric_limits<std::uint64_t>::max() && rows.size() > limit)
                rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(limit), rows.end());
        }
        constexpr std::size_t batch_size = 4096;
        for (std::size_t begin = 0; begin < rows.size(); begin += batch_size) {
            if (context->cancelled != nullptr && context->cancelled->load())
                return STREAMFIND_PLUGIN_CANCELLED;
            const auto count = std::min(batch_size, rows.size() - begin);
            std::vector<std::vector<std::string>> text(column_count, std::vector<std::string>(count));
            std::vector<std::vector<streamfind_plugin_string_view>> strings(column_count);
            std::vector<std::vector<int64_t>> integers(column_count);
            std::vector<std::vector<double>> doubles(column_count);
            std::vector<std::vector<uint8_t>> booleans(column_count);
            std::vector<std::vector<uint8_t>> validity(column_count, std::vector<uint8_t>((count + 7) / 8, 0));
            std::vector<streamfind_plugin_batch_column> columns(column_count);
            for (uint32_t column_index = 0; column_index < column_count; ++column_index) {
                const auto &request = requested_columns[column_index];
                // A read descriptor is metadata supplied by the plugin.  The
                // proxy owns the authoritative schema, so an omitted/unknown
                // type must not turn a valid remote read into an argument
                // failure; represent it as UTF-8 text at the boundary.
                auto request_type = request.type;
                if (request_type < STREAMFIND_PLUGIN_COLUMN_UTF8 ||
                    request_type > STREAMFIND_PLUGIN_COLUMN_BINARY)
                    request_type = STREAMFIND_PLUGIN_COLUMN_UTF8;
                strings[column_index].resize(count);
                integers[column_index].resize(count);
                doubles[column_index].resize(count);
                booleans[column_index].resize(count);
                for (std::size_t row = 0; row < count; ++row) {
                    const auto &value = rows[begin + row].at(std::string(request.name, request.name_size));
                    if (value.is_null()) continue;
                    text[column_index][row] = value.is_string() ? value.get<std::string>() : value.dump();
                    const auto numeric_type = request_type == STREAMFIND_PLUGIN_COLUMN_INT64 ||
                                              request_type == STREAMFIND_PLUGIN_COLUMN_FLOAT64;
                    if (numeric_type && (text[column_index][row].empty() ||
                                         text[column_index][row] == "null" ||
                                         text[column_index][row] == "NULL"))
                        continue;
                    validity[column_index][row / 8] |= static_cast<uint8_t>(1u << (row % 8));
                    switch (request_type) {
                    case STREAMFIND_PLUGIN_COLUMN_UTF8:
                    case STREAMFIND_PLUGIN_COLUMN_TIMESTAMP:
                    case STREAMFIND_PLUGIN_COLUMN_DECIMAL:
                    case STREAMFIND_PLUGIN_COLUMN_BINARY:
                        if ((request.flags & STREAMFIND_PLUGIN_COLUMN_FLAG_ARRAY) != 0) {
                            const auto parsed = Json::parse(text[column_index][row], nullptr, false);
                            if (parsed.is_discarded() || !parsed.is_array()) return STREAMFIND_PLUGIN_SCHEMA_ERROR;
                        }
                        strings[column_index][row] = {text[column_index][row].data(),
                                                       static_cast<uint32_t>(text[column_index][row].size())};
                        break;
                    case STREAMFIND_PLUGIN_COLUMN_INT64:
                        try {
                            integers[column_index][row] = std::stoll(text[column_index][row]);
                        } catch (const std::exception &) {
                            throw std::invalid_argument("invalid integer in " + table + "." +
                                                        std::string(request.name, request.name_size) +
                                                        ": " + text[column_index][row]);
                        }
                        break;
                    case STREAMFIND_PLUGIN_COLUMN_FLOAT64:
                        try {
                            doubles[column_index][row] = std::stod(text[column_index][row]);
                        } catch (const std::exception &) {
                            throw std::invalid_argument("invalid real in " + table + "." +
                                                        std::string(request.name, request.name_size) +
                                                        ": " + text[column_index][row]);
                        }
                        break;
                    case STREAMFIND_PLUGIN_COLUMN_BOOL:
                        if (text[column_index][row] != "true" && text[column_index][row] != "false" &&
                            text[column_index][row] != "0" && text[column_index][row] != "1")
                            return STREAMFIND_PLUGIN_SCHEMA_ERROR;
                        booleans[column_index][row] = text[column_index][row] == "true" || text[column_index][row] == "1";
                        break;
                    default: break;
                    }
                }
                auto &output = columns[column_index];
                output = request;
                output.type = request_type;
                output.data = (request_type == STREAMFIND_PLUGIN_COLUMN_UTF8 ||
                               request_type == STREAMFIND_PLUGIN_COLUMN_TIMESTAMP ||
                               request_type == STREAMFIND_PLUGIN_COLUMN_DECIMAL ||
                               request_type == STREAMFIND_PLUGIN_COLUMN_BINARY) ? static_cast<const void *>(strings[column_index].data())
                           : request_type == STREAMFIND_PLUGIN_COLUMN_INT64 ? static_cast<const void *>(integers[column_index].data())
                           : request_type == STREAMFIND_PLUGIN_COLUMN_FLOAT64 ? static_cast<const void *>(doubles[column_index].data())
                           : static_cast<const void *>(booleans[column_index].data());
                output.row_count = count;
                output.element_size = (request_type == STREAMFIND_PLUGIN_COLUMN_UTF8 ||
                                       request_type == STREAMFIND_PLUGIN_COLUMN_TIMESTAMP ||
                                       request_type == STREAMFIND_PLUGIN_COLUMN_DECIMAL ||
                                       request_type == STREAMFIND_PLUGIN_COLUMN_BINARY) ? sizeof(streamfind_plugin_string_view)
                                     : request_type == STREAMFIND_PLUGIN_COLUMN_INT64 ? sizeof(int64_t)
                                     : request_type == STREAMFIND_PLUGIN_COLUMN_FLOAT64 ? sizeof(double)
                                     : sizeof(uint8_t);
                output.validity_bitmap = validity[column_index].data();
            }
            const auto status = consume(consumer_context, columns.data(), column_count, count);
            if (status != STREAMFIND_PLUGIN_OK) return status;
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

streamfind_plugin_status plugin_append_batch(
    void *execution_context,
    const char *table_name,
    uint32_t table_name_size,
    const streamfind_plugin_batch_column *columns,
    uint32_t column_count,
    uint64_t row_count,
    void *) {
    if (execution_context == nullptr || table_name == nullptr || table_name_size == 0 ||
        columns == nullptr || column_count == 0) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    }
    auto *context = detail::context_from(execution_context);
    try {
        const std::string table(table_name, table_name_size);
        if (!detail::allowed_table(*context, table)) return STREAMFIND_PLUGIN_NOT_ALLOWED;
        std::vector<std::string> names;
        names.reserve(column_count);
        for (uint32_t column_index = 0; column_index < column_count; ++column_index) {
            const auto &column = columns[column_index];
            if (column.name == nullptr || column.name_size == 0 || column.row_count != row_count) {
                return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            }
            if (!detail::allowed_column(*context, table,
                                        std::string(column.name, column.name_size), true)) {
                detail::report_data_service_error(*context,
                    "plugin output column is not allowed: " +
                    std::string(table) + "." + std::string(column.name, column.name_size));
                return STREAMFIND_PLUGIN_NOT_ALLOWED;
            }
            names.emplace_back(column.name, column.name_size);
        }
        std::vector<std::vector<std::optional<std::string>>> rows(
            static_cast<std::size_t>(row_count),
            std::vector<std::optional<std::string>>(column_count));
        for (uint32_t column_index = 0; column_index < column_count; ++column_index) {
            for (uint64_t row = 0; row < row_count; ++row) {
                rows[static_cast<std::size_t>(row)][column_index] =
                    detail::column_value(columns[column_index], row);
            }
        }
        {
            Json serialized_rows = Json::array();
            for (const auto &row : rows) {
                Json serialized_row = Json::array();
                for (const auto &value : row)
                    serialized_row.push_back(value ? Json(*value) : Json(nullptr));
                serialized_rows.push_back(std::move(serialized_row));
            }
            detail::proxy_call(*context, "append_rows",
                               Json{{"table", table}, {"columns", names}, {"rows", serialized_rows}});
        }
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

streamfind_plugin_status plugin_emit_table_batch(
    void *execution_context,
    const char *output_contract_id,
    uint32_t output_contract_id_size,
    const streamfind_plugin_batch_column *columns,
    uint32_t column_count,
    uint64_t row_count,
    void *) {
    if (execution_context == nullptr || output_contract_id == nullptr ||
        output_contract_id_size == 0 || columns == nullptr || column_count == 0)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    if (!context->emit_table_batch)
        return STREAMFIND_PLUGIN_NOT_ALLOWED;
    for (uint32_t index = 0; index < column_count; ++index) {
        if (columns[index].name == nullptr || columns[index].name_size == 0 ||
            columns[index].row_count != row_count)
            return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    }
    return context->emit_table_batch(
        execution_context, output_contract_id, output_contract_id_size,
        columns, column_count, row_count, nullptr);
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

streamfind_plugin_status plugin_update_batch(
    void *execution_context, const char *table_name, uint32_t table_name_size,
    const char *key_column, uint32_t key_column_size,
    const streamfind_plugin_batch_column *columns, uint32_t column_count,
    uint64_t row_count, uint64_t *affected_row_count, void *) {
    if (execution_context == nullptr || table_name == nullptr || key_column == nullptr || columns == nullptr ||
        affected_row_count == nullptr || table_name_size == 0 || key_column_size == 0 || column_count < 2 || row_count == 0)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    if (!context->data_proxy_request) return STREAMFIND_PLUGIN_NOT_ALLOWED;
    try {
        const std::string table_name_text(table_name, table_name_size);
        if (!detail::allowed_table(*context, table_name_text)) return STREAMFIND_PLUGIN_NOT_ALLOWED;
        const std::string key_name(key_column, key_column_size);
        if (!detail::allowed_column(*context, table_name_text, key_name, false))
            return STREAMFIND_PLUGIN_NOT_ALLOWED;
        const auto table = detail::quoted_identifier(table_name, table_name_size);
        const auto key = detail::quoted_identifier(key_column, key_column_size);
        int key_index = -1;
        std::unordered_set<std::string> names;
        for (uint32_t index = 0; index < column_count; ++index) {
            if (columns[index].name == nullptr || columns[index].name_size == 0)
                return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            if (columns[index].row_count != row_count) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            const std::string name(columns[index].name, columns[index].name_size);
            if (!names.insert(name).second) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            if (name == key_name) key_index = static_cast<int>(index);
            if (name != key_name && !detail::allowed_column(*context, table_name_text, name, true))
                return STREAMFIND_PLUGIN_NOT_ALLOWED;
        }
        if (key_index < 0) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
        std::vector<std::optional<std::string>> keys;
        std::unordered_set<std::string> unique_keys;
        for (uint64_t row = 0; row < row_count; ++row) {
            auto value = detail::column_value(columns[key_index], row);
            if (!value || !unique_keys.insert(*value).second) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            keys.push_back(std::move(value));
        }
        std::string predicate = key + " IN (";
        for (std::size_t row = 0; row < keys.size(); ++row)
            predicate += (row == 0 ? "" : ",") + detail::sql_literal(keys[row]);
        predicate += ")";
        const auto count_sql = "SELECT COUNT(*) AS affected_row_count FROM " + table + " WHERE " + predicate;
        const auto count = detail::proxy_call(*context, "query", Json{{"sql", count_sql}});
        *affected_row_count = count.value("rows", Json::array()).empty()
            ? 0 : std::stoull(count.at("rows").front().value("affected_row_count", std::string{"0"}));
        std::string sql = "UPDATE " + table + " SET ";
        bool first = true;
        for (uint32_t index = 0; index < column_count; ++index) {
            if (static_cast<int>(index) == key_index) continue;
            if (!first) sql += ", ";
            first = false;
            const auto column = detail::quoted_identifier(columns[index].name, columns[index].name_size);
            sql += column + " = CASE " + key;
            for (uint64_t row = 0; row < row_count; ++row)
                sql += " WHEN " + detail::sql_literal(keys[static_cast<std::size_t>(row)]) +
                       " THEN " + detail::sql_literal(detail::column_value(columns[index], row));
            sql += " ELSE " + column + " END";
        }
        sql += " WHERE " + predicate;
        detail::proxy_call(*context, "execute", Json{{"sql", sql}});
        return STREAMFIND_PLUGIN_OK;
    } catch (const std::invalid_argument &) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    } catch (...) {
            detail::report_data_service_error(*context, "unknown plugin data-service failure");
            return STREAMFIND_PLUGIN_ERROR;
        }
}

streamfind_plugin_status plugin_update_composite_batch(
    void *execution_context, const char *table_name, uint32_t table_name_size,
    const streamfind_plugin_batch_column *key_columns, uint32_t key_column_count,
    const streamfind_plugin_batch_column *columns, uint32_t column_count,
    uint64_t row_count, uint64_t *affected_row_count, void *) {
    if (execution_context == nullptr || table_name == nullptr || key_columns == nullptr || columns == nullptr ||
        affected_row_count == nullptr || table_name_size == 0 || key_column_count == 0 || column_count == 0 || row_count == 0)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    if (!context->data_proxy_request) return STREAMFIND_PLUGIN_NOT_ALLOWED;
    try {
        const std::string table_name_text(table_name, table_name_size);
        if (!detail::allowed_table(*context, table_name_text)) {
            return STREAMFIND_PLUGIN_NOT_ALLOWED;
        }
        std::unordered_set<std::string> all_names;
        for (uint32_t index = 0; index < key_column_count; ++index) {
            const auto &column = key_columns[index];
            if (column.name == nullptr || column.name_size == 0 || column.row_count != row_count)
                return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            const std::string name(column.name, column.name_size);
            if (!all_names.insert(name).second || !detail::allowed_column(*context, table_name_text, name, false)) {
                return STREAMFIND_PLUGIN_NOT_ALLOWED;
            }
        }
        for (uint32_t index = 0; index < column_count; ++index) {
            const auto &column = columns[index];
            if (column.name == nullptr || column.name_size == 0 || column.row_count != row_count)
                return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            const std::string name(column.name, column.name_size);
            if (!all_names.insert(name).second || !detail::allowed_column(*context, table_name_text, name, true)) {
                return STREAMFIND_PLUGIN_NOT_ALLOWED;
            }
        }
        const auto table = detail::quoted_identifier(table_name, table_name_size);
        std::vector<std::string> predicates;
        std::unordered_set<std::string> unique_keys;
        for (uint64_t row = 0; row < row_count; ++row) {
            std::string predicate = "(";
            std::string identity;
            for (uint32_t key = 0; key < key_column_count; ++key) {
                const auto value = detail::column_value(key_columns[key], row);
                if (!value) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
                if (key != 0) { predicate += " AND "; identity += '\x1f'; }
                predicate += detail::quoted_identifier(key_columns[key].name, key_columns[key].name_size) +
                             " = " + detail::sql_literal(value);
                identity += *value;
            }
            predicate += ")";
            if (!unique_keys.insert(identity).second) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            predicates.push_back(std::move(predicate));
        }
        std::string where;
        for (std::size_t row = 0; row < predicates.size(); ++row)
            where += (row == 0 ? "" : " OR ") + predicates[row];
        const auto count_sql = "SELECT COUNT(*) AS affected_row_count FROM " + table + " WHERE " + where;
        const auto count = detail::proxy_call(*context, "query", Json{{"sql", count_sql}});
        *affected_row_count = count.value("rows", Json::array()).empty()
            ? 0 : std::stoull(count.at("rows").front().value("affected_row_count", std::string{"0"}));

        std::string sql = "UPDATE " + table + " SET ";
        for (uint32_t column = 0; column < column_count; ++column) {
            if (column != 0) sql += ", ";
            const auto name = detail::quoted_identifier(columns[column].name, columns[column].name_size);
            sql += name + " = CASE";
            for (uint64_t row = 0; row < row_count; ++row)
                sql += " WHEN " + predicates[static_cast<std::size_t>(row)] + " THEN " +
                       detail::sql_literal(detail::column_value(columns[column], row));
            sql += " ELSE " + name + " END";
        }
        const auto update_sql = sql + " WHERE " + where;
        detail::proxy_call(*context, "execute", Json{{"sql", update_sql}});
        return STREAMFIND_PLUGIN_OK;
    } catch (const std::invalid_argument &) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    } catch (...) {
            detail::report_data_service_error(*context, "unknown plugin data-service failure");
            return STREAMFIND_PLUGIN_ERROR;
        }
}

streamfind_plugin_status plugin_delete_batch(
    void *execution_context, const char *table_name, uint32_t table_name_size,
    const char *key_column, uint32_t key_column_size,
    const streamfind_plugin_batch_column *keys, uint64_t row_count,
    uint64_t *affected_row_count, void *) {
    if (execution_context == nullptr || table_name == nullptr || key_column == nullptr || keys == nullptr ||
        affected_row_count == nullptr || table_name_size == 0 || key_column_size == 0 || row_count == 0)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    if (!context->data_proxy_request) return STREAMFIND_PLUGIN_NOT_ALLOWED;
    try {
        const std::string table_name_text(table_name, table_name_size);
        if (!detail::allowed_table(*context, table_name_text) ||
            !detail::allowed_column(*context, table_name_text,
                                    std::string(key_column, key_column_size), false))
            return STREAMFIND_PLUGIN_NOT_ALLOWED;
        if (keys->name == nullptr || keys->name_size == 0)
            return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
        if (keys->row_count != row_count || std::string(keys->name, keys->name_size) !=
            std::string(key_column, key_column_size)) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
        std::unordered_set<std::string> unique;
        std::string predicate = detail::quoted_identifier(key_column, key_column_size) + " IN (";
        for (uint64_t row = 0; row < row_count; ++row) {
            const auto value = detail::column_value(*keys, row);
            if (!value || !unique.insert(*value).second) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            if (row != 0) predicate += ",";
            predicate += detail::sql_literal(value);
        }
        predicate += ")";
        const auto table = detail::quoted_identifier(table_name, table_name_size);
        const auto count_sql = "SELECT COUNT(*) AS affected_row_count FROM " + table + " WHERE " + predicate;
        const auto count = detail::proxy_call(*context, "query", Json{{"sql", count_sql}});
        *affected_row_count = count.value("rows", Json::array()).empty()
            ? 0 : std::stoull(count.at("rows").front().value("affected_row_count", std::string{"0"}));
        detail::proxy_call(*context, "execute", Json{{"sql", "DELETE FROM " + table + " WHERE " + predicate}});
        return STREAMFIND_PLUGIN_OK;
    } catch (const std::invalid_argument &) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    } catch (...) {
            detail::report_data_service_error(*context, "unknown plugin data-service failure");
            return STREAMFIND_PLUGIN_ERROR;
        }
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
