#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "streamfind/catalogue.hpp"
#include "streamfind/core_operations.hpp"
#include "streamfind/project.hpp"
#include "streamfind/project_table_store.hpp"
#include "../support/tmp_projects.hpp"

int main()
{
    try
    {
        const auto path = streamfind::test::tmp_projects_dir() / "table_contract_contract.duckdb";
        std::error_code error;
        std::filesystem::remove(path, error);

        streamfind::TableSchema schema;
        schema.columns = {
            {"id", "Identifier", streamfind::ParameterType::integer, true},
            {"signal", "Signal", streamfind::ParameterType::real, true},
        };
        streamfind::StaticTableContract static_contract("featuresTable", "FEATURES", schema);
        streamfind::DynamicTableContract dynamic_contract("anyTable", "DYNAMIC", {
            {"id", "INTEGER", streamfind::ParameterType::integer, false, streamfind::Json::object()},
            {"signal", "DOUBLE", streamfind::ParameterType::real, true, streamfind::Json::object()},
        });

        if (dynamic_contract.kind() != streamfind::TableContract::Kind::dynamic_schema ||
            static_contract.kind() != streamfind::TableContract::Kind::static_schema ||
            dynamic_contract.columns().size() != 2)
            throw std::runtime_error("table contract kinds or columns are incorrect");
        if (!dynamic_contract.accepts(static_contract) || static_contract.accepts(dynamic_contract))
            throw std::runtime_error("static and dynamic contract direction is incorrect");
        if (!streamfind::table_contracts_compatible("featuresTable", "anyTable", "duckdb_table", "duckdb_table") ||
            !streamfind::table_contracts_compatible("anyTable", "featuresTable", "duckdb_table", "duckdb_table") ||
            streamfind::table_contracts_compatible("featuresTable", "anyTable", "json", "duckdb_table"))
            throw std::runtime_error("central compatibility rule is incorrect");

        streamfind::Project project = streamfind::Project::create({path, {}});
        project.execute_sql("CREATE TABLE RUNTIME_TABLE (id INTEGER NOT NULL, signal DOUBLE, label VARCHAR)");
        project.execute_sql("INSERT INTO RUNTIME_TABLE VALUES (1, 1.25, 'first')");
        const auto inspected = streamfind::inspect_dynamic_table_contract(project, "RUNTIME_TABLE");
        if (inspected.columns().size() != 3 || inspected.columns().at(0).name != "id" ||
            inspected.columns().at(0).type != streamfind::ParameterType::integer ||
            inspected.columns().at(1).type != streamfind::ParameterType::real ||
            inspected.columns().at(2).type != streamfind::ParameterType::string ||
            inspected.columns().at(0).nullable)
            throw std::runtime_error("runtime DuckDB schema was not resolved correctly");
        const auto serialized = inspected.to_json();
        if (serialized.value("kind", "") != "dynamic" || serialized.at("columns").size() != 3)
            throw std::runtime_error("runtime contract serialization is incorrect");

        const auto entries = streamfind::catalogue::load(STREAMFIND_CATALOGUE_PATH);
        if (!entries)
            throw std::runtime_error("runtime catalogue could not be loaded");
        streamfind::OperationRegistry operations;
        streamfind::core_operations::register_operations(*entries, operations);
        const auto *writer = operations.find("streamfind.write_table_csv");
        if (!writer || writer->definition().input_ports.size() != 1 ||
            writer->definition().input_ports.front().semantic_contract != "anyTable")
            throw std::runtime_error("CSV writer does not expose the AnyTable contract");

        const auto csv_path = streamfind::test::tmp_projects_dir() / "table_contract_export.csv";
        std::filesystem::remove(csv_path, error);
        writer->run_workflow(project, {{"csv_path", csv_path.string()}}, "csv-writer", {
            {"anyTable", {{"contract_id", "anyTable"}, {"physical_table", "RUNTIME_TABLE"}}}
        });
        std::ifstream csv(csv_path);
        std::string contents((std::istreambuf_iterator<char>(csv)), std::istreambuf_iterator<char>());
        if (contents.find("id,signal,label") == std::string::npos)
            throw std::runtime_error("CSV writer did not emit the table header");
        const auto *reader = operations.find("streamfind.read_csv");
        if (!reader || reader->definition().output_ports.empty())
            throw std::runtime_error("CSV reader does not expose an AnyTable output");
        const auto read_result = reader->run_workflow(project, {{"csv_input_path", csv_path.string()}}, "csv-reader", {});
        const auto read_table = project.query_json("SELECT physical_table FROM ARTIFACT_INVENTORY WHERE artifact_id = '" +
                                                   read_result.at("artifact_id").get<std::string>() + "'");
        if (read_table.empty() || project.query_json("SELECT COUNT(*) AS count FROM \"" +
            read_table.at(0).at("physical_table").get<std::string>() + "\"").at(0).at("count") != "1")
            throw std::runtime_error("CSV reader did not import the data rows");
        std::filesystem::remove(csv_path, error);

        project.execute_sql("CREATE TABLE LEFT_ROWS (id INTEGER, value VARCHAR)");
        project.execute_sql("CREATE TABLE RIGHT_ROWS (id INTEGER, value VARCHAR)");
        project.execute_sql("CREATE TABLE LEFT_TABLE (id INTEGER, left_value VARCHAR)");
        project.execute_sql("CREATE TABLE RIGHT_TABLE (id INTEGER, right_value DOUBLE)");
        project.execute_sql("INSERT INTO LEFT_ROWS VALUES (1, 'one'), (2, 'two')");
        project.execute_sql("INSERT INTO RIGHT_ROWS VALUES (3, 'three'), (4, 'four')");
        project.execute_sql("INSERT INTO LEFT_TABLE VALUES (1, 'one'), (2, 'two')");
        project.execute_sql("INSERT INTO RIGHT_TABLE VALUES (1, 1.5), (2, 2.5)");
        std::string left_rows_artifact;
        std::string right_rows_artifact;
        std::string left_artifact;
        std::string right_artifact;
        streamfind::ProjectTableStore::transaction(project, {}, [&](streamfind::ProjectTableStore &tables) {
            left_rows_artifact = tables.allocate_table_artifact("anyTable", "test", "left-rows-source", project.get_workflow().version,
                                                                streamfind::Json::array({{{"name", "id"}, {"type", "integer"}},
                                                                             {{"name", "value"}, {"type", "string"}}})).first;
            right_rows_artifact = tables.allocate_table_artifact("anyTable", "test", "right-rows-source", project.get_workflow().version,
                                                                 streamfind::Json::array({{{"name", "id"}, {"type", "integer"}},
                                                                              {{"name", "value"}, {"type", "string"}}})).first;
            left_artifact = tables.allocate_table_artifact("anyTable", "test", "left-source", project.get_workflow().version,
                                                           streamfind::Json::array({{{"name", "id"}, {"type", "integer"}},
                                                                        {{"name", "left_value"}, {"type", "string"}}})).first;
            right_artifact = tables.allocate_table_artifact("anyTable", "test", "right-source", project.get_workflow().version,
                                                            streamfind::Json::array({{{"name", "id"}, {"type", "integer"}},
                                                                         {{"name", "right_value"}, {"type", "real"}}})).first;
        });
        const auto *row_binding = operations.find("streamfind.bind_table_rows");
        const auto *column_binding = operations.find("streamfind.bind_table_columns");
        if (!row_binding || !column_binding || row_binding->definition().input_ports.size() != 2 ||
            row_binding->definition().input_ports.at(0).semantic_contract != "anyTable")
            throw std::runtime_error("generic table binding operations are not registered correctly");
        const auto row_result = row_binding->run_workflow(project, {}, "row-binding", {
            {"leftTable", {{"artifact_id", left_rows_artifact}, {"physical_table", "LEFT_ROWS"}}},
            {"rightTable", {{"artifact_id", right_rows_artifact}, {"physical_table", "RIGHT_ROWS"}}}
        });
        const auto row_table = project.query_json("SELECT physical_table FROM ARTIFACT_INVENTORY WHERE artifact_id = '" +
                                                  row_result.at("artifact_id").get<std::string>() + "'");
        if (row_table.empty() || project.query_json("SELECT COUNT(*) AS count FROM \"" +
            row_table.at(0).at("physical_table").get<std::string>() + "\"").at(0).at("count") != "4")
            throw std::runtime_error("row binding did not append all input rows");
        const auto column_result = column_binding->run_workflow(project, {{"row_key_columns", {"id"}}}, "column-binding", {
            {"leftTable", {{"artifact_id", left_artifact}, {"physical_table", "LEFT_TABLE"}}},
            {"rightTable", {{"artifact_id", right_artifact}, {"physical_table", "RIGHT_TABLE"}}}
        });
        const auto column_table = project.query_json("SELECT physical_table FROM ARTIFACT_INVENTORY WHERE artifact_id = '" +
                                                     column_result.at("artifact_id").get<std::string>() + "'");
        if (column_table.empty() || project.query_json("SELECT COUNT(*) AS count FROM \"" +
            column_table.at(0).at("physical_table").get<std::string>() + "\"").at(0).at("count") != "2")
            throw std::runtime_error("column binding did not align the input rows");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "failure: " << error.what() << '\n';
        return 3;
    }
}
