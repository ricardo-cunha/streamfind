#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

#include "streamfind/project.hpp"
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
        first.validate();
    } catch (const Error &error) {
        missing_domain_table_rejected = error.code() == ErrorCode::SchemaMismatch;
    }
    if (!missing_domain_table_rejected) throw std::runtime_error("missing domain table was accepted");
    for (const auto &table : schema.at("domains").at("mass_spec").at("required_tables"))
        first.execute_sql("CREATE TABLE " + table.get<std::string>() + " (value VARCHAR)");
    first.validate();
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
