#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>

#include "streamfind/catalogue.hpp"
#include "streamfind/catalogue_binding.hpp"
#include "streamfind/mass_spec/register.hpp"
#include "streamfind/raman/register.hpp"
#include "streamfind/sensors/register.hpp"
#include "streamfind/mcp.hpp"
#include "streamfind/project_table_store.hpp"

#ifndef STREAMFIND_CATALOGUE_PATH
#error STREAMFIND_CATALOGUE_PATH is required
#endif

namespace streamfind::test::registration_contract {

catalogue::OperationBinding probe_binding() {
    return {"mass_spec.get_analyses_info", [](Project &, const Json &) { return Json::array(); }, {}};
}

catalogue::DomainModuleBinding probe_module() {
    return {"mass_spec.base", "mass_spec", "1", {}, {}, {probe_binding()}, {}, {}};
}

void assert_project_scoped_schema_binding() {
    auto module = probe_module();
    module.schema_binding = [](ProjectTableStore &tables) {
        tables.ensure_table("STAGE5_PROOF", "CREATE TABLE STAGE5_PROOF (value INTEGER)");
    };
    const auto path = std::filesystem::path(STREAMFIND_TEST_REPO_ROOT) / "tmp" / "projects" / "stage5-proof.duckdb";
    std::filesystem::create_directories(path.parent_path());
    std::error_code error;
    std::filesystem::remove(path, error);
    ProjectOptions options;
    options.database_path = path;
    options.domain = "mass_spec";
    auto project = Project::create(options);
    catalogue::install_module_schema(module, project);
    if (!ProjectTableStore(project).has_table("STAGE5_PROOF"))
        throw std::runtime_error("project-scoped schema binding did not install its table");
    const auto state = project.query_json("SELECT schema_version FROM MODULE_SCHEMA WHERE module_id = 'mass_spec.base'");
    if (state.size() != 1 || state.at(0).value("schema_version", "") != "1")
        throw std::runtime_error("module schema state was not persisted");

    auto upgraded = module;
    upgraded.schema_version = 2;
    upgraded.schema_migration = [](ProjectTableStore &tables, int from_version) {
        if (from_version != 1) throw std::runtime_error("unexpected migration source version");
        tables.execute("ALTER TABLE STAGE5_PROOF ADD COLUMN migrated INTEGER");
    };
    catalogue::install_module_schema(upgraded, project);
    const auto upgraded_state = project.query_json("SELECT schema_version FROM MODULE_SCHEMA WHERE module_id = 'mass_spec.base'");
    if (upgraded_state.size() != 1 || upgraded_state.at(0).value("schema_version", "") != "2")
        throw std::runtime_error("module schema migration version was not persisted");
    const auto migrated = project.query_json("DESCRIBE STAGE5_PROOF");
    if (std::none_of(migrated.begin(), migrated.end(), [](const auto &column) {
            return column.value("column_name", "") == "migrated";
        }))
        throw std::runtime_error("module schema migration did not alter the table");

    bool foreign_sql_rejected = false;
    try {
        ProjectTableStore::transaction(project, {"STAGE5_PROOF"}, [](ProjectTableStore &tables) {
            tables.query("SELECT * FROM MASS_SPEC_ANALYSES");
        });
    } catch (const std::exception &) {
        foreign_sql_rejected = true;
    }
    if (!foreign_sql_rejected)
        throw std::runtime_error("transaction store accepted SQL for a foreign table");

    auto failed = upgraded;
    failed.schema_version = 3;
    failed.schema_migration = [](ProjectTableStore &tables, int) {
        tables.ensure_table("STAGE5_ROLLBACK", "CREATE TABLE STAGE5_ROLLBACK (value INTEGER)");
        throw std::runtime_error("intentional migration failure");
    };
    bool migration_rejected = false;
    try {
        catalogue::install_module_schema(failed, project);
    } catch (const std::exception &) {
        migration_rejected = true;
    }
    if (!migration_rejected || ProjectTableStore(project).has_table("STAGE5_ROLLBACK"))
        throw std::runtime_error("failed schema migration was not rolled back");
}

void assert_rejected(const Json &entries, const catalogue::DomainModuleBinding &module) {
    MethodRegistry methods;
    OperationRegistry operations;
    bool rejected = false;
    try {
        catalogue::register_module(module, entries, methods, operations);
    } catch (const std::exception &) {
        rejected = true;
    }
    if (!rejected) throw std::runtime_error("invalid module binding was accepted");
}

Json catalogue_document(const Json &entries) {
    return Json{{"version", 2}, {"entries", entries}};
}

void assert_import_rejected(const Json &base, const Json &plugin) {
    bool rejected = false;
    try {
        catalogue::import_plugin_catalogue(base, plugin, "mass_spec", {"mass_spec.base"});
    } catch (const std::exception &) {
        rejected = true;
    }
    if (!rejected) throw std::runtime_error("invalid plugin catalogue was accepted");
}

int run() {
    const auto entries = catalogue::load(STREAMFIND_CATALOGUE_PATH);
    if (!entries) throw std::runtime_error("catalogue load failed");

    std::set<std::string> expected_methods;
    std::set<std::string> expected_operations;
    for (const auto &entry : *entries) {
        if (entry.value("domain", "") != "mass_spec" || !entry.value("executable", false)) continue;
        if (entry.value("kind", "") == "method") expected_methods.insert(entry.at("canonical_id"));
        if (entry.value("kind", "") == "operation") expected_operations.insert(entry.at("canonical_id"));
    }

    MethodRegistry methods;
    OperationRegistry operations;
    mass_spec::register_plugin(*entries, methods, operations);
    assert_project_scoped_schema_binding();
    std::set<std::string> actual_methods;
    for (const auto &definition : methods.list("mass_spec")) actual_methods.insert(definition.id);
    std::set<std::string> actual_operations;
    for (const auto &definition : operations.list("mass_spec")) actual_operations.insert(definition.id);
    if (actual_methods != expected_methods || actual_operations != expected_operations)
        throw std::runtime_error("registered capabilities differ from semantic catalogue");

    MethodRegistry raman_methods;
    OperationRegistry raman_operations;
    raman::register_plugin(*entries, raman_methods, raman_operations);
    if (raman_methods.list("raman").size() != 0 || raman_operations.list("raman").size() != 2)
        throw std::runtime_error("Raman registration does not match the SDK module contract");

    MethodRegistry sensors_methods;
    OperationRegistry sensors_operations;
    sensors::register_plugin(*entries, sensors_methods, sensors_operations);
    if (!sensors_methods.list("sensors").empty() || !sensors_operations.list("sensors").empty())
        throw std::runtime_error("Sensors registration does not match the SDK module contract");

    mcp::Session session(methods, operations);
    const auto tools = session.handle({{"id", 1}, {"method", "tools/list"}}).at("result").at("tools");
    std::set<std::string> mcp_operations;
    for (const auto &tool : tools) {
        const auto name = tool.at("name").get<std::string>();
        if (name.rfind("mass_spec.", 0) == 0) mcp_operations.insert(name);
    }
    std::set<std::string> expected_mcp;
    for (const auto &entry : *entries) {
        if (entry.value("kind", "") == "operation" && entry.value("domain", "") == "mass_spec" &&
            entry.value("executable", false) && entry.value("exposed", false))
            expected_mcp.insert(entry.at("canonical_id"));
    }
    if (mcp_operations != expected_mcp) throw std::runtime_error("MCP intersection differs from catalogue");

    auto unknown = probe_module();
    unknown.operations[0].id = "mass_spec.unknown";
    assert_rejected(*entries, unknown);

    auto wrong_kind_entries = *entries;
    for (auto &entry : wrong_kind_entries)
        if (entry.value("canonical_id", "") == "mass_spec.get_analyses_info") entry["kind"] = "method";
    assert_rejected(wrong_kind_entries, probe_module());

    auto wrong_domain_entries = *entries;
    for (auto &entry : wrong_domain_entries)
        if (entry.value("canonical_id", "") == "mass_spec.get_analyses_info") entry["domain"] = "raman";
    assert_rejected(wrong_domain_entries, probe_module());

    auto wrong_module_entries = *entries;
    for (auto &entry : wrong_module_entries)
        if (entry.value("canonical_id", "") == "mass_spec.get_analyses_info") entry["module_id"] = "mass_spec.nta";
    assert_rejected(wrong_module_entries, probe_module());

    auto duplicate = probe_module();
    duplicate.operations.push_back(probe_binding());
    assert_rejected(*entries, duplicate);

    auto missing_table = probe_module();
    missing_table.tables = {"MASS_SPEC_DOES_NOT_EXIST"};
    assert_rejected(*entries, missing_table);

    auto missing_dependency = probe_module();
    missing_dependency.required_modules = {"mass_spec.missing"};
    assert_rejected(*entries, missing_dependency);

    const Json base = catalogue_document(Json::array({
        Json{{"canonical_id", "core.operation"}, {"domain", "streamfind"},
             {"module_id", "streamfind.base"}},
    }));
    const Json plugin = catalogue_document(Json::array({
        Json{{"canonical_id", "mass_spec.plugin_operation"}, {"domain", "mass_spec"},
             {"module_id", "mass_spec.base"}},
    }));
    const auto merged = catalogue::import_plugin_catalogue(
        base, plugin, "mass_spec", {"mass_spec.base"});
    if (merged.at("entries").size() != 2 || base.at("entries").size() != 1)
        throw std::runtime_error("plugin catalogue import did not produce an isolated merged snapshot");

    assert_import_rejected(base, catalogue_document(Json::array({
        Json{{"canonical_id", "core.operation"}, {"domain", "mass_spec"},
             {"module_id", "mass_spec.base"}},
    })));
    assert_import_rejected(base, catalogue_document(Json::array({
        Json{{"canonical_id", "mass_spec.wrong_domain"}, {"domain", "raman"},
             {"module_id", "mass_spec.base"}},
    })));
    assert_import_rejected(base, catalogue_document(Json::array({
        Json{{"canonical_id", "mass_spec.wrong_module"}, {"domain", "mass_spec"},
             {"module_id", "mass_spec.nta"}},
    })));
    assert_import_rejected(base, Json{{"version", 1}, {"entries", Json::array()}});
    assert_import_rejected(base, Json{{"version", 2}, {"entries", Json::object()}});
    return 0;
}

}  // namespace streamfind::test::registration_contract

int main() {
    try {
        return streamfind::test::registration_contract::run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
