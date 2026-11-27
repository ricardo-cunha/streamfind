#include <filesystem>
#include <stdexcept>

#include "streamfind/plugin_registry.hpp"
#include "../support/tmp_projects.hpp"

int main() {
    const auto valid = streamfind::parse_plugin_registry(R"json({
        "schema_version": 1,
        "generation": 4,
        "installed": [{
            "plugin_id": "native.plugin.sensors",
            "version": "0.8.0",
            "platform": "windows-x86_64",
            "package_path": "C:/Users/example/.streamfind/extensions/plugins/native.plugin.sensors/0.8.0/windows-x86_64",
            "package_digest": "sha256:abc",
            "origin": "bundled",
            "trust": "verified",
            "active": true
        }]
    })json");
    if (!valid.valid || valid.registry.generation != 4 || valid.registry.installed.size() != 1)
        throw std::runtime_error("valid plugin registry was rejected");

    const auto unsupported = streamfind::parse_plugin_registry(
        R"json({"schema_version": 2, "generation": 0, "installed": []})json");
    if (unsupported.valid || unsupported.diagnostics.find("schema_version") == std::string::npos)
        throw std::runtime_error("unsupported plugin registry schema was accepted");

    const auto duplicate = streamfind::parse_plugin_registry(R"json({
        "schema_version": 1, "generation": 1, "installed": [
            {"plugin_id":"native.plugin.sensors","version":"0.8.0","platform":"windows-x86_64","package_path":"a","package_digest":"sha256:a","origin":"bundled","trust":"verified","active":true},
            {"plugin_id":"native.plugin.sensors","version":"0.8.0","platform":"windows-x86_64","package_path":"b","package_digest":"sha256:b","origin":"local","trust":"verified","active":false}
        ]
    })json");
    if (duplicate.valid || duplicate.diagnostics.find("duplicate installed") == std::string::npos)
        throw std::runtime_error("conflicting plugin registry identity was accepted");

    const auto path = streamfind::test::tmp_projects_dir() / "plugin-registry-contract.json";
    streamfind::write_plugin_registry_atomic(path, valid.registry);
    const auto loaded = streamfind::load_plugin_registry(path);
    std::error_code error;
    std::filesystem::remove(path, error);
    if (!loaded.valid || loaded.registry.installed.front().plugin_id != "native.plugin.sensors")
        throw std::runtime_error("atomically written plugin registry could not be read back");
    const auto active = streamfind::find_active_plugin(loaded.registry, "native.plugin.sensors", "windows-x86_64");
    if (!active || active->version != "0.8.0" || active->package_digest != "sha256:abc")
        throw std::runtime_error("active registry package could not be resolved by canonical identity");
    if (streamfind::find_active_plugin(loaded.registry, "native.plugin.sensors", "linux-x86_64"))
        throw std::runtime_error("registry resolved a package for the wrong platform");
    return 0;
}
