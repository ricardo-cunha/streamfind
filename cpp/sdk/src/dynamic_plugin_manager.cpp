#include "streamfind/sdk/dynamic_plugin_manager.hpp"

#include <atomic>

#include <cstring>
#include <optional>
#include <fstream>
#include <set>
#include <iostream>
#include <mutex>
#include <streambuf>

#include "streamfind/project_table_store.hpp"
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

std::vector<std::string> entry_tables(const Json &entry) {
    std::set<std::string> unique;
    const auto effects = entry.contains("effects") && entry.at("effects").is_object()
                             ? entry.at("effects")
                             : Json::object();
    for (const char *key : {"reads", "writes"}) {
        const auto values = effects.value(key, Json::array());
        if (!values.is_array()) {
            continue;
        }
        for (const auto &value : values) {
            if (value.is_string()) {
                unique.insert(value.get<std::string>());
            }
        }
    }
    return {unique.begin(), unique.end()};
}

Json invoke_dynamic(
    DynamicPluginLoadResult &plugin,
    Project &project,
    const std::string &capability_id,
    const Json &parameters,
    const std::vector<std::string> &owned_tables,
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
    const auto workflow_revision = project.get_workflow().version;
    Json request = {{"capability_id", capability_id}, {"parameters", parameters},
                    {"inputs", input_artifacts}};
    Json response;
    Json emitted_results = Json::object();
    std::vector<std::string> transaction_tables = owned_tables;
    if (input_artifacts.is_object()) {
        for (const auto &item : input_artifacts.items()) {
            if (!item.value().is_object()) continue;
            const auto table = item.value().value("physical_table", std::string{});
            if (!table.empty() && std::find(transaction_tables.begin(), transaction_tables.end(), table) == transaction_tables.end())
                transaction_tables.push_back(table);
        }
    }
    const auto manifest = !plugin.manifest.domain.empty()
        ? catalogue::table_manifest_json(plugin.manifest.domain, "")
        : std::optional<Json>{};

    if (manifest && !owned_tables.empty())
        ProjectTableStore::install_manifest_schema(project, plugin.manifest.domain, "");
    ProjectTableStore::transaction(project, transaction_tables, [&](ProjectTableStore &tables) {
        PluginDataServiceContext context;
        std::atomic_bool cancelled{false};
        const auto producer_instance = workflow_instance;
        std::vector<std::tuple<std::string, std::string, std::string>> lineage_inputs;
        if (input_artifacts.is_object()) {
            for (const auto &[target_port, artifact] : input_artifacts.items()) {
                if (!artifact.is_object()) continue;
                lineage_inputs.emplace_back(
                    artifact.value("artifact_id", ""),
                    artifact.value("contract_id", ""), target_port);
            }
        }
        context.tables = &tables;
        context.cancelled = &cancelled;
        context.progress = [&project](double, std::string_view message) {
            if (!message.empty()) project.log_operation(message);
        };
        context.allowed_tables = transaction_tables;
        if (manifest) {
            for (const auto &table : *manifest) {
                const auto table_name = table.value("table_name", std::string{});
                if (std::find(owned_tables.begin(), owned_tables.end(), table_name) == owned_tables.end()) continue;
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
        if (manifest && input_artifacts.is_object()) {
            for (const auto &[target_port, artifact] : input_artifacts.items()) {
                if (!artifact.is_object()) continue;
                const auto physical_table = artifact.value("physical_table", std::string{});
                const auto contract = artifact.value("contract_id", std::string{});
                if (physical_table.empty() || contract.empty()) continue;
                for (const auto &table : *manifest) {
                    if (table.value("resource_id", std::string{}) != contract) continue;
                    for (const auto &column : table.value("columns", Json::array())) {
                        const auto name = column.value("name", std::string{});
                        if (name.empty()) continue;
                        context.readable_columns[physical_table].insert(name);
                        context.column_types[physical_table][name] =
                            column.value("type", std::string{}) == "integer"
                                ? STREAMFIND_PLUGIN_COLUMN_INT64
                                : STREAMFIND_PLUGIN_COLUMN_UTF8;
                    }
                    break;
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
                const auto allocation = tables.allocate_table_artifact(
                    contract, capability_id, producer_instance, workflow_revision, columns);
                tables.append_artifact_lineage(allocation.first, lineage_inputs);
                context.output_tables[contract] = allocation.second;
                context.allowed_tables.push_back(allocation.second);
                for (const auto &column : columns) {
                    const auto name = column.value("name", std::string{});
                    if (name.empty()) continue;
                    context.writable_columns[allocation.second].insert(name);
                    const auto type = column.value("type", std::string{"string"});
                    context.column_types[allocation.second][name] = type == "integer"
                        ? STREAMFIND_PLUGIN_COLUMN_INT64
                        : type == "boolean" ? STREAMFIND_PLUGIN_COLUMN_BOOL
                        : type == "real" ? STREAMFIND_PLUGIN_COLUMN_FLOAT64
                        : type == "timestamp" ? STREAMFIND_PLUGIN_COLUMN_TIMESTAMP
                        : STREAMFIND_PLUGIN_COLUMN_UTF8;
                }
                break;
            }
        }
        context.emit_table_batch = [&context](
            void *execution_context, const char *contract, uint32_t contract_size,
            const streamfind_plugin_batch_column *columns, uint32_t column_count,
            uint64_t row_count, void *user_data) -> streamfind_plugin_status {
            if (contract == nullptr || contract_size == 0)
                return STREAMFIND_PLUGIN_SCHEMA_ERROR;
            const std::string contract_id(contract, contract_size);
            const auto output = context.output_tables.find(contract_id);
            if (output == context.output_tables.end() || output->second.empty())
                return STREAMFIND_PLUGIN_SCHEMA_ERROR;
            return plugin_append_batch(
                execution_context, output->second.data(),
                static_cast<uint32_t>(output->second.size()), columns, column_count,
                row_count, user_data);
        };
        context.emit_result = [&emitted_results, &tables, &capability_id, &producer_instance, &lineage_inputs, workflow_revision](
            void *, const char *contract, uint32_t contract_size,
            const char *payload, uint64_t payload_size, void *) -> streamfind_plugin_status {
            if (contract == nullptr || contract_size == 0 || payload == nullptr)
                return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            const auto parsed = Json::parse(payload, payload + payload_size, nullptr, false);
            if (parsed.is_discarded()) return STREAMFIND_PLUGIN_SCHEMA_ERROR;
            const auto contract_id = std::string(contract, contract_size);
            const auto artifact_id = tables.publish_result_artifact(
                contract_id, parsed.dump(), capability_id, producer_instance, workflow_revision);
            tables.append_artifact_lineage(artifact_id, lineage_inputs);
            emitted_results[contract_id] = Json{{"artifact_id", artifact_id}, {"payload", parsed}};
            return STREAMFIND_PLUGIN_OK;
        };
        streamfind_plugin_buffer buffer{};
        const auto text = request.dump();
        std::lock_guard log_lock(detail::operation_log_stream_mutex);
        detail::OperationLogStreambuf log_stream([&project](std::string_view message) {
            project.log_operation(message);
        });
        detail::ScopedCerrRedirect cerr_redirect(&log_stream);
        const auto status = plugin.plugin.invoke(
            &context, text.data(), static_cast<uint32_t>(text.size()), &buffer, plugin.plugin.user_data);
        if (status != STREAMFIND_PLUGIN_OK) {
            throw Error(ErrorCode::MethodExecution,
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
        if (!emitted_results.empty())
            response["emitted_results"] = std::move(emitted_results);
    });
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
        const auto tables = detail::entry_tables(entry);
        if (entry.value("kind", std::string{}) == "operation") {
            OperationDefinition definition;
            definition.id = id;
            definition.name = entry.value("label", id);
            definition.description = entry.value("definition", "");
            definition.domain = entry.value("domain", "");
            definition.version = entry.value("operation_version", "1");
            definition.project_entry = entry.value("project_entry", false);
            definition.cacheable = entry.value("cacheable", false);
            definition.parameters = ParameterSchema::from_json(
                parameter_schema(entry));
            for (const auto &port : entry.value("input_ports", Json::array()))
                definition.input_ports.push_back(OperationDefinition::Port::from_json(port));
            for (const auto &port : entry.value("output_ports", Json::array()))
                definition.output_ports.push_back(OperationDefinition::Port::from_json(port));
            operations.register_operation(Operation(
                std::move(definition),
                [&plugin, id, tables, output_ports = entry.value("output_ports", Json::array())](
                    Project &project, const Json &parameters,
                    const std::string &operation_instance, const Json &input_artifacts) {
                    return detail::invoke_dynamic(plugin, project, id, parameters, tables,
                                                  output_ports, operation_instance, input_artifacts);
                }, {}));
        }
    }
}

}  // namespace streamfind::sdk
