#include <iostream>
#include <stdexcept>
#include <string>

#include "streamfind/catalogue.hpp"

namespace streamfind::test {

void expect_rejected(const streamfind::Json &base,
                     const streamfind::Json &plugin,
                     const std::string &plugin_id,
                     const std::string &module_id,
                     const std::string &ontology_namespace) {
    try {
        streamfind::catalogue::import_plugin_catalogue(
            base, plugin, plugin_id, {module_id}, ontology_namespace);
    } catch (const std::invalid_argument &) {
        return;
    }
    throw std::runtime_error("invalid plugin catalogue ownership was accepted");
}

}  // namespace streamfind::test

int main() {
    try {
        const std::string plugin_id = "native.plugin.example";
        const std::string module_id = "example.module";
        const std::string ontology_namespace = "https://streamfind.dev/catalogue/plugins/example#";
        const streamfind::Json base = {
            {"version", 2},
            {"entries", streamfind::Json::array({streamfind::Json{{"canonical_id", "core.entry"}}})},
            {"tables", streamfind::Json::array()}};
        const streamfind::Json plugin = {
            {"version", 2},
            {"metadata", streamfind::Json{{"plugin_id", plugin_id},
                                           {"ontology_namespace", ontology_namespace},
                                           {"named_graph", ontology_namespace}}},
            {"entries", streamfind::Json::array({streamfind::Json{{"canonical_id", "example.entry"},
                                                                     {"plugin", plugin_id},
                                                                     {"module_id", module_id}}})},
            {"tables", streamfind::Json::array()}};

        const auto merged = streamfind::catalogue::import_plugin_catalogue(
            base, plugin, plugin_id, {module_id}, ontology_namespace);
        if (merged.at("entries").size() != 2)
            throw std::runtime_error("valid plugin catalogue was not merged");

        auto wrong_namespace = plugin;
        wrong_namespace["metadata"]["named_graph"] = "https://streamfind.dev/catalogue/plugins/other#";
        streamfind::test::expect_rejected(
            base, wrong_namespace, plugin_id, module_id, ontology_namespace);

        auto wrong_owner = plugin;
        wrong_owner["entries"][0]["plugin"] = "native.plugin.other";
        streamfind::test::expect_rejected(
            base, wrong_owner, plugin_id, module_id, ontology_namespace);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "failure: " << error.what() << '\n';
        return 3;
    }
}
