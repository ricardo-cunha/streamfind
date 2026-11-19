#include "streamfind/core_operations.hpp"

#include "streamfind/catalogue_binding.hpp"
#include "streamfind/data_proxy_protocol.hpp"
#include "streamfind/operation_execution_context.hpp"
#include "streamfind/result_table_schema.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>
#include <charconv>
#include <string_view>
#include <set>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <arrow/api.h>
#include <arrow/array/concatenate.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace streamfind::core_operations {
namespace detail {

void validate_paths(const Json &parameters) {
    const auto &paths = parameters.at("paths");
    if (!paths.is_array())
        throw std::invalid_argument("paths must be an array");
    for (const auto &path : paths) {
        if (!path.is_string() || path.get<std::string>().empty())
            throw std::invalid_argument("paths must contain non-empty strings");
    }
}

Json select_paths(OperationExecutionContext &, const Json &parameters,
                  const std::string &operation_instance, const Json &) {
    validate_paths(parameters);
    (void)operation_instance;
    return parameters.at("paths");
}

std::string quote_identifier(const std::string &value) {
    std::string quoted = "\"";
    for (const char character : value)
        quoted += character == '\"' ? "\"\"" : std::string(1, character);
    return quoted + "\"";
}

std::string quote_sql_string(const std::string &value) {
    std::string quoted = "'";
    for (const char character : value)
        quoted += character == '\'' ? "''" : std::string(1, character);
    return quoted + "'";
}

void validate_csv_path(const Json &parameters) {
    const auto path = parameters.at("csv_path").get<std::string>();
    if (path.empty())
        throw std::invalid_argument("csvPath must not be empty");
}

void validate_csv_input_path(const Json &parameters) {
    const auto path = parameters.at("csv_input_path").get<std::string>();
    if (path.empty())
        throw std::invalid_argument("csv_input_path must not be empty");
    const std::filesystem::path file(path);
    if (!std::filesystem::exists(file) || !std::filesystem::is_regular_file(file))
        throw std::invalid_argument("csv_input_path must identify an existing file");
}

Json csv_list_item_json(const std::shared_ptr<arrow::Scalar> &scalar) {
    if (!scalar || !scalar->is_valid) return nullptr;
    if (const auto string_scalar = std::dynamic_pointer_cast<arrow::StringScalar>(scalar))
        return std::string(string_scalar->view());
    if (const auto list_scalar = std::dynamic_pointer_cast<arrow::BaseListScalar>(scalar)) {
        Json values = Json::array();
        if (!list_scalar->value) return values;
        values.get_ref<Json::array_t &>().reserve(static_cast<std::size_t>(list_scalar->value->length()));
        for (std::int64_t index = 0; index < list_scalar->value->length(); ++index) {
            const auto item = list_scalar->value->GetScalar(index);
            if (!item.ok()) throw std::runtime_error(item.status().ToString());
            values.push_back(csv_list_item_json(*item));
        }
        return values;
    }

    const auto rendered = scalar->ToString();
    try {
        return Json::parse(rendered);
    } catch (...) {
        return rendered;
    }
}

void validate_json_path(const Json &parameters) {
    const auto path = parameters.at("json_path").get<std::string>();
    if (path.empty())
        throw std::invalid_argument("json_path must not be empty");
}

Json runtime_column_definition(const streamfind::RuntimeTableColumn &column);
std::string canonical_column_expression(const streamfind::RuntimeTableColumn &column,
                                       const std::string &qualified_name);

Json write_table_csv(OperationExecutionContext &context, const Json &parameters,
                     const std::string &operation_instance, const Json &inputs) {
    validate_csv_path(parameters);
    const auto input = inputs.find("anyTable");
    if (input == inputs.end() || !input->is_object())
        throw std::invalid_argument("Connect a table input.");
    const auto csv_path = parameters.at("csv_path").get<std::string>();
    std::string result_id;
    const auto result_ref = input->find("$result_ref");
    if (result_ref != input->end() && result_ref->is_string())
        result_id = result_ref->get<std::string>();
    if (result_id.empty()) {
        const auto descriptor_id = input->find("result_id");
        if (descriptor_id != input->end() && descriptor_id->is_string())
            result_id = descriptor_id->get<std::string>();
    }
    if (result_id.empty())
        throw std::invalid_argument("The connected table input must reference a published result.");
    if (context.data_proxy_session_id().empty() || context.data_proxy_run_id().empty())
        throw std::runtime_error("CSV result export requires an active service worker session.");

    static std::atomic_uint64_t request_sequence{0};
    const auto request_id = "csv-export-" + std::to_string(request_sequence.fetch_add(1));
    const auto grant_request = data_proxy::request(
        request_id, context.data_proxy_session_id(), context.data_proxy_run_id(), {},
        operation_instance, {}, Json{{"method", "lease_result_input"}, {"result_id", result_id}});
    const auto grant_response = data_proxy::Envelope::from_json(context.request_data(grant_request));
    if (!grant_response.error_code.empty())
        throw std::runtime_error(grant_response.error_code + ": " + grant_response.error_message);

    const auto schema_request = data_proxy::request(
        request_id + "-describe", context.data_proxy_session_id(), context.data_proxy_run_id(), {},
        operation_instance, {}, Json{{"method", "describe_result"}, {"result_id", result_id}});
    const auto schema_response = data_proxy::Envelope::from_json(context.request_data(schema_request));
    if (!schema_response.error_code.empty())
        throw std::runtime_error(schema_response.error_code + ": " + schema_response.error_message);
    const auto schema = schema_response.payload.value("schema", Json::array());
    if (!schema.is_array()) throw std::runtime_error("Published table result has an invalid schema.");

    static std::atomic_uint64_t export_sequence{0};
    const std::filesystem::path destination(csv_path);
    auto temporary_output = destination;
    temporary_output += ".streamfind-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(export_sequence.fetch_add(1)) + ".tmp";
    struct TemporaryOutputCleanup {
        std::filesystem::path path;
        bool published{false};
        ~TemporaryOutputCleanup() {
            if (published) return;
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{temporary_output};
    std::ofstream output(temporary_output, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output) throw std::invalid_argument("csv_path is not writable");
    const auto csv_cell = [](const Json &value) {
        if (value.is_null()) return std::string{};
        const auto text = value.is_string() ? value.get<std::string>() : value.dump();
        if (text.find_first_of(",\"\r\n") == std::string::npos) return text;
        std::string escaped = "\"";
        for (const auto character : text) escaped += character == '"' ? "\"\"" : std::string(1, character);
        return escaped + "\"";
    };
    std::vector<std::string> columns;
    columns.reserve(schema.size());
    for (const auto &column : schema) {
        if (!column.is_object() || !column.value("name", Json{}).is_string())
            throw std::runtime_error("Published table result has an invalid column definition.");
        columns.push_back(column.at("name").get<std::string>());
    }
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index != 0) output << ',';
        output << csv_cell(columns[index]);
    }
    output << "\r\n";

    const auto lease_id = grant_response.payload.value("lease_id", std::string{});
    if (lease_id.empty()) throw std::runtime_error("Service did not grant a result read handle.");
    const auto native_handle = context.receive_result_input_handle();
    data_proxy::ArrowFileBatchReader reader(native_handle);
    std::uint64_t offset = 0;
    for (;;) {
        const auto batch = reader.read_typed(offset, 4096, columns);
        if (!batch) break;
        for (std::int64_t row = 0; row < batch->num_rows(); ++row) {
            for (std::size_t index = 0; index < columns.size(); ++index) {
                if (index != 0) output << ',';
                const auto array = batch->column(static_cast<int>(index));
                if (array->IsNull(row)) continue;
                const auto scalar = array->GetScalar(row);
                if (!scalar.ok()) throw std::runtime_error(scalar.status().ToString());
                const auto string_scalar = std::dynamic_pointer_cast<arrow::StringScalar>(*scalar);
                const auto list_scalar = std::dynamic_pointer_cast<arrow::BaseListScalar>(*scalar);
                const Json value = list_scalar
                    ? csv_list_item_json(*scalar)
                    : string_scalar ? Json(std::string(string_scalar->view())) : Json((*scalar)->ToString());
                output << csv_cell(value);
            }
            output << "\r\n";
        }
        offset += static_cast<std::uint64_t>(batch->num_rows());
    }
    output.flush();
    if (!output) throw std::runtime_error("Failed while writing CSV output");
    output.close();
    if (!output) throw std::runtime_error("Failed while closing CSV output");
#ifdef _WIN32
    if (!MoveFileExW(temporary_output.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot publish CSV output (Windows error " +
                                 std::to_string(GetLastError()) + ")");
#else
    std::error_code publish_error;
    std::filesystem::rename(temporary_output, destination, publish_error);
    if (publish_error) throw std::runtime_error("cannot publish CSV output: " + publish_error.message());
#endif
    cleanup.published = true;
    (void)operation_instance;
    return Json{{"status", "finished"}, {"csvPath", csv_path}};
}

Json write_json(OperationExecutionContext &, const Json &parameters,
                const std::string &operation_instance, const Json &inputs) {
    validate_json_path(parameters);
    const auto input = inputs.find("anyJson");
    if (input == inputs.end() || !input->is_object())
        throw std::invalid_argument("Connect a JSON input.");
    const auto payload = input->find("payload");
    if (payload == input->end())
        throw std::invalid_argument("The connected JSON artifact has no payload.");
    const Json value = *payload;
    std::ofstream output(parameters.at("json_path").get<std::string>(), std::ios::out | std::ios::trunc);
    if (!output)
        throw std::invalid_argument("json_path is not writable");
    output << value.dump(2) << '\n';
    if (!output)
        throw std::runtime_error("Failed while writing JSON output");
    (void)operation_instance;
    return Json{{"status", "finished"}, {"jsonPath", parameters.at("json_path")}};
}

Json read_csv(OperationExecutionContext &context, const Json &parameters,
              const std::string &instance, const Json &) {
    validate_csv_input_path(parameters);
    const auto csv_path = parameters.at("csv_input_path").get<std::string>();
    const auto source = "read_csv_auto(" + quote_sql_string(csv_path) + ", HEADER = true, AUTO_DETECT = true)";
    const auto description = context.query("DESCRIBE SELECT * FROM " + source);
    if (description.empty())
        throw std::invalid_argument("CSV input has no columns");
    Json columns = Json::array();
    for (const auto &column : description) {
        const auto name = column.value("column_name", std::string{});
        const auto type = column.value("column_type", std::string{});
        if (name.empty() || type.empty())
            throw std::invalid_argument("CSV input returned an invalid runtime schema");
        columns.push_back(runtime_column_definition(
            streamfind::RuntimeTableColumn{name, type, streamfind::ParameterType::string, true, Json::object()}));
    }
    std::string target_columns;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index != 0) target_columns += ", ";
        const auto name = columns.at(index).at("name").get<std::string>();
        const auto runtime_type = description.at(index).value("column_type", std::string{});
        const streamfind::RuntimeTableColumn runtime_column{
            name, runtime_type, streamfind::ParameterType::string, true, Json::object()};
        target_columns += canonical_column_expression(runtime_column, quote_identifier(name)) +
                          " AS " + quote_identifier(name);
    }
    const auto select_sql = "SELECT " + target_columns + " FROM " + source;
    const auto descriptor = Json{{"contract_id", "anyTable"}, {"kind", "table"},
                                 {"format", "arrow-ipc-file"}, {"schema", columns},
                                 {"producer_operation", "streamfind.read_csv"},
                                 {"producer_instance", instance},
                                 {"workflow_revision", context.workflow_revision()}};
    const auto allocation = context.outputs().begin(descriptor);
    const auto handle = allocation.value("output_handle", std::string{});
    if (handle.empty()) throw std::runtime_error("runtime did not allocate CSV output");
    try {
        context.outputs().write_query_result(handle, 0, select_sql);
        const auto reference = context.outputs().finish(handle);
        return {{"status", "finished"}, {"emitted_results", {{"anyTable", reference}}}};
    } catch (...) {
        context.outputs().abort(handle, "CSV output failed");
        throw;
    }
}

const Json &required_input(const Json &inputs, const char *port) {
    const auto found = inputs.find(port);
    if (found == inputs.end() || !found->is_object())
        throw std::invalid_argument(std::string("Connect a table input: ") + port + ".");
    return *found;
}

std::string required_result_id(const Json &input, const char *port) {
    const auto reference = input.find("$result_ref");
    if (reference != input.end() && reference->is_string() && !reference->get<std::string>().empty())
        return reference->get<std::string>();
    const auto result_id = input.find("result_id");
    if (result_id != input.end() && result_id->is_string() && !result_id->get<std::string>().empty())
        return result_id->get<std::string>();
    throw std::invalid_argument(std::string("Table input does not reference a published result: ") + port);
}

std::string source_output_port(const Json &input, const char *fallback) {
    for (const auto *field : {"output_port_id", "output_port", "contract_id"}) {
        const auto value = input.find(field);
        if (value != input.end() && value->is_string() && !value->get<std::string>().empty())
            return value->get<std::string>();
    }
    return fallback;
}

std::uint64_t required_result_row_count(const Json &descriptor, const char *port) {
    const auto found = descriptor.find("row_count");
    if (found == descriptor.end() || (!found->is_number_unsigned() && !found->is_number_integer()))
        throw std::invalid_argument(std::string("Published table has no valid row_count: ") + port);
    if (!found->is_number_unsigned() && found->is_number_integer() && found->get<std::int64_t>() < 0)
        throw std::invalid_argument(std::string("Published table has a negative row_count: ") + port);
    return found->get<std::uint64_t>();
}

Json request_result_descriptor(OperationExecutionContext &context, const std::string &instance,
                               const std::string &result_id, const std::string &request_id) {
    const auto response = data_proxy::Envelope::from_json(context.request_data(data_proxy::request(
        request_id, context.data_proxy_session_id(), context.data_proxy_run_id(), {}, instance, {},
        Json{{"method", "describe_result"}, {"result_id", result_id}})));
    if (!response.error_code.empty())
        throw std::runtime_error(response.error_code + ": " + response.error_message);
    if (response.payload.value("kind", std::string{}) != "table" ||
        response.payload.value("format", std::string{}) != "arrow-ipc-file")
        throw std::invalid_argument("Connected result is not a published Arrow table: " + result_id);
    return response.payload;
}

std::unique_ptr<data_proxy::ArrowFileBatchReader> open_result_reader(
    OperationExecutionContext &context, const std::string &instance,
    const std::string &result_id, const std::string &request_id) {
    const auto response = data_proxy::Envelope::from_json(context.request_data(data_proxy::request(
        request_id, context.data_proxy_session_id(), context.data_proxy_run_id(), {}, instance, {},
        Json{{"method", "lease_result_input"}, {"result_id", result_id}})));
    if (!response.error_code.empty())
        throw std::runtime_error(response.error_code + ": " + response.error_message);
    if (response.payload.value("lease_id", std::string{}).empty())
        throw std::runtime_error("Service did not grant a result read handle for " + result_id);
    return std::make_unique<data_proxy::ArrowFileBatchReader>(context.receive_result_input_handle());
}

std::string upper_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return value;
}

std::shared_ptr<arrow::RecordBatch> read_aligned_window(
    data_proxy::ArrowFileBatchReader &reader, const std::shared_ptr<arrow::Schema> &schema,
    std::uint64_t offset, std::uint64_t limit) {
    std::vector<std::vector<std::shared_ptr<arrow::Array>>> columns(schema->num_fields());
    std::uint64_t rows_read = 0;
    while (rows_read < limit) {
        auto part = reader.read_typed(offset + rows_read, limit - rows_read);
        if (!part) break;
        if (part->schema()->num_fields() != schema->num_fields())
            throw std::runtime_error("Arrow input schema changed while binding columns");
        for (int index = 0; index < schema->num_fields(); ++index) {
            if (!part->schema()->field(index)->Equals(schema->field(index)))
                throw std::runtime_error("Arrow input schema does not match its published descriptor");
            columns.at(static_cast<std::size_t>(index)).push_back(part->column(index));
        }
        rows_read += static_cast<std::uint64_t>(part->num_rows());
    }
    if (rows_read == 0) return nullptr;
    std::vector<std::shared_ptr<arrow::Array>> aligned;
    aligned.reserve(columns.size());
    for (const auto &parts : columns) {
        if (parts.empty()) throw std::runtime_error("Arrow input batch omitted a declared column");
        if (parts.size() == 1) aligned.push_back(parts.front());
        else {
            auto combined = arrow::Concatenate(parts, arrow::default_memory_pool());
            if (!combined.ok()) throw std::runtime_error(combined.status().ToString());
            aligned.push_back(*combined);
        }
    }
    return arrow::RecordBatch::Make(schema, static_cast<std::int64_t>(rows_read), std::move(aligned));
}

Json runtime_column_definition(const streamfind::RuntimeTableColumn &column) {
    const auto type = upper_copy(column.duckdb_type);
    Json definition{{"name", column.name}, {"nullable", column.nullable}};
    if (type.starts_with("BOOL")) definition["type"] = "boolean";
    else if (type.starts_with("DECIMAL(")) {
        const auto comma = type.find(',');
        const auto close = type.find(')', comma == std::string::npos ? 0 : comma + 1);
        if (comma == std::string::npos || close == std::string::npos)
            throw std::invalid_argument("cannot preserve DuckDB decimal type for column " + column.name + ": " + column.duckdb_type);
        int precision = 0;
        int scale = 0;
        const auto parse_integer = [](std::string_view text, int &value) {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return !text.empty() && error == std::errc{} && end == text.data() + text.size();
        };
        const auto precision_text = std::string_view(type).substr(8, comma - 8);
        const auto scale_text = std::string_view(type).substr(comma + 1, close - comma - 1);
        if (!parse_integer(precision_text, precision) || !parse_integer(scale_text, scale))
            throw std::invalid_argument("cannot parse DuckDB decimal type for column " + column.name + ": " + column.duckdb_type);
        definition["type"] = "decimal";
        definition["precision"] = precision;
        definition["scale"] = scale;
    } else if (type.starts_with("TINYINT") || type.starts_with("SMALLINT") || type.starts_with("INTEGER") ||
               type == "INT" || type.starts_with("INT(") || type.starts_with("BIGINT")) definition["type"] = "integer";
    else if (type.starts_with("DOUBLE") || type.starts_with("FLOAT") || type.starts_with("REAL")) definition["type"] = "real";
    else if (type.starts_with("TIMESTAMP")) definition["type"] = "timestamp";
    else if (type.starts_with("BLOB")) definition["type"] = "binary";
    else if (type.ends_with("[]")) {
        const auto child = type.substr(0, type.size() - 2);
        if (child == "TINYINT" || child == "SMALLINT" || child == "INTEGER" || child == "INT" || child == "BIGINT")
            definition["items"] = Json{{"type", "integer"}};
        else if (child == "DOUBLE" || child == "FLOAT" || child == "REAL")
            definition["items"] = Json{{"type", "real"}};
        else if (child == "VARCHAR" || child == "TEXT" || child == "STRING")
            definition["items"] = Json{{"type", "string"}};
        else throw std::invalid_argument("unsupported DuckDB list element type for column " + column.name + ": " + column.duckdb_type);
        definition["type"] = "array";
    } else if (type.find("JSON") != std::string::npos || type.starts_with("STRUCT") || type.starts_with("MAP")) definition["type"] = "object";
    else if (type.starts_with("VARCHAR") || type.starts_with("TEXT") || type.starts_with("STRING") ||
             type.starts_with("UUID") || type.starts_with("DATE")) definition["type"] = "string";
    else throw std::invalid_argument("unsupported DuckDB result type for column " + column.name + ": " + column.duckdb_type);
    return definition;
}

std::string canonical_column_expression(const streamfind::RuntimeTableColumn &column,
                                       const std::string &qualified_name) {
    const auto type = upper_copy(column.duckdb_type);
    const auto cast = [&](const std::string &target) {
        return "CAST(" + qualified_name + " AS " + target + ")";
    };
    if (type.starts_with("BOOL")) return cast("BOOLEAN");
    if (type.starts_with("TINYINT") || type.starts_with("SMALLINT") || type.starts_with("INTEGER") ||
        type == "INT" || type.starts_with("INT(") || type.starts_with("BIGINT")) return cast("BIGINT");
    if (type.starts_with("DOUBLE") || type.starts_with("FLOAT") || type.starts_with("REAL")) return cast("DOUBLE");
    if (type.starts_with("TIMESTAMP")) return cast("TIMESTAMP");
    if (type.starts_with("BLOB")) return cast("BLOB");
    if (type.ends_with("[]")) {
        const auto child = type.substr(0, type.size() - 2);
        const auto item_target = child == "TINYINT" || child == "SMALLINT" || child == "INTEGER" ||
            child == "INT" || child == "BIGINT" ? "BIGINT" :
            child == "DOUBLE" || child == "FLOAT" || child == "REAL" ? "DOUBLE" : "VARCHAR";
        return "list_transform(" + qualified_name + ", value -> CAST(value AS " + item_target + "))";
    }
    if (type.starts_with("STRUCT") || type.starts_with("MAP")) return "to_json(" + qualified_name + ")";
    if (type.find("JSON") != std::string::npos || type.starts_with("VARCHAR") || type.starts_with("TEXT") ||
        type.starts_with("STRING") || type.starts_with("UUID") || type.starts_with("DATE")) return cast("VARCHAR");
    if (type.starts_with("DECIMAL(")) return qualified_name;
    throw std::invalid_argument("cannot preserve DuckDB column " + column.name + " with type " + column.duckdb_type);
}

Json bind_table_rows(OperationExecutionContext &context, const Json &, const std::string &instance, const Json &inputs) {
    const auto &left_input = required_input(inputs, "leftTable");
    const auto &right_input = required_input(inputs, "rightTable");
    const auto left_id = required_result_id(left_input, "leftTable");
    const auto right_id = required_result_id(right_input, "rightTable");
    static std::atomic_uint64_t request_sequence{0};
    const auto request_id = "bind-rows-" + std::to_string(request_sequence.fetch_add(1));
    const auto left_descriptor = request_result_descriptor(context, instance, left_id, request_id + "-left-schema");
    const auto right_descriptor = request_result_descriptor(context, instance, right_id, request_id + "-right-schema");
    const auto schema = left_descriptor.value("schema", Json::array());
    if (!schema.is_array() || schema.empty() || schema != right_descriptor.value("schema", Json::array()))
        throw std::invalid_argument("Tables must have identical column names, order, and published types.");
    const auto descriptor = Json{{"contract_id", "anyTable"}, {"kind", "table"}, {"format", "arrow-ipc-file"},
                                 {"schema", schema}, {"producer_operation", "streamfind.bind_table_rows"},
                                 {"producer_instance", instance}, {"workflow_revision", context.workflow_revision()},
                                 {"provenance", {{"lineage_inputs", Json::array({
                                     {{"result_id", left_id},
                                      {"source_port", source_output_port(left_input, "leftTable")},
                                      {"target_port", "leftTable"}},
                                     {{"result_id", right_id},
                                      {"source_port", source_output_port(right_input, "rightTable")},
                                      {"target_port", "rightTable"}}
                                 })}}}};
    const auto allocation = context.outputs().begin(descriptor);
    const auto handle = allocation.value("output_handle", std::string{});
    if (handle.empty()) throw std::runtime_error("Runtime did not allocate row-binding output.");
    try {
        auto left_reader = open_result_reader(context, instance, left_id, request_id + "-left-read");
        auto right_reader = open_result_reader(context, instance, right_id, request_id + "-right-read");
        std::uint64_t sequence = 0;
        for (auto *reader : {left_reader.get(), right_reader.get()}) {
            std::uint64_t offset = 0;
            for (;;) {
                if (context.cancellation_requested())
                    throw Error(ErrorCode::Cancelled, "row binding was cancelled");
                auto batch = reader->read_typed(offset, 4096);
                if (!batch) break;
                context.outputs().write_table_batch(handle, sequence++, batch);
                offset += static_cast<std::uint64_t>(batch->num_rows());
            }
        }
        return {{"status", "finished"}, {"emitted_results", {{"anyTable", context.outputs().finish(handle)}}}};
    } catch (...) {
        context.outputs().abort(handle, "row binding failed");
        throw;
    }
}

Json bind_table_columns(OperationExecutionContext &context, const Json &, const std::string &instance, const Json &inputs) {
    const auto &left_input = required_input(inputs, "leftTable");
    const auto &right_input = required_input(inputs, "rightTable");
    const auto left_id = required_result_id(left_input, "leftTable");
    const auto right_id = required_result_id(right_input, "rightTable");
    static std::atomic_uint64_t request_sequence{0};
    const auto request_id = "bind-columns-" + std::to_string(request_sequence.fetch_add(1));
    const auto left_descriptor = request_result_descriptor(context, instance, left_id, request_id + "-left-schema");
    const auto right_descriptor = request_result_descriptor(context, instance, right_id, request_id + "-right-schema");
    const auto left_schema = left_descriptor.value("schema", Json::array());
    const auto right_schema = right_descriptor.value("schema", Json::array());
    if (!left_schema.is_array() || left_schema.empty() || !right_schema.is_array() || right_schema.empty())
        throw std::invalid_argument("Both connected results must publish non-empty Arrow table schemas.");
    const auto left_rows = required_result_row_count(left_descriptor, "leftTable");
    const auto right_rows = required_result_row_count(right_descriptor, "rightTable");
    if (left_rows != right_rows)
        throw std::invalid_argument("Tables must have the same number of rows for horizontal column binding.");
    Json columns = left_schema;
    std::set<std::string> names;
    const auto append_validated_names = [&names](const Json &schema) {
        for (const auto &column : schema) {
            const auto name = column.value("name", std::string{});
            if (name.empty() || !names.insert(name).second)
                throw std::invalid_argument("Table column names must be non-empty and unique across both inputs: " + name);
        }
    };
    append_validated_names(left_schema);
    append_validated_names(right_schema);
    for (const auto &column : right_schema) columns.push_back(column);
    auto left_arrow_schema = result_table_arrow_schema(left_schema);
    const auto right_arrow_schema = result_table_arrow_schema(right_schema);
    std::vector<std::shared_ptr<arrow::Field>> fields = left_arrow_schema->fields();
    fields.insert(fields.end(), right_arrow_schema->fields().begin(), right_arrow_schema->fields().end());
    const auto output_schema = arrow::schema(std::move(fields));
    const auto descriptor = Json{{"contract_id", "anyTable"}, {"kind", "table"}, {"format", "arrow-ipc-file"},
                                 {"schema", columns}, {"producer_operation", "streamfind.bind_table_columns"},
                                 {"row_count", left_rows},
                                 {"producer_instance", instance}, {"workflow_revision", context.workflow_revision()},
                                 {"provenance", {{"lineage_inputs", Json::array({
                                     {{"result_id", left_id},
                                      {"source_port", source_output_port(left_input, "leftTable")},
                                      {"target_port", "leftTable"}},
                                     {{"result_id", right_id},
                                      {"source_port", source_output_port(right_input, "rightTable")},
                                      {"target_port", "rightTable"}}
                                 })}}}};
    const auto allocation = context.outputs().begin(descriptor);
    const auto handle = allocation.value("output_handle", std::string{});
    if (handle.empty()) throw std::runtime_error("Runtime did not allocate column-binding output.");
    try {
        auto left_reader = open_result_reader(context, instance, left_id, request_id + "-left-read");
        auto right_reader = open_result_reader(context, instance, right_id, request_id + "-right-read");
        std::uint64_t sequence = 0;
        for (std::uint64_t offset = 0; offset < left_rows;) {
            if (context.cancellation_requested())
                throw Error(ErrorCode::Cancelled, "column binding was cancelled");
            const auto window = std::min<std::uint64_t>(4096, left_rows - offset);
            auto left_batch = read_aligned_window(*left_reader, left_arrow_schema, offset, window);
            auto right_batch = read_aligned_window(*right_reader, right_arrow_schema, offset, window);
            if (!left_batch || !right_batch || left_batch->num_rows() != static_cast<std::int64_t>(window) ||
                right_batch->num_rows() != left_batch->num_rows())
                throw std::runtime_error("Arrow input row count does not match its published result descriptor");
            auto combined_columns = left_batch->columns();
            combined_columns.insert(combined_columns.end(), right_batch->columns().begin(), right_batch->columns().end());
            const auto batch = arrow::RecordBatch::Make(output_schema, left_batch->num_rows(), std::move(combined_columns));
            context.outputs().write_table_batch(handle, sequence++, batch);
            offset += window;
        }
        if (read_aligned_window(*left_reader, left_arrow_schema, left_rows, 1) ||
            read_aligned_window(*right_reader, right_arrow_schema, right_rows, 1))
            throw std::runtime_error("Arrow input contains more rows than its published result descriptor");
        return {{"status", "finished"}, {"emitted_results", {{"anyTable", context.outputs().finish(handle)}}}};
    } catch (...) {
        context.outputs().abort(handle, "column binding failed");
        throw;
    }
}

}  // namespace detail

void register_operations(const Json &entries, OperationRegistry &operations) {
    for (const auto &entry : entries) {
        if (entry.value("kind", "") != "operation") continue;
        const auto canonical_id = entry.value("canonical_id", "");
        if (canonical_id == "streamfind.select_paths") {
            auto definition = catalogue::operation_definition(entry);
            operations.register_operation(
                Operation(std::move(definition), &detail::select_paths, &detail::validate_paths));
        } else if (canonical_id == "streamfind.write_table_csv") {
            auto definition = catalogue::operation_definition(entry);
            operations.register_operation(
                Operation(std::move(definition), &detail::write_table_csv, &detail::validate_csv_path));
        } else if (canonical_id == "streamfind.write_json") {
            auto definition = catalogue::operation_definition(entry);
            operations.register_operation(
                Operation(std::move(definition), &detail::write_json, &detail::validate_json_path));
        } else if (canonical_id == "streamfind.read_csv") {
            auto definition = catalogue::operation_definition(entry);
            operations.register_operation(
                Operation(std::move(definition), &detail::read_csv, &detail::validate_csv_input_path));
        } else if (canonical_id == "streamfind.bind_table_rows") {
            auto definition = catalogue::operation_definition(entry);
            operations.register_operation(
                Operation(std::move(definition), &detail::bind_table_rows));
        } else if (canonical_id == "streamfind.bind_table_columns") {
            auto definition = catalogue::operation_definition(entry);
            operations.register_operation(
                Operation(std::move(definition), &detail::bind_table_columns));
        }
    }
    if (!operations.find("streamfind.select_paths"))
        throw std::invalid_argument("catalogue: core operation is missing streamfind.select_paths");
    if (!operations.find("streamfind.write_table_csv"))
        throw std::invalid_argument("catalogue: core operation is missing streamfind.write_table_csv");
    if (!operations.find("streamfind.write_json"))
        throw std::invalid_argument("catalogue: core operation is missing streamfind.write_json");
    if (!operations.find("streamfind.read_csv"))
        throw std::invalid_argument("catalogue: core operation is missing streamfind.read_csv");
    if (!operations.find("streamfind.bind_table_rows"))
        throw std::invalid_argument("catalogue: core operation is missing streamfind.bind_table_rows");
    if (!operations.find("streamfind.bind_table_columns"))
        throw std::invalid_argument("catalogue: core operation is missing streamfind.bind_table_columns");
}

}  // namespace streamfind::core_operations
