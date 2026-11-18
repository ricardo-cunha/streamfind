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
#include <tuple>
#include <unordered_map>

#include "streamfind/catalogue.hpp"
#include "streamfind/sdk/plugin_data_service.hpp"

#include <arrow/c/bridge.h>

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
        for (auto &[_, artifact] : runtime_inputs.items()) {
            if (!artifact.is_object()) continue;
            const auto result_id = artifact.value("result_id", std::string{});
            if (!result_id.empty()) artifact["result_ref"] = "result:" + result_id;
        }
    }
    Json request = {{"capability_id", capability_id}, {"parameters", parameters},
                    {"inputs", runtime_inputs}};
    Json response;
    Json emitted_results = Json::object();
    std::unordered_map<std::string, std::string> table_output_handles;
    std::unordered_map<std::string, std::uint64_t> table_output_sequences;
    std::unordered_map<std::string, std::string> output_contract_port_ids;
    for (const auto &port : output_ports) {
        if (!port.is_object()) continue;
        const auto contract = port.value("semantic_contract", std::string{});
        const auto port_id = port.value("id", std::string{});
        if (contract.empty() || port_id.empty()) continue;
        const auto [found, inserted] = output_contract_port_ids.emplace(contract, port_id);
        if (!inserted && found->second != port_id)
            throw Error(ErrorCode::SchemaMismatch,
                "dynamic output contract is ambiguous across ports: " + contract);
    }
    const auto manifest = !plugin.manifest.domain.empty()
        ? catalogue::table_manifest_json(plugin.manifest.domain, "")
        : std::optional<Json>{};

    OperationOutputSink *operation_output_sink = &execution_context.outputs();
    auto execute_body = [&]() {
        PluginDataServiceContext context;
        const auto producer_instance = workflow_instance;
        std::vector<std::tuple<std::string, std::string, std::string>> lineage_inputs;
        const auto add_lineage = [&](const std::string &result_id, const std::string &source_port,
                                     const std::string &target_port) {
            const auto edge = std::make_tuple(result_id, source_port, target_port);
            if (std::find(lineage_inputs.begin(), lineage_inputs.end(), edge) == lineage_inputs.end())
                lineage_inputs.push_back(edge);
        };
        if (runtime_inputs.is_object()) {
            for (const auto &[target_port, artifact] : runtime_inputs.items()) {
                if (!artifact.is_object()) continue;
                const auto result_id = artifact.value("result_id", std::string{});
                const auto source_port = artifact.value(
                    "output_port_id", artifact.value("contract_id", std::string{}));
                if (!result_id.empty() && !source_port.empty())
                    add_lineage(result_id, source_port, target_port);
            }
        }
        if (request.contains("parameters") && request.at("parameters").is_object()) {
            for (const auto &[parameter_name, value] : request.at("parameters").items()) {
                if (!value.is_object() || !value.contains("$result_ref") ||
                    !value.at("$result_ref").is_string()) continue;
                const auto result_id = value.at("$result_ref").get<std::string>();
                const auto source_port = value.value("output_port_id", std::string{});
                if (!result_id.empty() && !source_port.empty())
                    add_lineage(result_id, source_port, "parameter:" + parameter_name);
            }
        }
        context.operation_instance = workflow_instance;
        context.data_proxy_operation_id = capability_id;
        context.data_proxy_session_id = execution_context.data_proxy_session_id();
        context.data_proxy_run_id = execution_context.data_proxy_run_id();
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
        context.receive_result_input_handle = [&execution_context] {
            return execution_context.receive_result_input_handle();
        };
        std::unordered_set<std::string> registered_input_results;
        const auto register_table_input = [&](const std::string &result_id) {
            if (result_id.empty() || !registered_input_results.insert(result_id).second) return;
            const auto response_json = execution_context.request_data(data_proxy::request(
                "describe-result-" + result_id,
                execution_context.data_proxy_session_id(),
                execution_context.data_proxy_run_id(), {}, producer_instance, {},
                Json{{"method", "describe_result"}, {"result_id", result_id}}));
            const auto response = data_proxy::Envelope::from_json(response_json);
            if (!response.error_code.empty())
                throw Error(ErrorCode::MethodExecution,
                    response.error_code + ": " + response.error_message);
            if (response.payload.value("state", std::string{}) != "published" ||
                response.payload.value("kind", std::string{}) != "table" ||
                response.payload.value("format", std::string{}) != "arrow-ipc-file")
                throw Error(ErrorCode::MethodExecution,
                    "workflow input is not a published Arrow table: " + result_id);
            const auto result_reference = "result:" + result_id;
            const auto schema = response.payload.value("schema", Json::array());
            if (!schema.is_array())
                throw Error(ErrorCode::MethodExecution,
                    "published result schema is invalid: " + result_id);
            for (const auto &column : schema) {
                const auto name = column.value("name", std::string{});
                if (name.empty())
                    throw Error(ErrorCode::MethodExecution,
                        "published result schema contains an empty column name: " + result_id);
                context.readable_columns[result_reference].insert(name);
            }
            context.allowed_result_refs.push_back(result_reference);
        };
        if (runtime_inputs.is_object()) {
            for (const auto &[_, artifact] : runtime_inputs.items()) {
                if (!artifact.is_object()) continue;
                const auto result_id = artifact.value("result_id", std::string{});
                if (result_id.empty()) continue;
                register_table_input(result_id);
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
                if (operation_output_sink) {
                    const auto allocation = operation_output_sink->begin(Json{
                        {"contract_id", contract}, {"kind", "table"},
                        {"format", "arrow-ipc-file"}, {"extension", "arrow"},
                        {"media_type", "application/vnd.apache.arrow.file"}, {"schema", columns},
                        {"producer_operation", capability_id},
                        {"producer_instance", producer_instance},
                        {"workflow_revision", workflow_revision},
                        {"output_port_id", output_contract_port_ids.at(contract)},
                        {"computation_key", execution_context.computation_key()},
                        {"cacheable", !execution_context.computation_key().empty()}});
                    const auto handle = allocation.value("output_handle", std::string{});
                    if (handle.empty()) throw Error(ErrorCode::MethodExecution,
                                                    "service did not allocate table output lease: " + contract);
                    table_output_handles[contract] = handle;
                    table_output_sequences[contract] = 0;
                }
                break;
            }
        }
        context.emit_table_batch = [&context, &table_output_handles, &table_output_sequences,
                                    operation_output_sink](
            void *execution_context, const char *contract, uint32_t contract_size,
            struct ArrowArray *array, struct ArrowSchema *schema,
            void *user_data) -> streamfind_plugin_status {
            if (contract == nullptr || contract_size == 0)
                return STREAMFIND_PLUGIN_SCHEMA_ERROR;
            const std::string contract_id(contract, contract_size);
            const auto output = table_output_handles.find(contract_id);
            if (output == table_output_handles.end() || output->second.empty()) {
                if (context.report_error)
                    context.report_error("workflow output contract is not allocated: " + contract_id);
                return STREAMFIND_PLUGIN_SCHEMA_ERROR;
            }
            try {
                auto batch = arrow::ImportRecordBatch(array, schema);
                if (!batch.ok()) throw std::runtime_error(batch.status().ToString());
                operation_output_sink->write_table_batch(
                    output->second, table_output_sequences[contract_id]++,
                    *batch);
                return STREAMFIND_PLUGIN_OK;
            } catch (const std::exception &error) {
                if (context.report_error) context.report_error(error.what());
                return STREAMFIND_PLUGIN_ERROR;
            }
        };
        context.emit_result = [&emitted_results, &capability_id, &producer_instance, &lineage_inputs,
                               &output_contract_port_ids, workflow_revision, operation_output_sink,
                               &execution_context, &context](
            void *, const char *contract, uint32_t contract_size,
            const char *payload, uint64_t payload_size, void *) -> streamfind_plugin_status {
            if (contract == nullptr || contract_size == 0 || payload == nullptr)
                return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            const auto parsed = Json::parse(payload, payload + payload_size, nullptr, false);
            if (parsed.is_discarded()) return STREAMFIND_PLUGIN_SCHEMA_ERROR;
            const auto contract_id = std::string(contract, contract_size);
            const auto output_port = output_contract_port_ids.find(contract_id);
            if (output_port == output_contract_port_ids.end()) {
                if (context.report_error)
                    context.report_error("workflow JSON output contract is not declared: " + contract_id);
                return STREAMFIND_PLUGIN_SCHEMA_ERROR;
            }
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
                             {"output_port_id", output_port->second},
                             {"computation_key", execution_context.computation_key()},
                             {"cacheable", !execution_context.computation_key().empty()},
                             {"provenance", Json{{"lineage_inputs", lineage}}}});
                    const auto result_id = manifest.value("result_id", allocation.value("result_id", std::string{}));
                    if (result_id.empty()) return STREAMFIND_PLUGIN_ERROR;
                    emitted_results[output_port->second] = Json{
                        {"result_id", result_id},
                        {"payload_ref", "result:" + result_id},
                        {"contract_id", contract_id},
                        {"output_port_id", output_port->second},
                        {"representation", "json"},
                        {"payload", parsed},
                        {"state", "published"}};
                    return STREAMFIND_PLUGIN_OK;
                } catch (const std::exception &error) {
                    if (context.report_error) context.report_error(error.what());
                    try {
                        operation_output_sink->abort(output_handle, "dynamic plugin result emission failed");
                    } catch (...) {
                        // Keep the publication failure as the primary diagnostic.
                    }
                    return STREAMFIND_PLUGIN_ERROR;
                } catch (...) {
                    if (context.report_error)
                        context.report_error("unknown error while publishing dynamic plugin JSON result");
                    try {
                        operation_output_sink->abort(output_handle, "dynamic plugin result emission failed");
                    } catch (...) {
                        // Keep the publication failure as the primary diagnostic.
                    }
                    return STREAMFIND_PLUGIN_ERROR;
                }
            }
        };
        if (request.contains("parameters") && request.at("parameters").is_object()) {
            for (auto &[parameter_name, value] : request["parameters"].items()) {
                if (!value.is_object() || !value.contains("$result_ref") ||
                    !value.at("$result_ref").is_string()) continue;
                const auto result_id = value.at("$result_ref").get<std::string>();
                if (result_id.empty()) throw Error(ErrorCode::MethodExecution,
                    "table parameter has an empty result reference: " + parameter_name);
                register_table_input(result_id);
            }
        }
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
            if (buffer.data != nullptr || buffer.size != 0)
                plugin.plugin.release_buffer(&buffer, plugin.plugin.user_data);
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
                         {"format", "arrow-ipc-file"},
                         {"media_type", "application/vnd.apache.arrow.file"},
                         {"producer_operation", capability_id},
                         {"producer_instance", producer_instance},
                         {"workflow_revision", workflow_revision},
                         {"output_port_id", output_contract_port_ids.at(contract)},
                         {"computation_key", execution_context.computation_key()},
                         {"cacheable", !execution_context.computation_key().empty()},
                         {"provenance", Json{{"lineage_inputs", [&] {
                             Json lineage = Json::array();
                             for (const auto &[result_id, source_port, target_port] : lineage_inputs)
                                 lineage.push_back({{"result_id", result_id},
                                                    {"source_port", source_port},
                                                    {"target_port", target_port}});
                             return lineage;
                         }()}}}});
                const auto result_id = manifest.value("result_id", std::string{});
                if (result_id.empty())
                    throw Error(ErrorCode::MethodExecution,
                                "service did not return a published table result: " + contract);
                const auto output_port = output_contract_port_ids.at(contract);
                emitted_results[output_port] = Json{
                    {"result_id", result_id}, {"result_ref", "result:" + result_id},
                    {"payload_ref", "result:" + result_id},
                    {"contract_id", contract}, {"representation", "table"},
                    {"output_port_id", output_port},
                    {"format", "arrow-ipc-file"}, {"state", "published"}};
            }
        }
        if (!emitted_results.empty())
            response["emitted_results"] = std::move(emitted_results);
    };
    try {
        execute_body();
    } catch (...) {
        if (operation_output_sink) {
            for (const auto &[_, handle] : table_output_handles) {
                try {
                    operation_output_sink->abort(handle, "dynamic plugin invocation did not complete");
                } catch (...) {
                    // Keep the invocation failure as the primary diagnostic;
                    // the sink owns any remaining lease cleanup on teardown.
                }
            }
        }
        throw;
    }
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
