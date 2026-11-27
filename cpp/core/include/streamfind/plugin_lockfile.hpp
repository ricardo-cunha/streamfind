#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "streamfind/export.hpp"
#include "streamfind/plugin_registry.hpp"

namespace streamfind {

struct STREAMFIND_CORE_API PluginLockEntry {
    std::string plugin_id;
    std::string version;
    std::string package_digest;
    int abi_major{0};
    int abi_minor{0};
    std::vector<std::string> operation_ids;
    std::map<std::string, std::string> result_contract_versions;
};

struct STREAMFIND_CORE_API PluginLockfile {
    int schema_version{1};
    std::vector<PluginLockEntry> plugins;
};

struct STREAMFIND_CORE_API PluginLockfileResult {
    bool valid{false};
    PluginLockfile lockfile;
    std::string diagnostics;
};

STREAMFIND_CORE_API PluginLockfileResult
parse_plugin_lockfile(const std::string &json_text);

STREAMFIND_CORE_API PluginLockfileResult
load_plugin_lockfile(const std::filesystem::path &path);

STREAMFIND_CORE_API void
write_plugin_lockfile_atomic(const std::filesystem::path &path,
                             const PluginLockfile &lockfile);

STREAMFIND_CORE_API void
validate_plugin_lockfile_against_registry(const PluginLockfile &lockfile,
                                          const PluginRegistry &registry);

}  // namespace streamfind
