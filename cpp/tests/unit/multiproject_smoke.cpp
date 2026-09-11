#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

#include "streamfind/catalogue.hpp"
#include "streamfind/project.hpp"
#include "streamfind/project_table_store.hpp"
#include "../tmp_projects.hpp"

#ifndef STREAMFIND_MULTIPROJECT_FIXTURE
#error STREAMFIND_MULTIPROJECT_FIXTURE is required
#endif
#ifndef STREAMFIND_DOMAIN_SCHEMA_FIXTURE
#error STREAMFIND_DOMAIN_SCHEMA_FIXTURE is required
#endif

namespace streamfind::multiproject {

Json fixture() {
    std::ifstream input(STREAMFIND_MULTIPROJECT_FIXTURE);
    return Json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
}

Json domain_schema_fixture() {
    std::ifstream input(STREAMFIND_DOMAIN_SCHEMA_FIXTURE);
    return Json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
}

std::string duckdb_type(const std::string &semantic_type) {
    if (semantic_type == "integer") return "INTEGER";
    if (semantic_type == "real") return "DOUBLE";
    if (semantic_type == "boolean") return "BOOLEAN";
    if (semantic_type == "timestamp") return "TIMESTAMP";
    if (semantic_type == "binary") return "BLOB";
    if (semantic_type == "object" || semantic_type == "array") return "JSON";
    return "VARCHAR";
}

std::string quote_identifier(const std::string &value) {
    std::string quoted = "\"";
    for (const char character : value) quoted += character == '"' ? "\"\"" : std::string(1, character);
    return quoted + "\"";
}

void run() {
    const auto expected = fixture();
    const auto root = streamfind::test::tmp_projects_dir();
    const auto first_path = root / "streamfind-cpp-project-a.duckdb";
    const auto second_path = root / "streamfind-cpp-project-b.duckdb";
    std::filesystem::remove(first_path);
    std::filesystem::remove(second_path);
    const auto &first_spec = expected.at("databases").at(0);
    const auto &second_spec = expected.at("databases").at(1);
    ProjectOptions first_options{first_path, first_spec.at("domain_id").get<std::string>(), {{"owner", "project-a"}}};
    first_options.domain = first_spec.at("domain_id").get<std::string>();
    ProjectOptions second_options{second_path, second_spec.at("domain_id").get<std::string>(), {{"owner", "project-b"}}};
    second_options.domain = second_spec.at("domain_id").get<std::string>();
    auto first = Project::create(first_options);
    first.set_metadata({{"owner", "project-a"}});
    const auto schema = domain_schema_fixture();
    bool missing_domain_table_rejected = false;
    try {
        ProjectTableStore(first).require_manifest("mass_spec", "mass_spec.base");
    } catch (const Error &error) {
        missing_domain_table_rejected = error.code() == ErrorCode::SchemaMismatch;
    }
    if (!missing_domain_table_rejected) throw std::runtime_error("missing domain table was accepted");
    const std::array modules{"mass_spec.base", "mass_spec.chromatograms", "mass_spec.nta"};
    std::vector<Json> manifests;
    for (const auto module : modules) {
        const auto manifest = catalogue::table_manifest_json("mass_spec", module);
        if (!manifest) throw std::runtime_error("MassSpec table manifest unavailable");
        manifests.push_back(*manifest);
    }
    for (const auto &table : schema.at("domains").at("mass_spec").at("required_tables")) {
        const Json *found = nullptr;
        for (const auto &manifest : manifests) {
            const auto candidate = std::find_if(manifest.begin(), manifest.end(), [&](const auto &entry) {
                return entry.value("table_name", "") == table.get<std::string>();
            });
            if (candidate != manifest.end()) {
                found = &*candidate;
                break;
            }
        }
        if (!found) throw std::runtime_error("fixture table missing from manifest");
        std::string columns;
        for (const auto &column : found->value("columns", Json::array())) {
            if (!columns.empty()) columns += ", ";
            columns += quote_identifier(column.value("name", "")) + " " +
                       duckdb_type(column.value("type", "string"));
        }
        first.execute_sql("CREATE TABLE " + quote_identifier(table.get<std::string>()) + " (" + columns + ")");
    }
    for (const auto module : modules) ProjectTableStore(first).require_manifest("mass_spec", module);
    bool same_file_rejected = false;
    try {
        Project::create({first_path, "mass_spec", {}});
    } catch (const Error &error) {
        same_file_rejected = error.code() == ErrorCode::ProjectAlreadyExists;
    }
    if (!same_file_rejected) throw std::runtime_error("second project was accepted in one DuckDB file");
    auto second = Project::create(second_options);
    second.set_metadata({{"owner", "project-b"}});
        first.close();
    second.close();
    auto reopened_first = Project::open(first_options);
    auto reopened_second = Project::open(second_options);
    if (reopened_first.get_domain() != first_spec.at("domain_id").get<std::string>() ||
        reopened_second.get_domain() != second_spec.at("domain_id").get<std::string>()) {
        throw std::runtime_error("separate project files did not reopen");
    }
    reopened_first.close();
    reopened_second.close();

    const auto invalid_path = root / "streamfind-cpp-invalid-registry.duckdb";
    std::filesystem::remove(invalid_path);
    auto invalid = Project::create({invalid_path, "raman", {}});
    invalid.execute_sql("CREATE TABLE PROJECTS (legacy_key VARCHAR NOT NULL, domain_id VARCHAR NOT NULL)");
    invalid.close();
    bool legacy_registry_rejected = false;
    try {
        Project::open({invalid_path, "raman", {}});
    } catch (const Error &error) {
        legacy_registry_rejected = error.code() == ErrorCode::SchemaMismatch;
    }
    if (!legacy_registry_rejected) throw std::runtime_error("legacy PROJECTS registry was accepted");
    std::filesystem::remove(invalid_path);

    std::filesystem::remove(first_path);
    std::filesystem::remove(second_path);
}

} // namespace streamfind::multiproject

int main() {
    try {
        streamfind::multiproject::run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
