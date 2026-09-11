#include "streamfind/sdk/catalogue_builder.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace streamfind::sdk::detail {

std::filesystem::path argument(int argc, char **argv, const std::string &name) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == name) return argv[index + 1];
    }
    return {};
}

bool has_flag(int argc, char **argv, const std::string &name) {
    for (int index = 1; index < argc; ++index)
        if (argv[index] == name) return true;
    return false;
}

}  // namespace streamfind::sdk::detail

int main(int argc, char **argv) {
    using streamfind::sdk::detail::argument;
    using streamfind::sdk::detail::has_flag;
    streamfind::sdk::CatalogueBuildRequest request;
    request.resources.core_directory = argument(argc, argv, "--core-semantic");
    for (int index = 1; index + 1 < argc; ++index)
        if (std::string(argv[index]) == "--plugin-semantic")
            request.resources.plugin_directories.emplace_back(argv[index + 1]);
    request.resources.domain_id = argument(argc, argv, "--domain").string();
    request.output_json = argument(argc, argv, "--output-json");
    request.output_matrix = argument(argc, argv, "--output-matrix");
    request.output_database = argument(argc, argv, "--output-db");
    request.catalogue_kind = argument(argc, argv, "--catalogue-kind").string();
    if (request.catalogue_kind.empty()) request.catalogue_kind = "plugin";
    if (has_flag(argc, argv, "--core-only")) request.resources.plugin_directories.clear();
    request.jena_home = streamfind::sdk::jena_home();
    if (request.resources.core_directory.empty() || request.output_database.empty() ||
        (request.catalogue_kind != "aggregate" && request.resources.domain_id.empty()) ||
        (request.catalogue_kind == "plugin" && request.resources.plugin_directories.empty())) {
        std::cerr << "usage: streamfind-sdk-catalogue --core-semantic DIR --plugin-semantic DIR --domain ID "
                     "--catalogue-kind plugin|core [--core-only] --output-json FILE "
                     "--output-matrix FILE --output-db FILE\n";
        return 2;
    }
    const auto result = streamfind::sdk::build_plugin_catalogue(request);
    if (!result.success) {
        std::cerr << result.diagnostics << '\n';
        return 1;
    }
    return 0;
}
