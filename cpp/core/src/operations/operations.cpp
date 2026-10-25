#include "streamfind/core_operations.hpp"

#include "streamfind/catalogue_binding.hpp"
#include "streamfind/operation_execution_context.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace streamfind::core_operations {
namespace detail {

void validate_paths(const Json &parameters) {
    const auto &paths = parameters.at("paths");
    if (!paths.is_array())
        throw std::invalid_argument("paths must be an array");
    for (const auto &path : paths) {
        if (!path.is_string() || path.get<std::string>().empty())
            throw std::invalid_argument("paths must contain non-empty strings");
    }
}

Json select_paths(OperationExecutionContext &, const Json &parameters,
                  const std::string &operation_instance, const Json &) {
    validate_paths(parameters);
    (void)operation_instance;
    return parameters.at("paths");
}

std::string quote_identifier(const std::string &value) {
    std::string quoted = "\"";
    for (const char character : value)
        quoted += character == '\"' ? "\"\"" : std::string(1, character);
    return quoted + "\"";
}

std::string quote_sql_string(const std::string &value) {
    std::string quoted = "'";
    for (const char character : value)
        quoted += character == '\'' ? "''" : std::string(1, character);
    return quoted + "'";
}

void validate_csv_path(const Json &parameters) {
    const auto path = parameters.at("csv_path").get<std::string>();
    if (path.empty())
        throw std::invalid_argument("csvPath must not be empty");
}

void validate_csv_input_path(const Json &parameters) {
    const auto path = parameters.at("csv_input_path").get<std::string>();
    if (path.empty())
        throw std::invalid_argument("csv_input_path must not be empty");
    const std::filesystem::path file(path);
    if (!std::filesystem::exists(file) || !std::filesystem::is_regular_file(file))
        throw std::invalid_argument("csv_input_path must identify an existing file");
}

void validate_json_path(const Json &parameters) {
    const auto path = parameters.at("json_path").get<std::string>();
    if (path.empty())
        throw std::invalid_argument("json_path must not be empty");
}

Json runtime_column_definition(const streamfind::RuntimeTableColumn &column);

Json write_table_csv(OperationExecutionContext &context, const Json &parameters,
                     const std::string &operation_instance, const Json &inputs) {
    validate_csv_path(parameters);
    const auto input = inputs.find("anyTable");
    if (input == inputs.end() || !input->is_object())
        throw std::invalid_argument("Connect a table input.");
    const auto physical_table = input->value("physical_table", std::string{});
    if (physical_table.empty())
        throw std::invalid_argument("The connected table artifact has no physical table.");

    const auto csv_path = parameters.at("csv_path").get<std::string>();
    const auto sql = "COPY (SELECT * FROM " + quote_identifier(physical_table) + ") TO " +
                     quote_sql_string(csv_path) + " (HEADER, DELIMITER ',')";
    context.query(sql);
    (void)operation_instance;
    return Json{{"status", "finished"}, {"csvPath", csv_path}};
}

Json write_json(OperationExecutionContext &, const Json &parameters,
                const std::string &operation_instance, const Json &inputs) {
    validate_json_path(parameters);
    const auto input = inputs.find("anyJson");
    if (input == inputs.end() || !input->is_object())
        throw std::invalid_argument("Connect a JSON input.");
    const auto payload = input->find("payload");
    if (payload == input->end() || payload->is_null())
        throw std::invalid_argument("The connected JSON artifact has no payload.");
    Json value = *payload;
    if (value.is_string()) {
        try { value = Json::parse(value.get<std::string>()); }
        catch (const std::exception &error) {
            throw std::invalid_argument(std::string("The connected JSON artifact is invalid: ") + error.what());
        }
    }
    std::ofstream output(parameters.at("json_path").get<std::string>(), std::ios::out | std::ios::trunc);
    if (!output)
        throw std::invalid_argument("json_path is not writable");
    output << value.dump(2) << '\n';
    if (!output)
        throw std::runtime_error("Failed while writing JSON output");
    (void)operation_instance;
    return Json{{"status", "finished"}, {"jsonPath", parameters.at("json_path")}};
}

Json read_csv(OperationExecutionContext &context, const Json &parameters,
              const std::string &instance, const Json &) {
    validate_csv_input_path(parameters);
    const auto csv_path = parameters.at("csv_input_path").get<std::string>();
    const auto source = "read_csv_auto(" + quote_sql_string(csv_path) + ", HEADER = true, AUTO_DETECT = true)";
    const auto description = context.query("DESCRIBE SELECT * FROM " + source);
    if (description.empty())
        throw std::invalid_argument("CSV input has no columns");
    Json columns = Json::array();
    for (const auto &column : description) {
        const auto name = column.value("column_name", std::string{});
        const auto type = column.value("column_type", std::string{});
        if (name.empty() || type.empty())
            throw std::invalid_argument("CSV input returned an invalid runtime schema");
        columns.push_back(runtime_column_definition(
            streamfind::RuntimeTableColumn{name, type, streamfind::ParameterType::string, true, Json::object()}));
    }
    std::string target_columns;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index != 0) target_columns += ", ";
        target_columns += quote_identifier(columns.at(index).at("name").get<std::string>());
    }
    const auto rows = context.query("SELECT " + target_columns + " FROM " + source);
    const auto descriptor = Json{{"contract_id", "anyTable"}, {"kind", "table"},
                                 {"format", "arrow-ipc-batches"}, {"schema", columns},
                                 {"producer_operation", "streamfind.read_csv"},
                                 {"producer_instance", instance},
                                 {"workflow_revision", context.workflow_revision()}};
    const auto allocation = context.outputs().begin(descriptor);
    const auto handle = allocation.value("output_handle", std::string{});
    if (handle.empty()) throw std::runtime_error("runtime did not allocate CSV output");
    try {
        context.outputs().write_rows(handle, 0, rows);
        const auto reference = context.outputs().finish(handle);
        return {{"status", "finished"}, {"emitted_results", {{"anyTable", reference}}}};
    } catch (...) {
        context.outputs().abort(handle, "CSV output failed");
        throw;
    }
}

const Json &required_input(const Json &inputs, const char *port) {
    const auto found = inputs.find(port);
    if (found == inputs.end() || !found->is_object() || !found->contains("physical_table"))
        throw std::invalid_argument(std::string("Connect a table input: ") + port + ".");
    if (found->at("physical_table").get<std::string>().empty())
        throw std::invalid_argument(std::string("The connected table input has no physical table: ") + port + ".");
    return *found;
}

std::string upper_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return value;
}

void require_matching_schema(const streamfind::DynamicTableContract &left,
                             const streamfind::DynamicTableContract &right) {
    if (left.columns().size() != right.columns().size())
        throw std::invalid_argument("Tables must have exactly the same number of columns.");
    for (std::size_t index = 0; index < left.columns().size(); ++index) {
        const auto &left_column = left.columns().at(index);
        const auto &right_column = right.columns().at(index);
        if (left_column.name != right_column.name ||
            upper_copy(left_column.duckdb_type) != upper_copy(right_column.duckdb_type))
            throw std::invalid_argument("Tables must have identical column names, order, and DuckDB types.");
    }
}

std::vector<std::string> row_keys(const Json &parameters) {
    const auto value = parameters.value("row_key_columns", Json::array());
    if (!value.is_array() || value.empty())
        throw std::invalid_argument("row_key_columns must contain at least one column.");
    std::vector<std::string> keys;
    std::set<std::string> unique;
    for (const auto &item : value) {
        if (!item.is_string() || item.get<std::string>().empty() || !unique.insert(item.get<std::string>()).second)
            throw std::invalid_argument("row_key_columns must contain unique, non-empty column names.");
        keys.push_back(item.get<std::string>());
    }
    return keys;
}

const streamfind::RuntimeTableColumn &find_column(
    const streamfind::DynamicTableContract &table, const std::string &name) {
    const auto found = std::find_if(table.columns().begin(), table.columns().end(),
        [&](const auto &column) { return column.name == name; });
    if (found == table.columns().end())
        throw std::invalid_argument("Row key column is missing: " + name);
    return *found;
}

void require_unique_matching_keys(OperationExecutionContext &context, const std::string &left_name,
                                  const std::string &right_name,
                                  const std::vector<std::string> &keys) {
    std::string key_list;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (index != 0) key_list += ", ";
        key_list += quote_identifier(keys.at(index));
    }
    for (const auto &table : {left_name, right_name}) {
        const auto duplicate = context.query(
            "SELECT COUNT(*) AS duplicate_groups FROM (SELECT " + key_list + " FROM " +
            quote_identifier(table) + " GROUP BY " + key_list + " HAVING COUNT(*) > 1) duplicates");
        if (!duplicate.empty() && duplicate.at(0).value("duplicate_groups", std::string{"0"}) != "0")
            throw std::invalid_argument("Row key columns must be unique in both tables.");
    }
    const auto left_only = context.query(
        "SELECT COUNT(*) AS mismatch FROM (SELECT " + key_list + " FROM " + quote_identifier(left_name) +
        " EXCEPT SELECT " + key_list + " FROM " + quote_identifier(right_name) + ") differences");
    const auto right_only = context.query(
        "SELECT COUNT(*) AS mismatch FROM (SELECT " + key_list + " FROM " + quote_identifier(right_name) +
        " EXCEPT SELECT " + key_list + " FROM " + quote_identifier(left_name) + ") differences");
    if ((!left_only.empty() && left_only.at(0).value("mismatch", std::string{"0"}) != "0") ||
        (!right_only.empty() && right_only.at(0).value("mismatch", std::string{"0"}) != "0"))
        throw std::invalid_argument("Row key sets must match exactly between both tables.");
}

DynamicTableContract inspect_dynamic_table_contract(OperationExecutionContext &context,
                                                     const std::string &table_name) {
    const auto description = context.query("DESCRIBE " + quote_identifier(table_name));
    std::vector<RuntimeTableColumn> columns;
    for (const auto &column : description) {
        const auto name = column.value("column_name", std::string{});
        const auto type = column.value("column_type", std::string{});
        if (name.empty() || type.empty()) throw std::invalid_argument("table schema is invalid");
        columns.push_back({name, type, ParameterType::string, true, Json::object()});
    }
    return DynamicTableContract("anyTable", table_name, std::move(columns));
}

Json runtime_column_definition(const streamfind::RuntimeTableColumn &column) {
    const auto type = upper_copy(column.duckdb_type);
    std::string semantic_type = "string";
    if (type.find("BOOL") != std::string::npos) semantic_type = "boolean";
    else if (type.find("INT") != std::string::npos || type.find("DECIMAL") != std::string::npos) semantic_type = "integer";
    else if (type.find("DOUBLE") != std::string::npos || type.find("FLOAT") != std::string::npos || type.find("REAL") != std::string::npos) semantic_type = "real";
    else if (type.find("TIMESTAMP") != std::string::npos) semantic_type = "timestamp";
    else if (type.find("BLOB") != std::string::npos) semantic_type = "binary";
    else if (type.find("LIST") != std::string::npos || type.find("ARRAY") != std::string::npos) semantic_type = "array";
    else if (type.find("JSON") != std::string::npos || type.find("STRUCT") != std::string::npos || type.find("MAP") != std::string::npos) semantic_type = "object";
    return {{"name", column.name}, {"type", semantic_type}};
}

Json bind_table_rows(OperationExecutionContext &context, const Json &, const std::string &instance, const Json &inputs) {
    const auto &left_input = required_input(inputs, "leftTable");
    const auto &right_input = required_input(inputs, "rightTable");
    const auto left_name = left_input.at("physical_table").get<std::string>();
    const auto right_name = right_input.at("physical_table").get<std::string>();
    const auto left = inspect_dynamic_table_contract(context, left_name);
    const auto right = inspect_dynamic_table_contract(context, right_name);
    require_matching_schema(left, right);
    Json columns = Json::array();
    for (const auto &column : left.columns()) columns.push_back(runtime_column_definition(column));
    std::string select_columns;
    for (std::size_t index = 0; index < left.columns().size(); ++index) {
        if (index != 0) select_columns += ", ";
        select_columns += quote_identifier(left.columns().at(index).name);
    }
    const auto rows = context.query("SELECT " + select_columns + " FROM " + quote_identifier(left_name) +
                                   " UNION ALL SELECT " + select_columns + " FROM " + quote_identifier(right_name));
    const auto descriptor = Json{{"contract_id", "anyTable"}, {"kind", "table"}, {"format", "arrow-ipc-batches"},
                                 {"schema", columns}, {"producer_operation", "streamfind.bind_table_rows"},
                                 {"producer_instance", instance}, {"workflow_revision", context.workflow_revision()},
                                 {"provenance", { {"lineage_inputs", {
                                     {{"result_id", left_input.value("result_id", left_input.value("result_id", "")), "source_port", "leftTable", "target_port", "leftTable"}},
                                     {{"result_id", right_input.value("result_id", right_input.value("result_id", "")), "source_port", "rightTable", "target_port", "rightTable"}}
                                 }} }}};
    const auto allocation = context.outputs().begin(descriptor);
    const auto handle = allocation.value("output_handle", std::string{});
    try { context.outputs().write_rows(handle, 0, rows); return {{"status", "finished"}, {"emitted_results", {{"anyTable", context.outputs().finish(handle)}}}}; }
    catch (...) { context.outputs().abort(handle, "row binding failed"); throw; }
}

Json bind_table_columns(OperationExecutionContext &context, const Json &parameters, const std::string &instance, const Json &inputs) {
    const auto &left_input = required_input(inputs, "leftTable");
    const auto &right_input = required_input(inputs, "rightTable");
    const auto left_name = left_input.at("physical_table").get<std::string>();
    const auto right_name = right_input.at("physical_table").get<std::string>();
    const auto left = inspect_dynamic_table_contract(context, left_name);
    const auto right = inspect_dynamic_table_contract(context, right_name);
    const auto keys = row_keys(parameters);
    for (const auto &key : keys) {
        const auto &left_column = find_column(left, key);
        const auto &right_column = find_column(right, key);
        if (upper_copy(left_column.duckdb_type) != upper_copy(right_column.duckdb_type))
            throw std::invalid_argument("Row key columns must have matching DuckDB types: " + key);
    }
    require_unique_matching_keys(context, left_name, right_name, keys);
    std::set<std::string> left_names;
    for (const auto &column : left.columns()) left_names.insert(column.name);
    Json columns = Json::array();
    for (const auto &column : left.columns()) columns.push_back(runtime_column_definition(column));
    std::string right_select;
    std::string join;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (index != 0) join += " AND ";
        join += "l." + quote_identifier(keys.at(index)) + " = r." + quote_identifier(keys.at(index));
    }
    for (const auto &column : right.columns()) {
        if (left_names.contains(column.name)) {
            if (std::find(keys.begin(), keys.end(), column.name) == keys.end())
                throw std::invalid_argument("Non-key column exists in both tables: " + column.name);
            continue;
        }
        columns.push_back(runtime_column_definition(column));
        if (!right_select.empty()) right_select += ", ";
        right_select += "r." + quote_identifier(column.name);
    }
    std::string left_select;
    for (const auto &column : left.columns()) {
        if (!left_select.empty()) left_select += ", ";
        left_select += "l." + quote_identifier(column.name);
    }
    if (right_select.empty()) throw std::invalid_argument("The right table has no non-key columns to bind.");
    const auto rows = context.query("SELECT " + left_select + ", " + right_select +
                                   " FROM " + quote_identifier(left_name) + " l INNER JOIN " +
                                   quote_identifier(right_name) + " r ON " + join);
    const auto descriptor = Json{{"contract_id", "anyTable"}, {"kind", "table"}, {"format", "arrow-ipc-batches"},
                                 {"schema", columns}, {"producer_operation", "streamfind.bind_table_columns"},
                                 {"producer_instance", instance}, {"workflow_revision", context.workflow_revision()},
                                 {"provenance", { {"lineage_inputs", {
                                     {{"result_id", left_input.value("result_id", left_input.value("result_id", "")), "source_port", "leftTable", "target_port", "leftTable"}},
                                     {{"result_id", right_input.value("result_id", right_input.value("result_id", "")), "source_port", "rightTable", "target_port", "rightTable"}}
                                 }} }}};
    const auto allocation = context.outputs().begin(descriptor);
    const auto handle = allocation.value("output_handle", std::string{});
    try { context.outputs().write_rows(handle, 0, rows); return {{"status", "finished"}, {"emitted_results", {{"anyTable", context.outputs().finish(handle)}}}}; }
    catch (...) { context.outputs().abort(handle, "column binding failed"); throw; }
}

}  // namespace detail

void register_operations(const Json &entries, OperationRegistry &operations) {
    for (const auto &entry : entries) {
        if (entry.value("kind", "") != "operation") continue;
        const auto canonical_id = entry.value("canonical_id", "");
        if (canonical_id == "streamfind.select_paths") {
            auto definition = catalogue::operation_definition(entry);
            operations.register_operation(
                Operation(std::move(definition), &detail::select_paths, &detail::validate_paths));
        } else if (canonical_id == "streamfind.write_table_csv") {
            auto definition = catalogue::operation_definition(entry);
            operations.register_operation(
                Operation(std::move(definition), &detail::write_table_csv, &detail::validate_csv_path));
        } else if (canonical_id == "streamfind.write_json") {
            auto definition = catalogue::operation_definition(entry);
            operations.register_operation(
                Operation(std::move(definition), &detail::write_json, &detail::validate_json_path));
        } else if (canonical_id == "streamfind.read_csv") {
            auto definition = catalogue::operation_definition(entry);
            operations.register_operation(
                Operation(std::move(definition), &detail::read_csv, &detail::validate_csv_input_path));
        } else if (canonical_id == "streamfind.bind_table_rows") {
            auto definition = catalogue::operation_definition(entry);
            operations.register_operation(
                Operation(std::move(definition), &detail::bind_table_rows));
        } else if (canonical_id == "streamfind.bind_table_columns") {
            auto definition = catalogue::operation_definition(entry);
            operations.register_operation(
                Operation(std::move(definition), &detail::bind_table_columns));
        }
    }
    if (!operations.find("streamfind.select_paths"))
        throw std::invalid_argument("catalogue: core operation is missing streamfind.select_paths");
    if (!operations.find("streamfind.write_table_csv"))
        throw std::invalid_argument("catalogue: core operation is missing streamfind.write_table_csv");
    if (!operations.find("streamfind.write_json"))
        throw std::invalid_argument("catalogue: core operation is missing streamfind.write_json");
    if (!operations.find("streamfind.read_csv"))
        throw std::invalid_argument("catalogue: core operation is missing streamfind.read_csv");
    if (!operations.find("streamfind.bind_table_rows"))
        throw std::invalid_argument("catalogue: core operation is missing streamfind.bind_table_rows");
    if (!operations.find("streamfind.bind_table_columns"))
        throw std::invalid_argument("catalogue: core operation is missing streamfind.bind_table_columns");
}

}  // namespace streamfind::core_operations
