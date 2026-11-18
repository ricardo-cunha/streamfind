#include <filesystem>
#include <algorithm>
#include <iostream>
#include <stdexcept>

#include "streamfind/project.hpp"
#include "../support/tmp_projects.hpp"

int main() {
    const auto path = streamfind::test::tmp_projects_dir() / "streamfind-artifact-publication.duckdb";
    std::error_code error;
    std::filesystem::remove(path, error);
    try {
        auto project = streamfind::Project::create({path, {}});
        project.register_result_manifest({
            {"result_id", "arrow-table-result"}, {"contract_id", "test.table"},
            {"kind", "table"}, {"format", "arrow-ipc-file"}, {"state", "published"},
            {"producer_operation", "test.publish_table"}, {"producer_instance", "publisher"},
            {"workflow_revision", 1}, {"row_count", 1},
            {"schema", streamfind::Json::array({{{"name", "value"}, {"type", "string"}}})}});
        project.register_result_manifest({
            {"result_id", "json-result"}, {"contract_id", "test.json"},
            {"kind", "json"}, {"format", "json"}, {"state", "published"},
            {"producer_operation", "test.publish_json"}, {"producer_instance", "node_1"},
            {"workflow_revision", 1},
            {"provenance", {{"lineage_inputs", streamfind::Json::array({
                {{"result_id", "arrow-table-result"}, {"source_port", "table"}, {"target_port", "left"}},
                {{"result_id", "arrow-table-result"}, {"source_port", "table"}, {"target_port", "right"}}
            })}}}});

        const auto inventory = project.get_result_inventory();
        if (inventory.size() != 2)
            throw std::runtime_error("published result manifests were not registered");
        const auto table = std::find_if(inventory.begin(), inventory.end(), [](const auto &entry) {
            return entry.value("result_id", "") == "arrow-table-result";
        });
        const auto json = std::find_if(inventory.begin(), inventory.end(), [](const auto &entry) {
            return entry.value("result_id", "") == "json-result";
        });
        if (table == inventory.end() || table->value("representation", "") != "table" ||
            table->value("producer_instance", "") != "publisher" || table->contains("physical_table") ||
            (table->contains("payload") && !table->at("payload").is_null()))
            throw std::runtime_error("Arrow table result was not registered as metadata only");
        if (json == inventory.end() || json->value("representation", "") != "json" ||
            json->value("producer_instance", "") != "node_1")
            throw std::runtime_error("JSON result was not registered with its producer metadata");
        const auto lineage = project.query_json(
            "SELECT result_id, source_result_id, source_port_id, target_port_id "
            "FROM RESULT_LINEAGE ORDER BY target_port_id");
        if (lineage.size() != 2 ||
            lineage.at(0).value("result_id", "") != "json-result" ||
            lineage.at(0).value("source_result_id", "") != "arrow-table-result" ||
            lineage.at(0).value("source_port_id", "") != "table" ||
            lineage.at(0).value("target_port_id", "") != "left" ||
            lineage.at(1).value("result_id", "") != "json-result" ||
            lineage.at(1).value("source_result_id", "") != "arrow-table-result" ||
            lineage.at(1).value("source_port_id", "") != "table" ||
            lineage.at(1).value("target_port_id", "") != "right")
            throw std::runtime_error("JSON output lineage did not preserve both edges from the same table result");
        const auto table_lineage = project.query_json(
            "SELECT result_id FROM RESULT_LINEAGE WHERE result_id = 'arrow-table-result'");
        if (!table_lineage.empty())
            throw std::runtime_error("an output with no inputs received fabricated lineage");

        project.execute_sql("CREATE TABLE ARTIFACT_123_456 (value VARCHAR)");
        project.clear_all_results();
        const auto remaining_tables = project.list_tables();
        if (std::find(remaining_tables.begin(), remaining_tables.end(), "ARTIFACT_123_456") == remaining_tables.end() ||
            !project.get_result_inventory().empty())
            throw std::runtime_error("clearing result metadata modified a DuckDB table or retained store registrations");

        project.close();
        std::filesystem::remove(path, error);
        std::cout << "Artifact publication contract passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::filesystem::remove(path);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
