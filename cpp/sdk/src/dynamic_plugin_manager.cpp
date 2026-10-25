#include "streamfind/sdk/dynamic_plugin_manager.hpp"
#include "streamfind/operation_execution_context.hpp"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cctype>

#include <cstring>
#include <optional>
#include <fstream>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <unordered_map>

#include "streamfind/catalogue.hpp"
#include "streamfind/sdk/plugin_data_service.hpp"

namespace streamfind::sdk::detail {

class OperationLogStreambuf final : public std::streambuf {
public:
    explicit OperationLogStreambuf(std::function<void(std::string_view)> callback)
        : callback_(std::move(callback)) {}
    ~OperationLogStreambuf() override { if (!buffer_.empty()) callback_(buffer_); }

protected:
    int_type overflow(int_type character) override {
        if (character == traits_type::eof()) return traits_type::not_eof(character);
        buffer_.push_back(static_cast<char>(character));
        if (character == '\n') flush_line();
        return character;
    }
    std::streamsize xsputn(const char *data, std::streamsize size) override {
        buffer_.append(data, static_cast<std::size_t>(size));
        std::size_t newline;
        while ((newline = buffer_.find('\n')) != std::string::npos) {
            callback_(std::string_view(buffer_).substr(0, newline));
            buffer_.erase(0, newline + 1);
        }
        return size;
    }

private:
    void flush_line() {
        if (!buffer_.empty() && buffer_.back() == '\n') buffer_.pop_back();
        if (!buffer_.empty()) callback_(buffer_);
        buffer_.clear();
    }
    std::function<void(std::string_view)> callback_;
    std::string buffer_;
};

std::mutex operation_log_stream_mutex;

class ScopedCerrRedirect final {
public:
    explicit ScopedCerrRedirect(std::streambuf *replacement)
        : previous_(std::cerr.rdbuf(replacement)) {}
    ~ScopedCerrRedirect() { std::cerr.rdbuf(previous_); }

private:
    std::streambuf *previous_;
};

std::string dynamic_platform_tag() {
#if defined(_WIN32)
    return "windows-x86_64";
#elif defined(__linux__)
    return "linux-x86_64";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unknown";
#endif
}

bool supports_platform(const PluginManifest &manifest, const std::string &platform) {
    for (const auto &candidate : manifest.platforms) {
        if (candidate == platform) {
            return true;
        }
    }
    return false;
}

}  // namespace streamfind::sdk::detail

namespace streamfind::sdk {

namespace detail {

streamfind_plugin_column_type dynamic_column_type(const std::string &duckdb_type) {
    std::string type;
    type.reserve(duckdb_type.size());
    for (const auto character : duckdb_type)
        type.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    if (type == "BOOLEAN") return STREAMFIND_PLUGIN_COLUMN_BOOL;
    if (type.find("INT") != std::string::npos || type == "HUGEINT" || type == "UBIGINT")
        return STREAMFIND_PLUGIN_COLUMN_INT64;
    if (type.find("DOUBLE") != std::string::npos || type.find("FLOAT") != std::string::npos ||
        type.find("DECIMAL") != std::string::npos || type == "REAL")
        return STREAMFIND_PLUGIN_COLUMN_FLOAT64;
    if (type.find("TIMESTAMP") != std::string::npos || type == "DATE" || type == "TIME")
        return STREAMFIND_PLUGIN_COLUMN_TIMESTAMP;
    return STREAMFIND_PLUGIN_COLUMN_UTF8;
}

std::string quoted_identifier(const std::string &value) {
    std::string result = "\"";
    for (const auto character : value)
        result += character == '"' ? "\"\"" : std::string(1, character);
    result += '"';
    return result;
}

Json table_batch_rows(const streamfind_plugin_batch_column *columns,
                      uint32_t column_count, uint64_t row_count) {
    Json rows = Json::array();
    for (uint64_t row = 0; row < row_count; ++row) {
        Json value = Json::object();
        for (uint32_t column_index = 0; column_index < column_count; ++column_index) {
            const auto &column = columns[column_index];
            if (column.name == nullptr || column.name_size == 0 || column.data == nullptr ||
                column.element_size == 0)
                throw std::invalid_argument("table output contains an invalid column");
            const auto valid = column.validity_bitmap == nullptr ||
                (column.validity_bitmap[row / 8] & static_cast<uint8_t>(1u << (row % 8))) != 0;
            const std::string name(column.name, column.name_size);
            if (!valid) {
                value[name] = nullptr;
                continue;
            }
            switch (column.type) {
            case STREAMFIND_PLUGIN_COLUMN_UTF8:
            case STREAMFIND_PLUGIN_COLUMN_TIMESTAMP:
            case STREAMFIND_PLUGIN_COLUMN_DECIMAL:
            case STREAMFIND_PLUGIN_COLUMN_BINARY: {
                if (column.element_size != sizeof(streamfind_plugin_string_view))
                    throw std::invalid_argument("table output string column has invalid element size");
                const auto &item = static_cast<const streamfind_plugin_string_view *>(column.data)[row];
                const std::string text(item.data == nullptr ? "" : item.data, item.size);
                if ((column.flags & STREAMFIND_PLUGIN_COLUMN_FLAG_ARRAY) != 0) {
                    const auto array = Json::parse(text, nullptr, false);
                    if (array.is_discarded() || !array.is_array())
                        throw std::invalid_argument("table output array column is not valid JSON");
                    value[name] = array;
                } else value[name] = text;
                break;
            }
            case STREAMFIND_PLUGIN_COLUMN_INT64:
                if (column.element_size != sizeof(int64_t))
                    throw std::invalid_argument("table output integer column has invalid element size");
                value[name] = static_cast<const int64_t *>(column.data)[row];
                break;
            case STREAMFIND_PLUGIN_COLUMN_FLOAT64:
                if (column.element_size != sizeof(double))
                    throw std::invalid_argument("table output real column has invalid element size");
                value[name] = static_cast<const double *>(column.data)[row];
                break;
            case STREAMFIND_PLUGIN_COLUMN_BOOL:
                if (column.element_size != sizeof(uint8_t))
                    throw std::invalid_argument("table output boolean column has invalid element size");
                value[name] = static_cast<const uint8_t *>(column.data)[row] != 0;
                break;
            default:
                throw std::invalid_argument("table output column type is unsupported");
            }
        }
        rows.push_back(std::move(value));
    }
    return rows;
}

Json invoke_dynamic(
    DynamicPluginLoadResult &plugin,
    OperationExecutionContext &execution_context,
    const std::string &capability_id,
    const Json &parameters,
    const Json &output_ports,
    const std::string &workflow_instance,
    const Json &input_artifacts) {
    if (workflow_instance.empty()) {
        throw Error(ErrorCode::MethodExecution,
                    "workflow operation invocation requires an operation instance id");
    }
    if (plugin.plugin.invoke == nullptr || plugin.plugin.release_buffer == nullptr) {
        throw Error(ErrorCode::MethodExecution, "dynamic plugin has no JSON invocation contract");
    }
    if (!execution_context.has_output_sink()) {
        throw Error(ErrorCode::MethodExecution,
                    "dynamic plugin execution requires the supervised worker runtime and data proxy");
    }
    const auto workflow_revision = execution_context.workflow_revision();
    Json runtime_inputs = input_artifacts;
    if (runtime_inputs.is_object()) {
        for (auto &[port, artifact] : runtime_inputs.items()) {
            if (!artifact.is_object()) continue;
            const auto result_id = artifact.value("result_id", std::string{});
            if (!result_id.empty() && !artifact.contains("physical_table"))
                artifact["physical_table"] = "result:" + result_id;
        }
    }
    Json request = {{"capability_id", capability_id}, {"parameters", parameters},
                    {"inputs", runtime_inputs}};
    Json response;
    Json emitted_results = Json::object();
    std::unordered_map<std::string, std::string> table_output_handles;
    std::unordered_map<std::string, std::uint64_t> table_output_sequences;
    const auto manifest = !plugin.manifest.domain.empty()
        ? catalogue::table_manifest_json(plugin.manifest.domain, "")
        : std::optional<Json>{};

    OperationOutputSink *operation_output_sink = &execution_context.outputs();
    auto execute_body = [&]() {
        PluginDataServiceContext context;
        std::atomic_bool cancelled{false};
        const auto producer_instance = workflow_instance;
        std::vector<std::tuple<std::string, std::string, std::string>> lineage_inputs;
        if (runtime_inputs.is_object()) {
            for (const auto &[target_port, artifact] : runtime_inputs.items()) {
                if (!artifact.is_object()) continue;
                lineage_inputs.emplace_back(
                    artifact.value("result_id", artifact.value("result_id", "")),
                    artifact.value("contract_id", ""), target_port);
            }
        }
        context.operation_instance = workflow_instance;
        context.data_proxy_session_id = execution_context.data_proxy_session_id();
        context.data_proxy_run_id = execution_context.data_proxy_run_id();
        context.cancelled = &cancelled;
        context.cancellation_requested = [&execution_context] { return execution_context.cancellation_requested(); };
        context.progress = [&execution_context](double fraction, std::string_view message) {
            if (message.empty()) return;
            execution_context.report_progress(fraction, message);
        };
        plugin.runtime_diagnostics.clear();
        context.report_error = [&plugin](std::string_view message) {
            plugin.runtime_diagnostics.assign(message.data(), message.size());
        };
        context.data_proxy_request = [&execution_context](const data_proxy::Envelope &request) {
            return execution_context.request_data(request);
        };
        context.output_sink = operation_output_sink;
        if (manifest) {
            for (const auto &table : *manifest) {
                const auto table_name = table.value("table_name", std::string{});
                for (const auto &column : table.value("columns", Json::array())) {
                    const auto column_name = column.value("name", std::string{});
                    if (column_name.empty()) continue;
                    context.readable_columns[table_name].insert(column_name);
                    context.writable_columns[table_name].insert(column_name);
                    const auto semantic_type = column.value("type", std::string{"string"});
                    const auto type = semantic_type == "integer"
                        ? STREAMFIND_PLUGIN_COLUMN_INT64
                        : semantic_type == "boolean"
                        ? STREAMFIND_PLUGIN_COLUMN_BOOL
                        : semantic_type == "real"
                        ? STREAMFIND_PLUGIN_COLUMN_FLOAT64
                        : semantic_type == "timestamp"
                        ? STREAMFIND_PLUGIN_COLUMN_TIMESTAMP
                        : STREAMFIND_PLUGIN_COLUMN_UTF8;
                    context.column_types[table_name][column_name] = type;
                }
            }
        }
        if (manifest && runtime_inputs.is_object()) {
            for (const auto &[target_port, artifact] : runtime_inputs.items()) {
                if (!artifact.is_object()) continue;
                const auto physical_table = artifact.value("physical_table", std::string{});
                const auto contract = artifact.value("contract_id", std::string{});
                if (physical_table.empty() || contract.empty()) continue;
                bool schema_found = false;
                for (const auto &table : *manifest) {
                    if (table.value("resource_id", std::string{}) != contract) continue;
                    schema_found = true;
                    for (const auto &column : table.value("columns", Json::array())) {
                        const auto name = column.value("name", std::string{});
                        if (name.empty()) continue;
                        context.readable_columns[physical_table].insert(name);
                        const auto semantic_type = column.value("type", std::string{});
                        context.column_types[physical_table][name] = semantic_type == "integer"
                            ? STREAMFIND_PLUGIN_COLUMN_INT64
                            : semantic_type == "real" ? STREAMFIND_PLUGIN_COLUMN_FLOAT64
                            : semantic_type == "boolean" ? STREAMFIND_PLUGIN_COLUMN_BOOL
                            : semantic_type == "timestamp" ? STREAMFIND_PLUGIN_COLUMN_TIMESTAMP
                            : STREAMFIND_PLUGIN_COLUMN_UTF8;
                    }
                    break;
                }
                if (!schema_found && physical_table.rfind("result:", 0) == 0) {
                    const auto result_id = physical_table.substr(std::string("result:").size());
                    const auto response_json = execution_context.request_data(data_proxy::request(
                        "describe-result-" + result_id,
                        execution_context.data_proxy_session_id(),
                        execution_context.data_proxy_run_id(), {}, producer_instance, {},
                        Json{{"method", "describe_result"}, {"result_id", result_id}}));
                    const auto response = data_proxy::Envelope::from_json(response_json);
                    if (!response.error_code.empty())
                        throw Error(ErrorCode::MethodExecution, response.error_code + ": " + response.error_message);
                    for (const auto &column : response.payload.value("schema", Json::array())) {
                        const auto name = column.value("name", std::string{});
                        if (name.empty()) continue;
                        context.readable_columns[physical_table].insert(name);
                        context.column_types[physical_table][name] = detail::dynamic_column_type(
                            column.value("type", std::string{}));
                    }
                }
            }
        }
        // Dynamic input ports such as anyTable intentionally do not declare a
        // fixed column manifest. Resolve their runtime schema through the
        // service proxy when running in a worker.
        if (runtime_inputs.is_object()) {
            for (const auto &[target_port, artifact] : runtime_inputs.items()) {
                if (!artifact.is_object()) continue;
                const auto physical_table = artifact.value("physical_table", std::string{});
                if (physical_table.empty()) continue;
                if (physical_table.rfind("result:", 0) == 0) continue;
                Json description;
                const auto response_json = execution_context.request_data(data_proxy::request(
                    "describe-table-" + physical_table,
                    execution_context.data_proxy_session_id(),
                    execution_context.data_proxy_run_id(), {}, producer_instance, {},
                    Json{{"method", "describe"}, {"table", physical_table}}));
                const auto response = data_proxy::Envelope::from_json(response_json);
                if (!response.error_code.empty())
                    throw Error(ErrorCode::MethodExecution, response.error_code + ": " + response.error_message);
                description = response.payload.value("columns", Json::array());
                for (const auto &column : description) {
                    const auto name = column.value("column_name", std::string{});
                    if (name.empty()) continue;
                    context.readable_columns[physical_table].insert(name);
                    context.column_types[physical_table][name] = detail::dynamic_column_type(
                        column.value("column_type", std::string{}));
                }
            }
        }
        for (const auto &port : output_ports) {
            if (!port.is_object()) continue;
            const auto contract = port.value("semantic_contract", std::string{});
            const auto representations = port.value("representations", Json::array());
            if (contract.empty() || !representations.is_array() ||
                std::find(representations.begin(), representations.end(), "table") == representations.end() || !manifest)
                continue;
            for (const auto &table : *manifest) {
                if (table.value("resource_id", std::string{}) != contract) continue;
                const auto columns = table.value("columns", Json::array());
                std::string output_table_key;
                if (operation_output_sink) {
                    const auto allocation = operation_output_sink->begin(Json{
                        {"contract_id", contract}, {"kind", "table"},
                        {"format", "arrow-ipc-batches"}, {"extension", "arrow"},
                        {"media_type", "application/vnd.apache.arrow.stream"}, {"schema", columns},
                        {"producer_operation", capability_id},
                        {"producer_instance", producer_instance},
                        {"workflow_revision", workflow_revision},
                        {"output_port_id", contract},
                        {"computation_key", execution_context.computation_key()},
                        {"cacheable", !execution_context.computation_key().empty()}});
                    const auto handle = allocation.value("output_handle", std::string{});
                    if (handle.empty()) throw Error(ErrorCode::MethodExecution,
                                                    "service did not allocate table output lease: " + contract);
                    table_output_handles[contract] = handle;
                    table_output_sequences[contract] = 0;
                    context.output_tables[contract] = handle;
                    output_table_key = handle;
                }
                for (const auto &column : columns) {
                    const auto name = column.value("name", std::string{});
                    if (name.empty()) continue;
                    context.writable_columns[output_table_key].insert(name);
                    const auto type = column.value("type", std::string{"string"});
                    context.column_types[output_table_key][name] = type == "integer"
                        ? STREAMFIND_PLUGIN_COLUMN_INT64
                        : type == "boolean" ? STREAMFIND_PLUGIN_COLUMN_BOOL
                        : type == "real" ? STREAMFIND_PLUGIN_COLUMN_FLOAT64
                        : type == "timestamp" ? STREAMFIND_PLUGIN_COLUMN_TIMESTAMP
                        : STREAMFIND_PLUGIN_COLUMN_UTF8;
                }
                break;
            }
        }
        context.emit_table_batch = [&context, &table_output_handles, &table_output_sequences,
                                    operation_output_sink](
            void *execution_context, const char *contract, uint32_t contract_size,
            const streamfind_plugin_batch_column *columns, uint32_t column_count,
            uint64_t row_count, void *user_data) -> streamfind_plugin_status {
            if (contract == nullptr || contract_size == 0)
                return STREAMFIND_PLUGIN_SCHEMA_ERROR;
            const std::string contract_id(contract, contract_size);
            const auto output = context.output_tables.find(contract_id);
            if (output == context.output_tables.end() || output->second.empty()) {
                if (context.report_error)
                    context.report_error("workflow output contract is not allocated: " + contract_id);
                return STREAMFIND_PLUGIN_SCHEMA_ERROR;
            }
            try {
                const auto rows = table_batch_rows(columns, column_count, row_count);
                operation_output_sink->write_rows(output->second,
                                                  table_output_sequences[contract_id]++, rows);
                return STREAMFIND_PLUGIN_OK;
            } catch (const std::exception &error) {
                if (context.report_error) context.report_error(error.what());
                return STREAMFIND_PLUGIN_ERROR;
            }
        };
        context.emit_result = [&emitted_results, &capability_id, &producer_instance, &lineage_inputs,
                               workflow_revision, operation_output_sink,
                               &execution_context](
            void *, const char *contract, uint32_t contract_size,
            const char *payload, uint64_t payload_size, void *) -> streamfind_plugin_status {
            if (contract == nullptr || contract_size == 0 || payload == nullptr)
                return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            const auto parsed = Json::parse(payload, payload + payload_size, nullptr, false);
            if (parsed.is_discarded()) return STREAMFIND_PLUGIN_SCHEMA_ERROR;
            const auto contract_id = std::string(contract, contract_size);
            {
                const auto descriptor = Json{
                    {"contract_id", contract_id},
                    {"kind", "json"},
                    {"format", "json"},
                    {"extension", "json"},
                    {"media_type", "application/json"},
                    {"producer_operation", capability_id},
                    {"producer_instance", producer_instance},
                    {"workflow_revision", workflow_revision}};
                const auto allocation = operation_output_sink->begin(descriptor);
                const auto output_handle = allocation.value("output_handle", std::string{});
                if (output_handle.empty()) return STREAMFIND_PLUGIN_ERROR;
                try {
                    const auto text = parsed.dump();
                    const std::vector<unsigned char> bytes(text.begin(), text.end());
                    operation_output_sink->write(output_handle, 0, bytes);
                    Json lineage = Json::array();
                    for (const auto &[result_id, source_port, target_port] : lineage_inputs)
                        lineage.push_back({{"result_id", result_id}, {"source_port", source_port},
                                           {"target_port", target_port}});
                    const auto manifest = operation_output_sink->finish(
                        output_handle,
                        Json{{"contract_id", contract_id},
                             {"kind", "json"},
                             {"media_type", "application/json"},
                             {"producer_operation", capability_id},
                             {"producer_instance", producer_instance},
                             {"workflow_revision", workflow_revision},
                             {"output_port_id", contract_id},
                             {"computation_key", execution_context.computation_key()},
                             {"cacheable", !execution_context.computation_key().empty()},
                             {"provenance", Json{{"lineage_inputs", lineage}}}});
                    const auto result_id = manifest.value("result_id", allocation.value("result_id", std::string{}));
                    if (result_id.empty()) return STREAMFIND_PLUGIN_ERROR;
                    emitted_results[contract_id] = Json{
                        {"result_id", result_id},
                        {"payload_ref", "result:" + result_id},
                        {"contract_id", contract_id},
                        {"representation", "json"},
                        {"payload", parsed},
                        {"state", "published"}};
                    return STREAMFIND_PLUGIN_OK;
                } catch (...) {
                    operation_output_sink->abort(output_handle, "dynamic plugin result emission failed");
                    return STREAMFIND_PLUGIN_ERROR;
                }
            }
        };
        streamfind_plugin_buffer buffer{};
        const auto text = request.dump();
        std::lock_guard log_lock(detail::operation_log_stream_mutex);
        detail::OperationLogStreambuf log_stream([&execution_context](std::string_view message) {
            execution_context.log(message);
        });
        detail::ScopedCerrRedirect cerr_redirect(&log_stream);
        const auto status = plugin.plugin.invoke(
            &context, text.data(), static_cast<uint32_t>(text.size()), &buffer, plugin.plugin.user_data);
        if (status != STREAMFIND_PLUGIN_OK) {
            if (operation_output_sink)
                for (const auto &[contract, handle] : table_output_handles)
                    operation_output_sink->abort(handle, "dynamic plugin invocation failed");
            throw Error(status == STREAMFIND_PLUGIN_CANCELLED ? ErrorCode::Cancelled : ErrorCode::MethodExecution,
                        "dynamic plugin invocation failed with status " + std::to_string(status) +
                        (plugin.runtime_diagnostics.empty() ? std::string{} : ": " + plugin.runtime_diagnostics));
        }
        if ((buffer.data == nullptr) != (buffer.size == 0)) {
            plugin.plugin.release_buffer(&buffer, plugin.plugin.user_data);
            throw Error(ErrorCode::MethodExecution,
                        "dynamic plugin returned an invalid response buffer");
        }
        try {
            response = Json::parse(std::string(buffer.data, buffer.size));
        } catch (const std::exception &error) {
            plugin.plugin.release_buffer(&buffer, plugin.plugin.user_data);
            throw Error(ErrorCode::MethodExecution,
                        std::string("dynamic plugin returned invalid JSON: ") + error.what());
        }
        plugin.plugin.release_buffer(&buffer, plugin.plugin.user_data);
        if (!response.is_object())
            response = Json{{"result", std::move(response)}};
        if (operation_output_sink) {
            for (const auto &[contract, handle] : table_output_handles) {
                const auto manifest = operation_output_sink->finish(
                    handle,
                    Json{{"contract_id", contract}, {"kind", "table"},
                         {"format", "arrow-ipc-batches"},
                         {"media_type", "application/vnd.apache.arrow.stream"},
                         {"producer_operation", capability_id},
                         {"producer_instance", producer_instance},
                         {"workflow_revision", workflow_revision},
                         {"output_port_id", contract},
                         {"computation_key", execution_context.computation_key()},
                         {"cacheable", !execution_context.computation_key().empty()},
                         {"provenance", Json{{"lineage_inputs", lineage_inputs}}}});
                const auto result_id = manifest.value("result_id", std::string{});
                if (result_id.empty())
                    throw Error(ErrorCode::MethodExecution,
                                "service did not return a published table result: " + contract);
                emitted_results[contract] = Json{
                    {"result_id", result_id}, {"payload_ref", "result:" + result_id},
                    {"physical_table", "result:" + result_id},
                    {"contract_id", contract}, {"representation", "table"},
                    {"format", "arrow-ipc-batches"}, {"state", "published"}};
            }
        }
        if (!emitted_results.empty())
            response["emitted_results"] = std::move(emitted_results);
    };
    execute_body();
    return response;
}

}  // namespace detail

DynamicPluginLoadResult load_dynamic_plugin_package(const std::filesystem::path &package_root) {
    DynamicPluginLoadResult result;
    const auto manifest_path = package_root / "plugin.json";
    const auto manifest_result = load_plugin_manifest(manifest_path);
    if (!manifest_result.valid) {
        result.diagnostics = manifest_result.diagnostics;
        return result;
    }
    result.manifest = manifest_result.manifest;

    if (result.manifest.abi_major != STREAMFIND_PLUGIN_ABI_MAJOR) {
        result.diagnostics = "plugin ABI major is incompatible with the host";
        return result;
    }
    if (!detail::supports_platform(result.manifest, detail::dynamic_platform_tag())) {
        result.diagnostics = "plugin manifest does not support the current platform";
        return result;
    }

    const auto platform = detail::dynamic_platform_tag();
    const auto *library_name = result.manifest.library_for_platform(platform);
    if (library_name == nullptr) {
        result.diagnostics = "plugin manifest has no library for the current platform";
        return result;
    }

    const auto package_path = std::filesystem::weakly_canonical(package_root);
    const auto library_path = std::filesystem::weakly_canonical(package_path / *library_name);
    const auto relative = library_path.lexically_relative(package_path);
    if (relative.empty() || relative == ".." || relative.string().starts_with("..")) {
        result.diagnostics = "plugin library resolves outside its package root";
        return result;
    }

    auto library_result = DynamicLibrary::load(library_path);
    if (!library_result.loaded || !library_result.library) {
        result.diagnostics = library_result.diagnostics;
        return result;
    }

    std::string symbol_diagnostics;
    const auto symbol = library_result.library->symbol(
        "streamfind_plugin_get_descriptor", symbol_diagnostics);
    if (symbol == nullptr) {
        result.diagnostics = symbol_diagnostics;
        return result;
    }

    streamfind_plugin_get_descriptor_fn get_descriptor = nullptr;
    static_assert(sizeof(get_descriptor) == sizeof(symbol));
    std::memcpy(&get_descriptor, &symbol, sizeof(get_descriptor));
    if (get_descriptor == nullptr) {
        result.diagnostics = "plugin descriptor symbol is invalid";
        return result;
    }

    streamfind_plugin_descriptor descriptor{};
    const auto status = get_descriptor(
        STREAMFIND_PLUGIN_ABI_MAJOR,
        STREAMFIND_PLUGIN_ABI_MINOR,
        &descriptor);
    if (status != STREAMFIND_PLUGIN_OK) {
        result.diagnostics = "plugin descriptor negotiation failed with status " +
                             std::to_string(status);
        return result;
    }
    if (descriptor.struct_size < sizeof(streamfind_plugin_descriptor) ||
        descriptor.abi_major != STREAMFIND_PLUGIN_ABI_MAJOR ||
        descriptor.plugin_id == nullptr || descriptor.plugin_version == nullptr ||
        descriptor.register_plugin == nullptr) {
        result.diagnostics = "plugin descriptor is incomplete or incompatible";
        return result;
    }
    if (result.manifest.plugin_id != descriptor.plugin_id) {
        result.diagnostics = "plugin descriptor ID does not match its manifest";
        return result;
    }
    if (result.manifest.version != descriptor.plugin_version) {
        result.diagnostics = "plugin descriptor version does not match its manifest";
        return result;
    }

    result.descriptor = descriptor;
    result.library = std::move(library_result.library);
    result.loaded = true;
    return result;
}

streamfind_plugin_status register_dynamic_plugin(
    DynamicPluginLoadResult &plugin,
    const streamfind_plugin_host_api &host) {
    if (!plugin.loaded || !plugin.library || plugin.descriptor.register_plugin == nullptr) {
        plugin.diagnostics = "cannot register an unloaded dynamic plugin";
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    }
    if (host.struct_size < sizeof(streamfind_plugin_host_api) ||
        host.abi_major != STREAMFIND_PLUGIN_ABI_MAJOR) {
        plugin.diagnostics = "host ABI is incompatible with the dynamic plugin";
        return STREAMFIND_PLUGIN_INCOMPATIBLE_ABI;
    }

    plugin.plugin = {};
    plugin.plugin.struct_size = sizeof(streamfind_plugin_api);
    plugin.plugin.abi_major = STREAMFIND_PLUGIN_ABI_MAJOR;
    plugin.plugin.abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
    const auto status = plugin.descriptor.register_plugin(
        &host, &plugin.plugin, plugin.descriptor.user_data);
    if (status != STREAMFIND_PLUGIN_OK) {
        plugin.diagnostics = "dynamic plugin registration failed with status " +
                             std::to_string(status);
    }
    return status;
}

void register_dynamic_plugin_capabilities(
    DynamicPluginLoadResult &plugin,
    const Json &catalogue_entries,
    MethodRegistry &methods,
    OperationRegistry &operations) {
    if (!plugin.loaded || !catalogue_entries.is_array()) {
        throw Error(ErrorCode::InvalidArgument,
                    "dynamic plugin registration requires a loaded plugin and catalogue entries");
    }

    const auto parameter_schema = [](const Json &entry) {
        Json parameters = Json::array();
        const auto source = entry.contains("parameters") && entry.at("parameters").is_array()
                                ? entry.at("parameters")
                                : Json::array();
        for (const auto &item : source) {
            if (!item.is_object()) continue;
            auto definition = item;
            if (definition.contains("schema")) {
                definition["type"] = definition.at("schema");
                definition.erase("schema");
            }
            parameters.push_back(std::move(definition));
        }
        return parameters;
    };
    for (const auto &entry : catalogue_entries) {
        if (!entry.is_object() || !entry.value("executable", false)) {
            continue;
        }
        const auto id = entry.value("canonical_id", std::string{});
        if (id.empty()) {
            throw Error(ErrorCode::InvalidArgument, "dynamic catalogue entry has no canonical_id");
        }
        if (entry.value("kind", std::string{}) == "operation") {
            OperationDefinition definition;
            definition.id = id;
            definition.name = entry.value("label", id);
            definition.description = entry.value("definition", "");
            definition.domain = entry.value("domain", "");
            definition.version = entry.value("operation_version", "1");
            definition.project_entry = entry.value("project_entry", false);

            definition.parameters = ParameterSchema::from_json(
                parameter_schema(entry));
            for (const auto &port : entry.value("input_ports", Json::array()))
                definition.input_ports.push_back(OperationDefinition::Port::from_json(port));
            for (const auto &port : entry.value("output_ports", Json::array()))
                definition.output_ports.push_back(OperationDefinition::Port::from_json(port));
            operations.register_operation(Operation(
                std::move(definition),
                [&plugin, id, output_ports = entry.value("output_ports", Json::array())](
                    OperationExecutionContext &context, const Json &parameters,
                    const std::string &operation_instance, const Json &input_artifacts) {
                    return detail::invoke_dynamic(plugin, context, id, parameters,
                                                  output_ports, operation_instance, input_artifacts);
                }, {}));
        }
    }
}

}  // namespace streamfind::sdk
