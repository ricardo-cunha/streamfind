#include "streamfind/dev/scaffold.hpp"

#include "streamfind/fingerprint.hpp"
#include "streamfind/plugin_registry.hpp"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

namespace streamfind::dev::lifecycle_detail {

using Json = nlohmann::json;

std::string read_text(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to read: " + path.string());
    std::ostringstream text; text << input.rdbuf(); return text.str();
}

void write_text(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.string() + ".tmp";
    { std::ofstream output(temporary, std::ios::binary | std::ios::trunc); if (!output) throw std::runtime_error("unable to write: " + path.string()); output << text; }
    std::filesystem::rename(temporary, path);
}

Json manifest(const std::filesystem::path &root) {
    const auto path = root / "plugin.json";
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("plugin.json is missing: " + path.string());
    const auto value = Json::parse(read_text(path));
    if (!value.is_object() || !value.contains("plugin") || !value.contains("version"))
        throw std::runtime_error("plugin.json must contain plugin and version");
    return value;
}

std::string platform() {
#ifdef _WIN32
    return "windows-x86_64";
#else
    return "linux-x86_64";
#endif
}

std::filesystem::path checked(const std::optional<std::filesystem::path> &value, const char *name,
                              bool exists, const std::filesystem::path &managed, const std::filesystem::path &workspace) {
    if (!value) throw std::invalid_argument(std::string("missing ") + name);
    return canonicalize_user_path(*value, exists, managed, workspace);
}

std::filesystem::path build_dir(const Request &request, const std::filesystem::path &plugin,
                                const std::filesystem::path &sdk, const std::filesystem::path &managed,
                                const std::filesystem::path &workspace) {
    if (request.build_path) return canonicalize_user_path(*request.build_path, false, managed, workspace);
    const auto key = streamfind::fingerprint::hash(plugin.string() + "\n" + sdk.string() + "\n" + platform() + "\n" + request.config);
    return managed / "dev" / "builds" / key / platform() / request.config;
}

std::vector<std::string> environment(const std::filesystem::path &sdk) {
    std::string path = (sdk / "bin").string();
    if (const auto *current = std::getenv("PATH"); current && *current) path += ";" + std::string(current);
    std::vector<std::string> values{"PATH=" + path, "CMAKE_PREFIX_PATH=" + (sdk / "sdk" / "native" / "cmake").string()};
    if (const auto *root = std::getenv("STREAMFIND_MSYS2_ROOT"); root && *root) {
        const auto ucrt = std::filesystem::path(root) / "ucrt64" / "bin";
        values.push_back("PATH=" + ucrt.string() + ";" + path);
        values.push_back("CC=" + (ucrt / "gcc.exe").string());
        values.push_back("CXX=" + (ucrt / "g++.exe").string());
        values.push_back("CMAKE_MAKE_PROGRAM=" + (ucrt / "ninja.exe").string());
    }
    return values;
}

std::filesystem::path tool(const char *name) {
    if (const auto *root = std::getenv("STREAMFIND_MSYS2_ROOT"); root && *root) {
        const auto candidate = std::filesystem::path(root) / "ucrt64" / "bin" / (std::string(name) + ".exe");
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    if (const auto *override_path = std::getenv(name); override_path && *override_path && std::filesystem::is_regular_file(override_path))
        return override_path;
    return name;
}

int checked_process(const std::filesystem::path &program, const std::vector<std::string> &args,
                   const std::filesystem::path &cwd, const std::vector<std::string> &env) {
    const auto result = run_process(program, args, cwd, 3600000, 8 * 1024 * 1024, std::nullopt, true, env);
    if (!result.output.empty()) std::cout << result.output;
    if (result.timed_out) throw std::runtime_error("child process timed out: " + program.string());
    if (result.cancelled) throw std::runtime_error("child process cancelled: " + program.string());
    if (result.exit_code != 0) throw std::runtime_error("child process failed (" + std::to_string(result.exit_code) + "): " + program.string());
    return 0;
}

std::filesystem::path validator(const std::filesystem::path &sdk) {
#ifdef _WIN32
    const auto path = sdk / "sdk" / "native" / "bin" / "streamfind_sdk_plugin_validator.exe";
#else
    const auto path = sdk / "sdk" / "native" / "bin" / "streamfind_sdk_plugin_validator";
#endif
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("SDK plugin validator is missing: " + path.string());
    return path;
}

std::filesystem::path configure_and_build(const Request &request, bool build, bool tests) {
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto workspace = resolve_workspace(request.workspace, managed);
    const auto plugin = checked(request.plugin_path, "--plugin PATH", true, managed, workspace);
    const auto sdk = checked(request.sdk, "--sdk PATH", true, managed, workspace);
    const auto build_root = build_dir(request, plugin, sdk, managed, workspace);
    std::filesystem::create_directories(build_root);
    const auto env = environment(sdk);
    if (build) checked_process(tool("cmake"), {"-G", "Ninja", "-S", plugin.string(), "-B", build_root.string(),
        "-Dstreamfind_DIR=" + (sdk / "sdk" / "native" / "cmake" / "streamfind").string(),
        "-DCMAKE_BUILD_TYPE=" + request.config}, plugin, env);
    if (build) checked_process(tool("cmake"), {"--build", build_root.string(), "--config", request.config}, plugin, env);
    if (tests) checked_process(tool("ctest"), {"--test-dir", build_root.string(), "--output-on-failure", "--no-tests=error"}, plugin, env);
    return build_root;
}

Json file_checksums(const std::filesystem::path &root) {
    Json files = Json::object();
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        const auto relative = std::filesystem::relative(entry.path(), root).generic_string();
        files[relative] = "streamfind:" + streamfind::fingerprint::hash(read_text(entry.path()));
    }
    return files;
}

std::filesystem::path package_from_build(const Request &request, const std::filesystem::path &build_root,
                                         const std::filesystem::path &sdk, const std::filesystem::path &plugin,
                                         const std::filesystem::path &managed, const std::filesystem::path &workspace) {
    const auto info = manifest(build_root);
    const auto id = info.at("plugin").get<std::string>();
    const auto version = info.at("version").get<std::string>();
    const auto stage = build_root / "phase6-stage";
    std::error_code error; std::filesystem::remove_all(stage, error);
    checked_process(tool("cmake"), {"--install", build_root.string(), "--prefix", stage.string()}, plugin, environment(sdk));
    auto staged = stage / "plugins" / "custom" / id;
    if (!std::filesystem::is_directory(staged)) staged = stage / "plugins" / id;
    checked_process(validator(sdk), {"--source-dir", plugin.string(), "--output-dir", staged.string(),
        "--manifest", (build_root / "plugin.json").string(), "--plugin", id,
#ifdef _WIN32
        "--library", info.at("library").at("windows-x86_64").get<std::string>()
#else
        "--library", info.at("library").at("linux-x86_64").get<std::string>()
#endif
    }, plugin, environment(sdk));
    const auto destination = request.output_file ? canonicalize_user_path(*request.output_file, false, managed, workspace)
        : managed / "extensions" / "packages" / id / version / platform();
    const auto temporary = destination.string() + ".staging";
    std::filesystem::remove_all(temporary, error);
    std::filesystem::create_directories(temporary);
    std::filesystem::copy(staged, temporary, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    for (const auto &file : {"README.md", "LICENSE-THIRD-PARTY.md"}) {
        const auto source_file = plugin / file;
        if (std::filesystem::is_regular_file(source_file)) std::filesystem::copy_file(source_file, std::filesystem::path(temporary) / file, std::filesystem::copy_options::overwrite_existing);
    }
    const auto contract = plugin / "tests" / "plugin_contract.cpp";
    if (std::filesystem::is_regular_file(contract)) {
        std::filesystem::create_directories(std::filesystem::path(temporary) / "tests");
        std::filesystem::copy_file(contract, std::filesystem::path(temporary) / "tests" / "plugin_contract.cpp", std::filesystem::copy_options::overwrite_existing);
    }
    const auto files = file_checksums(temporary);
    const auto digest = "streamfind:" + streamfind::fingerprint::hash(files.dump());
    write_text(std::filesystem::path(temporary) / "checksums.json", Json{{"schema_version", 1}, {"package_digest", digest}, {"files", files}}.dump(2) + "\n");
    write_text(std::filesystem::path(temporary) / "provenance.json", Json{{"source", plugin.string()}, {"sdk", sdk.string()}, {"platform", platform()}, {"config", request.config}, {"package_digest", digest}, {"contract_dependencies", info.value("contract_dependencies", Json::array())}}.dump(2) + "\n");
    std::filesystem::remove_all(destination, error);
    std::filesystem::rename(temporary, destination);
    std::cout << "packaged " << id << " " << version << " at " << destination.string() << "\n";
    return destination;
}

} // namespace streamfind::dev::lifecycle_detail

namespace streamfind::dev {

int build_plugin(const Request &request) { lifecycle_detail::configure_and_build(request, true, false); return 0; }
int test_plugin(const Request &request) { lifecycle_detail::configure_and_build(request, false, true); return 0; }
int validate_plugin(const Request &request) { const auto build = lifecycle_detail::configure_and_build(request, true, true); (void)build; return 0; }
int rebuild_plugin(const Request &request) {
    const auto build = lifecycle_detail::configure_and_build(request, true, true);
    std::cout << "rebuilt plugin from retained source at " << request.plugin_path->string()
              << " using build " << build.string() << "\n";
    return 0;
}
int package_plugin(const Request &request) {
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto workspace = resolve_workspace(request.workspace, managed);
    const auto plugin = lifecycle_detail::checked(request.plugin_path, "--plugin PATH", true, managed, workspace);
    const auto sdk = lifecycle_detail::checked(request.sdk, "--sdk PATH", true, managed, workspace);
    const auto build = lifecycle_detail::configure_and_build(request, true, true);
    lifecycle_detail::package_from_build(request, build, sdk, plugin, managed, workspace); return 0;
}

int install_plugin(const Request &request) {
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto workspace = resolve_workspace(request.workspace, managed);
    const auto source = lifecycle_detail::checked(request.package, "--package PATH", true, managed, workspace);
    const auto info = lifecycle_detail::manifest(source);
    const auto id = info.at("plugin").get<std::string>();
    const auto registry_id = info.at("plugin_id").get<std::string>();
    const auto version = info.at("version").get<std::string>();
    const auto checks = source / "checksums.json";
    if (!std::filesystem::is_regular_file(checks)) throw std::runtime_error("verified package is missing checksums.json");
    const auto metadata = nlohmann::json::parse(lifecycle_detail::read_text(checks));
    const auto digest = metadata.at("package_digest").get<std::string>();
    for (const auto &[relative, expected] : metadata.at("files").items()) {
        const auto file = source / relative;
        if (!std::filesystem::is_regular_file(file) ||
            ("streamfind:" + streamfind::fingerprint::hash(lifecycle_detail::read_text(file))) != expected.get<std::string>())
            throw std::runtime_error("package checksum verification failed: " + relative);
    }
    const auto target = managed / "extensions" / "plugins" / registry_id / version / lifecycle_detail::platform();
    const auto registry_path = managed / "extensions" / "registry.json";
    streamfind::PluginRegistry registry;
    if (std::filesystem::exists(registry_path)) {
        const auto loaded = streamfind::load_plugin_registry(registry_path);
        if (!loaded.valid) throw std::runtime_error(loaded.diagnostics);
        registry = loaded.registry;
    }
    for (const auto &entry : registry.installed) if (entry.plugin_id == registry_id && entry.version == version && entry.platform == lifecycle_detail::platform()) {
        if (entry.package_digest == digest) { std::cout << "already installed " << id << " " << version << "\n"; return 0; }
        throw std::runtime_error("different package already installed at same ID/version/platform; increment plugin version in plugin.json");
    }
    const auto temporary = target.string() + ".staging"; std::error_code error; std::filesystem::remove_all(temporary, error);
    std::filesystem::create_directories(temporary);
    std::filesystem::copy(source, temporary, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove_all(target, error); std::filesystem::create_directories(target.parent_path()); std::filesystem::rename(temporary, target);
    std::filesystem::path source_path;
    const auto provenance = source / "provenance.json";
    if (std::filesystem::is_regular_file(provenance)) {
        try {
            const auto document = nlohmann::json::parse(lifecycle_detail::read_text(provenance));
            if (document.value("source", nlohmann::json{}).is_string())
                source_path = document.at("source").get<std::string>();
        } catch (const std::exception &) { }
    }
    registry.generation++;
    registry.installed.push_back({registry_id, version, lifecycle_detail::platform(), target, digest,
                                  "user", "verified", false, "compatible", {}, source_path});
    streamfind::write_plugin_registry_atomic(registry_path, registry);
    std::cout << "installed inactive " << id << " " << version << " at " << target.string() << "\n"; return 0;
}

int uninstall_plugin(const Request &request) {
    const auto managed = request.managed_home.value_or(resolve_managed_home()); const auto path = managed / "extensions" / "registry.json";
    const auto loaded = streamfind::load_plugin_registry(path); if (!loaded.valid) throw std::runtime_error(loaded.diagnostics);
    auto registry = loaded.registry; bool removed = false;
    for (auto it = registry.installed.begin(); it != registry.installed.end();) {
        if (it->plugin_id == request.name && (!request.version.empty() && it->version != request.version)) { ++it; continue; }
        if (it->plugin_id == request.name && it->active) throw std::runtime_error("cannot uninstall active plugin; deactivate it first");
        if (it->plugin_id == request.name) { std::error_code error; std::filesystem::remove_all(it->package_path, error); it = registry.installed.erase(it); removed = true; } else ++it;
    }
    if (!removed) throw std::runtime_error("installed plugin was not found: " + request.name);
    registry.generation++; streamfind::write_plugin_registry_atomic(path, registry); return 0;
}

int deactivate_plugin(const Request &request) {
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto path = managed / "extensions" / "registry.json";
    const auto loaded = streamfind::load_plugin_registry(path);
    if (!loaded.valid) throw std::runtime_error(loaded.diagnostics);
    auto registry = loaded.registry; bool changed = false;
    for (auto &entry : registry.installed) {
        if (entry.plugin_id == request.name && (request.version.empty() || entry.version == request.version)) {
            entry.active = false; changed = true;
        }
    }
    if (!changed) throw std::runtime_error("installed plugin was not found: " + request.name);
    registry.generation++; streamfind::write_plugin_registry_atomic(path, registry); return 0;
}

int activate_plugin(const Request &request) {
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto path = managed / "extensions" / "registry.json";
    const auto loaded = streamfind::load_plugin_registry(path);
    if (!loaded.valid) throw std::runtime_error(loaded.diagnostics);
    auto registry = loaded.registry; bool found = false; bool changed = false;
    for (auto &entry : registry.installed) {
        if (entry.plugin_id != request.name || entry.platform != lifecycle_detail::platform()) continue;
        found = true;
        const bool requested = request.version.empty() || entry.version == request.version;
        if (requested && !entry.active) { entry.active = true; changed = true; }
        else if (!requested && entry.active) { entry.active = false; changed = true; }
    }
    if (!found) throw std::runtime_error("installed plugin was not found: " + request.name);
    if (request.version.empty()) {
        bool active = false;
        for (const auto &entry : registry.installed)
            if (entry.plugin_id == request.name && entry.platform == lifecycle_detail::platform() && entry.active) active = true;
        if (!active) throw std::runtime_error("no version selected for activation: " + request.name);
    }
    if (changed) { registry.generation++; streamfind::write_plugin_registry_atomic(path, registry); }
    std::cout << "activated " << request.name << (request.version.empty() ? "" : " " + request.version) << "\n";
    return 0;
}

} // namespace streamfind::dev
