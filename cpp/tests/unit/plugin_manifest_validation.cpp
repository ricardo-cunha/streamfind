#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "streamfind/sdk/plugin_manifest.hpp"

int main() {
    const auto valid = streamfind::sdk::parse_plugin_manifest(R"json({
        "plugin_id": "sensors",
        "name": "Sensors",
        "version": "0.2.0",
        "abi_version": {"major": 1, "minor": 0},
        "sdk_compatibility": {"minimum": "0.2.0", "maximum": "0.2.x"},
        "library": {
            "windows-x86_64": "streamfind_sensors.dll",
            "linux-x86_64": "libstreamfind_sensors.so"
        },
        "domain": "sensors",
        "semantic_catalogue": "catalogue.duckdb",
        "static_composition": true,
        "platforms": ["windows", "linux"]
    })json");
    if (!valid.valid || valid.manifest.plugin_id != "sensors") {
        return 1;
    }

    const auto traversal = streamfind::sdk::parse_plugin_manifest(R"json({
        "plugin_id": "sensors",
        "name": "Sensors",
        "version": "0.2.0",
        "abi_version": {"major": 1, "minor": 0},
        "sdk_compatibility": {"minimum": "0.2.0", "maximum": "0.2.x"},
        "library": {
            "windows-x86_64": "../mass_spec.dll",
            "linux-x86_64": "../libmass_spec.so"
        },
        "domain": "sensors",
        "semantic_catalogue": "catalogue.duckdb",
        "static_composition": true,
        "platforms": ["windows"]
    })json");
    if (traversal.valid || traversal.diagnostics.find("relative") == std::string::npos) {
        return 2;
    }

    const auto malformed = streamfind::sdk::parse_plugin_manifest("not-json");
    if (malformed.valid || malformed.diagnostics.empty()) {
        return 3;
    }

    return 0;
}
