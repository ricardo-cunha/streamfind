#include <filesystem>
#include <stdexcept>

#include "streamfind/plugin_lockfile.hpp"
#include "../support/tmp_projects.hpp"

int main() {
    const auto valid = streamfind::parse_plugin_lockfile(R"json({
        "schema_version": 1,
        "plugins": [{
            "plugin_id": "native.plugin.mass_spec",
            "version": "0.8.0",
            "package_digest": "sha256:mass-spec",
            "abi_version": {"major": 3, "minor": 0},
            "operation_ids": ["mass_spec.read_mass_spec_files"],
            "result_contract_versions": {"mass_spec.read_mass_spec_files": "1"}
        }]
    })json");
    if (!valid.valid || valid.lockfile.plugins.size() != 1 || valid.lockfile.plugins.front().abi_major != 3)
        throw std::runtime_error("valid plugin lockfile was rejected");

    const auto unsupported = streamfind::parse_plugin_lockfile(
        R"json({"schema_version": 2, "plugins": []})json");
    if (unsupported.valid || unsupported.diagnostics.find("schema_version") == std::string::npos)
        throw std::runtime_error("unsupported plugin lockfile schema was accepted");

    const auto duplicate = streamfind::parse_plugin_lockfile(R"json({
        "schema_version": 1,
        "plugins": [
            {"plugin_id":"native.plugin.mass_spec","version":"0.8.0","package_digest":"sha256:a","abi_version":{"major":3,"minor":0},"operation_ids":[],"result_contract_versions":{}},
            {"plugin_id":"native.plugin.mass_spec","version":"0.8.1","package_digest":"sha256:b","abi_version":{"major":3,"minor":0},"operation_ids":[],"result_contract_versions":{}}
        ]
    })json");
    if (duplicate.valid || duplicate.diagnostics.find("duplicate plugin") == std::string::npos)
        throw std::runtime_error("duplicate plugin lockfile entry was accepted");

    const auto path = streamfind::test::tmp_projects_dir() / "project.extensions.lock.json";
    streamfind::write_plugin_lockfile_atomic(path, valid.lockfile);
    const auto loaded = streamfind::load_plugin_lockfile(path);
    std::error_code error;
    std::filesystem::remove(path, error);
    if (!loaded.valid || loaded.lockfile.plugins.front().package_digest != "sha256:mass-spec")
        throw std::runtime_error("atomically written plugin lockfile could not be read back");
    return 0;
}
