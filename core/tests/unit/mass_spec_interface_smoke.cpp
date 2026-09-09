#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>

#include "streamfind/catalogue.hpp"
#include "streamfind/mass_spec/register.hpp"
#include "streamfind/project.hpp"
#include "../tmp_projects.hpp"

#ifndef STREAMFIND_CATALOGUE_PATH
#error STREAMFIND_CATALOGUE_PATH is required
#endif

int main() {
    const auto entries = streamfind::catalogue::load(STREAMFIND_CATALOGUE_PATH);
    if (!entries) {
        std::cerr << "mass-spec interface: catalogue load failed\n";
        return 1;
    }

    const auto expected = static_cast<std::size_t>(std::count_if(
        entries->begin(), entries->end(), [](const auto &entry) {
            return entry.value("kind", "") == "operation" &&
                   entry.value("domain", "") == "mass_spec";
        }));

    streamfind::OperationRegistry operations;
    streamfind::mass_spec::register_operations(operations);
    if (operations.list("mass_spec").size() != expected) {
        std::cerr << "mass-spec interface: operation count mismatch\n";
        return 1;
    }

    for (const auto &definition : operations.list("mass_spec")) {
        const auto found = std::find_if(entries->begin(), entries->end(), [&](const auto &entry) {
            return entry.value("canonical_id", "") == definition.id;
        });
        if (found == entries->end()) {
            std::cerr << "mass-spec interface: catalogue entry missing for " << definition.id << '\n';
            return 1;
        }
        std::set<std::string> actual;
        for (const auto &parameter : definition.parameters.definitions) {
            actual.insert(parameter.name);
        }
        std::set<std::string> expected_parameters;
        for (const auto &parameter : found->value("parameters", streamfind::Json::array())) {
            expected_parameters.insert(parameter.value("name", ""));
        }
        if (actual != expected_parameters) {
            std::cerr << "mass-spec interface: parameter mismatch for " << definition.id << '\n';
            return 1;
        }
    }

    const auto database = streamfind::test::tmp_projects_dir() / "streamfind-cpp-mass-spec-interface.duckdb";
    std::error_code error;
    std::filesystem::remove(database, error);
    auto project = streamfind::Project::create(
        {database, "mass_spec", {{"owner", "project-a"}}});

    const auto analyses = project.run_operation("mass_spec.get_analyses_info", {}, operations);
    if (analyses.at("row_count") != 0) {
        std::cerr << "mass-spec interface: new project is not empty\n";
        return 1;
    }
    const auto tables = project.list_tables();
    for (const auto &table : {"MASS_SPEC_ANALYSES", "MASS_SPEC_SPECTRA_HEADERS",
                              "MASS_SPEC_CHROMATOGRAMS_HEADERS"}) {
        if (std::find(tables.begin(), tables.end(), table) == tables.end()) {
            std::cerr << "mass-spec interface: missing table " << table << '\n';
            return 1;
        }
    }
    project.close();
    std::filesystem::remove(database, error);
    return 0;
}
