#include "streamfind/sdk/plugin_manifest.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace streamfind::sdk::plugin_validation {

struct Arguments {
    std::filesystem::path source_dir;
    std::filesystem::path output_dir;
    std::filesystem::path manifest;
    std::string domain;
    std::string library;
};

void usage() {
    std::cerr << "usage: streamfind_sdk_plugin_validator"
                 " --source-dir <dir> --output-dir <dir>"
                 " --manifest <file> --domain <id> --library <file>\n";
}

bool take_value(int &index, int argc, char **argv, std::string_view option, std::string &value) {
    if (argv[index] != option || index + 1 >= argc)
        return false;
    value = argv[++index];
    return !value.empty();
}

bool parse_arguments(int argc, char **argv, Arguments &arguments) {
    for (int index = 1; index < argc; ++index) {
        std::string value;
        if (take_value(index, argc, argv, "--source-dir", value))
            arguments.source_dir = value;
        else if (take_value(index, argc, argv, "--output-dir", value))
            arguments.output_dir = value;
        else if (take_value(index, argc, argv, "--manifest", value))
            arguments.manifest = value;
        else if (take_value(index, argc, argv, "--domain", value))
            arguments.domain = value;
        else if (take_value(index, argc, argv, "--library", value))
            arguments.library = value;
        else {
            usage();
            return false;
        }
    }
    return !arguments.source_dir.empty() && !arguments.output_dir.empty() &&
           !arguments.manifest.empty() && !arguments.domain.empty() && !arguments.library.empty();
}

bool require_directory(const std::filesystem::path &path, const char *label) {
    if (std::filesystem::is_directory(path))
        return true;
    std::cerr << "plugin validation: missing " << label << " directory: " << path << '\n';
    return false;
}

bool require_file(const std::filesystem::path &path, const char *label) {
    if (std::filesystem::is_regular_file(path))
        return true;
    std::cerr << "plugin validation: missing " << label << ": " << path << '\n';
    return false;
}

int run(const Arguments &arguments) {
    bool valid = true;
    valid &= require_file(arguments.source_dir / "CMakeLists.txt", "CMakeLists.txt");
    valid &= require_file(arguments.source_dir / "plugin.json", "source plugin.json");
    valid &= require_directory(arguments.source_dir / "semantic", "semantic");
    valid &= require_directory(arguments.source_dir / "src", "src");
    valid &= require_file(arguments.source_dir / "src/plugin_entrypoint.cpp", "src/plugin_entrypoint.cpp");
    valid &= require_directory(arguments.source_dir / "src/methods", "src/methods");
    valid &= require_directory(arguments.source_dir / "src/operations", "src/operations");
    valid &= require_directory(arguments.source_dir / "src/utils", "src/utils");
    if (!valid)
        return 1;

    const auto source_manifest = load_plugin_manifest(arguments.manifest);
    if (!source_manifest.valid) {
        std::cerr << "plugin validation: invalid source manifest: "
                  << source_manifest.diagnostics << '\n';
        return 2;
    }
    const auto &manifest = source_manifest.manifest;
    if (manifest.plugin_id != arguments.domain || manifest.domain != arguments.domain) {
        std::cerr << "plugin validation: manifest domain does not match "
                  << arguments.domain << '\n';
        return 3;
    }
    if (manifest.static_composition || manifest.semantic_catalogue != "catalogue.duckdb") {
        std::cerr << "plugin validation: manifest is not a dynamic catalogue plugin\n";
        return 4;
    }
    bool declared_library = false;
    for (const auto &[platform, library] : manifest.libraries)
        declared_library |= library == arguments.library;
    if (!declared_library) {
        std::cerr << "plugin validation: built library is absent from manifest: "
                  << arguments.library << '\n';
        return 5;
    }

    const auto output_manifest_path = arguments.output_dir / "plugin.json";
    const auto output_catalogue = arguments.output_dir / "catalogue.duckdb";
    const auto output_library = arguments.output_dir / arguments.library;
    valid &= require_file(output_manifest_path, "staged plugin.json");
    valid &= require_file(output_catalogue, "staged catalogue.duckdb");
    valid &= require_file(output_library, "staged plugin library");
    if (!valid)
        return 6;

    const auto output_manifest = load_plugin_manifest(output_manifest_path);
    if (!output_manifest.valid || output_manifest.manifest.plugin_id != manifest.plugin_id ||
        output_manifest.manifest.domain != manifest.domain) {
        std::cerr << "plugin validation: staged manifest does not match source manifest\n";
        return 7;
    }
    std::cout << "SDK plugin validation passed: " << arguments.domain << '\n';
    return 0;
}

}  // namespace streamfind::sdk::plugin_validation

int main(int argc, char **argv) {
    streamfind::sdk::plugin_validation::Arguments arguments;
    if (!streamfind::sdk::plugin_validation::parse_arguments(argc, argv, arguments))
        return 64;
    return streamfind::sdk::plugin_validation::run(arguments);
}
