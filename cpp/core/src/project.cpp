/**
 * @file project.cpp
 * @brief Project persistence, method metadata, workflow execution, and cache storage.
 */

#include "streamfind/project.hpp"
#include "streamfind/fingerprint.hpp"
#include "streamfind/project_table_store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>

#include <functional>

#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>

#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <duckdb.h>

namespace streamfind
{
    constexpr int PROJECT_SCHEMA_VERSION = 2;
    namespace detail
    {

        using Statement = duckdb_prepared_statement;

        std::string db_error(duckdb_result &result)
        {
            const char *message = duckdb_result_error(&result);
            return message ? message : "DuckDB operation failed";
        }

        bool is_generated_artifact_table(const std::string &table)
        {
            static const std::regex pattern("^ARTIFACT_[0-9]+_[0-9]+$");
            return std::regex_match(table, pattern);
        }

        void check(duckdb_state state, const std::string &context)
        {
            if (state == DuckDBError)
            {
                throw Error(ErrorCode::DatabaseError, context);
            }
        }

        class ResultGuard
        {
        public:
            explicit ResultGuard(duckdb_result &result) : result_(result) {}
            ~ResultGuard() { duckdb_destroy_result(&result_); }
            ResultGuard(const ResultGuard &) = delete;
            ResultGuard &operator=(const ResultGuard &) = delete;

        private:
            duckdb_result &result_;
        };

        class StatementGuard
        {
        public:
            explicit StatementGuard(Statement statement) : statement_(statement) {}
            ~StatementGuard()
            {
                if (statement_)
                    duckdb_destroy_prepare(&statement_);
            }
            StatementGuard(const StatementGuard &) = delete;
            StatementGuard &operator=(const StatementGuard &) = delete;

        private:
            Statement statement_;
        };

        class AppenderGuard
        {
        public:
            explicit AppenderGuard(duckdb_appender *appender) : appender_(appender) {}
            ~AppenderGuard()
            {
                if (appender_ && *appender_)
                    duckdb_appender_destroy(appender_);
            }
            AppenderGuard(const AppenderGuard &) = delete;
            AppenderGuard &operator=(const AppenderGuard &) = delete;

        private:
            duckdb_appender *appender_;
        };

        void query(duckdb_connection connection, const std::string &sql,
                   const std::string &context)
        {
            duckdb_result result{};
            if (duckdb_query(connection, sql.c_str(), &result) == DuckDBError)
            {
                const std::string message = context + ": " + db_error(result);
                duckdb_destroy_result(&result);
                throw Error(ErrorCode::DatabaseError, message);
            }
            duckdb_destroy_result(&result);
        }

        template <typename Bind, typename Read>
        void prepared(duckdb_connection connection, const std::string &sql,
                      const std::string &context, Bind bind, Read read)
        {
            Statement statement = nullptr;
            if (duckdb_prepare(connection, sql.c_str(), &statement) == DuckDBError)
            {
                const char *message = statement ? duckdb_prepare_error(statement) : nullptr;
                const std::string error = context + ": " + (message ? message : "prepare failed");
                if (statement)
                    duckdb_destroy_prepare(&statement);
                throw Error(ErrorCode::DatabaseError, error);
            }
            StatementGuard statement_guard(statement);
            bind(statement);
            duckdb_result result{};
            if (duckdb_execute_prepared(statement, &result) == DuckDBError)
            {
                const std::string message = context + ": " + db_error(result);
                duckdb_destroy_result(&result);
                throw Error(ErrorCode::DatabaseError, message);
            }
            ResultGuard result_guard(result);
            read(result);
        }

        std::string value_string(duckdb_result &result, idx_t column, idx_t row)
        {
            char *value = duckdb_value_varchar(&result, column, row);
            if (!value)
                return {};
            std::string output(value);
            duckdb_free(value);
            return output;
        }

        Json parse_json(const std::string &value, const char *context)
        {
            if (value.empty())
                return Json::object();
            try
            {
                return Json::parse(value);
            }
            catch (const std::exception &error)
            {
                throw Error(ErrorCode::SchemaMismatch,
                            std::string(context) + ": invalid JSON: " + error.what());
            }
        }

        std::string json_text(const Json &value)
        {
            return value.is_null() ? "null" : value.dump();
        }


        void bind_text(Statement statement, idx_t index, const std::string &value)
        {
            duckdb_bind_varchar(statement, index, value.c_str());
        }

        class WorkflowFileLock
        {
        public:
            explicit WorkflowFileLock(const std::filesystem::path &database_path)
                : path_(database_path.string() + ".workflow.lock")
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (true)
                {
                    std::error_code error;
                    if (std::filesystem::create_directory(path_, error))
                    {
                        owned_ = true;
                        owner_token_ = std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + "." +
                                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
                        std::ofstream owner(path_ / "owner", std::ios::trunc);
                        owner << owner_token_;
                        heartbeat_ = std::thread([this]
                        {
                            while (!stopping_.load())
                            {
                                for (int tick = 0; tick < 10 && !stopping_.load(); ++tick)
                                    std::this_thread::sleep_for(std::chrono::seconds(1));
                                if (!stopping_.load())
                                {
                                    std::error_code heartbeat_error;
                                    std::ifstream owner(path_ / "owner");
                                    std::string token;
                                    std::getline(owner, token);
                                    if (token != owner_token_)
                                    {
                                        lost_.store(true);
                                        stopping_.store(true);
                                        continue;
                                    }
                                    std::filesystem::last_write_time(path_ / "owner", std::filesystem::file_time_type::clock::now(), heartbeat_error);
                                    if (heartbeat_error) lost_.store(true);
                                }
                            }
                        });
                        return;
                    }
                    if (std::filesystem::exists(path_, error))
                    {
                        const auto owner_path = path_ / "owner";
                        const auto timestamp_path = std::filesystem::exists(owner_path, error) ? owner_path : path_;
                        const auto modified = std::filesystem::last_write_time(timestamp_path, error);
                        if (!error && std::filesystem::file_time_type::clock::now() - modified > std::chrono::seconds(30))
                        {
                            const auto stale_path = path_.string() + ".stale." +
                                                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
                            std::filesystem::rename(path_, stale_path, error);
                            if (!error)
                            {
                                std::filesystem::remove_all(stale_path, error);
                                continue;
                            }
                        }
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                        throw Error(ErrorCode::MethodExecution, "Timed out waiting for workflow execution lock");
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
            }

            ~WorkflowFileLock()
            {
                if (owned_)
                {
                    stopping_.store(true);
                    if (heartbeat_.joinable()) heartbeat_.join();
                    std::error_code error;
                    std::string token;
                    {
                        std::ifstream owner(path_ / "owner");
                        std::getline(owner, token);
                    }
                    if (token == owner_token_) std::filesystem::remove_all(path_, error);
                }
            }

            WorkflowFileLock(const WorkflowFileLock &) = delete;
            WorkflowFileLock &operator=(const WorkflowFileLock &) = delete;

            bool healthy() const
            {
                if (lost_.load()) return false;
                std::ifstream owner(path_ / "owner");
                std::string token;
                std::getline(owner, token);
                return token == owner_token_;
            }

        private:
            std::filesystem::path path_;
            bool owned_{false};
            std::string owner_token_;
            std::atomic_bool stopping_{false};
            std::atomic_bool lost_{false};
            std::thread heartbeat_;
        };

        bool has_column(duckdb_connection connection, const char *table, const char *column)
        {
            Statement statement = nullptr;
            const std::string sql = "SELECT 1 FROM information_schema.columns WHERE table_name = ? AND column_name = ? LIMIT 1";
            if (duckdb_prepare(connection, sql.c_str(), &statement) == DuckDBError)
            {
                if (statement)
                    duckdb_destroy_prepare(&statement);
                throw Error(ErrorCode::DatabaseError, "inspect schema");
            }
            StatementGuard guard(statement);
            bind_text(statement, 1, table);
            bind_text(statement, 2, column);
            duckdb_result result{};
            if (duckdb_execute_prepared(statement, &result) == DuckDBError)
            {
                const std::string message = db_error(result);
                duckdb_destroy_result(&result);
                throw Error(ErrorCode::DatabaseError, "inspect schema: " + message);
            }
            ResultGuard result_guard(result);
            return duckdb_row_count(&result) != 0;
        }

        bool has_table(duckdb_connection connection, const char *table)
        {
            Statement statement = nullptr;
            const std::string sql = "SELECT 1 FROM information_schema.tables WHERE table_schema = 'main' AND table_name = ? LIMIT 1";
            if (duckdb_prepare(connection, sql.c_str(), &statement) == DuckDBError)
            {
                if (statement)
                    duckdb_destroy_prepare(&statement);
                throw Error(ErrorCode::DatabaseError, "inspect tables");
            }
            StatementGuard guard(statement);
            bind_text(statement, 1, table);
            duckdb_result result{};
            if (duckdb_execute_prepared(statement, &result) == DuckDBError)
            {
                const std::string message = db_error(result);
                duckdb_destroy_result(&result);
                throw Error(ErrorCode::DatabaseError, "inspect tables: " + message);
            }
            ResultGuard result_guard(result);
            return duckdb_row_count(&result) != 0;
        }

        idx_t project_row_count(duckdb_connection connection)
        {
            duckdb_result result{};
            if (duckdb_query(connection, "SELECT COUNT(*) FROM PROJECT", &result) == DuckDBError)
            {
                const std::string message = db_error(result);
                duckdb_destroy_result(&result);
                throw Error(ErrorCode::DatabaseError, "count PROJECT rows: " + message);
            }
            ResultGuard guard(result);
            return static_cast<idx_t>(duckdb_value_int64(&result, 0, 0));
        }

        std::optional<int> project_schema_version(duckdb_connection connection)
        {
            duckdb_result result{};
            if (duckdb_query(connection, "SELECT schema_version FROM PROJECT LIMIT 1", &result) == DuckDBError)
            {
                const std::string message = db_error(result);
                duckdb_destroy_result(&result);
                throw Error(ErrorCode::DatabaseError, "read PROJECT schema version: " + message);
            }
            ResultGuard guard(result);
            if (duckdb_row_count(&result) == 0)
                return std::nullopt;
            return duckdb_value_int32(&result, 0, 0);
        }

        void ensure_schema(duckdb_connection connection,
                           const ProjectOptions &)
        {
            if (has_table(connection, "PROJECT") && !has_column(connection, "PROJECT", "schema_version"))
                throw Error(ErrorCode::SchemaMismatch, "PROJECT schema version is missing");
            const auto version = has_table(connection, "PROJECT") ? project_schema_version(connection) : std::nullopt;
            if (version && *version != PROJECT_SCHEMA_VERSION)
                throw Error(ErrorCode::SchemaMismatch,
                            "unsupported PROJECT schema version: " + std::to_string(*version));
            query(connection,
                  "CREATE TABLE IF NOT EXISTS PROJECT (metadata JSON, workflow JSON, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, schema_version INTEGER NOT NULL DEFAULT " + std::to_string(PROJECT_SCHEMA_VERSION) + ", framework_version VARCHAR NOT NULL DEFAULT '" STREAMFIND_FRAMEWORK_VERSION "')",
                  "create PROJECT table");


            query(connection,
                  "CREATE TABLE IF NOT EXISTS AUDIT_TRAIL (operation_type VARCHAR NOT NULL, object_type VARCHAR NOT NULL, operation_details JSON, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)",
                  "create AUDIT_TRAIL table");
            query(connection,
                  "CREATE TABLE IF NOT EXISTS WORKFLOW_REVISION (revision INTEGER PRIMARY KEY, workflow JSON NOT NULL, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)",
                  "create WORKFLOW_REVISION table");
            query(connection,
                  "CREATE TABLE IF NOT EXISTS WORKFLOW_EXECUTION (workflow_revision INTEGER NOT NULL, launch_snapshot JSON NOT NULL DEFAULT '{}', status VARCHAR NOT NULL, progress JSON NOT NULL DEFAULT '{}', result_reference VARCHAR, process_id VARCHAR, server_id VARCHAR, started_at TIMESTAMP, completed_at TIMESTAMP, error VARCHAR, created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)",
                  "create WORKFLOW_EXECUTION table");
            query(connection,
                  "CREATE TABLE IF NOT EXISTS WORKFLOW_EXECUTION_STEP (workflow_revision INTEGER NOT NULL, step_index INTEGER NOT NULL PRIMARY KEY, operation VARCHAR NOT NULL, parameters JSON NOT NULL DEFAULT '{}', parameter_hash VARCHAR NOT NULL, cache_key VARCHAR NOT NULL, status VARCHAR NOT NULL, progress JSON NOT NULL DEFAULT '{}', result_reference VARCHAR, error_code VARCHAR, error_message VARCHAR, started_at TIMESTAMP, completed_at TIMESTAMP, updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)",
                  "create WORKFLOW_EXECUTION table");
            query(connection,
                  "CREATE TABLE IF NOT EXISTS ARTIFACT_INVENTORY (artifact_id VARCHAR PRIMARY KEY, contract_id VARCHAR NOT NULL, representation VARCHAR NOT NULL, physical_table VARCHAR, payload JSON, producer_operation VARCHAR NOT NULL, producer_instance VARCHAR NOT NULL, workflow_revision INTEGER NOT NULL, status VARCHAR NOT NULL DEFAULT 'published', created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)",
                  "create ARTIFACT_INVENTORY table");
            query(connection,
                  "CREATE TABLE IF NOT EXISTS ARTIFACT_LINEAGE (artifact_id VARCHAR NOT NULL, source_artifact_id VARCHAR NOT NULL, source_port_id VARCHAR, target_port_id VARCHAR, PRIMARY KEY (artifact_id, source_artifact_id, source_port_id, target_port_id))",
                  "create ARTIFACT_LINEAGE table");
            query(connection,
                  "CREATE TABLE IF NOT EXISTS ARTIFACT_CACHE (fingerprint VARCHAR PRIMARY KEY, operation_id VARCHAR NOT NULL, status VARCHAR NOT NULL DEFAULT 'complete', created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, last_used_at TIMESTAMP)",
                  "create ARTIFACT_CACHE table");
            query(connection,
                  "CREATE TABLE IF NOT EXISTS ARTIFACT_CACHE_OUTPUT (fingerprint VARCHAR NOT NULL, output_port_id VARCHAR NOT NULL, ordinal INTEGER NOT NULL, artifact_id VARCHAR NOT NULL, PRIMARY KEY (fingerprint, output_port_id, ordinal))",
                  "create ARTIFACT_CACHE_OUTPUT table");
            query(connection,
                  "CREATE INDEX IF NOT EXISTS idx_artifact_cache_output_artifact ON ARTIFACT_CACHE_OUTPUT (artifact_id)",
                  "index ARTIFACT_CACHE_OUTPUT artifact");
        }

        std::string now_string()
        {
            return "current";
        }

        std::string sql_quote(const std::string &value)
        {
            std::string result = "'";
            for (const char character : value)
                result += character == '\'' ? "''" : std::string(1, character);
            return result + "'";
        }

        std::string identifier_quote(const std::string &value)
        {
            std::string result = "\"";
            for (const char character : value)
                result += character == '"' ? "\"\"" : std::string(1, character);
            return result + "\"";
        }

        Json snapshot_tables(duckdb_connection connection, const std::vector<std::string> &tables)
        {
            Json snapshots = Json::object();
            for (const auto &table : tables)
            {
                Json rows = Json::array();
                duckdb_result result{};
                const std::string filter;
                const std::string sql = "SELECT to_json(t) FROM " + identifier_quote(table) + " t" + filter;
                if (duckdb_query(connection, sql.c_str(), &result) == DuckDBError)
                {
                    const std::string message = db_error(result);
                    duckdb_destroy_result(&result);
                    throw Error(ErrorCode::DatabaseError, "snapshot " + table + ": " + message);
                }
                ResultGuard guard(result);
                for (idx_t row = 0; row < duckdb_row_count(&result); ++row)
                    rows.push_back(parse_json(value_string(result, 0, row), "table snapshot"));
                snapshots[table] = std::move(rows);
            }
            return snapshots;
        }

        void restore_tables(duckdb_connection connection, const Json &snapshots)
        {
            for (auto table = snapshots.begin(); table != snapshots.end(); ++table)
            {
                const std::string table_name = table.key();
                duckdb_result schema{};
                const std::string describe = "DESCRIBE " + identifier_quote(table_name);
                if (duckdb_query(connection, describe.c_str(), &schema) == DuckDBError)
                {
                    const std::string message = db_error(schema);
                    duckdb_destroy_result(&schema);
                    throw Error(ErrorCode::DatabaseError, "describe cached table: " + message);
                }
                ResultGuard schema_guard(schema);
                std::vector<std::pair<std::string, std::string>> columns;
                for (idx_t row = 0; row < duckdb_row_count(&schema); ++row)
                    columns.emplace_back(value_string(schema, 0, row), value_string(schema, 1, row));
                query(connection, "DELETE FROM " + identifier_quote(table_name), "clear cached table");
                for (const auto &row : table.value())
                {
                    std::string sql = "INSERT INTO " + identifier_quote(table_name) + " VALUES (";
                    for (std::size_t index = 0; index < columns.size(); ++index)
                    {
                        if (index)
                            sql += ", ";
                        const auto &value = row.value(columns[index].first, Json(nullptr));
                        if (value.is_null())
                            sql += "NULL";
                        else if (value.is_string())
                            sql += sql_quote(value.get<std::string>());
                        else if (value.is_boolean())
                            sql += value.get<bool>() ? "TRUE" : "FALSE";
                        else
                            sql += value.dump();
                    }
                    sql += ")";
                    query(connection, sql, "restore cached table");
                }
            }
        }

        void execution_row(duckdb_connection connection, int revision, std::size_t index, const std::string &operation, const std::string &parameter_hash, const Json &inputs, const std::string &status, const std::string &cache_key, const std::string &launch_snapshot, const std::string &error = {})
        {
            duckdb_result parent_result{};
            if (duckdb_query(connection, "SELECT process_id FROM WORKFLOW_EXECUTION LIMIT 1", &parent_result) == DuckDBError)
            {
                const std::string message = "inspect workflow execution: " + db_error(parent_result);
                duckdb_destroy_result(&parent_result);
                throw Error(ErrorCode::DatabaseError, message);
            }
            const bool has_parent = duckdb_row_count(&parent_result) != 0;
            duckdb_destroy_result(&parent_result);
            if (!has_parent)
            {
                prepared(connection, "INSERT INTO WORKFLOW_EXECUTION (workflow_revision, launch_snapshot, status, error) VALUES (?, ?, ?, ?)", "write workflow execution", [&](Statement statement)
                         { duckdb_bind_int32(statement, 1, revision); bind_text(statement, 2, launch_snapshot); bind_text(statement, 3, status); bind_text(statement, 4, error); }, [](duckdb_result &) {});
            }
            else
            {
                query(connection, "UPDATE WORKFLOW_EXECUTION SET workflow_revision = " + std::to_string(revision) + ", launch_snapshot = " + sql_quote(launch_snapshot) + ", status = CASE WHEN process_id IS NULL OR process_id = '' THEN " + sql_quote(status) + " ELSE status END, error = " + sql_quote(error) + ", updated_at = CURRENT_TIMESTAMP", "update workflow execution");
            }
            prepared(connection, "INSERT INTO WORKFLOW_EXECUTION_STEP (workflow_revision, step_index, operation, parameters, parameter_hash, cache_key, status) VALUES (?, ?, ?, ?::JSON, ?, ?, ?) ON CONFLICT(step_index) DO UPDATE SET workflow_revision = excluded.workflow_revision, operation = excluded.operation, parameters = excluded.parameters, parameter_hash = excluded.parameter_hash, cache_key = excluded.cache_key, status = excluded.status", "write workflow execution entry", [&](Statement statement)
                     { duckdb_bind_int32(statement, 1, revision); duckdb_bind_int32(statement, 2, static_cast<int>(index)); bind_text(statement, 3, operation); bind_text(statement, 4, json_text(inputs)); bind_text(statement, 5, parameter_hash); bind_text(statement, 6, cache_key); bind_text(statement, 7, status); }, [](duckdb_result &) {});
        }

        const char *parameter_type_name(ParameterType type)
        {
            switch (type)
            {
            case ParameterType::string:
                return "string";
            case ParameterType::integer:
                return "integer";
            case ParameterType::real:
                return "real";
            case ParameterType::boolean:
                return "boolean";
            case ParameterType::array:
                return "array";
            case ParameterType::object:
                return "object";
            case ParameterType::table:
                return "table";
            }
            return "unknown";
        }

        ParameterType parameter_type_from_name(const std::string &name)
        {
            if (name == "string")
                return ParameterType::string;
            if (name == "integer")
                return ParameterType::integer;
            if (name == "real")
                return ParameterType::real;
            if (name == "boolean")
                return ParameterType::boolean;
            if (name == "array")
                return ParameterType::array;
            if (name == "object")
                return ParameterType::object;
            if (name == "table")
                return ParameterType::table;
            throw Error(ErrorCode::InvalidArgument, "Unknown parameter type: " + name);
        }

        bool parameter_type_matches(ParameterType type, const Json &value)
        {
            switch (type)
            {
            case ParameterType::string:
                return value.is_string();
            case ParameterType::integer:
                return value.is_number_integer();
            case ParameterType::real:
                return value.is_number();
            case ParameterType::boolean:
                return value.is_boolean();
            case ParameterType::array:
                return value.is_array();
            case ParameterType::object:
                return value.is_object();
            case ParameterType::table:
                return value.is_object() && value.contains("columns");
            }
            return false;
        }

    } // namespace detail

    using namespace detail;

    Error::Error(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    ErrorCode Error::code() const noexcept { return code_; }

    bool valid_execution_transition(ExecutionState from, ExecutionState to) noexcept
    {
        return (from == ExecutionState::queued &&
                (to == ExecutionState::running || to == ExecutionState::cancelled)) ||
               (from == ExecutionState::running &&
                (to == ExecutionState::completed || to == ExecutionState::failed ||
                 to == ExecutionState::cancelling || to == ExecutionState::interrupted)) ||
               (from == ExecutionState::cancelling && to == ExecutionState::cancelled);
    }


    Json TableColumnDefinition::to_json() const
    {
        return {{"name", name}, {"description", description}, {"type", detail::parameter_type_name(type)}, {"required", required}};
    }

    TableColumnDefinition TableColumnDefinition::from_json(const Json &value)
    {
        if (!value.is_object() || !value.contains("name") || !value.contains("type"))
        {
            throw Error(ErrorCode::InvalidArgument, "Table column requires name and type");
        }
        return {value.at("name").get<std::string>(), value.value("description", ""),
                detail::parameter_type_from_name(value.at("type").get<std::string>()),
                value.value("required", true)};
    }

    Json TableSchema::to_json() const
    {
        Json output = Json::array();
        for (const auto &column : columns)
            output.push_back(column.to_json());
        return output;
    }

    TableSchema TableSchema::from_json(const Json &value)
    {
        if (!value.is_array())
            throw Error(ErrorCode::InvalidArgument, "Table schema must be an array");
        TableSchema schema;
        for (const auto &column : value)
            schema.columns.push_back(TableColumnDefinition::from_json(column));
        return schema;
    }

    std::size_t Table::row_count() const
    {
        if (columns.empty())
            return 0;
        return std::visit([](const auto &values)
                          { return values.size(); }, columns.front().values);
    }

    void Table::validate(const std::optional<TableSchema> &schema) const
    {
        std::size_t rows = 0;
        bool first = true;
        for (const auto &column : columns)
        {
            if (column.name.empty())
                throw Error(ErrorCode::WorkflowValidation, "Table column name must not be empty");
            const std::size_t length = std::visit([](const auto &values)
                                                  { return values.size(); }, column.values);
            if (first)
            {
                rows = length;
                first = false;
            }
            if (length != rows)
                throw Error(ErrorCode::WorkflowValidation, "Table columns must have equal lengths");
            if (schema)
            {
                const auto it = std::find_if(schema->columns.begin(), schema->columns.end(),
                                             [&](const auto &definition)
                                             { return definition.name == column.name; });
                if (it == schema->columns.end() || it->type != column.type)
                {
                    throw Error(ErrorCode::WorkflowValidation, "Table column does not match its schema: " + column.name);
                }
            }
        }
        if (schema)
        {
            for (const auto &definition : schema->columns)
            {
                const auto it = std::find_if(columns.begin(), columns.end(),
                                             [&](const auto &column)
                                             { return column.name == definition.name; });
                if (definition.required && it == columns.end())
                {
                    throw Error(ErrorCode::WorkflowValidation, "Missing required table column: " + definition.name);
                }
            }
        }
    }

    Json Table::to_json() const
    {
        Json output = Json::array();
        for (const auto &column : columns)
        {
            Json values = Json::array();
            std::visit([&](const auto &items)
                       { for (const auto &item : items) values.push_back(item); }, column.values);
            output.push_back({{"name", column.name}, {"type", detail::parameter_type_name(column.type)}, {"values", values}});
        }
        return {{"columns", output}};
    }

    Table Table::from_json(const Json &value)
    {
        if (!value.is_object() || !value.at("columns").is_array())
        {
            throw Error(ErrorCode::WorkflowValidation, "Table value requires a columns array");
        }
        Table table;
        for (const auto &item : value.at("columns"))
        {
            const auto type = detail::parameter_type_from_name(item.at("type").get<std::string>());
            const Json &values = item.at("values");
            if (!values.is_array())
                throw Error(ErrorCode::WorkflowValidation, "Table column values must be arrays");
            TableColumn column;
            column.name = item.at("name").get<std::string>();
            column.type = type;
            switch (type)
            {
            case ParameterType::string:
                column.values = values.get<std::vector<std::string>>();
                break;
            case ParameterType::integer:
                column.values = values.get<std::vector<std::int64_t>>();
                break;
            case ParameterType::real:
                column.values = values.get<std::vector<double>>();
                break;
            case ParameterType::boolean:
                column.values = values.get<std::vector<bool>>();
                break;
            default:
                throw Error(ErrorCode::WorkflowValidation, "Table columns must be scalar types");
            }
            table.columns.push_back(std::move(column));
        }
        table.validate();
        return table;
    }

    Json TypeDescriptor::to_json() const
    {
        Json output = {{"type", detail::parameter_type_name(kind)}};
        if (kind == ParameterType::array)
        {
            if (!items)
                throw Error(ErrorCode::InvalidArgument, "Array type requires an items type");
            output["items"] = items->to_json();
        }
        else if (kind == ParameterType::table && table_schema)
        {
            output["columns"] = table_schema->to_json();
        }
        return output;
    }

    TypeDescriptor TypeDescriptor::from_json(const Json &value)
    {
        if (!value.is_object() || !value.contains("type"))
        {
            throw Error(ErrorCode::InvalidArgument, "Type descriptor requires a type");
        }
        TypeDescriptor descriptor;
        descriptor.kind = detail::parameter_type_from_name(value.at("type").get<std::string>());
        if (descriptor.kind == ParameterType::array)
        {
            if (!value.contains("items"))
                throw Error(ErrorCode::InvalidArgument, "Array type requires an items type");
            descriptor.items = std::make_shared<TypeDescriptor>(from_json(value.at("items")));
        }
        else if (descriptor.kind == ParameterType::table && value.contains("columns"))
        {
            descriptor.table_schema = TableSchema::from_json(value.at("columns"));
        }
        return descriptor;
    }

    void TypeDescriptor::validate(const Json &value) const
    {
        if (kind == ParameterType::array)
        {
            if (!items || !value.is_array())
                throw Error(ErrorCode::WorkflowValidation, "Invalid array parameter");
            for (const auto &item : value)
                items->validate(item);
            return;
        }
        if (kind == ParameterType::table)
        {
            if (value.is_array())
            {
                if (!table_schema)
                {
                    for (const auto &row : value)
                        if (!row.is_object())
                            throw Error(ErrorCode::WorkflowValidation, "Table rows must be objects");
                    return;
                }
                for (const auto &row : value)
                {
                    if (!row.is_object())
                        throw Error(ErrorCode::WorkflowValidation, "Table rows must be objects");
                    for (const auto &[name, item] : row.items())
                    {
                        const auto column = std::find_if(table_schema->columns.begin(), table_schema->columns.end(),
                                                         [&](const auto &candidate) { return candidate.name == name; });
                        if (column == table_schema->columns.end())
                            throw Error(ErrorCode::WorkflowValidation, "Unknown table column: " + name);
                        if (!item.is_null() && !parameter_type_matches(column->type, item))
                            throw Error(ErrorCode::WorkflowValidation, "Invalid value for table column: " + name);
                    }
                    for (const auto &column : table_schema->columns)
                        if (column.required && (!row.contains(column.name) || row.at(column.name).is_null()))
                            throw Error(ErrorCode::WorkflowValidation, "Missing required table column: " + column.name);
                }
                return;
            }
            Table::from_json(value).validate(table_schema);
            return;
        }
        if (!parameter_type_matches(kind, value))
        {
            throw Error(ErrorCode::WorkflowValidation,
                        std::string("Invalid parameter type; expected ") + detail::parameter_type_name(kind));
        }
    }

    Json ParameterDefinition::to_json() const
    {
        return {{"name", name}, {"description", description}, {"type", type.to_json()}, {"default", default_value}, {"required", required}, {"example", example}, {"constraints", constraints}, {"ui", ui}};
    }

    ParameterDefinition ParameterDefinition::from_json(const Json &value)
    {
        if (!value.is_object() || !value.contains("name") || !value.contains("type"))
        {
            throw Error(ErrorCode::InvalidArgument, "Parameter definition requires name and type");
        }
        ParameterDefinition definition;
        definition.name = value.at("name").get<std::string>();
        definition.description = value.value("description", "");
        definition.type = TypeDescriptor::from_json(value.at("type"));
        definition.default_value = value.value("default", Json(nullptr));
        definition.required = value.value("required", false);
        definition.example = value.value("example", Json(nullptr));
        definition.constraints = value.value("constraints", Json::object());
        definition.ui = value.value("ui", Json::object());
        return definition;
    }

    Json ParameterSchema::to_json() const
    {
        Json output = Json::array();
        for (const auto &definition : definitions)
            output.push_back(definition.to_json());
        return output;
    }

    ParameterSchema ParameterSchema::from_json(const Json &value)
    {
        if (!value.is_array())
            throw Error(ErrorCode::InvalidArgument, "Parameter schema must be an array");
        ParameterSchema schema;
        for (const auto &item : value)
            schema.definitions.push_back(ParameterDefinition::from_json(item));
        return schema;
    }

    Json ParameterSchema::resolve_and_validate(const Json &values) const
    {
        if (!values.is_null() && !values.is_object())
        {
            throw Error(ErrorCode::WorkflowValidation, "Method parameters must be an object");
        }
        Json resolved = Json::object();
        for (const auto &definition : definitions)
        {
            if (!definition.default_value.is_null())
                resolved[definition.name] = definition.default_value;
            if (definition.required && definition.default_value.is_null() &&
                (!values.is_object() || !values.contains(definition.name)))
            {
                throw Error(ErrorCode::WorkflowValidation,
                            "Missing required parameter: " + definition.name);
            }
        }
        if (values.is_object())
        {
            for (const auto &[name, value] : values.items())
            {
                const auto it = std::find_if(definitions.begin(), definitions.end(),
                                             [&](const auto &definition)
                                             { return definition.name == name; });
                if (it == definitions.end())
                {
                    throw Error(ErrorCode::WorkflowValidation, "Unknown parameter: " + name);
                }
                resolved[name] = value;
            }
        }
        for (const auto &definition : definitions)
        {
            if (!resolved.contains(definition.name))
                continue;
            try
            {
                definition.type.validate(resolved.at(definition.name));
            }
            catch (const std::exception &error)
            {
                throw Error(ErrorCode::WorkflowValidation,
                            "Invalid parameter '" + definition.name + "': " + error.what());
            }
        }
        return resolved;
    }

    Json ParameterValues::to_json() const { return values; }

    ParameterValues ParameterValues::from_json(const Json &value)
    {
        if (!value.is_null() && !value.is_object())
        {
            throw Error(ErrorCode::WorkflowValidation, "Parameter values must be an object");
        }
        return {value.is_null() ? Json::object() : value};
    }

    class ProjectMethodExecutionContext final : public sdk::MethodExecutionContext
    {
    public:
        ProjectMethodExecutionContext(Project &project, const sdk::ExecutionServices &services)
            : project_(project), services_(services) {}

        Json query(std::string_view sql) const override { return project_.query_json(std::string(sql)); }
        void execute(std::string_view sql) override { project_.execute_sql(std::string(sql)); }
        bool has_table(std::string_view table_name) const override {
            return ProjectTableStore(project_).has_table(std::string(table_name));
        }
        Json metadata() const override { return project_.get_metadata(); }
        bool cancellation_requested() const noexcept override {
            return services_.cancellation_requested && services_.cancellation_requested();
        }
        void report_progress(double fraction, std::string_view message) override {
            if (services_.report_progress) services_.report_progress(fraction, message);
        }
        void log(std::string_view level, std::string_view message) override {
            if (services_.log) services_.log(level, message);
        }

    private:
        Project &project_;
        const sdk::ExecutionServices &services_;
    };

    Method::Method(MethodDefinition definition, MethodExecutor executor,
                   MethodValidator validator, sdk::ContextMethodExecutor context_executor)
        : definition_(std::move(definition)), executor_(std::move(executor)),
          validator_(std::move(validator)), context_executor_(std::move(context_executor))
    {
        if (definition_.id.empty())
        {
            throw Error(ErrorCode::InvalidArgument, "Method id must not be empty");
        }
        std::vector<std::string> parameter_names;
        for (const auto &parameter : definition_.parameters.definitions)
        {
            if (parameter.name.empty() ||
                std::find(parameter_names.begin(), parameter_names.end(), parameter.name) != parameter_names.end())
            {
                throw Error(ErrorCode::InvalidArgument, "Method parameter names must be unique and non-empty");
            }
            parameter_names.push_back(parameter.name);
        }
    }

    const MethodDefinition &Method::definition() const noexcept { return definition_; }

    Json Method::to_json() const
    {
        return {
            {"id", definition_.id}, {"name", definition_.name}, {"description", definition_.description}, {"version", definition_.version}, {"domain", definition_.domain}, {"reads", definition_.reads}, {"single_occurrence", definition_.single_occurrence}, {"developer", definition_.developer}, {"contact", definition_.contact}, {"link", definition_.link}, {"doi", definition_.doi}, {"parameters", definition_.parameters.to_json()}, {"writes", definition_.writes}};
    }

    MethodDefinition Method::definition_from_json(const Json &value)
    {
        if (!value.is_object() || !value.contains("id"))
        {
            throw Error(ErrorCode::InvalidArgument, "Method metadata requires an id");
        }
        MethodDefinition definition;
        definition.id = value.at("id").get<std::string>();
        definition.name = value.value("name", definition.id);
        definition.description = value.value("description", "");
        definition.version = value.value("version", "1");
        definition.domain = value.value("domain", "");
        definition.reads = value.value("reads", std::vector<std::string>{});
        definition.single_occurrence = value.value("single_occurrence", false);
        definition.developer = value.value("developer", "");
        definition.contact = value.value("contact", "");
        definition.link = value.value("link", "");
        definition.doi = value.value("doi", "");
        definition.parameters = ParameterSchema::from_json(value.value("parameters", Json::array()));

        definition.writes = value.value("writes", std::vector<std::string>{});
        return definition;
    }

    Json Method::resolve_parameters(const Json &value) const
    {
        Json resolved = definition_.parameters.resolve_and_validate(value);
        validate_parameters(resolved);
        return resolved;
    }

    void Method::validate_parameters(const Json &value) const
    {
        if (!validator_)
            return;
        try
        {
            validator_(value);
        }
        catch (const Error &)
        {
            throw;
        }
        catch (const std::exception &error)
        {
            throw Error(ErrorCode::WorkflowValidation,
                        definition_.id + ": invalid parameters: " + error.what());
        }
    }

    Json Method::run(Project &project, const Json &parameters,
                     const sdk::ExecutionServices &services) const
    {
        if (!executor_ && !context_executor_)
        {
            throw Error(ErrorCode::MethodExecution,
                        "Method has no implementation: " + definition_.id);
        }
        try
        {
            const auto resolved = resolve_parameters(parameters);
            if (context_executor_)
            {
                ProjectMethodExecutionContext context(project, services);
                return context_executor_(context, resolved);
            }
            return executor_(project, resolved);
        }
        catch (const Error &)
        {
            throw;
        }
        catch (const std::exception &error)
        {
            throw Error(ErrorCode::MethodExecution,
                        definition_.id + ": " + error.what());
        }
    }

    void MethodRegistry::register_method(Method method)
    {
        if (find(method.definition().id))
        {
            throw Error(ErrorCode::InvalidArgument,
                        "Method already registered: " + method.definition().id);
        }
        methods_.push_back(std::move(method));
    }

    const Method *MethodRegistry::find(const std::string &id) const noexcept
    {
        const auto it = std::find_if(methods_.begin(), methods_.end(),
                                     [&](const Method &method)
                                     { return method.definition().id == id; });
        return it == methods_.end() ? nullptr : &*it;
    }

    std::vector<MethodDefinition> MethodRegistry::list(const std::string &domain) const
    {
        std::vector<MethodDefinition> output;
        output.reserve(methods_.size());
        for (const auto &method : methods_)
        {
            if (domain.empty() || method.definition().domain == domain)
                output.push_back(method.definition());
        }
        return output;
    }

    Json OperationDefinition::Port::to_json() const
    {
        return {{"id", id},
                {"semantic_contract", semantic_contract},
                {"cardinality", cardinality},
                {"data_kind", data_kind},
                {"representations", representations},
                {"optional", optional}};
    }

    OperationDefinition::Port OperationDefinition::Port::from_json(const Json &value)
    {
        if (!value.is_object() || value.value("id", "").empty())
            throw Error(ErrorCode::SchemaMismatch, "Operation port requires a non-empty id");
        Port port;
        port.id = value.at("id").get<std::string>();
        port.semantic_contract = value.value("semantic_contract", "");
        port.cardinality = value.value("cardinality", "one");
        port.data_kind = value.value("data_kind", "");
        port.representations = value.value("representations", std::vector<std::string>{});
        port.optional = value.value("optional", false);
        if (port.semantic_contract.empty())
            throw Error(ErrorCode::SchemaMismatch, "Operation port requires semantic_contract: " + port.id);
        if (port.cardinality != "one" && port.cardinality != "many")
            throw Error(ErrorCode::SchemaMismatch, "Invalid operation port cardinality: " + port.id);
        return port;
    }

    Operation::Operation(OperationDefinition definition,
                         WorkflowOperationExecutor executor,
                         OperationValidator validator)
        : definition_(std::move(definition)), validator_(std::move(validator)),
          executor_(std::move(executor))
    {
        if (definition_.id.empty())
            throw Error(ErrorCode::InvalidArgument, "Operation id must not be empty");
    }
    const OperationDefinition &Operation::definition() const noexcept { return definition_; }
    Json Operation::to_json() const
    {
        Json input_ports = Json::array();
        for (const auto &port : definition_.input_ports)
            input_ports.push_back(port.to_json());
        Json output_ports = Json::array();
        for (const auto &port : definition_.output_ports)
            output_ports.push_back(port.to_json());
        return {{"id", definition_.id},
                {"name", definition_.name},
                {"description", definition_.description},
                {"domain", definition_.domain},
                {"version", definition_.version},
                {"project_entry", definition_.project_entry},

                {"parameters", definition_.parameters.to_json()},
                {"input_ports", input_ports},
                {"output_ports", output_ports}};
    }
    Json Operation::resolve_parameters(const Json &value) const { return definition_.parameters.resolve_and_validate(value); }
    Json Operation::run_workflow(Project &project, const Json &value,
                                 const std::string &operation_instance,
                                 const Json &inputs) const
    {
        if (!executor_)
            throw Error(ErrorCode::MethodExecution, "Operation has no workflow implementation: " + definition_.id);
        try {
            Json parameter_values = value;
            for (auto it = inputs.begin(); it != inputs.end(); ++it) {
                if (it.key().rfind("parameter:", 0) != 0) continue;
                parameter_values[it.key().substr(std::string("parameter:").size())] = it.value();
            }
            const auto resolved = resolve_parameters(parameter_values);
            if (validator_) validator_(resolved);
            const auto result = executor_(project, resolved, operation_instance, inputs);
            const auto inventory = project.get_artifact_inventory();
            const auto artifact_text = [](const Json &artifact, const char *key) {
                const auto value = artifact.find(key);
                return value != artifact.end() && value->is_string() ? value->get<std::string>() : std::string{};
            };
            for (const auto &port : definition_.output_ports) {
                const bool is_table = port.data_kind == "duckdb_table";
                if (port.semantic_contract.empty()) continue;
                const bool already_published = std::any_of(inventory.begin(), inventory.end(), [&](const Json &artifact) {
                    return artifact_text(artifact, "producer_instance") == operation_instance &&
                           artifact_text(artifact, "contract_id") == port.semantic_contract &&
                           artifact_text(artifact, "workflow_revision") == std::to_string(project.get_workflow().version) &&
                           artifact_text(artifact, "status") == "published";
                });
                if (is_table) {
                    if (!already_published)
                        throw Error(ErrorCode::MethodExecution,
                                    "operation did not publish table output " + port.semantic_contract);
                    continue;
                }
                if (already_published) continue;
                Json payload = port.semantic_contract == "operationSuccessSignal" ? Json(true) : result;
                if (result.is_object()) {
                    if (result.contains(port.id)) payload = result.at(port.id);
                    else if (result.contains(port.semantic_contract)) payload = result.at(port.semantic_contract);
                }
                project.publish_result_artifact(port.semantic_contract, payload,
                                                definition_.id, operation_instance,
                                                project.get_workflow().version);
            }
            return result;
        } catch (const Error &) {
            throw;
        } catch (const std::exception &error) {
            throw Error(ErrorCode::MethodExecution,
                        definition_.id + ": " + error.what());
        }
    }

    void OperationRegistry::register_operation(Operation operation)
    {
        if (find(operation.definition().id))
            throw Error(ErrorCode::InvalidArgument, "Operation already registered: " + operation.definition().id);
        operations_.push_back(std::move(operation));
    }
    const Operation *OperationRegistry::find(const std::string &id) const noexcept
    {
        const auto it = std::find_if(operations_.begin(), operations_.end(), [&](const Operation &operation)
                                     { return operation.definition().id == id; });
        return it == operations_.end() ? nullptr : &*it;
    }
    std::vector<OperationDefinition> OperationRegistry::list(const std::string &domain) const
    {
        std::vector<OperationDefinition> output;
        for (const auto &operation : operations_)
            if (domain.empty() || operation.definition().domain == domain)
                output.push_back(operation.definition());
        return output;
    }


    Json WorkflowOperation::to_json() const
    {
        Json output = {{"id", id}, {"operation", operation},
                       {"parameters", parameters.to_json()}, {"inputs", inputs}};
        if (!position.empty()) output["position"] = position;
        return output;
    }

    WorkflowOperation WorkflowOperation::from_json(const Json &value)
    {
        if (!value.is_object() || value.value("id", "").empty() ||
            value.value("operation", "").empty())
            throw Error(ErrorCode::WorkflowValidation,
                        "Workflow operation requires id and operation");
        WorkflowOperation output;
        output.id = value.at("id").get<std::string>();
        output.operation = value.at("operation").get<std::string>();
        output.parameters = ParameterValues::from_json(value.value("parameters", Json::object()));
        output.inputs = value.value("inputs", Json::object());
        if (!output.inputs.is_object())
            throw Error(ErrorCode::WorkflowValidation,
                        "Workflow operation inputs must be an object: " + output.id);
        output.position = value.value("position", Json::object());
        if (!output.position.is_object())
            throw Error(ErrorCode::WorkflowValidation,
                        "Workflow operation position must be an object: " + output.id);
        for (const auto &coordinate : {"x", "y"})
            if (output.position.contains(coordinate) && !output.position.at(coordinate).is_number())
                throw Error(ErrorCode::WorkflowValidation,
                            "Workflow operation position coordinate must be numeric: " + output.id);
        return output;
    }

    Json WorkflowConnection::to_json() const
    {
        return {{"source_operation", source_operation}, {"source_port", source_port},

                {"target_operation", target_operation}, {"target_port", target_port}};
    }

    WorkflowConnection WorkflowConnection::from_json(const Json &value)
    {
        if (!value.is_object())
            throw Error(ErrorCode::WorkflowValidation,
                        "Workflow connection must be an object");
        WorkflowConnection output{
            value.value("source_operation", ""), value.value("source_port", ""),

            value.value("target_operation", ""), value.value("target_port", "")};
        if (output.source_operation.empty() || output.source_port.empty() ||
            output.target_operation.empty() || output.target_port.empty())
            throw Error(ErrorCode::WorkflowValidation,
                        "Workflow connection requires source and target operation ports");
        return output;
    }


    void Workflow::validate(const OperationRegistry &registry) const
    {
        if (schema_version != 1)
            throw Error(ErrorCode::SchemaMismatch,
                        "Unsupported workflow schema version: " + std::to_string(schema_version));
        if (!metadata.is_object())
            throw Error(ErrorCode::WorkflowValidation, "Workflow metadata must be an object");
        const auto require_metadata_text = [&](const char *key) {
            const auto value = metadata.find(key);
            if (value == metadata.end() || !value->is_string() || value->get<std::string>().empty())
                throw Error(ErrorCode::WorkflowValidation,
                            std::string("Workflow metadata requires a non-empty ") + key);
        };
        require_metadata_text("name");
        require_metadata_text("description");

        if (operations.empty() && connections.empty()) return;

        std::set<std::string> operation_ids;
        std::map<std::string, const OperationDefinition *> definitions;
        std::map<std::string, std::set<std::string>> connected_inputs;
        std::map<std::string, std::vector<std::string>> outgoing;
        std::map<std::string, std::size_t> indegree;
        for (const auto &operation : operations) {
            if (operation.id.empty() || !operation_ids.insert(operation.id).second)
                throw Error(ErrorCode::WorkflowValidation,
                            "Workflow operation ids must be unique and non-empty");
            const auto *registered = registry.find(operation.operation);
            if (!registered)
                throw Error(ErrorCode::WorkflowValidation,
                            "Workflow operation is unavailable in the current installation: " + operation.operation);
            const auto &definition = registered->definition();
            try {
                registered->resolve_parameters(operation.parameters.values);
            } catch (const std::exception &error) {
                throw Error(ErrorCode::WorkflowValidation,
                            "Invalid parameters for workflow operation " + operation.id + ": " + error.what());
            }
            definitions.emplace(operation.id, &definition);
            indegree.emplace(operation.id, 0);
        }
        for (const auto &connection : connections) {
            const auto source = definitions.find(connection.source_operation);
            const auto target = definitions.find(connection.target_operation);
            if (source == definitions.end() || target == definitions.end())
                throw Error(ErrorCode::WorkflowValidation,
                            "Workflow connection references an unknown operation instance");
            const auto source_port = std::find_if(
                source->second->output_ports.begin(), source->second->output_ports.end(),
                [&](const auto &port) { return port.id == connection.source_port; });
            if (source_port == source->second->output_ports.end())
                throw Error(ErrorCode::WorkflowValidation,
                            "Workflow connection references unknown output port " + connection.source_port +
                            " on " + connection.source_operation);
            const auto input_port = std::find_if(
                target->second->input_ports.begin(), target->second->input_ports.end(),
                [&](const auto &port) { return port.id == connection.target_port; });
            const bool parameter_binding = connection.target_port.rfind("parameter:", 0) == 0 &&
                std::find_if(target->second->parameters.definitions.begin(),
                             target->second->parameters.definitions.end(), [&](const auto &parameter) {
                                 return "parameter:" + parameter.name == connection.target_port;
                             }) != target->second->parameters.definitions.end();
            if (input_port == target->second->input_ports.end() && !parameter_binding)
                throw Error(ErrorCode::WorkflowValidation,
                            "Workflow connection references unknown input port " + connection.target_port +
                            " on " + connection.target_operation);
            if (!connected_inputs[connection.target_operation].insert(connection.target_port).second)
                throw Error(ErrorCode::WorkflowValidation,
                            "Workflow target port has more than one connection: " + connection.target_operation +
                            "." + connection.target_port);
            if (input_port != target->second->input_ports.end() &&
                source_port->semantic_contract != input_port->semantic_contract)
                throw Error(ErrorCode::WorkflowValidation,
                            "Workflow connection has incompatible contracts: " + connection.source_port +
                            " -> " + connection.target_port);
            outgoing[connection.source_operation].push_back(connection.target_operation);
            ++indegree[connection.target_operation];
        }
        for (const auto &[operation_id, definition] : definitions)
            for (const auto &port : definition->input_ports)
                if (!port.optional && !connected_inputs[operation_id].contains(port.id))
                    throw Error(ErrorCode::WorkflowValidation,
                                "Required workflow input is not connected: " + operation_id + "." + port.id);
        std::vector<std::string> ready;
        for (const auto &[id, degree] : indegree) if (degree == 0) ready.push_back(id);
        std::size_t visited = 0;
        for (std::size_t index = 0; index < ready.size(); ++index) {
            ++visited;
            for (const auto &target : outgoing[ready[index]])
                if (--indegree[target] == 0) ready.push_back(target);
        }
        if (visited != operations.size())
            throw Error(ErrorCode::WorkflowValidation,
                        "Workflow operation connections contain a cycle");
    }

    Json Workflow::to_json() const
    {
        Json output = {{"schema_version", schema_version}, {"workflow_id", workflow_id},
                       {"name", name}, {"version", version},
                       {"metadata", metadata},
                       {"operations", Json::array()}, {"connections", Json::array()}};
        for (const auto &operation : operations)
            output["operations"].push_back(operation.to_json());
        for (const auto &connection : connections)
            output["connections"].push_back(connection.to_json());
        return output;
    }

    bool Method::implemented() const noexcept {
        return static_cast<bool>(executor_) || static_cast<bool>(context_executor_);
    }

    Workflow Workflow::from_json(const Json &value)
    {
        if (value.is_null())
            return {};
        Workflow workflow;
        if (!value.is_object())
            throw Error(ErrorCode::WorkflowValidation, "Workflow must be an operation graph object");
        if (value.contains("steps"))
            throw Error(ErrorCode::WorkflowValidation, "Legacy workflow steps are not supported; use operations and connections");
        workflow.name = value.value("name", "");
        workflow.schema_version = value.value("schema_version", 1);
        workflow.workflow_id = value.value("workflow_id", "");
        workflow.version = value.value("version", 1);
        workflow.metadata = value.value("metadata", Json::object());
        if (workflow.metadata.is_null()) workflow.metadata = Json::object();
        if (workflow.metadata.is_string()) {
            try {
                const auto parsed = Json::parse(workflow.metadata.get<std::string>());
                if (parsed.is_object()) workflow.metadata = parsed;
            } catch (const std::exception &) {
            }
        }
        if (!workflow.metadata.is_object()) workflow.metadata = Json::object();
        if (!workflow.metadata.contains("name")) workflow.metadata["name"] = "Untitled workflow";
        if (!workflow.metadata.contains("description"))
            workflow.metadata["description"] = "Describe the purpose of this workflow.";

        for (const auto &item : value.value("operations", Json::array()))
            workflow.operations.push_back(WorkflowOperation::from_json(item));
        for (const auto &item : value.value("connections", Json::array()))
            workflow.connections.push_back(WorkflowConnection::from_json(item));
        return workflow;
    }

    struct Project::Impl
    {
        ProjectOptions options;
        ProjectInfo info;
        mutable std::mutex mutex;
        mutable std::mutex workflow_execution_mutex;
        bool closed{false};
        Project::OperationLogCallback operation_log_callback;
        std::atomic_bool *cancellation_flag{nullptr};
    };

    class Connection
    {
    public:
        Connection(const Project::Impl &impl)
        {
            duckdb_config config = nullptr;
            if (duckdb_create_config(&config) == DuckDBError)
            {
                throw Error(ErrorCode::DatabaseError, "create DuckDB config failed");
            }
            char *error = nullptr;
            if (duckdb_open_ext(impl.options.database_path.string().c_str(), &database_, config, &error) != DuckDBSuccess)
            {
                const std::string message = error ? error : "open DuckDB database failed";
                if (error)
                    duckdb_free(error);
                duckdb_destroy_config(&config);
                throw Error(ErrorCode::DatabaseError, message);
            }
            duckdb_destroy_config(&config);
            if (duckdb_connect(database_, &connection_) != DuckDBSuccess)
            {
                duckdb_close(&database_);
                throw Error(ErrorCode::DatabaseError, "connect DuckDB database failed");
            }
        }
        ~Connection()
        {
            if (connection_)
                duckdb_disconnect(&connection_);
            if (database_)
                duckdb_close(&database_);
        }
        duckdb_connection get() const noexcept { return connection_; }

    private:
        duckdb_database database_{nullptr};
        duckdb_connection connection_{nullptr};
    };

    void ensure_active(const Project::Impl &impl)
    {
        if (impl.closed)
            throw Error(ErrorCode::InvalidArgument, "Project is closed");
    }

    std::vector<std::string> workflow_domains(const Json &workflow)
    {
        if (!workflow.is_object()) return {};
        std::set<std::string> domains;
        for (const auto &operation : workflow.value("operations", Json::array()))
        {
            const auto operation_id = operation.value("operation", "");
            const auto separator = operation_id.find('.');
            if (separator != std::string::npos && separator > 0)
                domains.insert(operation_id.substr(0, separator));
        }
        return {domains.begin(), domains.end()};
    }

    ProjectInfo read_info(duckdb_connection connection)
    {
        ProjectInfo info;
        prepared(connection, "SELECT metadata, workflow, schema_version, framework_version, created_at FROM PROJECT LIMIT 1", "read PROJECT row", [](Statement) {}, [&](duckdb_result &result)
                 {
                 if (duckdb_row_count(&result) == 0) throw Error(ErrorCode::ProjectNotFound, "Project row not found");
                 info.metadata = parse_json(value_string(result, 0, 0), "PROJECT metadata");
                 info.domains = workflow_domains(parse_json(value_string(result, 1, 0), "PROJECT workflow"));
                 info.schema_version = duckdb_value_int32(&result, 2, 0);
                 info.framework_version = value_string(result, 3, 0);
                 info.created_at = value_string(result, 4, 0); });
        return info;
    }

    void audit(duckdb_connection connection, const std::string &operation,
               const std::string &object, const Json &details)
    {
        prepared(connection, "INSERT INTO AUDIT_TRAIL (operation_type, object_type, operation_details) VALUES (?, ?, ?)", "write audit trail", [&](Statement statement)
                 {
                 bind_text(statement, 1, operation);
                 bind_text(statement, 2, object);
                 bind_text(statement, 3, json_text(details)); }, [](duckdb_result &) {});
    }

    Project project_from_options(const ProjectOptions &options, bool creating)
    {
        if (options.database_path.empty())
            throw Error(ErrorCode::InvalidArgument, "Project database path is required");
        const bool exists = options.database_path != ":memory:" && std::filesystem::exists(options.database_path);
        if (!creating && !exists)
            throw Error(ErrorCode::ProjectNotFound, "Project database does not exist");
        if (!exists && options.database_path != ":memory:")
        {
            const auto parent = options.database_path.parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent);
        }
        auto impl = std::make_shared<Project::Impl>();
        impl->options = options;
        Connection connection(*impl);
        ensure_schema(connection.get(), options);
        const idx_t existing_projects = project_row_count(connection.get());
        if (!creating && existing_projects != 1)
            throw Error(ErrorCode::SchemaMismatch, "project database must contain exactly one PROJECT row");
        if (creating && existing_projects != 0)
            throw Error(ErrorCode::ProjectAlreadyExists, "DuckDB file already contains a project");
        if (creating)
        {
            Json initial_workflow = {{"schema_version", 1},
                                     {"workflow_id", "workflow"},
                                     {"name", "Workflow"},
                                     {"version", 1},
                                     {"metadata", options.workflow_metadata},
                                     {"operations", Json::array()}, {"connections", Json::array()}};
            initial_workflow = Workflow::from_json(initial_workflow).to_json();
            prepared(connection.get(), "INSERT INTO PROJECT (metadata, workflow) VALUES (?, ?)", "create PROJECT row", [&](Statement statement)
                     { bind_text(statement, 1, json_text(options.metadata)); bind_text(statement, 2, json_text(initial_workflow)); }, [](duckdb_result &) {});
        }
        impl->info = read_info(connection.get());
        audit(connection.get(), creating ? "create" : "open", "project", Json::object());
        if (!creating)
            query(connection.get(), "UPDATE WORKFLOW_EXECUTION SET status = 'interrupted', completed_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE status = 'running'", "recover workflow executions");
        return Project(std::move(impl));
    }

    Project Project::create(const ProjectOptions &options) { return project_from_options(options, true); }
    Project Project::open(const ProjectOptions &options) { return project_from_options(options, false); }

    Project::Project(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

    Project::Project(Project &&other) noexcept = default;
    Project &Project::operator=(Project &&other) noexcept = default;
    Project::~Project() { close(); }

    const ProjectInfo &Project::info() const
    {
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        return impl_->info;
    }

    Json Project::get_metadata() const
    {
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        return impl_->info.metadata;
    }

    const std::filesystem::path &Project::get_database_path() const noexcept { return impl_->options.database_path; }

    std::vector<std::string> Project::get_domains() const
    {
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        return impl_->info.domains;
    }

    void Project::validate() const
    {
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);
        query(connection.get(), "SELECT metadata, workflow, schema_version, framework_version FROM PROJECT LIMIT 0", "validate PROJECT schema");
        read_info(connection.get());

        query(connection.get(), "SELECT operation_type, object_type, operation_details, created_at FROM AUDIT_TRAIL LIMIT 0", "validate AUDIT_TRAIL schema");
        query(connection.get(), "SELECT revision, workflow, created_at FROM WORKFLOW_REVISION LIMIT 0", "validate WORKFLOW_REVISION schema");
        query(connection.get(), "SELECT workflow_revision, launch_snapshot, status, progress, result_reference, started_at, completed_at, error, created_at, updated_at FROM WORKFLOW_EXECUTION LIMIT 0", "validate WORKFLOW_EXECUTION schema");
        query(connection.get(), "SELECT workflow_revision, step_index, operation, parameters, parameter_hash, cache_key, status, progress, result_reference, error_code, error_message, started_at, completed_at, updated_at FROM WORKFLOW_EXECUTION_STEP LIMIT 0", "validate WORKFLOW_EXECUTION_STEP schema");
    }

    void Project::set_metadata(Json metadata)
    {
        if (!metadata.is_object())
            throw Error(ErrorCode::InvalidArgument, "Project metadata must be an object");
        for (const auto &[key, value] : metadata.items())
            if (value.is_object() || value.is_array())
                throw Error(ErrorCode::InvalidArgument, "Project metadata values must be scalar: " + key);
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);
        prepared(connection.get(), "UPDATE PROJECT SET metadata = ?, updated_at = CURRENT_TIMESTAMP", "update metadata", [&](Statement statement)
                 { bind_text(statement, 1, json_text(metadata)); }, [](duckdb_result &) {});
        audit(connection.get(), "update", "metadata", metadata);
        impl_->info.metadata = std::move(metadata);
    }

    Project Project::copy(const ProjectOptions &options) const
    {
        const auto workflow_value = get_workflow();
        Project destination = Project::create(options);
        destination.set_metadata(impl_->info.metadata);
        destination.set_workflow(workflow_value);
        return destination;
    }

    Workflow Project::get_workflow() const
    {
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);
        Json value;
        prepared(connection.get(), "SELECT workflow FROM PROJECT", "read workflow", [&](Statement statement) {}, [&](duckdb_result &result)
                 { if (duckdb_row_count(&result)) value = parse_json(value_string(result, 0, 0), "workflow"); });
        return Workflow::from_json(value);
    }

    void Project::set_workflow(Workflow workflow_value)
    {
        if (workflow_value.schema_version != 1)
            throw Error(ErrorCode::SchemaMismatch, "Unsupported workflow schema version");
        const auto execution = query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1");
        if (!execution.empty())
        {
            const auto status = execution.at(0).value("status", "");
            if (status == "queued" || status == "running" || status == "cancelling")
                throw Error(ErrorCode::InvalidArgument, "workflow mutation is blocked while an execution is active");
        }
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);
        prepared(connection.get(), "UPDATE PROJECT SET workflow = ?, updated_at = CURRENT_TIMESTAMP", "update workflow", [&](Statement statement)
                 { bind_text(statement, 1, json_text(workflow_value.to_json())); }, [](duckdb_result &) {});
        audit(connection.get(), "update", "workflow", workflow_value.to_json());
    }

    void Project::set_workflow(Workflow workflow_value, const OperationRegistry &registry)
    {
        const auto previous_workflow = get_workflow();
        workflow_value.validate(registry);
        const auto execution = query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1");
        if (!execution.empty())
        {
            const auto status = execution.at(0).value("status", "");
            if (status == "queued" || status == "running" || status == "cancelling")
                throw Error(ErrorCode::InvalidArgument,
                            "workflow mutation is blocked while an execution is active");
        }
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);
        std::set<std::string> changed_operations;
        for (const auto &operation : previous_workflow.operations) {
            const auto current = std::find_if(workflow_value.operations.begin(), workflow_value.operations.end(),
                                              [&](const auto &candidate) { return candidate.id == operation.id; });
            if (current == workflow_value.operations.end() ||
                current->operation != operation.operation ||
                current->parameters.values.dump() != operation.parameters.values.dump())
                changed_operations.insert(operation.id);
        }
        for (const auto &operation : workflow_value.operations) {
            const auto previous = std::find_if(previous_workflow.operations.begin(), previous_workflow.operations.end(),
                                               [&](const auto &candidate) { return candidate.id == operation.id; });
            if (previous == previous_workflow.operations.end())
                continue;
            if (previous->operation != operation.operation ||
                previous->parameters.values.dump() != operation.parameters.values.dump())
                changed_operations.insert(operation.id);
        }
        prepared(connection.get(), "UPDATE PROJECT SET workflow = ?, updated_at = CURRENT_TIMESTAMP", "update operation workflow", [&](Statement statement)
                 { bind_text(statement, 1, json_text(workflow_value.to_json())); }, [](duckdb_result &) {});
        prepared(connection.get(), "INSERT OR REPLACE INTO WORKFLOW_REVISION (revision, workflow) VALUES (?, ?)", "save operation workflow revision", [&](Statement statement)
                 { duckdb_bind_int32(statement, 1, workflow_value.version); bind_text(statement, 2, json_text(workflow_value.to_json())); }, [](duckdb_result &) {});
        for (const auto &operation_id : changed_operations)
            query(connection.get(), "UPDATE ARTIFACT_INVENTORY SET status = 'stale' WHERE producer_instance = " + detail::sql_quote(operation_id),
                  "invalidate changed operation artifacts");
        audit(connection.get(), "update", "workflow", workflow_value.to_json());
    }

    void Project::clear_workflow_history()
    {
        const auto current = get_workflow();
        const auto inventory = get_artifact_inventory();
        const auto lineage_rows = query_json("SELECT artifact_id, source_artifact_id FROM ARTIFACT_LINEAGE");

        std::unordered_set<std::string> current_instances;
        for (const auto &operation : current.operations)
            current_instances.insert(operation.id);

        const auto revision_of = [](const Json &artifact) {
            const auto value = artifact.find("workflow_revision");
            if (value == artifact.end() || value->is_null()) return -1;
            if (value->is_number_integer()) return value->get<int>();
            if (value->is_string() && !value->get<std::string>().empty()) return std::stoi(value->get<std::string>());
            return -1;
        };
        const auto text_of = [](const Json &object, const char *key) {
            const auto value = object.find(key);
            return value != object.end() && value->is_string() ? value->get<std::string>() : std::string{};
        };
        const auto key_of = [](const Json &artifact) {
            const auto text = [](const Json &object, const char *key) {
                const auto value = object.find(key);
                return value != object.end() && value->is_string() ? value->get<std::string>() : std::string{};
            };
            return text(artifact, "producer_instance") + "\n" + text(artifact, "contract_id");
        };
        std::unordered_map<std::string, std::string> retained_by_output;
        std::unordered_map<std::string, Json> retained;
        std::unordered_map<std::string, std::vector<std::string>> sources_by_artifact;
        for (const auto &lineage : lineage_rows)
            sources_by_artifact[text_of(lineage, "artifact_id")].push_back(text_of(lineage, "source_artifact_id"));
        for (const auto &artifact : inventory)
        {
            if (text_of(artifact, "status") != "published" ||
                !current_instances.contains(text_of(artifact, "producer_instance")))
                continue;
            const auto artifact_id = text_of(artifact, "artifact_id");
            const auto key = key_of(artifact);
            const auto previous = retained.find(key);
            if (previous == retained.end() ||
                revision_of(artifact) > revision_of(previous->second) ||
                (revision_of(artifact) == revision_of(previous->second) &&
                 text_of(artifact, "created_at") > text_of(previous->second, "created_at")))
            {
                retained[key] = artifact;
                retained_by_output[key] = artifact_id;
            }
        }
        bool pruned;
        do
        {
            pruned = false;
            for (auto it = retained_by_output.begin(); it != retained_by_output.end(); )
            {
                const auto sources = sources_by_artifact.find(it->second);
                const bool has_removed_source = sources != sources_by_artifact.end() &&
                    std::any_of(sources->second.begin(), sources->second.end(), [&](const std::string &source) {
                        return std::none_of(retained_by_output.begin(), retained_by_output.end(), [&](const auto &entry) {
                            return entry.second == source;
                        });
                    });
                if (!has_removed_source)
                {
                    ++it;
                    continue;
                }
                retained.erase(it->first);
                it = retained_by_output.erase(it);
                pruned = true;
            }
        } while (pruned);

        const auto physical_artifact_tables = query_json(
            "SELECT table_name FROM information_schema.tables "
            "WHERE table_schema = 'main' AND substr(upper(table_name), 1, 9) = 'ARTIFACT_' "
            "AND table_name NOT IN ('ARTIFACT_CACHE', 'ARTIFACT_CACHE_OUTPUT', "
            "'ARTIFACT_INVENTORY', 'ARTIFACT_LINEAGE')");
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);

        for (const auto &artifact : inventory)
        {
            const auto artifact_id = text_of(artifact, "artifact_id");
            const auto key = key_of(artifact);
            if (retained_by_output.contains(key) && retained_by_output.at(key) == artifact_id)
                continue;
            const auto table = text_of(artifact, "physical_table");
            if (!table.empty())
            {
                std::string quoted = "\"";
                for (const char character : table) quoted += character == '\"' ? "\"\"" : std::string(1, character);
                quoted += "\"";
                query(connection.get(), "DROP TABLE IF EXISTS " + quoted, "clear workflow artifact table");
            }
            query(connection.get(), "DELETE FROM ARTIFACT_LINEAGE WHERE artifact_id = " + detail::sql_quote(artifact_id) +
                                      " OR source_artifact_id = " + detail::sql_quote(artifact_id),
                  "clear removed workflow lineage");
            query(connection.get(), "DELETE FROM ARTIFACT_INVENTORY WHERE artifact_id = " + detail::sql_quote(artifact_id),
                  "clear removed workflow artifact");
        }
        // A previous cleanup could have removed an inventory row without
        // removing its physical table (for example after an interrupted
        // cleanup or an older implementation).  Physical artifact tables
        // are owned by the inventory, so remove every unretained table with
        // the artifact-table prefix as well.
        std::unordered_set<std::string> retained_tables;
        for (const auto &[key, artifact] : retained)
        {
            static_cast<void>(key);
            const auto table = text_of(artifact, "physical_table");
            if (!table.empty()) retained_tables.insert(table);
        }
        for (const auto &entry : physical_artifact_tables)
        {
            const auto table = text_of(entry, "table_name");
            if (table.empty() || retained_tables.contains(table) || !detail::is_generated_artifact_table(table)) continue;
            std::string quoted = "\"";
            for (const char character : table) quoted += character == '\"' ? "\"\"" : std::string(1, character);
            quoted += "\"";
            query(connection.get(), "DROP TABLE IF EXISTS " + quoted, "clear orphaned workflow artifact table");
        }
        query(connection.get(), "DELETE FROM WORKFLOW_REVISION WHERE revision <> " + std::to_string(current.version), "clear workflow revisions");
        query(connection.get(), "DELETE FROM ARTIFACT_CACHE_OUTPUT", "clear artifact cache outputs");
        query(connection.get(), "DELETE FROM ARTIFACT_CACHE", "clear artifact cache entries");
        query(connection.get(), "DELETE FROM ARTIFACT_LINEAGE AS lineage WHERE NOT EXISTS (SELECT 1 FROM ARTIFACT_INVENTORY AS artifact WHERE artifact.artifact_id = lineage.artifact_id) OR NOT EXISTS (SELECT 1 FROM ARTIFACT_INVENTORY AS source WHERE source.artifact_id = lineage.source_artifact_id)", "clear orphaned workflow lineage");
        audit(connection.get(), "update", "workflow", current.to_json());
    }

    void Project::clear_artifact_cache()
    {
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);
        query(connection.get(), "DELETE FROM ARTIFACT_CACHE_OUTPUT", "clear artifact cache outputs");
        query(connection.get(), "DELETE FROM ARTIFACT_CACHE", "clear artifact cache entries");
    }

    void Project::clear_all_artifacts()
    {
        const auto inventory = get_artifact_inventory();
        const auto physical_artifact_tables = query_json(
            "SELECT table_name FROM information_schema.tables "
            "WHERE table_schema = 'main' AND substr(upper(table_name), 1, 9) = 'ARTIFACT_' "
            "AND table_name NOT IN ('ARTIFACT_CACHE', 'ARTIFACT_CACHE_OUTPUT', "
            "'ARTIFACT_INVENTORY', 'ARTIFACT_LINEAGE')");
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);
        const auto drop_table = [&](const std::string &table) {
            std::string quoted = "\"";
            for (const char character : table) quoted += character == '\"' ? "\"\"" : std::string(1, character);
            quoted += "\"";
            query(connection.get(), "DROP TABLE IF EXISTS " + quoted, "clear artifact table");
        };
        for (const auto &artifact : inventory) {
            const auto physical_table_value = artifact.find("physical_table");
            const auto physical_table = physical_table_value != artifact.end() && physical_table_value->is_string()
                ? physical_table_value->get<std::string>()
                : std::string{};
            if (!physical_table.empty() && detail::is_generated_artifact_table(physical_table)) drop_table(physical_table);
        }
        for (const auto &entry : physical_artifact_tables) {
            const auto table_value = entry.find("table_name");
            const auto table = table_value != entry.end() && table_value->is_string()
                ? table_value->get<std::string>()
                : std::string{};
            if (!table.empty() && detail::is_generated_artifact_table(table)) drop_table(table);
        }
        query(connection.get(), "DELETE FROM ARTIFACT_LINEAGE", "clear artifact lineage");
        query(connection.get(), "DELETE FROM ARTIFACT_INVENTORY", "clear artifact inventory");
        query(connection.get(), "DELETE FROM ARTIFACT_CACHE_OUTPUT", "clear artifact cache outputs");
        query(connection.get(), "DELETE FROM ARTIFACT_CACHE", "clear artifact cache entries");
    }

    std::vector<std::string> Project::list_tables() const
    {
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);
        std::vector<std::string> output;
        query(connection.get(), "SELECT table_name FROM information_schema.tables WHERE table_schema = 'main' ORDER BY table_name", "list tables");
        duckdb_result result{};
        if (duckdb_query(connection.get(), "SELECT table_name FROM information_schema.tables WHERE table_schema = 'main' ORDER BY table_name", &result) == DuckDBError)
        {
            const std::string message = db_error(result);
            duckdb_destroy_result(&result);
            throw Error(ErrorCode::DatabaseError, message);
        }
        ResultGuard guard(result);
        for (idx_t row = 0; row < duckdb_row_count(&result); ++row)
            output.push_back(value_string(result, 0, row));
        return output;
    }

    void Project::execute_sql(const std::string &sql) const
    {
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);
        query(connection.get(), sql, "execute project SQL");
    }

    void Project::append_rows(const std::string &table_name,
                              const std::vector<std::string> &column_names,
                              const std::vector<std::vector<std::optional<std::string>>> &rows) const
    {
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);

        duckdb_appender appender = nullptr;
        if (duckdb_appender_create(connection.get(), nullptr, table_name.c_str(), &appender) == DuckDBError)
        {
            const char *message = appender ? duckdb_appender_error(appender) : nullptr;
            throw Error(ErrorCode::DatabaseError,
                        message ? message : ("create DuckDB appender failed for " + table_name));
        }
        AppenderGuard appender_guard(&appender);

        // Select only the supplied columns as the appender's active column list; every
        // other table column (e.g. created_at) is filled with its DEFAULT value.
        for (const auto &name : column_names)
        {
            if (duckdb_appender_add_column(appender, name.c_str()) == DuckDBError)
            {
                const char *message = duckdb_appender_error(appender);
                throw Error(ErrorCode::DatabaseError,
                            message ? message : ("add appender column " + name));
            }
        }

        // Reflect each active column's DuckDB type so numeric values are appended as
        // numbers (not text), matching the R bindings' typed append behaviour.
        const idx_t active_columns = duckdb_appender_column_count(appender);
        std::vector<duckdb_type> types(static_cast<std::size_t>(active_columns));
        for (idx_t col = 0; col < active_columns; ++col)
        {
            duckdb_logical_type logical = duckdb_appender_column_type(appender, col);
            types[static_cast<std::size_t>(col)] = duckdb_get_type_id(logical);
            duckdb_destroy_logical_type(&logical);
        }

        for (const auto &row : rows)
        {
            if (row.size() != column_names.size())
            {
                throw Error(ErrorCode::InvalidArgument,
                            "append_rows row/column count mismatch for " + table_name);
            }
            if (duckdb_appender_begin_row(appender) == DuckDBError)
            {
                const char *message = duckdb_appender_error(appender);
                throw Error(ErrorCode::DatabaseError, message ? message : "begin appender row");
            }
            for (idx_t col = 0; col < active_columns; ++col)
            {
                const auto &cell = row[static_cast<std::size_t>(col)];
                duckdb_state state = DuckDBSuccess;
                if (!cell)
                {
                    state = duckdb_append_null(appender);
                }
                else
                {
                    switch (types[static_cast<std::size_t>(col)])
                    {
                    case DUCKDB_TYPE_VARCHAR:
                        state = duckdb_append_varchar(appender, cell->c_str());
                        break;
                    case DUCKDB_TYPE_DOUBLE:
                        state = duckdb_append_double(appender, std::stod(*cell));
                        break;
                    case DUCKDB_TYPE_FLOAT:
                        state = duckdb_append_float(appender, std::stof(*cell));
                        break;
                    case DUCKDB_TYPE_INTEGER:
                        state = duckdb_append_int32(appender, static_cast<std::int32_t>(std::stoll(*cell)));
                        break;
                    case DUCKDB_TYPE_BIGINT:
                        state = duckdb_append_int64(appender, std::stoll(*cell));
                        break;
                    case DUCKDB_TYPE_SMALLINT:
                        state = duckdb_append_int16(appender, static_cast<std::int16_t>(std::stoll(*cell)));
                        break;
                    case DUCKDB_TYPE_TINYINT:
                        state = duckdb_append_int8(appender, static_cast<std::int8_t>(std::stoll(*cell)));
                        break;
                    case DUCKDB_TYPE_BOOLEAN:
                        state = duckdb_append_bool(appender, *cell == "true" || *cell == "TRUE" || *cell == "1");
                        break;
                    default:
                        throw Error(ErrorCode::DatabaseError,
                                    "append_rows unsupported column type for " + table_name + " col " + std::to_string(col));
                    }
                }
                if (state == DuckDBError)
                {
                    const char *message = duckdb_appender_error(appender);
                    throw Error(ErrorCode::DatabaseError, message ? message : "append appender value");
                }
            }
            if (duckdb_appender_end_row(appender) == DuckDBError)
            {
                const char *message = duckdb_appender_error(appender);
                throw Error(ErrorCode::DatabaseError, message ? message : "end appender row");
            }
        }
        if (duckdb_appender_close(appender) == DuckDBError)
        {
            const char *message = duckdb_appender_error(appender);
            throw Error(ErrorCode::DatabaseError, message ? message : "close appender");
        }
    }

    Json Project::query_json(const std::string &sql) const
    {
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);
        Json rows = Json::array();
        duckdb_result result;
        if (duckdb_query(connection.get(), sql.c_str(), &result) == DuckDBError)
        {
            const std::string message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "query failed";
            duckdb_destroy_result(&result);
            throw Error(ErrorCode::DatabaseError, message);
        }
        const auto columns = duckdb_column_count(&result);
        const auto count = duckdb_row_count(&result);
        for (idx_t row = 0; row < count; ++row)
        {
            Json object = Json::object();
            for (idx_t column = 0; column < columns; ++column)
            {
                const auto name = duckdb_column_name(&result, column);
                if (duckdb_value_is_null(&result, column, row))
                    object[name] = nullptr;
                else
                {
                    char *value = duckdb_value_varchar(&result, column, row);
                    object[name] = value ? value : "";
                    if (value)
                        duckdb_free(value);
                }
            }
            rows.push_back(std::move(object));
        }
        duckdb_destroy_result(&result);
        return rows;
    }


    std::vector<AuditEntry> Project::get_audit_trail() const
    {
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);
        std::vector<AuditEntry> output;
        prepared(connection.get(), "SELECT operation_type, object_type, operation_details, created_at FROM AUDIT_TRAIL ORDER BY created_at ASC", "read audit trail", [&](Statement statement) {}, [&](duckdb_result &result)
                 { for (idx_t row = 0; row < duckdb_row_count(&result); ++row) output.push_back({value_string(result, 0, row), value_string(result, 1, row), parse_json(value_string(result, 2, row), "audit details"), value_string(result, 3, row)}); });
        return output;
    }

    Json Project::get_workflow_execution() const
    {
        return query_json("SELECT workflow_revision, step_index, operation, parameters, parameter_hash, status, completed_at, result_reference, error_message AS error, cache_key FROM WORKFLOW_EXECUTION_STEP ORDER BY workflow_revision, step_index");
    }

    Json Project::get_artifact_inventory() const
    {
        return query_json("SELECT artifact_id, contract_id, representation, physical_table, payload, producer_operation, producer_instance, workflow_revision, status, created_at FROM ARTIFACT_INVENTORY ORDER BY created_at, artifact_id");
    }

    Json Project::get_current_artifact_inventory() const
    {
        auto artifacts = query_json("SELECT artifact_id, contract_id, representation, physical_table, payload, producer_operation, producer_instance, workflow_revision, status, created_at FROM (SELECT *, ROW_NUMBER() OVER (PARTITION BY producer_instance, contract_id ORDER BY workflow_revision DESC, created_at DESC, artifact_id DESC) AS artifact_rank FROM ARTIFACT_INVENTORY WHERE status = 'published') AS current_artifacts WHERE artifact_rank = 1 ORDER BY producer_instance, contract_id");
        for (auto &artifact : artifacts) {
            if (artifact.value("representation", std::string{}) != "table") continue;
            const auto table_value = artifact.find("physical_table");
            if (table_value == artifact.end() || !table_value->is_string() || table_value->get<std::string>().empty()) {
                artifact["status"] = "stale";
                continue;
            }
            std::string quoted = "\"";
            for (const char character : table_value->get<std::string>()) quoted += character == '\"' ? "\"\"" : std::string(1, character);
            quoted += "\"";
            try { query_json("DESCRIBE " + quoted); }
            catch (const std::exception &error) {
                artifact["status"] = "stale";
                artifact["availability_error"] = error.what();
            }
        }
        return artifacts;
    }

    std::string Project::publish_result_artifact(const std::string &contract_id,
                                                  const Json &payload,
                                                  const std::string &producer_operation,
                                                  const std::string &producer_instance,
                                                  int workflow_revision)
    {
        if (contract_id.empty() || producer_operation.empty() || producer_instance.empty())
            throw Error(ErrorCode::InvalidArgument, "invalid result artifact publication");
        const auto quote = [](const std::string &value) {
            std::string output = "'";
            for (const char character : value)
                output += character == '\'' ? "''" : std::string(1, character);
            return output + "'";
        };
        const auto artifact_id = "artifact_" + fingerprint::hash(contract_id + producer_instance + std::to_string(workflow_revision) +
                                                                  fingerprint::canonical_json(payload));
        execute_sql("INSERT OR REPLACE INTO ARTIFACT_INVENTORY (artifact_id, contract_id, representation, payload, producer_operation, producer_instance, workflow_revision, status) VALUES (" +
                    quote(artifact_id) + ", " + quote(contract_id) + ", 'json', " + quote(payload.dump()) + "::JSON, " +
                    quote(producer_operation) + ", " + quote(producer_instance) + ", " + std::to_string(workflow_revision) + ", 'published')");
        return artifact_id;
    }

    Json Project::resolve_workflow_inputs(const std::string &operation_id) const
    {
        return resolve_workflow_inputs(operation_id, Json::object());
    }

    Json Project::resolve_workflow_inputs(const std::string &operation_id,
                                          const Json &current_artifacts) const
    {
        if (operation_id.empty())
            throw Error(ErrorCode::InvalidArgument, "operation_id is required");
        const auto workflow = get_workflow();
        const auto inventory = get_artifact_inventory();
        Json resolved = Json::object();
        for (const auto &connection : workflow.connections) {
            if (connection.target_operation != operation_id) continue;
            const Json *selected = nullptr;
            const auto current_source = current_artifacts.value(connection.source_operation, Json::object());
            const auto current_artifact_id = current_source.value(connection.source_port, "");
            if (!current_artifact_id.empty()) {
                for (const auto &artifact : inventory)
                    if (artifact.value("artifact_id", "") == current_artifact_id &&
                        artifact.value("status", "") == "published") {
                        selected = &artifact;
                        break;
                    }
            }
            int selected_revision = -1;
            std::string selected_created_at;
            std::string selected_artifact_id;
            for (const auto &artifact : inventory) {
                if (!current_artifact_id.empty() && selected != nullptr)
                    break;
                const auto contract = artifact.value("contract_id", "");
                const bool port_matches = contract == connection.source_port ||
                    (contract.size() > connection.source_port.size() + 1 &&
                     contract.ends_with("#" + connection.source_port));
                if (artifact.value("producer_instance", "") == connection.source_operation &&
                    port_matches &&
                    artifact.value("status", "") == "published") {
                    int revision = -1;
                    try {
                        const auto revision_value = artifact.find("workflow_revision");
                        if (revision_value == artifact.end() || revision_value->is_null()) continue;
                        if (revision_value->is_number_integer()) revision = revision_value->get<int>();
                        else if (revision_value->is_string()) revision = std::stoi(revision_value->get<std::string>());
                        else continue;
                    } catch (...) { continue; }
                    if (revision > workflow.version)
                        continue;
                    const auto created_at = artifact.value("created_at", "");
                    const auto artifact_id = artifact.value("artifact_id", "");
                    const bool older_revision = revision < selected_revision;
                    const bool same_revision_older = revision == selected_revision &&
                        selected != nullptr &&
                        (created_at < selected_created_at ||
                         (created_at == selected_created_at && artifact_id <= selected_artifact_id));
                    if (older_revision || same_revision_older)
                        continue;
                    selected = &artifact;
                    selected_revision = revision;
                    selected_created_at = created_at;
                    selected_artifact_id = artifact_id;
                }
            }
            if (selected == nullptr)
                throw Error(ErrorCode::WorkflowValidation,
                            "No published artifact for connection " +
                            connection.source_operation + "." + connection.source_port);
            if (connection.target_port.rfind("parameter:", 0) == 0) {
                const auto &artifact_payload = selected->at("payload");
                if (artifact_payload.is_string())
                    resolved[connection.target_port] = Json::parse(artifact_payload.get<std::string>());
                else if (!artifact_payload.is_null())
                    resolved[connection.target_port] = artifact_payload;
                else if (selected->value("representation", "") == "table") {
                    const auto physical_table = selected->value("physical_table", std::string{});
                    if (physical_table.empty())
                        throw Error(ErrorCode::WorkflowValidation,
                                    "Table artifact has no physical table for connection " +
                                    connection.source_operation + "." + connection.source_port);
                    std::string quoted_table = "\"";
                    for (const char character : physical_table)
                        quoted_table += character == '\"' ? "\"\"" : std::string(1, character);
                    quoted_table += "\"";
                    resolved[connection.target_port] = query_json("SELECT * FROM " + quoted_table);
                } else
                    resolved[connection.target_port] = nullptr;
            } else {
                resolved[connection.target_port] = *selected;
            }
        }
        return resolved;
    }

    Json Project::run_operation_graph(const OperationRegistry &registry)
    {
        std::lock_guard execution_lock(impl_->workflow_execution_mutex);
        detail::WorkflowFileLock process_lock(impl_->options.database_path);
        if (!process_lock.healthy())
            throw Error(ErrorCode::MethodExecution, "Workflow execution lock was lost");
        const auto workflow = get_workflow();
        workflow.validate(registry);
        const std::string launch_snapshot = workflow.to_json().dump();
        {
            std::lock_guard lock(impl_->mutex);
            ensure_active(*impl_);
            Connection connection(*impl_);
            query(connection.get(), "DELETE FROM WORKFLOW_EXECUTION_STEP", "reset graph execution steps");
        }
        std::map<std::string, const WorkflowOperation *> by_id;
        std::map<std::string, std::size_t> indegree;
        std::map<std::string, std::vector<std::string>> outgoing;
        for (const auto &operation : workflow.operations) {
            by_id.emplace(operation.id, &operation);
            indegree[operation.id] = 0;
        }
        for (const auto &connection : workflow.connections) {
            outgoing[connection.source_operation].push_back(connection.target_operation);
            ++indegree[connection.target_operation];
        }
        std::vector<std::string> ready;
        for (const auto &[id, degree] : indegree) if (degree == 0) ready.push_back(id);
        Json executions = Json::array();
        Json current_artifacts = Json::object();
        for (std::size_t index = 0; index < ready.size(); ++index) {
            if (!process_lock.healthy())
                throw Error(ErrorCode::MethodExecution, "Workflow execution lock was lost");
            const auto &operation_id = ready[index];
            const auto &operation = *by_id.at(operation_id);
            const auto *executor = registry.find(operation.operation);
            if (!executor)
                throw Error(ErrorCode::WorkflowValidation,
                            "Unknown workflow operation: " + operation.operation);
            const auto inputs = resolve_workflow_inputs(operation_id, current_artifacts);
            Json effective_parameters = operation.parameters.values;
            for (auto it = inputs.begin(); it != inputs.end(); ++it) {
                if (it.key().rfind("parameter:", 0) != 0) continue;
                effective_parameters[it.key().substr(std::string("parameter:").size())] = it.value();
            }
            const auto resolved_parameters = executor->resolve_parameters(effective_parameters);
            const auto parameter_hash = fingerprint::hash(fingerprint::canonical_json(resolved_parameters));
            const auto cache_key = fingerprint::operation(operation.operation, operation.id, executor->definition().version,
                                                          resolved_parameters, inputs);
            const auto cached_artifacts = query_json(
                "SELECT cached.output_port_id, cached.artifact_id, CAST(inventory.payload AS VARCHAR) AS payload "
                "FROM ARTIFACT_CACHE_OUTPUT AS cached "
                "JOIN ARTIFACT_CACHE AS cache ON cache.fingerprint = cached.fingerprint "
                "JOIN ARTIFACT_INVENTORY AS inventory ON inventory.artifact_id = cached.artifact_id "
                "WHERE cached.fingerprint = " + detail::sql_quote(cache_key) +
                " AND cache.status = 'complete' AND inventory.status = 'published' "
                " AND NOT EXISTS (SELECT 1 FROM ARTIFACT_INVENTORY AS newer "
                "WHERE newer.producer_instance = inventory.producer_instance "
                "AND newer.contract_id = inventory.contract_id AND newer.status = 'published' "
                "AND (CAST(newer.workflow_revision AS INTEGER) > CAST(inventory.workflow_revision AS INTEGER) "
                "OR (CAST(newer.workflow_revision AS INTEGER) = CAST(inventory.workflow_revision AS INTEGER) "
                "AND newer.created_at > inventory.created_at))) "
                "ORDER BY cached.output_port_id, cached.ordinal");
            {
                std::lock_guard lock(impl_->mutex);
                ensure_active(*impl_);
                Connection connection(*impl_);
                execution_row(connection.get(), workflow.version, index, operation.operation,
                              parameter_hash, inputs, "running", cache_key, launch_snapshot);
            }
            try {
            bool complete_cached_outputs = !cached_artifacts.empty();
            for (const auto &port : executor->definition().output_ports) {
                if (port.optional)
                    continue;
                const bool present = std::any_of(cached_artifacts.begin(), cached_artifacts.end(),
                                                 [&](const Json &artifact) {
                                                     const auto output_port = artifact.value("output_port_id", "");
                                                     return output_port == port.id || output_port == port.semantic_contract ||
                                                            (output_port.size() > port.id.size() + 1 &&
                                                             output_port.ends_with("#" + port.id));
                                                 });
                if (!present) {
                    complete_cached_outputs = false;
                    break;
                }
            }
            if (complete_cached_outputs) {
                Json cached_result = {{"emitted_results", Json::object()}};
                for (const auto &artifact : cached_artifacts) {
                    Json reference = {{"artifact_id", artifact.value("artifact_id", "")}};
                    const auto payload_value = artifact.find("payload");
                    const auto payload = payload_value != artifact.end() && payload_value->is_string()
                        ? payload_value->get<std::string>() : std::string{};
                    if (!payload.empty() && payload != "null")
                        reference["payload"] = parse_json(payload, "cached artifact payload");
                    cached_result["emitted_results"][artifact.value("output_port_id", "")] = std::move(reference);
                }
                execute_sql("UPDATE ARTIFACT_CACHE SET last_used_at = CURRENT_TIMESTAMP WHERE fingerprint = " +
                            detail::sql_quote(cache_key));
                {
                    std::lock_guard lock(impl_->mutex);
                    ensure_active(*impl_);
                    Connection connection(*impl_);
                    execution_row(connection.get(), workflow.version, index, operation.operation,
                                  parameter_hash, inputs, "completed", cache_key, launch_snapshot);
                }
                execute_sql("UPDATE WORKFLOW_EXECUTION_STEP SET result_reference = " +
                            detail::sql_quote(cached_result.at("emitted_results").dump()) +
                            " WHERE step_index = " + std::to_string(index));
                log_operation("operation.cache_reused (" + operation.operation + "): Reused cached artifacts; operation execution skipped.");
                executions.push_back({{"operation_id", operation.id},
                                      {"operation", operation.operation},
                                      {"inputs", inputs}, {"result", cached_result}, {"cache_hit", true}});
                for (const auto &artifact : cached_artifacts)
                    current_artifacts[operation.id][artifact.value("output_port_id", "")] =
                        artifact.value("artifact_id", "");
                for (const auto &target : outgoing[operation_id])
                    if (--indegree[target] == 0) ready.push_back(target);
                continue;
            }
            const auto result = executor->run_workflow(
                *this, effective_parameters, operation.id, inputs);
            if (!process_lock.healthy())
                throw Error(ErrorCode::MethodExecution, "Workflow execution lock was lost");
            if (result.contains("emitted_results") && result.at("emitted_results").is_object()) {
                bool complete_outputs = true;
                for (const auto &port : executor->definition().output_ports) {
                    if (port.optional) continue;
                    bool present = false;
                    for (const auto &[output_port, artifact] : result.at("emitted_results").items()) {
                        static_cast<void>(artifact);
                        if (output_port == port.id || output_port == port.semantic_contract) {
                            present = true;
                            break;
                        }
                    }
                    if (!present) { complete_outputs = false; break; }
                }
                if (complete_outputs)
                    execute_sql("INSERT OR REPLACE INTO ARTIFACT_CACHE (fingerprint, operation_id, status, last_used_at) VALUES (" +
                                detail::sql_quote(cache_key) + ", " + detail::sql_quote(operation.operation) +
                                ", 'complete', CURRENT_TIMESTAMP)");
                else {
                    execute_sql("DELETE FROM ARTIFACT_CACHE_OUTPUT WHERE fingerprint = " + detail::sql_quote(cache_key));
                    execute_sql("DELETE FROM ARTIFACT_CACHE WHERE fingerprint = " + detail::sql_quote(cache_key));
                }
                int ordinal = 0;
                for (const auto &[output_port, artifact] : result.at("emitted_results").items()) {
                    const auto artifact_id = artifact.value("artifact_id", std::string{});
                    if (!complete_outputs || artifact_id.empty()) continue;
                    execute_sql("INSERT OR REPLACE INTO ARTIFACT_CACHE_OUTPUT (fingerprint, output_port_id, ordinal, artifact_id) VALUES (" +
                                detail::sql_quote(cache_key) + ", " + detail::sql_quote(output_port) + ", " +
                                std::to_string(ordinal++) + ", " + detail::sql_quote(artifact_id) + ")");
                }
            }
            {
                std::lock_guard lock(impl_->mutex);
                ensure_active(*impl_);
                Connection connection(*impl_);
                execution_row(connection.get(), workflow.version, index, operation.operation,
                              parameter_hash, inputs, "completed", cache_key, launch_snapshot);
            }
            if (result.contains("emitted_results") && result.at("emitted_results").is_object())
                execute_sql("UPDATE WORKFLOW_EXECUTION_STEP SET result_reference = " +
                            detail::sql_quote(result.at("emitted_results").dump()) +
                            " WHERE step_index = " + std::to_string(index));
            executions.push_back({{"operation_id", operation.id},
                                  {"operation", operation.operation},
                                  {"inputs", inputs}, {"result", result}});
            if (result.contains("emitted_results") && result.at("emitted_results").is_object())
                for (const auto &[output_port, artifact] : result.at("emitted_results").items())
                    current_artifacts[operation.id][output_port] = artifact.value("artifact_id", "");
            for (const auto &target : outgoing[operation_id])
                if (--indegree[target] == 0) ready.push_back(target);
            } catch (const std::exception &error) {
                execute_sql("UPDATE WORKFLOW_EXECUTION_STEP SET status = 'failed', error_message = " +
                            detail::sql_quote(error.what()) + ", updated_at = CURRENT_TIMESTAMP WHERE step_index = " +
                            std::to_string(index));
                throw;
            } catch (...) {
                execute_sql("UPDATE WORKFLOW_EXECUTION_STEP SET status = 'failed', error_message = 'unknown workflow step failure', updated_at = CURRENT_TIMESTAMP WHERE step_index = " +
                            std::to_string(index));
                throw;
            }
        }
        if (executions.size() != workflow.operations.size())
            throw Error(ErrorCode::WorkflowValidation,
                        "Workflow operation graph could not be scheduled");
        return {{"status", "completed"}, {"operations", executions}};
    }

    Json Project::run_worker(const std::string &worker_id, const OperationRegistry &registry)
    {
        WorkflowExecutionManager manager(*this);
        manager.scheduler_tick(worker_id);
        try
        {
            run_operation_graph(registry);
            manager.release_worker(worker_id, ExecutionState::completed);
        }
        catch (const std::exception &error)
        {
            try
            {
                execute_sql("UPDATE WORKFLOW_EXECUTION SET error = " + detail::sql_quote(error.what()) + ", updated_at = CURRENT_TIMESTAMP");
                manager.release_worker(worker_id, ExecutionState::failed);
            }
            catch (...)
            {
            }
            throw;
        }
        return manager.current();
    }

    Json Project::run_operation(const std::string &operation_id, const Json &parameters,
                                const OperationRegistry &registry,
                                const std::string &operation_instance,
                                const Json &provided_inputs)
    {
        const Operation *operation = registry.find(operation_id);
        if (!operation)
            throw Error(ErrorCode::InvalidArgument, "Unknown operation: " + operation_id);
        const auto instance = operation_instance.empty() ? operation_id : operation_instance;
        const auto workflow = get_workflow();
        const auto workflow_operation = std::find_if(workflow.operations.begin(), workflow.operations.end(),
                                                      [&](const auto &candidate) { return candidate.id == instance; });
        if (workflow_operation != workflow.operations.end() || operation->definition().input_ports.empty() || !provided_inputs.is_null())
        {
            std::unique_lock execution_lock(impl_->workflow_execution_mutex);
            std::optional<detail::WorkflowFileLock> process_lock;
            try
            {
                process_lock.emplace(impl_->options.database_path);
                if (!process_lock->healthy())
                    throw Error(ErrorCode::MethodExecution, "Workflow execution lock was lost");
                auto inputs = resolve_workflow_inputs(instance);
                if (!provided_inputs.is_null() && provided_inputs.is_object())
                    for (auto it = provided_inputs.begin(); it != provided_inputs.end(); ++it)
                        inputs[it.key()] = it.value();
                Json effective_parameters = parameters;
                for (auto it = inputs.begin(); it != inputs.end(); ++it) {
                    if (it.key().rfind("parameter:", 0) != 0) continue;
                    effective_parameters[it.key().substr(std::string("parameter:").size())] = it.value();
                }
                const auto resolved_parameters = operation->resolve_parameters(effective_parameters);
                const auto cache_key = fingerprint::operation(operation_id, instance, operation->definition().version,
                                                               resolved_parameters, inputs);
                const auto text_value = [](const Json &value, const char *key) {
                    const auto item = value.find(key);
                    return item != value.end() && item->is_string() ? item->get<std::string>() : std::string{};
                };
                const auto cached = query_json(
                    "SELECT output_port_id, inventory.artifact_id, CAST(inventory.payload AS VARCHAR) AS payload "
                    "FROM ARTIFACT_CACHE_OUTPUT AS cached "
                    "JOIN ARTIFACT_CACHE AS cache ON cache.fingerprint = cached.fingerprint "
                    "JOIN ARTIFACT_INVENTORY AS inventory ON inventory.artifact_id = cached.artifact_id "
                    "WHERE cached.fingerprint = " + detail::sql_quote(cache_key) +
                    " AND cache.status = 'complete' AND inventory.status = 'published' "
                    " AND NOT EXISTS (SELECT 1 FROM ARTIFACT_INVENTORY AS newer "
                    "WHERE newer.producer_instance = inventory.producer_instance "
                    "AND newer.contract_id = inventory.contract_id AND newer.status = 'published' "
                    "AND (CAST(newer.workflow_revision AS INTEGER) > CAST(inventory.workflow_revision AS INTEGER) "
                    "OR (CAST(newer.workflow_revision AS INTEGER) = CAST(inventory.workflow_revision AS INTEGER) "
                    "AND newer.created_at > inventory.created_at))) "
                    " ORDER BY cached.output_port_id, cached.ordinal");
                bool complete = !cached.empty();
                for (const auto &port : operation->definition().output_ports) {
                    if (port.optional) continue;
                    const auto present = std::any_of(cached.begin(), cached.end(), [&](const Json &item) {
                        const auto output_port = text_value(item, "output_port_id");
                        return output_port == port.id || output_port == port.semantic_contract;
                    });
                    if (!present) { complete = false; break; }
                }
                if (complete) {
                    Json reused = Json::object();
                    for (const auto &item : cached) {
                        Json reference = {{"artifact_id", text_value(item, "artifact_id")}};
                        const auto payload = text_value(item, "payload");
                        if (!payload.empty() && payload != "null") reference["payload"] = parse_json(payload, "cached artifact payload");
                        reused[text_value(item, "output_port_id")] = std::move(reference);
                    }
                    execute_sql("UPDATE ARTIFACT_CACHE SET last_used_at = CURRENT_TIMESTAMP WHERE fingerprint = " + detail::sql_quote(cache_key));
                    log_operation("operation.cache_reused (" + operation_id + "): Reused cached artifacts; operation execution skipped.");
                    return Json{{"emitted_results", reused}, {"cache_hit", true}};
                }
                const auto result = operation->run_workflow(*this, effective_parameters, instance, inputs);
                if (!process_lock->healthy())
                    throw Error(ErrorCode::MethodExecution, "Workflow execution lock was lost");
                const auto inventory = get_artifact_inventory();
                Json published = Json::object();
                bool complete_outputs = true;
                for (const auto &port : operation->definition().output_ports) {
                    if (port.optional) continue;
                    auto artifact = inventory.end();
                    for (auto candidate = inventory.begin(); candidate != inventory.end(); ++candidate) {
                        if (text_value(*candidate, "producer_instance") != instance ||
                            text_value(*candidate, "contract_id") != port.semantic_contract ||
                            text_value(*candidate, "status") != "published" ||
                            text_value(*candidate, "artifact_id").empty())
                            continue;
                        if (artifact == inventory.end()) {
                            artifact = candidate;
                            continue;
                        }
                        const auto candidate_revision = std::stoi(text_value(*candidate, "workflow_revision").empty() ? "-1" : text_value(*candidate, "workflow_revision"));
                        const auto selected_revision = std::stoi(text_value(*artifact, "workflow_revision").empty() ? "-1" : text_value(*artifact, "workflow_revision"));
                        const auto candidate_created = text_value(*candidate, "created_at");
                        const auto selected_created = text_value(*artifact, "created_at");
                        if (candidate_revision > selected_revision ||
                            (candidate_revision == selected_revision &&
                             (candidate_created > selected_created ||
                              (candidate_created == selected_created &&
                               text_value(*candidate, "artifact_id") > text_value(*artifact, "artifact_id")))))
                            artifact = candidate;
                    }
                    if (artifact == inventory.end()) { complete_outputs = false; break; }
                    published[port.id] = text_value(*artifact, "artifact_id");
                }
                if (complete_outputs)
                    execute_sql("INSERT OR REPLACE INTO ARTIFACT_CACHE (fingerprint, operation_id, status, last_used_at) VALUES (" +
                                detail::sql_quote(cache_key) + ", " + detail::sql_quote(operation_id) + ", 'complete', CURRENT_TIMESTAMP)");
                else {
                    execute_sql("DELETE FROM ARTIFACT_CACHE_OUTPUT WHERE fingerprint = " + detail::sql_quote(cache_key));
                    execute_sql("DELETE FROM ARTIFACT_CACHE WHERE fingerprint = " + detail::sql_quote(cache_key));
                }
                int ordinal = 0;
                if (complete_outputs)
                    for (const auto &[output_port, artifact_id] : published.items())
                        execute_sql("INSERT OR REPLACE INTO ARTIFACT_CACHE_OUTPUT (fingerprint, output_port_id, ordinal, artifact_id) VALUES (" +
                                    detail::sql_quote(cache_key) + ", " + detail::sql_quote(output_port) + ", " +
                                    std::to_string(ordinal++) + ", " + detail::sql_quote(artifact_id.get<std::string>()) + ")");
                return result;
            }
            catch (const Error &error)
            {
                if (error.code() != ErrorCode::WorkflowValidation)
                    throw;
                process_lock.reset();
                execution_lock.unlock();
                const auto graph_result = run_operation_graph(registry);
                for (const auto &execution : graph_result.value("operations", Json::array()))
                    if (execution.value("operation_id", std::string{}) == instance)
                        return execution.value("result", Json::object());
                throw Error(ErrorCode::WorkflowValidation, "Workflow did not execute operation: " + instance);
            }
        }
        Json inputs = provided_inputs;
        if (inputs.is_null()) {
            if (operation->definition().input_ports.empty())
                inputs = Json::object();
            else
                inputs = resolve_workflow_inputs(instance);
        }
        std::lock_guard execution_lock(impl_->workflow_execution_mutex);
        detail::WorkflowFileLock process_lock(impl_->options.database_path);
        if (!process_lock.healthy())
            throw Error(ErrorCode::MethodExecution, "Workflow execution lock was lost");
        const Json result = operation->run_workflow(*this, parameters, instance, inputs);
        if (!process_lock.healthy())
            throw Error(ErrorCode::MethodExecution, "Workflow execution lock was lost");
        std::lock_guard lock(impl_->mutex);
        ensure_active(*impl_);
        Connection connection(*impl_);
        audit(connection.get(), "complete", "operation", Json{{"operation", operation_id}});
        return result;
    }

    void Project::set_operation_log_callback(OperationLogCallback callback)
    {
        std::lock_guard lock(impl_->mutex);
        impl_->operation_log_callback = std::move(callback);
    }

    void Project::set_cancellation_flag(std::atomic_bool *flag) noexcept
    {
        std::lock_guard lock(impl_->mutex);
        impl_->cancellation_flag = flag;
    }

    bool Project::cancellation_requested() const noexcept
    {
        return impl_->cancellation_flag != nullptr && impl_->cancellation_flag->load();
    }

    void Project::log_operation(std::string_view message) const
    {
        OperationLogCallback callback;
        {
            std::lock_guard lock(impl_->mutex);
            callback = impl_->operation_log_callback;
        }
        if (callback) callback(message);
    }

    void Project::close() noexcept
    {
        if (impl_)
        {
            std::lock_guard lock(impl_->mutex);
            impl_->closed = true;
        }
    }

    namespace detail
    {
        const char *execution_state_name(ExecutionState state)
        {
            switch (state)
            {
            case ExecutionState::queued:
                return "queued";
            case ExecutionState::running:
                return "running";
            case ExecutionState::cancelling:
                return "cancelling";
            case ExecutionState::cancelled:
                return "cancelled";
            case ExecutionState::completed:
                return "completed";
            case ExecutionState::failed:
                return "failed";
            case ExecutionState::interrupted:
                return "interrupted";
            }
            throw Error(ErrorCode::InvalidArgument, "unknown execution state");
        }

        ExecutionState execution_state(const std::string &value)
        {
            if (value == "queued")
                return ExecutionState::queued;
            if (value == "running")
                return ExecutionState::running;
            if (value == "cancelling")
                return ExecutionState::cancelling;
            if (value == "cancelled")
                return ExecutionState::cancelled;
            if (value == "completed")
                return ExecutionState::completed;
            if (value == "failed")
                return ExecutionState::failed;
            if (value == "interrupted")
                return ExecutionState::interrupted;
            throw Error(ErrorCode::InvalidArgument, "unknown execution state: " + value);
        }
    }

    WorkflowExecutionManager::WorkflowExecutionManager(Project &project) noexcept : project_(&project) {}

    Json WorkflowExecutionManager::create(const Json &request)
    {
        const auto existing = project_->query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1");
        if (!existing.empty())
        {
            const auto status = existing.at(0).value("status", "");
            if (status == "queued" || status == "running" || status == "cancelling")
            {
                throw Error(ErrorCode::InvalidArgument,
                            "an active workflow execution already owns this project");
            }
        }
        const auto revision = request.value("workflow_revision", 0);
        const auto progress = request.value("progress", Json::object());
        const std::string columns = "workflow_revision, launch_snapshot, status, progress";
        const std::string values = std::to_string(revision) + ", " + detail::sql_quote(project_->get_workflow().to_json().dump()) + ", 'queued', " + detail::sql_quote(progress.dump());
        project_->execute_sql("DELETE FROM WORKFLOW_EXECUTION");
        project_->execute_sql("INSERT INTO WORKFLOW_EXECUTION (" + columns + ") VALUES (" + values + ")");
        return current();
    }

    Json WorkflowExecutionManager::current() const
    {
        auto rows = project_->query_json("SELECT workflow_revision, status, progress, result_reference, error FROM WORKFLOW_EXECUTION LIMIT 1");
        if (rows.empty())
            throw Error(ErrorCode::InvalidArgument, "workflow execution not found");
        return rows.at(0);
    }

    Json WorkflowExecutionManager::list() const
    {
        auto rows = project_->query_json("SELECT workflow_revision, status, progress, result_reference, error FROM WORKFLOW_EXECUTION");
        return rows;
    }

    Json WorkflowExecutionManager::transition(ExecutionState state)
    {
        const auto rows = project_->query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1");
        if (rows.empty())
            throw Error(ErrorCode::InvalidArgument, "workflow execution not found");
        const auto row = rows.at(0);
        const auto current_state = detail::execution_state(row.value("status", ""));
        if (!valid_execution_transition(current_state, state))
            throw Error(ErrorCode::InvalidArgument, "invalid execution transition");
        const auto target = detail::execution_state_name(state);
        project_->execute_sql("UPDATE WORKFLOW_EXECUTION SET status = " + detail::sql_quote(target) + ", completed_at = CASE WHEN " + detail::sql_quote(target) + " IN ('completed', 'failed', 'cancelled', 'interrupted') THEN CURRENT_TIMESTAMP ELSE completed_at END, updated_at = CURRENT_TIMESTAMP");
        return current();
    }

    Json WorkflowExecutionManager::cancel()
    {
        const auto rows = project_->query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1");
        if (rows.empty())
            throw Error(ErrorCode::InvalidArgument, "workflow execution not found");
        const auto current_state = detail::execution_state(rows.at(0).at("status").get<std::string>());
        if (current_state == ExecutionState::queued)
            return transition(ExecutionState::cancelled);
        if (current_state == ExecutionState::running)
            return transition(ExecutionState::cancelling);
        throw Error(ErrorCode::InvalidArgument, "execution cannot be cancelled");
    }

    Json WorkflowExecutionManager::scheduler_tick(const std::string &worker_id)
    {
        if (worker_id.empty())
            throw Error(ErrorCode::InvalidArgument, "worker_id must not be empty");
        project_->execute_sql("UPDATE WORKFLOW_EXECUTION SET status = 'running', process_id = " + detail::sql_quote(worker_id) + ", server_id = " + detail::sql_quote(worker_id) + ", started_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE status = 'queued' AND (process_id IS NULL OR process_id = '')");
        const auto rows = project_->query_json("SELECT status, process_id FROM WORKFLOW_EXECUTION LIMIT 1");
        if (rows.empty() || rows.at(0).value("status", "") != "running" || rows.at(0).value("process_id", "") != worker_id)
        {
            throw Error(ErrorCode::InvalidArgument, "stale or conflicting workflow worker claim");
        }
        return current();
    }

    Json WorkflowExecutionManager::release_worker(const std::string &worker_id, ExecutionState state)
    {
        if (worker_id.empty() || (state != ExecutionState::completed && state != ExecutionState::failed && state != ExecutionState::cancelled && state != ExecutionState::interrupted))
        {
            throw Error(ErrorCode::InvalidArgument, "invalid workflow worker release");
        }
        const auto target = detail::execution_state_name(state);
        project_->execute_sql("UPDATE WORKFLOW_EXECUTION SET status = " + detail::sql_quote(target) + ", completed_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE process_id = " + detail::sql_quote(worker_id) + " AND status IN ('running', 'cancelling')");
        const auto rows = project_->query_json("SELECT status, process_id FROM WORKFLOW_EXECUTION LIMIT 1");
        if (rows.empty() || rows.at(0).value("status", "") != target || rows.at(0).value("process_id", "") != worker_id)
        {
            throw Error(ErrorCode::InvalidArgument, "stale workflow worker cannot release execution");
        }
        return current();
    }

    std::size_t WorkflowExecutionManager::recover_interrupted()
    {
        const auto rows = list();
        std::size_t running = 0;
        for (const auto &row : rows)
            if (row.value("status", "") == "running")
                ++running;
        project_->execute_sql("UPDATE WORKFLOW_EXECUTION SET status = 'interrupted', completed_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE status = 'running'");
        return running;
    }

} // namespace streamfind
