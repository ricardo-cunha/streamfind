#include "streamfind/dev/command_protocol.hpp"

#include "streamfind/fingerprint.hpp"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace streamfind::dev::viewer_detail {

using Json = nlohmann::json;

std::string read_text(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to read viewer file: " + path.string());
    std::ostringstream text; text << input.rdbuf(); return text.str();
}

void write_atomic(const std::filesystem::path &path, const Json &value) {
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.string() + ".tmp";
    { std::ofstream output(temporary, std::ios::binary | std::ios::trunc); if (!output) throw std::runtime_error("unable to write viewer state: " + path.string()); output << value.dump(2) << '\n'; }
    std::filesystem::rename(temporary, path);
}

std::string platform() {
#ifdef _WIN32
    return "windows-x86_64";
#else
    return "linux-x86_64";
#endif
}

Json manifest(const std::filesystem::path &root) {
    const auto value = Json::parse(read_text(root / "plugin.manifest.json"));
    if (!value.is_object() || value.value("kind", "") != "viewer" ||
        !value.value("id", std::string{}).starts_with("@streamfind/") ||
        !value.value("version", std::string{}).size() || !value.value("apiVersion", std::string{}).size())
        throw std::invalid_argument("viewer manifest requires kind, @streamfind ID, version and apiVersion");
    if (!std::regex_match(value.at("id").get<std::string>(), std::regex("@streamfind/[a-z][a-z0-9_-]*")))
        throw std::invalid_argument("viewer ID must match @streamfind/[a-z][a-z0-9_-]*");
    if (!value.contains("artifactContracts") || !value.at("artifactContracts").is_array())
        throw std::invalid_argument("viewer manifest requires artifactContracts");
    return value;
}

bool path_within(const std::filesystem::path &root, const std::filesystem::path &value);

std::string digest(const std::filesystem::path &root) {
    const auto checks = Json::parse(read_text(root / "checksums.json"));
    const auto expected = checks.value("package_digest", std::string{});
    if (expected.empty() || !checks.contains("files") || !checks.at("files").is_object())
        throw std::invalid_argument("viewer checksums.json is invalid");
    for (const auto &[relative, hash] : checks.at("files").items()) {
        const auto file = std::filesystem::weakly_canonical(root / relative);
        if (!std::filesystem::is_regular_file(file) || !path_within(root, file))
            throw std::invalid_argument("viewer checksum file is outside its package: " + relative);
        if ("streamfind:" + fingerprint::hash(read_text(file)) != hash.get<std::string>())
            throw std::invalid_argument("viewer package checksum failed: " + relative);
    }
    return expected;
}

std::filesystem::path package_root(const std::filesystem::path &managed, const std::string &id, const std::string &version) {
    return managed / "extensions" / "viewers" / id.substr(std::string("@streamfind/").size()) / version / platform();
}

bool path_within(const std::filesystem::path &root, const std::filesystem::path &value) {
    const auto canonical_root = std::filesystem::weakly_canonical(root);
    const auto canonical_value = std::filesystem::weakly_canonical(value);
    auto root_it = canonical_root.begin();
    auto value_it = canonical_value.begin();
    for (; root_it != canonical_root.end(); ++root_it, ++value_it)
        if (value_it == canonical_value.end() || *root_it != *value_it) return false;
    return true;
}

Json activation(const std::filesystem::path &root) {
    const auto path = root / "activation.json";
    if (!std::filesystem::is_regular_file(path)) return Json{{"active", false}};
    return Json::parse(read_text(path));
}

void set_activation(const std::filesystem::path &root, bool active) {
    const auto info = manifest(root);
    const auto package_digest = digest(root);
    write_atomic(root / "activation.json", Json{{"schema_version", 1}, {"id", info.at("id")},
        {"version", info.at("version")}, {"package_digest", package_digest}, {"active", active}});
}

template <typename Action>
int for_viewer(const Request &request, Action action) {
    if (!std::regex_match(request.name, std::regex("@streamfind/[a-z][a-z0-9_-]*")))
        throw std::invalid_argument("viewer ID must match @streamfind/[a-z][a-z0-9_-]*");
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto root = managed / "extensions" / "viewers";
    const auto id_root = root / request.name.substr(std::string("@streamfind/").size());
    if (!std::filesystem::is_directory(id_root)) throw std::runtime_error("viewer was not found: " + request.name);
    bool changed = false;
    for (const auto &version : std::filesystem::directory_iterator(id_root)) {
        if (!version.is_directory() || (!request.version.empty() && version.path().filename() != request.version)) continue;
        const auto package = version.path() / platform();
        if (!std::filesystem::is_directory(package)) continue;
        action(package); changed = true;
    }
    if (!changed) throw std::runtime_error("viewer version was not found: " + request.name);
    return 0;
}

} // namespace streamfind::dev::viewer_detail

namespace streamfind::dev {

int install_viewer(const Request &request) {
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto workspace = resolve_workspace(request.workspace, managed);
    const auto source = canonicalize_user_path(*request.package, true, managed, workspace);
    const auto info = viewer_detail::manifest(source);
    const auto id = info.at("id").get<std::string>();
    const auto version = info.at("version").get<std::string>();
    const auto package_digest = viewer_detail::digest(source);
    const auto target = viewer_detail::package_root(managed, id, version);
    if (std::filesystem::is_directory(target)) {
        if (viewer_detail::digest(target) == package_digest) {
            std::cout << "viewer already installed " << id << " " << version << "\n";
            return 0;
        }
        throw std::runtime_error("different viewer package already installed at same ID/version; increment version");
    }
    const auto temporary = target.string() + ".staging";
    std::error_code error; std::filesystem::remove_all(temporary, error);
    std::filesystem::create_directories(temporary);
    std::filesystem::copy(source, temporary, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove_all(target, error); std::filesystem::create_directories(target.parent_path()); std::filesystem::rename(temporary, target);
    viewer_detail::write_atomic(target / "activation.json", nlohmann::json{{"schema_version", 1}, {"id", id},
        {"version", version}, {"package_digest", package_digest}, {"active", false}});
    std::cout << "installed inactive viewer " << id << " " << version << " at " << target.string() << "\n";
    return 0;
}

int activate_viewer(const Request &request) {
    return viewer_detail::for_viewer(request, [](const auto &package) { viewer_detail::set_activation(package, true); });
}

int deactivate_viewer(const Request &request) {
    return viewer_detail::for_viewer(request, [](const auto &package) { viewer_detail::set_activation(package, false); });
}

int uninstall_viewer(const Request &request) {
    if (!std::regex_match(request.name, std::regex("@streamfind/[a-z][a-z0-9_-]*")))
        throw std::invalid_argument("viewer ID must match @streamfind/[a-z][a-z0-9_-]*");
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto id_root = managed / "extensions" / "viewers" / request.name.substr(std::string("@streamfind/").size());
    if (!std::filesystem::is_directory(id_root)) throw std::runtime_error("viewer was not found: " + request.name);
    bool removed = false;
    for (const auto &version : std::filesystem::directory_iterator(id_root)) {
        if (!version.is_directory() || (!request.version.empty() && version.path().filename() != request.version)) continue;
        const auto package = version.path() / viewer_detail::platform();
        if (viewer_detail::activation(package).value("active", false)) throw std::runtime_error("cannot uninstall active viewer; deactivate it first");
        std::error_code error; std::filesystem::remove_all(version.path(), error);
        removed = true;
    }
    if (!removed) throw std::runtime_error("viewer version was not found: " + request.name);
    return 0;
}

} // namespace streamfind::dev
