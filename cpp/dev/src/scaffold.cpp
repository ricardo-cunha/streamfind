#include "streamfind/dev/scaffold.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace streamfind::dev::scaffold_detail {

std::string replace_all(std::string text, const std::string &needle, const std::string &value) {
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        text.replace(position, needle.size(), value);
        position += value.size();
    }
    return text;
}

std::string identifier(const std::string &value) {
    if (!std::regex_match(value, std::regex("[a-z][a-z0-9_]*")))
        throw std::invalid_argument("plugin and operation names must match [a-z][a-z0-9_]*: " + value);
    return value;
}

void write_new(const std::filesystem::path &path, const std::string &content) {
    if (std::filesystem::exists(path)) throw std::runtime_error("refusing to overwrite existing file: " + path.string());
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("unable to create scaffold file: " + path.string());
    output << content;
}

void write_atomic(const std::filesystem::path &path, const std::string &content) {
    const auto temporary = path.string() + ".tmp-" +
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    { std::ofstream output(temporary, std::ios::binary); if (!output) throw std::runtime_error("unable to write scaffold update: " + path.string()); output << content; }
    std::filesystem::rename(temporary, path);
}

std::string operation_function(const std::string &plugin, const std::string &operation) {
    return "Json execute_" + operation + "(sdk::PluginHostAccess &access, const Json &parameters) {\n"
           "    if (access.is_cancelled()) throw std::runtime_error(\"operation cancelled\");\n"
           "    const auto value = parameters.value(\"value\", 1.0);\n"
           "    const auto scale = parameters.value(\"scale\", 2.0);\n"
           "    const Json result = {{\"operation\", \"" + plugin + "." + operation + "\"}, {\"value\", value * scale}};\n"
           "    access.emit_result(\"" + operation + "Result\", result.dump());\n"
           "    return result;\n"
           "}\n\n";
}

std::string operation_declaration(const std::string &operation) {
    return "Json execute_" + operation + "(sdk::PluginHostAccess &access, const Json &parameters);\n";
}

std::string operation_semantic(const std::string &plugin, const std::string &operation) {
    return "\nex:" + operation + "\n"
           "    a sf:Operation ;\n"
           "    sf:operationId \"" + plugin + "." + operation + "\" ;\n"
           "    sf:availableInPlugin ex:" + plugin + " ;\n"
           "    sf:providedByModule ex:operationsModule ;\n"
           "    skos:inScheme sfcore:scheme ;\n"
           "    skos:prefLabel \"" + operation + "\" ;\n"
           "    skos:definition \"Compute a deterministic numeric result for the " + operation + " operation.\" ;\n"
           "    sf:category \"example\" ;\n"
           "    sf:hasParameter ex:valueParameter, ex:scaleParameter ;\n"
           "    sf:hasOutputPort ex:" + operation + "Result ;\n"
           "    sf:returns ex:" + operation + "Result .\n\n"
           "ex:" + operation + "Result\n"
           "    a sf:OperationResult ;\n"
           "    sf:availableInPlugin ex:" + plugin + " ;\n"
           "    skos:inScheme sfcore:scheme ;\n"
           "    sf:dataKind \"json\" ;\n"
           "    skos:prefLabel \"" + operation + " result\" ;\n"
           "    skos:definition \"Numeric output published by the " + operation + " operation.\" ;\n"
           "    sf:type \"object\" .\n";
}

const char *cmake_template = R"CMAKE(cmake_minimum_required(VERSION 3.21)
project(@PROJECT@ VERSION 0.1.0 LANGUAGES CXX)

find_package(streamfind CONFIG REQUIRED)
get_filename_component(kit_root "${streamfind_DIR}/../../../.." ABSOLUTE)
find_program(catalogue_tool NAMES streamfind_sdk_catalogue
    PATHS "${kit_root}/sdk/native/bin" NO_DEFAULT_PATH REQUIRED)
file(GLOB ontology CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/semantic/*.ttl")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/catalogue.duckdb"
    COMMAND "${CMAKE_COMMAND}" -E env --modify "PATH=path_list_prepend:${kit_root}/bin" --
        "${catalogue_tool}" --core-semantic "${kit_root}/core/ontology"
        --plugin-semantic "${CMAKE_CURRENT_SOURCE_DIR}/semantic" --plugin @PLUGIN@
        --catalogue-kind plugin --ontology-namespace
        "https://streamfind.dev/catalogue/plugins/@PLUGIN@#"
        --output-json "${CMAKE_CURRENT_BINARY_DIR}/catalogue.json"
        --output-matrix "${CMAKE_CURRENT_BINARY_DIR}/capability_matrix.json"
        --output-db "${CMAKE_CURRENT_BINARY_DIR}/catalogue.duckdb"
    DEPENDS ${ontology} VERBATIM)
add_custom_target(plugin_catalogue DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/catalogue.duckdb")
file(STRINGS "${kit_root}/sdk/native/include/streamfind/plugin_abi.h" abi_definitions
    REGEX "STREAMFIND_PLUGIN_ABI_(MAJOR|MINOR) +")
foreach(definition IN LISTS abi_definitions)
    if(definition MATCHES "STREAMFIND_PLUGIN_ABI_(MAJOR|MINOR) += +([0-9]+)")
        set(abi_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endif()
endforeach()
configure_file(plugin.json.in plugin.json @ONLY)

add_library(@PROJECT@ SHARED src/plugin_entrypoint.cpp src/operations/operations.cpp)
add_dependencies(@PROJECT@ plugin_catalogue)
target_compile_features(@PROJECT@ PRIVATE cxx_std_20)
target_include_directories(@PROJECT@ PRIVATE src)
streamfind_add_plugin(@PROJECT@ @PLUGIN@)
set_target_properties(@PROJECT@ PROPERTIES OUTPUT_NAME "streamfind_@PLUGIN@")
if(WIN32)
    set_target_properties(@PROJECT@ PROPERTIES PREFIX "")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(plugin_contract tests/plugin_contract.cpp src/operations/operations.cpp)
    target_include_directories(plugin_contract PRIVATE src)
    target_link_libraries(plugin_contract PRIVATE @PROJECT@ streamfind::sdk)
    target_compile_features(plugin_contract PRIVATE cxx_std_20)
    add_test(NAME plugin_contract COMMAND plugin_contract)
endif()

install(TARGETS @PROJECT@ RUNTIME DESTINATION plugins/custom/custom.plugin.@PLUGIN@ LIBRARY DESTINATION plugins/custom/custom.plugin.@PLUGIN@)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/plugin.json" "${CMAKE_CURRENT_BINARY_DIR}/catalogue.duckdb" DESTINATION plugins/custom/custom.plugin.@PLUGIN@)
install(DIRECTORY semantic/ DESTINATION plugins/custom/custom.plugin.@PLUGIN@/ontology)
)CMAKE";

std::string manifest_legacy(const std::string &plugin) {
    return "{\n  \"schema_version\": 1,\n  \"plugin_id\": \"custom.plugin." + plugin + "\",\n  \"name\": \"" + plugin + "\",\n  \"version\": \"0.1.0\",\n  \"abi_version\": {\"major\": \"@abi_MAJOR@\", \"minor\": \"@abi_MINOR@\"},\n  \"sdk_compatibility\": {\"minimum\": \"@streamfind_VERSION@\", \"maximum\": \"@streamfind_VERSION_MAJOR@.@streamfind_VERSION_MINOR@.x\"},\n  \"library\": {\"windows-x86_64\": \"streamfind_" + plugin + ".dll\", \"linux-x86_64\": \"libstreamfind_" + plugin + ".so\"},\n  \"plugin\": \"" + plugin + "\",\n  \"ontology_namespace\": \"https://streamfind.dev/catalogue/plugins/" + plugin + "#\",\n  \"semantic_catalogue\": \"catalogue.duckdb\",\n  \"static_composition\": false,\n  \"platforms\": [\"windows-x86_64\", \"linux-x86_64\"]\n}\n";
}

std::string manifest(const std::string &plugin) {
    const auto template_text = std::string(R"JSON({
  "schema_version": 1,
  "plugin_id": "custom.plugin.@PLUGIN@",
  "name": "@PLUGIN@",
  "version": "0.1.0",
  "abi_version": {"major": @abi_MAJOR@, "minor": @abi_MINOR@},
  "sdk_compatibility": {"minimum": "@streamfind_VERSION@", "maximum": "@streamfind_VERSION_MAJOR@.@streamfind_VERSION_MINOR@.x"},
  "library": {"windows-x86_64": "streamfind_@PLUGIN@.dll", "linux-x86_64": "libstreamfind_@PLUGIN@.so"},
  "plugin": "@PLUGIN@",
  "ontology_namespace": "https://streamfind.dev/catalogue/plugins/@PLUGIN@#",
  "semantic_catalogue": "catalogue.duckdb",
  "static_composition": false,
  "platforms": ["windows-x86_64", "linux-x86_64"]
}
)JSON");
    return replace_all(template_text, "@PLUGIN@", plugin);
}

std::string operations_header(const std::string &plugin) {
    return "#pragma once\n#include <string_view>\n#include <nlohmann/json.hpp>\n#include <streamfind/sdk/plugin_host_access.hpp>\n\nnamespace streamfind::generated::operations {\nusing Json = nlohmann::json;\nusing Operation = Json (*)(streamfind::sdk::PluginHostAccess &, const Json &);\n// STREAMFIND_OPERATION_DECLARATIONS\nOperation find(std::string_view capability_id);\n} // namespace streamfind::generated::operations\n";
}

std::string operations_source(const std::string &plugin, const std::string &operation) {
    return "#include \"operations/operations.hpp\"\n#include <stdexcept>\n\nnamespace streamfind::generated::operations {\n" +
        operation_function(plugin, operation) +
        "Operation find(std::string_view capability_id) {\n    if (capability_id == \"" + plugin + "." + operation + "\") return &execute_" + operation + ";\n    return nullptr;\n}\n} // namespace streamfind::generated::operations\n";
}

std::string entrypoint(const std::string &plugin) {
    return R"CPP(#include "streamfind/plugin_abi.h"
#include "streamfind/sdk/plugin_host_access.hpp"
#include "operations/operations.hpp"
#include <cstring>
#include <stdexcept>

namespace streamfind::generated::detail {
using Json = nlohmann::json;
streamfind_plugin_status invoke(void *context, const char *request_json, uint32_t request_size,
                                streamfind_plugin_buffer *result, void *user_data) {
    if (!request_json || !result || !user_data) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    const auto &host = *static_cast<const streamfind_plugin_host_api *>(user_data);
    try {
        const auto request = Json::parse(std::string(request_json, request_size));
        const auto capability = request.at("capability_id").get<std::string>();
        const auto operation = operations::find(capability);
        if (!operation) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
        auto parameters = request.value("parameters", Json::object());
        parameters["_inputs"] = request.value("inputs", Json::object());
        sdk::PluginHostAccess access(host, context);
        const auto response = operation(access, parameters).dump();
        auto *buffer = static_cast<char *>(host.allocate(response.size(), alignof(char), host.user_data));
        if (!buffer) return STREAMFIND_PLUGIN_ERROR;
        std::memcpy(buffer, response.data(), response.size()); result->data = buffer;
        result->size = static_cast<uint32_t>(response.size()); return STREAMFIND_PLUGIN_OK;
    } catch (const std::exception &error) {
        if (host.report_error) host.report_error(error.what(), static_cast<uint32_t>(std::strlen(error.what())), host.user_data);
        return STREAMFIND_PLUGIN_ERROR;
    }
}
void release(streamfind_plugin_buffer *buffer, void *user_data) {
    if (!buffer || !buffer->data || !user_data) return;
    const auto &host = *static_cast<const streamfind_plugin_host_api *>(user_data);
    host.deallocate(const_cast<char *>(buffer->data), buffer->size, alignof(char), host.user_data); *buffer = {};
}
streamfind_plugin_status bind(const streamfind_plugin_host_api *host, streamfind_plugin_api *plugin, void *) {
    if (!host || !plugin) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    plugin->module_id = ")CPP" + plugin + R"CPP(.operations";
    plugin->plugin_id = "custom.plugin.)CPP" + plugin + R"CPP(";
    plugin->invoke = &invoke; plugin->release_buffer = &release;
    plugin->user_data = const_cast<streamfind_plugin_host_api *>(host); return STREAMFIND_PLUGIN_OK;
}
void shutdown(streamfind_plugin_api *, void *) {}
} // namespace streamfind::generated::detail

extern "C" STREAMFIND_PLUGIN_EXPORT streamfind_plugin_status
streamfind_plugin_get_descriptor(uint32_t major, uint32_t minor, streamfind_plugin_descriptor *descriptor) {
    if (!descriptor || major != STREAMFIND_PLUGIN_ABI_MAJOR || minor > STREAMFIND_PLUGIN_ABI_MINOR)
        return STREAMFIND_PLUGIN_INCOMPATIBLE_ABI;
    *descriptor = {}; descriptor->struct_size = sizeof(*descriptor);
    descriptor->abi_major = STREAMFIND_PLUGIN_ABI_MAJOR; descriptor->abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
    descriptor->plugin_id = "custom.plugin.)CPP" + plugin + R"CPP("; descriptor->plugin_version = "0.1.0";
    descriptor->register_plugin = &streamfind::generated::detail::bind;
    descriptor->shutdown_plugin = &streamfind::generated::detail::shutdown;
    return STREAMFIND_PLUGIN_OK;
}
)CPP";
}

std::string contract_test(const std::string &plugin, const std::string &operation) {
    return "#include <streamfind/plugin_abi.h>\n#include <operations/operations.hpp>\n#include <string_view>\n\nint main() {\n"
           "    streamfind_plugin_descriptor descriptor{};\n    if (streamfind_plugin_get_descriptor(STREAMFIND_PLUGIN_ABI_MAJOR, STREAMFIND_PLUGIN_ABI_MINOR, &descriptor) != STREAMFIND_PLUGIN_OK) return 1;\n"
           "    if (std::string_view(descriptor.plugin_id) != \"custom.plugin." + plugin + "\") return 2;\n"
           "    if (!streamfind::generated::operations::find(\"" + plugin + ".measure\")) return 3;\n"
           "    if (!streamfind::generated::operations::find(\"" + plugin + "." + operation + "\")) return 4;\n    return 0;\n}\n";
}

std::string semantic(const std::string &plugin, const std::string &operation) {
    return "@prefix sfcore: <https://streamfind.dev/catalogue/core#> .\n@prefix ex: <https://streamfind.dev/catalogue/plugins/" + plugin + "#> .\n@prefix sf: <https://streamfind.dev/vocabulary#> .\n@prefix skos: <http://www.w3.org/2004/02/skos/core#> .\n\nex:" + plugin + " a sf:Plugin ; skos:inScheme sfcore:scheme ; skos:prefLabel \"" + plugin + "\" ; skos:definition \"A generated native plugin.\" .\n\nex:operationsModule a sf:PluginModule ; sf:availableInPlugin ex:" + plugin + " ; skos:inScheme sfcore:scheme ; skos:prefLabel \"Operations\" ; skos:definition \"Generated operations.\" .\n\nex:valueParameter a sf:Parameter ; sf:availableInPlugin ex:" + plugin + " ; skos:inScheme sfcore:scheme ; skos:prefLabel \"Value\" ; skos:definition \"Numeric input value.\" ; sf:type \"real\" ; sf:required false ; sf:default \"1\" ; sf:example \"3\" .\n\nex:scaleParameter a sf:Parameter ; sf:availableInPlugin ex:" + plugin + " ; skos:inScheme sfcore:scheme ; skos:prefLabel \"Scale\" ; skos:definition \"Numeric scale.\" ; sf:type \"real\" ; sf:required false ; sf:default \"2\" ; sf:example \"2\" .\n" + operation_semantic(plugin, operation);
}

} // namespace streamfind::dev::scaffold_detail

namespace streamfind::dev {

int scaffold_plugin(const Request &request) {
    const auto plugin = scaffold_detail::identifier(request.name);
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto workspace = resolve_workspace(request.workspace, managed);
    const auto root = workspace / "plugins" / plugin;
    if (std::filesystem::exists(root) && !std::filesystem::is_empty(root))
        throw std::runtime_error("plugin destination is not empty: " + root.string());
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(root / "src" / "utils");
    const auto project = "custom_plugin_" + plugin;
    scaffold_detail::write_new(root / "CMakeLists.txt", scaffold_detail::replace_all(scaffold_detail::replace_all(scaffold_detail::cmake_template, "@PROJECT@", project), "@PLUGIN@", plugin));
    scaffold_detail::write_new(root / "plugin.json.in", scaffold_detail::manifest(plugin));
    scaffold_detail::write_new(root / "src" / "plugin_entrypoint.cpp", scaffold_detail::entrypoint(plugin));
    scaffold_detail::write_new(root / "src" / "operations" / "operations.hpp", scaffold_detail::operations_header(plugin));
    scaffold_detail::write_new(root / "src" / "operations" / "operations.cpp", scaffold_detail::operations_source(plugin, "measure"));
    scaffold_detail::write_new(root / "tests" / "plugin_contract.cpp", scaffold_detail::contract_test(plugin, "measure"));
    scaffold_detail::write_new(root / "semantic" / "operations.ttl", scaffold_detail::semantic(plugin, "measure"));
    scaffold_detail::write_new(root / "README.md", "# " + plugin + " plugin\n\nGenerated native plugin with a deterministic numeric `" + plugin + ".measure` operation. Build against the SDK in an extracted StreamFind runtime archive with CMake and Ninja.\n");
    scaffold_detail::write_new(root / "LICENSE-THIRD-PARTY.md", "# Third-party notices\n\nAdd notices for dependencies introduced by this plugin. The generated scaffold contains no third-party source.\n");
    std::cout << "created native plugin scaffold at " << root.string() << '\n';
    return 0;
}

int add_plugin_operation(const Request &request) {
    const auto operation = scaffold_detail::identifier(request.name);
    const auto root = std::filesystem::absolute(*request.plugin_path);
    const auto manifest = root / "plugin.json.in";
    const auto header = root / "src" / "operations" / "operations.hpp";
    const auto source = root / "src" / "operations" / "operations.cpp";
    const auto semantic = root / "semantic" / "operations.ttl";
    for (const auto &path : {manifest, header, source, semantic}) if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("generated plugin scaffold file is missing: " + path.string());
    auto source_text = [&]() { std::ifstream input(source); std::ostringstream text; text << input.rdbuf(); return text.str(); }();
    const auto plugin = root.filename().string();
    if (source_text.find("capability_id == \"" + plugin + "." + operation + "\"") != std::string::npos)
        throw std::runtime_error("operation already exists: " + plugin + "." + operation);
    auto header_text = [&]() { std::ifstream input(header); std::ostringstream text; text << input.rdbuf(); return text.str(); }();
    const auto declaration_marker = "// STREAMFIND_OPERATION_DECLARATIONS\n";
    if (header_text.find(declaration_marker) == std::string::npos) throw std::runtime_error("operation declaration marker is missing");
    header_text.replace(header_text.find(declaration_marker), std::string(declaration_marker).size(), declaration_marker + scaffold_detail::operation_declaration(operation));
    const auto function_marker = "namespace streamfind::generated::operations {\n";
    const auto function_position = source_text.find(function_marker);
    if (function_position == std::string::npos) throw std::runtime_error("operation function marker is missing");
    source_text.insert(function_position + std::string(function_marker).size(), scaffold_detail::operation_function(plugin, operation));
    const auto find_marker = "Operation find(std::string_view capability_id) {\n";
    const auto find_position = source_text.find(find_marker);
    if (find_position == std::string::npos) throw std::runtime_error("operation lookup marker is missing");
    source_text.insert(find_position + std::string(find_marker).size(), "    if (capability_id == \"" + plugin + "." + operation + "\") return &execute_" + operation + ";\n");
    auto semantic_text = [&]() { std::ifstream input(semantic); std::ostringstream text; text << input.rdbuf(); return text.str(); }();
    scaffold_detail::write_atomic(header, header_text);
    scaffold_detail::write_atomic(source, source_text);
    scaffold_detail::write_atomic(semantic, semantic_text + scaffold_detail::operation_semantic(plugin, operation));
    std::cout << "added operation " << plugin << "." << operation << " to " << root.string() << '\n';
    return 0;
}

} // namespace streamfind::dev
