#include "streamfind/dev/command_protocol.hpp"

#include "streamfind/fingerprint.hpp"
#include "streamfind/plugin_registry.hpp"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace streamfind::dev::viewer_development_detail {

using Json = nlohmann::json;

std::string read_text(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to read viewer file: " + path.string());
    std::ostringstream text; text << input.rdbuf(); return text.str();
}

void write_text(const std::filesystem::path &path, const std::string &value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("unable to write viewer file: " + path.string());
    output << value;
}

void write_json(const std::filesystem::path &path, const Json &value) {
    write_text(path, value.dump(2) + "\n");
}

std::string platform() {
#ifdef _WIN32
    return "windows-x86_64";
#else
    return "linux-x86_64";
#endif
}

std::filesystem::path node_root(const std::filesystem::path &managed) {
    const auto root = managed / "tools" / "node";
    if (!std::filesystem::is_directory(root))
        throw std::runtime_error("managed Node.js is unavailable; run streamfind-dev setup --target viewer first");
    std::filesystem::path selected;
    for (const auto &version : std::filesystem::directory_iterator(root)) {
        if (!version.is_directory()) continue;
#ifdef _WIN32
        const auto candidate = version.path() / "win-x64";
        if (std::filesystem::is_regular_file(candidate / "node.exe")) selected = candidate;
#else
        const auto candidate = version.path() / "linux-x64";
        if (std::filesystem::is_regular_file(candidate / "bin" / "node")) selected = candidate;
#endif
    }
    if (selected.empty()) throw std::runtime_error("managed Node.js installation is incomplete");
    return selected;
}

int npm(const Request &request, const std::filesystem::path &viewer,
        const std::vector<std::string> &arguments) {
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto node = node_root(managed);
#ifdef _WIN32
    const auto program = node / "npm.cmd";
    const auto current_path = std::getenv("PATH") == nullptr ? std::string{} : std::string(std::getenv("PATH"));
    const auto path = node.string() + ";" + current_path;
#else
    const auto program = node / "bin" / "npm";
    const auto current_path = std::getenv("PATH") == nullptr ? std::string{} : std::string(std::getenv("PATH"));
    const auto path = node.string() + "/bin:" + current_path;
#endif
    const auto result = run_process(program, arguments, viewer, 3600000, 8 * 1024 * 1024,
                                    std::nullopt, true, {"PATH=" + path});
    if (!result.output.empty()) std::cout << result.output;
    if (result.timed_out) throw std::runtime_error("viewer npm command timed out");
    if (result.cancelled) throw std::runtime_error("viewer npm command cancelled");
    if (result.exit_code != 0) throw std::runtime_error("viewer npm command failed (" + std::to_string(result.exit_code) + ")");
    return 0;
}

std::filesystem::path source(const Request &request) {
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto workspace = resolve_workspace(request.workspace, managed);
    return canonicalize_user_path(*request.viewer, true, managed, workspace);
}

Json manifest(const std::filesystem::path &root) {
    const auto value = Json::parse(read_text(root / "plugin.manifest.json"));
    if (!value.is_object() || value.value("kind", "") != "viewer" ||
        !std::regex_match(value.value("id", ""), std::regex("@streamfind/[a-z][a-z0-9_-]*")) ||
        value.value("version", "").empty() || value.value("apiVersion", "").empty())
        throw std::invalid_argument("viewer manifest is invalid");
    return value;
}

Json checksums(const std::filesystem::path &root) {
    Json files = Json::object();
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().filename() == "checksums.json") continue;
        const auto relative = std::filesystem::relative(entry.path(), root).generic_string();
        files[relative] = "streamfind:" + fingerprint::hash(read_text(entry.path()));
    }
    return files;
}

void scaffold_file(const std::filesystem::path &root, const std::string &name,
                   const std::string &contract) {
    const auto package = "@streamfind/" + name + "-viewer";
    write_json(root / "package.json", Json{{"name", package}, {"private", true}, {"version", "0.1.0"},
        {"type", "module"}, {"scripts", {{"build", "vite build"}, {"test", "vitest run"}}},
        {"dependencies", {{"react", "^18.3.1"}, {"react-dom", "^18.3.1"}}},
        {"devDependencies", {{"@vitejs/plugin-react", "^5.1.0"}, {"typescript", "^5.7.2"}, {"vite", "^6.0.7"}, {"vitest", "^5.0.1"}}}});
    write_json(root / "package-lock.json", Json{{"name", package}, {"version", "0.1.0"}, {"lockfileVersion", 3}, {"requires", true}, {"packages", Json{{"", Json{{"name", package}, {"version", "0.1.0"}, {"private", true}}}}}});
    write_json(root / "plugin.manifest.json", Json{{"schemaVersion", 1}, {"kind", "viewer"}, {"id", package},
        {"name", name + " viewer"}, {"version", "0.1.0"}, {"apiVersion", "1.0"},
        {"activation", "optional"}, {"source", "src/index.tsx"}, {"artifactContracts", Json::array({contract})}});
    write_text(root / "vite.config.ts", "import { defineConfig } from 'vite';\nimport react from '@vitejs/plugin-react';\nexport default defineConfig({ plugins: [react()], build: { manifest: true, lib: { entry: 'src/index.tsx', formats: ['es'], fileName: () => 'viewer.mjs' }, rollupOptions: { external: ['react', 'react-dom', 'react/jsx-runtime', '@streamfind/viewer-sdk'] } } });\n");
    write_json(root / "tsconfig.json", Json{{"compilerOptions", {{"target", "ES2022"}, {"module", "ESNext"}, {"moduleResolution", "Bundler"}, {"jsx", "react-jsx"}, {"strict", true}, {"noEmit", true}}}, {"include", Json::array({"src"})}});
    write_text(root / "src/index.tsx", "import { useState } from 'react';\nimport './viewer.css';\nconst contract = " + Json(contract).dump() + ";\nexport default { manifest: { schemaVersion: 1, kind: 'viewer', id: " + Json(package).dump() + ", name: " + Json(name + " viewer").dump() + ", version: '0.1.0', apiVersion: '1.0', artifactContracts: [contract] }, setup(api: any) { api.registerViewer({ id: " + Json(package + ".viewer").dump() + ", label: " + Json(name + " viewer").dump() + ", accepts: [contract], component: ({ context }: any) => { const [selected, setSelected] = useState(false); return <button className='streamfind-example-viewer' onClick={() => setSelected(!selected)}>{selected ? 'Selected' : 'Select'} {context.resultId}</button>; } }); } };\n");
    write_text(root / "src/viewer.css", ".streamfind-example-viewer { color: var(--accent); }\n");
    write_text(root / "src/index.test.tsx", "import { describe, expect, it } from 'vitest';\nimport viewer from './index';\ndescribe('viewer scaffold', () => { it('exports a viewer manifest', () => { expect(viewer.manifest.kind).toBe('viewer'); }); });\n");
    write_text(root / "README.md", "# StreamFind viewer\n\nBuild with `npm ci` and `npm run build`.\n");
}

} // namespace streamfind::dev::viewer_development_detail

namespace streamfind::dev {

using Json = nlohmann::json;

int scaffold_viewer(const Request &request) {
    if (!std::regex_match(request.name, std::regex("[a-z][a-z0-9_-]*")))
        throw std::invalid_argument("viewer name must match [a-z][a-z0-9_-]*");
    if (!request.contract.starts_with("https://")) throw std::invalid_argument("viewer contract must be a qualified HTTPS IRI");
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto workspace = resolve_workspace(request.workspace, managed);
    const auto root = workspace / "viewers" / request.name;
    if (std::filesystem::exists(root) && !std::filesystem::is_empty(root))
        throw std::runtime_error("viewer destination is not empty: " + root.string());
    viewer_development_detail::scaffold_file(root, request.name, request.contract);
    std::cout << "created viewer project at " << root.string() << "\n";
    return 0;
}

int build_viewer(const Request &request) {
    const auto root = viewer_development_detail::source(request);
    // A freshly scaffolded project has a deliberately small lockfile. Resolve
    // its declared dependencies once, then use npm ci for the reproducible
    // build that follows.
    viewer_development_detail::npm(request, root, {"install", "--package-lock-only", "--engine-strict"});
    viewer_development_detail::npm(request, root, {"ci", "--engine-strict"});
    return viewer_development_detail::npm(request, root, {"run", "build"});
}

int test_viewer(const Request &request) {
    const auto root = viewer_development_detail::source(request);
    return viewer_development_detail::npm(request, root, {"run", "test"});
}

int validate_viewer(const Request &request) {
    const auto root = viewer_development_detail::source(request);
    const auto info = viewer_development_detail::manifest(root);
    const auto bundle = root / "dist" / "viewer.mjs";
    if (!std::filesystem::is_regular_file(bundle)) throw std::runtime_error("viewer build is missing dist/viewer.mjs");
    if (!info.contains("artifactContracts") || info.at("artifactContracts").empty()) throw std::runtime_error("viewer has no artifact contracts");
    std::cout << "validated viewer " << info.at("id").get<std::string>() << "\n";
    return 0;
}

int package_viewer(const Request &request) {
    const auto root = viewer_development_detail::source(request);
    build_viewer(request);
    validate_viewer(request);
    const auto info = viewer_development_detail::manifest(root);
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto workspace = resolve_workspace(request.workspace, managed);
    const auto destination = request.output_file
        ? canonicalize_user_path(*request.output_file, false, managed, workspace)
        : managed / "extensions" / "packages" / "viewers" /
            info.at("id").get<std::string>().substr(std::string("@streamfind/").size()) /
            info.at("version").get<std::string>() / viewer_development_detail::platform();
    std::error_code error; std::filesystem::remove_all(destination, error);
    std::filesystem::create_directories(destination);
    std::filesystem::copy_file(root / "plugin.manifest.json", destination / "plugin.manifest.json");
    std::filesystem::copy(root / "dist", destination / "dist", std::filesystem::copy_options::recursive);
    const auto files = viewer_development_detail::checksums(destination);
    viewer_development_detail::write_json(destination / "checksums.json", Json{{"schema_version", 1},
        {"package_digest", "streamfind:" + fingerprint::hash(files.dump())}, {"files", files}});
    std::cout << "packaged viewer at " << destination.string() << "\n";
    return 0;
}

int list_extensions(const Request &request) {
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    Json result{{"schema_version", 1}, {"managed_home", managed.string()},
                {"plugins", Json::array()}, {"viewers", Json::array()}};
    if (request.target == "native" || request.target == "all") {
        const auto loaded = streamfind::load_plugin_registry(managed / "extensions" / "registry.json");
        if (loaded.valid) for (const auto &entry : loaded.registry.installed)
            result["plugins"].push_back({{"id", entry.plugin_id}, {"version", entry.version},
                {"platform", entry.platform}, {"package_path", entry.package_path.string()},
                {"package_digest", entry.package_digest}, {"active", entry.active}});
    }
    if (request.target == "viewer" || request.target == "all") {
        const auto root = managed / "extensions" / "viewers";
        if (std::filesystem::is_directory(root)) for (const auto &id : std::filesystem::directory_iterator(root)) {
            if (!id.is_directory()) continue;
            for (const auto &version : std::filesystem::directory_iterator(id.path())) {
                if (!version.is_directory()) continue;
                const auto package = version.path() / viewer_development_detail::platform();
                const auto manifest_path = package / "plugin.manifest.json";
                if (!std::filesystem::is_regular_file(manifest_path)) continue;
                try {
                    const auto info = viewer_development_detail::manifest(package);
                    Json activation = Json::object();
                    if (std::filesystem::is_regular_file(package / "activation.json"))
                        activation = Json::parse(viewer_development_detail::read_text(package / "activation.json"));
                    Json listed{{"id", info.at("id")}, {"version", info.at("version")},
                        {"package_path", package.string()}, {"active", activation.value("active", false)}};
                    result["viewers"].push_back(std::move(listed));
                } catch (const std::exception &) { }
            }
        }
    }
    std::cout << result.dump(2) << '\n';
    return 0;
}

int inspect_extension(const Request &request) {
    const auto managed = request.managed_home.value_or(resolve_managed_home());
    const auto workspace = resolve_workspace(request.workspace, managed);
    const auto root = canonicalize_user_path(*request.package, true, managed, workspace);
    Json result{{"package_path", root.string()}};
    if ((request.target == "native" || request.target == "all") && std::filesystem::is_regular_file(root / "plugin.json")) {
        result["kind"] = "native-plugin";
        result["manifest"] = Json::parse(viewer_development_detail::read_text(root / "plugin.json"));
    } else if ((request.target == "viewer" || request.target == "all") && std::filesystem::is_regular_file(root / "plugin.manifest.json")) {
        result["kind"] = "viewer";
        result["manifest"] = viewer_development_detail::manifest(root);
    } else throw std::invalid_argument("package does not contain a recognized native plugin or viewer manifest");
    if (std::filesystem::is_regular_file(root / "checksums.json"))
        result["checksums"] = Json::parse(viewer_development_detail::read_text(root / "checksums.json"));
    std::cout << result.dump(2) << '\n';
    return 0;
}

} // namespace streamfind::dev
