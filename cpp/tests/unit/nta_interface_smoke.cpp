#include <iostream>
#include <set>
#include <string>

#include "streamfind/catalogue.hpp"
#include "streamfind/mass_spec/register.hpp"

#ifndef STREAMFIND_CATALOGUE_PATH
#error STREAMFIND_CATALOGUE_PATH is required
#endif

int main() {
    const auto entries = streamfind::catalogue::load(STREAMFIND_CATALOGUE_PATH);
    if (!entries) {
        std::cerr << "NTA interface: catalogue load failed\n";
        return 1;
    }

    std::set<std::string> expected_ids;
    for (const auto &entry : *entries) {
        if (entry.value("kind", "") == "method" && entry.value("domain", "") == "mass_spec") {
            expected_ids.insert(entry.value("canonical_id", ""));
        }
    }
    if (expected_ids.size() != 19) {
        std::cerr << "NTA interface: expected 19 catalogue Methods, found " << expected_ids.size() << '\n';
        return 1;
    }

    streamfind::MethodRegistry methods;
    streamfind::OperationRegistry operations;
    streamfind::mass_spec::register_plugin(*entries, methods, operations);
    const auto registered = methods.list("mass_spec");
    std::set<std::string> actual_ids;
    for (const auto &definition : registered) {
        if (definition.id.rfind("mass_spec.", 0) == 0) actual_ids.insert(definition.id);
    }
    if (actual_ids != expected_ids) {
        std::cerr << "NTA interface: registered Method set differs from catalogue\n";
        return 1;
    }
    for (const auto &entry : *entries) {
        if (entry.value("kind", "") == "method" && entry.value("domain", "") == "mass_spec" &&
            entry.value("module_id", "") != "mass_spec.nta" &&
            entry.value("module_id", "") != "mass_spec.chromatograms") {
            std::cerr << "NTA interface: invalid module ownership\n";
            return 1;
        }
    }

    const auto *find_features = methods.find("mass_spec.find_features");
    if (!find_features->definition().cacheable || !find_features->definition().single_occurrence) {
        std::cerr << "NTA interface: find_features lifecycle metadata mismatch\n";
        return 1;
    }
    return 0;
}
