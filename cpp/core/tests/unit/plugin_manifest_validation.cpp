#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "streamfind/sdk/plugin_manifest.hpp"

int main() {
    const auto valid = streamfind::sdk::parse_plugin_manifest(R"json({
        "schema_version": 1,
        "plugin_id": "sensors",
        "name": "Sensors",
        "version": "0.4.1",
        "abi_version": {"major": 1, "minor": 0},
        "sdk_compatibility": {"minimum": "0.4.0", "maximum": "0.4.x"},
        "library": {
            "windows-x86_64": "streamfind_sensors.dll",
            "linux-x86_64": "libstreamfind_sensors.so"
        },
        "plugin": "sensors",
        "ontology_namespace": "https://streamfind.dev/catalogue/plugins/sensors#",
        "semantic_catalogue": "catalogue.duckdb",
        "static_composition": true,
        "platforms": ["windows", "linux"]
    })json");
    if (!valid.valid || valid.manifest.plugin_id != "sensors") {
        return 1;
    }

    const auto traversal = streamfind::sdk::parse_plugin_manifest(R"json({
        "schema_version": 1,
        "plugin_id": "sensors",
        "name": "Sensors",
        "version": "0.4.1",
        "abi_version": {"major": 1, "minor": 0},
        "sdk_compatibility": {"minimum": "0.4.0", "maximum": "0.4.x"},
        "library": {
            "windows-x86_64": "../mass_spec.dll",
            "linux-x86_64": "../libmass_spec.so"
        },
        "plugin": "sensors",
        "ontology_namespace": "https://streamfind.dev/catalogue/plugins/sensors#",
        "semantic_catalogue": "catalogue.duckdb",
        "static_composition": true,
        "platforms": ["windows"]
    })json");
    if (traversal.valid || traversal.diagnostics.find("relative") == std::string::npos) {
        return 2;
    }

    const auto unsupported_schema = streamfind::sdk::parse_plugin_manifest(R"json({
        "schema_version": 2,
        "plugin_id": "sensors",
        "name": "Sensors",
        "version": "0.4.1",
        "abi_version": {"major": 1, "minor": 0},
        "sdk_compatibility": {"minimum": "0.4.0", "maximum": "0.4.x"},
        "library": {"windows-x86_64": "streamfind_sensors.dll"},
        "plugin": "sensors",
        "ontology_namespace": "https://streamfind.dev/catalogue/plugins/sensors#",
        "semantic_catalogue": "catalogue.duckdb",
        "static_composition": true,
        "platforms": ["windows-x86_64"]
    })json");
    if (unsupported_schema.valid || unsupported_schema.diagnostics.find("schema_version") == std::string::npos) {
        return 3;
    }

    const auto invalid_namespace = streamfind::sdk::parse_plugin_manifest(R"json({
        "schema_version": 1,
        "plugin_id": "sensors",
        "name": "Sensors",
        "version": "0.4.1",
        "abi_version": {"major": 1, "minor": 0},
        "sdk_compatibility": {"minimum": "0.4.0", "maximum": "0.4.x"},
        "library": {"windows-x86_64": "streamfind_sensors.dll"},
        "plugin": "sensors",
        "ontology_namespace": "https://streamfind.dev/catalogue/plugins/sensors",
        "semantic_catalogue": "catalogue.duckdb",
        "static_composition": true,
        "platforms": ["windows-x86_64"]
    })json");
    if (invalid_namespace.valid || invalid_namespace.diagnostics.find("ontology_namespace") == std::string::npos) {
        return 4;
    }

    const auto catalogue_traversal = streamfind::sdk::parse_plugin_manifest(R"json({
        "schema_version": 1,
        "plugin_id": "sensors",
        "name": "Sensors",
        "version": "0.4.1",
        "abi_version": {"major": 1, "minor": 0},
        "sdk_compatibility": {"minimum": "0.4.0", "maximum": "0.4.x"},
        "library": {"windows-x86_64": "streamfind_sensors.dll"},
        "plugin": "sensors",
        "ontology_namespace": "https://streamfind.dev/catalogue/plugins/sensors#",
        "semantic_catalogue": "../catalogue.duckdb",
        "static_composition": true,
        "platforms": ["windows-x86_64"]
    })json");
    if (catalogue_traversal.valid || catalogue_traversal.diagnostics.find("semantic_catalogue") == std::string::npos) {
        return 5;
    }

    const auto malformed = streamfind::sdk::parse_plugin_manifest("not-json");
    if (malformed.valid || malformed.diagnostics.empty()) {
        return 6;
    }

    return 0;
}
