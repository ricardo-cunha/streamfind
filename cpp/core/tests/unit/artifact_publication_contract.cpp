#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "streamfind/project.hpp"
#include "streamfind/project_table_store.hpp"
#include "../support/tmp_projects.hpp"

int main() {
    const auto path = streamfind::test::tmp_projects_dir() / "streamfind-artifact-publication.duckdb";
    std::error_code error;
    std::filesystem::remove(path, error);
    try {
        auto project = streamfind::Project::create({path, {}});
        std::string result_id;
        std::string physical_table;
        streamfind::ProjectTableStore::transaction(project, {}, [&](streamfind::ProjectTableStore &tables) {
            const auto allocation = tables.allocate_table_artifact(
                "test.table", "test.publish_table", "publisher", 1,
                streamfind::Json::array({{{"name", "value"}, {"type", "string"}}}));
            result_id = allocation.first;
            physical_table = allocation.second;
            tables.append(physical_table, {"value"}, {{{std::string("published")}}});
        });
        const auto inventory = project.get_result_inventory();
        if (inventory.size() != 1 || inventory.at(0).value("result_id", "") != result_id ||
            inventory.at(0).value("physical_table", "") != physical_table ||
            inventory.at(0).value("producer_instance", "") != "publisher")
            throw std::runtime_error("table artifact was not published with its producer metadata");
        const auto rows = project.query_json("SELECT value FROM \"" + physical_table + "\"");
        if (rows != streamfind::Json::array({{{"value", "published"}}}))
            throw std::runtime_error("published artifact table does not contain the emitted row");
        streamfind::ProjectTableStore::transaction(project, {}, [&](streamfind::ProjectTableStore &tables) {
            tables.publish_result_artifact(
                "testJsonResult", R"({"value":"published"})", "test.json", "node_1", 1);
        });
        if (project.get_result_inventory().size() != 2)
            throw std::runtime_error("JSON artifact was not published");
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
