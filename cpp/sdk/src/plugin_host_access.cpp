#include "streamfind/sdk/plugin_host_access.hpp"
#include "streamfind/sdk/plugin_data_service.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::sdk {

namespace detail {

struct ReadRows {
    std::vector<std::string> names;
    Json rows = Json::array();
};

streamfind_plugin_status consume_rows(
    void *context, const streamfind_plugin_batch_column *columns,
    uint32_t column_count, uint64_t row_count) {
    auto &result = *static_cast<ReadRows *>(context);
    for (uint64_t row = 0; row < row_count; ++row) {
        Json object = Json::object();
        for (uint32_t column = 0; column < column_count; ++column) {
            const auto &source = columns[column];
            const bool valid = source.validity_bitmap == nullptr ||
                (source.validity_bitmap[row / 8] & (1u << (row % 8))) != 0;
            if (!valid) {
                object[result.names[column]] = nullptr;
                continue;
            }
            switch (source.type) {
            case STREAMFIND_PLUGIN_COLUMN_UTF8:
            case STREAMFIND_PLUGIN_COLUMN_TIMESTAMP:
            case STREAMFIND_PLUGIN_COLUMN_DECIMAL:
            case STREAMFIND_PLUGIN_COLUMN_BINARY: {
                if (source.element_size != sizeof(streamfind_plugin_string_view))
                    return STREAMFIND_PLUGIN_SCHEMA_ERROR;
                const auto *values = static_cast<const streamfind_plugin_string_view *>(source.data);
                object[result.names[column]] = std::string(values[row].data, values[row].size);
                break;
            }
            case STREAMFIND_PLUGIN_COLUMN_INT64:
                if (source.element_size != sizeof(int64_t)) return STREAMFIND_PLUGIN_SCHEMA_ERROR;
                object[result.names[column]] = static_cast<const int64_t *>(source.data)[row];
                break;
            case STREAMFIND_PLUGIN_COLUMN_FLOAT64:
                if (source.element_size != sizeof(double)) return STREAMFIND_PLUGIN_SCHEMA_ERROR;
                object[result.names[column]] = static_cast<const double *>(source.data)[row];
                break;
            case STREAMFIND_PLUGIN_COLUMN_BOOL:
                if (source.element_size != sizeof(uint8_t)) return STREAMFIND_PLUGIN_SCHEMA_ERROR;
                object[result.names[column]] = static_cast<const uint8_t *>(source.data)[row] != 0;
                break;
            default:
                return STREAMFIND_PLUGIN_SCHEMA_ERROR;
            }
        }
        result.rows.push_back(std::move(object));
    }
    return STREAMFIND_PLUGIN_OK;
}


}  // namespace detail

PluginHostAccess::PluginHostAccess(
    const streamfind_plugin_host_api &host, void *execution_context)
    : host_(host), execution_context_(execution_context) {
    if (execution_context_ == nullptr || host_.clear_table == nullptr ||
        host_.update_composite_batch == nullptr || host_.has_table == nullptr ||
        host_.read_batch == nullptr || host_.append_batch == nullptr ||
        host_.delete_batch == nullptr) {
        throw std::invalid_argument("incomplete plugin host API");
    }
}

streamfind_plugin_column_type PluginHostAccess::column_type(
    const std::string &table_name, const std::string &column_name) const {
    const auto &context = *static_cast<const PluginDataServiceContext *>(execution_context_);
    const auto table = context.column_types.find(table_name);
    if (table == context.column_types.end())
        throw std::runtime_error("missing schema metadata for table " + table_name);
    const auto column = table->second.find(column_name);
    if (column == table->second.end())
        throw std::runtime_error("missing schema metadata for column " + table_name + "." + column_name);
    return column->second;
}

Json PluginHostAccess::query(const std::string &sql) {
    const auto select = std::string("SELECT ");
    const auto from = sql.find(" FROM ", select.size());
    const auto order = sql.find(" ORDER BY", from == std::string::npos ? 0 : from + 6);
    if (from == std::string::npos || order == std::string::npos)
        throw std::runtime_error("unsupported plugin query shape");
    const auto table = sql.substr(from + 6, order - (from + 6));
    std::vector<std::string> names;
    std::size_t begin = select.size();
    while (begin < from) {
        const auto comma = sql.find(',', begin);
        const auto end = comma == std::string::npos || comma > from ? from : comma;
        auto name = sql.substr(begin, end - begin);
        if (!name.empty() && name.front() == ' ') name.erase(0, 1);
        if (name.empty()) throw std::runtime_error("empty plugin query column");
        names.push_back(std::move(name));
        begin = end + 1;
    }
    std::vector<streamfind_plugin_batch_column> requested(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto type = column_type(table, names[i]);
        requested[i] = {names[i].data(), static_cast<uint32_t>(names[i].size()),
                        type, 0, nullptr, 0, 0, nullptr};
    }
    detail::ReadRows result;
    result.names = names;
    const auto status = host_.read_batch(
        execution_context_, table.data(), static_cast<uint32_t>(table.size()), requested.data(),
        static_cast<uint32_t>(requested.size()), 0, UINT64_MAX, &detail::consume_rows, &result,
        host_.user_data);
    if (status != STREAMFIND_PLUGIN_OK)
        throw std::runtime_error("plugin read_batch failed with status " +
                                 std::to_string(status));
    for (auto &row : result.rows) {
        for (auto &[name, value] : row.items()) {
            if (!value.is_string() && !value.is_null()) value = value.dump();
        }
    }
    return result.rows;
}

Json PluginHostAccess::read(const std::string &table_name,
                             const std::vector<std::string> &column_names,
                             const std::string &order_by) {
    if (table_name.empty() || column_names.empty() || order_by.empty())
        throw std::invalid_argument("invalid dynamic project read request");
    std::string sql = "SELECT ";
    for (const auto &column : column_names) {
        if (column.empty()) throw std::invalid_argument("empty dynamic project read column");
        if (sql.size() > 7) sql += ",";
        sql += column;
    }
    auto result = query(sql + " FROM " + table_name + " ORDER BY " + order_by);
    for (auto &row : result) {
        for (const auto &name : column_names) {
            auto &value = row[name];
            if (value.is_null() || !value.is_string()) continue;
            const auto type = column_type(table_name, name);
            if (type == STREAMFIND_PLUGIN_COLUMN_INT64) value = std::stoll(value.get<std::string>());
            else if (type == STREAMFIND_PLUGIN_COLUMN_BOOL) value = value.get<std::string>() == "true";
            else if (type == STREAMFIND_PLUGIN_COLUMN_FLOAT64) value = std::stod(value.get<std::string>());
        }
    }
    return result;
}

void PluginHostAccess::clear_table(const std::string &table_name) {
    const auto status = host_.clear_table(
        execution_context_, table_name.data(), static_cast<uint32_t>(table_name.size()),
        host_.user_data);
    if (status != STREAMFIND_PLUGIN_OK)
        throw std::runtime_error("plugin clear_table failed with status " +
                                 std::to_string(status));
}

void PluginHostAccess::append(
    const std::string &table_name, const std::vector<std::string> &column_names,
    const std::vector<std::vector<std::optional<std::string>>> &rows) {
    if (rows.empty() || column_names.empty()) return;
    for (const auto &row : rows)
        if (row.size() != column_names.size()) throw std::invalid_argument("invalid dynamic append row");
    std::vector<std::vector<std::string>> text(column_names.size(), std::vector<std::string>(rows.size()));
    std::vector<std::vector<streamfind_plugin_string_view>> strings(column_names.size());
    std::vector<std::vector<int64_t>> integers(column_names.size());
    std::vector<std::vector<double>> doubles(column_names.size());
    std::vector<std::vector<uint8_t>> booleans(column_names.size());
    std::vector<std::vector<uint8_t>> validity(column_names.size(), std::vector<uint8_t>((rows.size() + 7) / 8));
    std::vector<streamfind_plugin_batch_column> columns(column_names.size());
    for (std::size_t column = 0; column < column_names.size(); ++column) {
        const auto type = column_type(table_name, column_names[column]);
        const auto integer = type == STREAMFIND_PLUGIN_COLUMN_INT64;
        const auto boolean = type == STREAMFIND_PLUGIN_COLUMN_BOOL;
        const auto text_type = type == STREAMFIND_PLUGIN_COLUMN_UTF8 ||
                               type == STREAMFIND_PLUGIN_COLUMN_TIMESTAMP ||
                               type == STREAMFIND_PLUGIN_COLUMN_DECIMAL ||
                               type == STREAMFIND_PLUGIN_COLUMN_BINARY;
        strings[column].resize(rows.size()); integers[column].resize(rows.size());
        doubles[column].resize(rows.size()); booleans[column].resize(rows.size());
        for (std::size_t row = 0; row < rows.size(); ++row) {
            if (!rows[row][column]) continue;
            text[column][row] = *rows[row][column];
            if (text[column][row].empty() && (integer || boolean || !text_type))
                continue;
            validity[column][row / 8] |= static_cast<uint8_t>(1u << (row % 8));
            try {
                if (integer) integers[column][row] = std::stoll(text[column][row]);
                else if (boolean) booleans[column][row] = text[column][row] == "true" || text[column][row] == "1";
                else if (!text_type) doubles[column][row] = std::stod(text[column][row]);
                else strings[column][row] = {text[column][row].data(), static_cast<uint32_t>(text[column][row].size())};
            } catch (const std::exception &error) {
                throw std::invalid_argument("dynamic append conversion failed for " + column_names[column] +
                                            " at row " + std::to_string(row) + ": " + error.what());
            }
        }
        auto &output = columns[column];
        output.name = column_names[column].data(); output.name_size = static_cast<uint32_t>(column_names[column].size());
        output.flags = 0;
        output.row_count = rows.size(); output.validity_bitmap = validity[column].data();
        if (integer) { output.type = type; output.data = integers[column].data(); output.element_size = sizeof(int64_t); }
        else if (boolean) { output.type = type; output.data = booleans[column].data(); output.element_size = sizeof(uint8_t); }
        else if (!text_type) { output.type = type; output.data = doubles[column].data(); output.element_size = sizeof(double); }
        else { output.type = type; output.data = strings[column].data(); output.element_size = sizeof(streamfind_plugin_string_view); }
    }
    const auto status = host_.append_batch(execution_context_, table_name.data(), static_cast<uint32_t>(table_name.size()),
                                           columns.data(), static_cast<uint32_t>(columns.size()), rows.size(), host_.user_data);
    if (status != STREAMFIND_PLUGIN_OK)
        throw std::runtime_error("plugin append_batch failed with status " + std::to_string(status));
}

void PluginHostAccess::emit_table_batch(
    const std::string &output_contract_id,
    const std::vector<streamfind_plugin_batch_column> &columns,
    std::uint64_t row_count) {
    if (output_contract_id.empty() || columns.empty())
        throw std::invalid_argument("invalid output batch");
    if (host_.emit_table_batch == nullptr)
        throw std::runtime_error("workflow output batch sink is unavailable");
    const auto status = host_.emit_table_batch(
        execution_context_, output_contract_id.data(),
        static_cast<uint32_t>(output_contract_id.size()), columns.data(),
        static_cast<uint32_t>(columns.size()), row_count, host_.user_data);
    if (status != STREAMFIND_PLUGIN_OK)
        throw std::runtime_error("workflow output batch emission failed with status " +
                                 std::to_string(status));
}

void PluginHostAccess::emit_table_rows(
    const std::string &output_contract_id,
    const std::vector<std::string> &column_names,
    const std::vector<std::string> &column_types,
    const Json &rows) {
    if (!rows.is_array() || column_names.size() != column_types.size())
        throw std::invalid_argument("invalid workflow table rows");
    std::vector<std::vector<std::string>> strings(column_names.size());
    std::vector<std::vector<int64_t>> integers(column_names.size());
    std::vector<std::vector<double>> reals(column_names.size());
    std::vector<std::vector<uint8_t>> booleans(column_names.size());
    std::vector<std::vector<streamfind_plugin_string_view>> views(column_names.size());
    std::vector<streamfind_plugin_batch_column> columns(column_names.size());
    for (std::size_t column = 0; column < column_names.size(); ++column) {
        for (const auto &row : rows) {
            const auto &value = row.at(column_names[column]);
            if (column_types[column] == "integer") {
                if (value.is_null()) integers[column].push_back(0);
                else if (value.is_number()) integers[column].push_back(value.get<int64_t>());
                else integers[column].push_back(std::stoll(value.get<std::string>()));
            } else if (column_types[column] == "real") {
                if (value.is_null()) reals[column].push_back(0.0);
                else if (value.is_number()) reals[column].push_back(value.get<double>());
                else reals[column].push_back(std::stod(value.get<std::string>()));
            } else if (column_types[column] == "boolean") {
                if (value.is_null()) booleans[column].push_back(0);
                else if (value.is_boolean()) booleans[column].push_back(value.get<bool>() ? 1 : 0);
                else booleans[column].push_back(value.get<std::string>() == "true" ? 1 : 0);
            } else strings[column].push_back(value.is_null() ? std::string{} : value.get<std::string>());
        }
        auto &descriptor = columns[column];
        descriptor.name = column_names[column].data();
        descriptor.name_size = static_cast<uint32_t>(column_names[column].size());
        descriptor.flags = 0;
        descriptor.row_count = rows.size();
        descriptor.validity_bitmap = nullptr;
        if (column_types[column] == "integer") {
            descriptor.type = STREAMFIND_PLUGIN_COLUMN_INT64;
            descriptor.data = integers[column].data();
            descriptor.element_size = sizeof(int64_t);
        } else if (column_types[column] == "real") {
            descriptor.type = STREAMFIND_PLUGIN_COLUMN_FLOAT64;
            descriptor.data = reals[column].data();
            descriptor.element_size = sizeof(double);
        } else if (column_types[column] == "boolean") {
            descriptor.type = STREAMFIND_PLUGIN_COLUMN_BOOL;
            descriptor.data = booleans[column].data();
            descriptor.element_size = sizeof(uint8_t);
        } else {
            views[column].reserve(strings[column].size());
            for (const auto &value : strings[column])
                views[column].push_back({value.data(), static_cast<uint32_t>(value.size())});
            descriptor.type = STREAMFIND_PLUGIN_COLUMN_UTF8;
            descriptor.data = views[column].data();
            descriptor.element_size = sizeof(streamfind_plugin_string_view);
        }
    }
    emit_table_batch(output_contract_id, columns, rows.size());
}

void PluginHostAccess::emit_result(const std::string &output_contract_id,
                                   const std::string &payload) {
    if (output_contract_id.empty())
        throw std::invalid_argument("empty output contract id");
    if (host_.emit_result == nullptr)
        throw std::runtime_error("workflow result sink is unavailable");
    const auto status = host_.emit_result(
        execution_context_, output_contract_id.data(),
        static_cast<uint32_t>(output_contract_id.size()), payload.data(),
        static_cast<std::uint64_t>(payload.size()), host_.user_data);
    if (status != STREAMFIND_PLUGIN_OK)
        throw std::runtime_error("workflow result emission failed with status " +
                                 std::to_string(status));
}

void PluginHostAccess::update_composite(
    const std::string &table_name,
    const std::vector<std::string> &key_columns,
    const std::vector<std::string> &update_columns,
    const std::vector<std::vector<std::optional<std::string>>> &rows) {
    if (table_name.empty() || key_columns.empty() || update_columns.empty() || rows.empty()) return;
    const std::size_t expected = key_columns.size() + update_columns.size();
    std::vector<std::vector<streamfind_plugin_string_view>> values(expected);
    std::vector<streamfind_plugin_batch_column> keys(key_columns.size());
    std::vector<streamfind_plugin_batch_column> updates(update_columns.size());
    for (auto &column : values) column.resize(rows.size());
    for (std::size_t row = 0; row < rows.size(); ++row) {
        if (rows[row].size() != expected) throw std::invalid_argument("invalid composite update row");
        for (std::size_t column = 0; column < expected; ++column) {
            if (!rows[row][column]) throw std::invalid_argument("null composite update value");
            values[column][row] = {rows[row][column]->data(), static_cast<uint32_t>(rows[row][column]->size())};
        }
    }
    for (std::size_t column = 0; column < key_columns.size(); ++column)
        keys[column] = {key_columns[column].data(), static_cast<uint32_t>(key_columns[column].size()), STREAMFIND_PLUGIN_COLUMN_UTF8, 0, values[column].data(), rows.size(), sizeof(streamfind_plugin_string_view), nullptr};
    for (std::size_t column = 0; column < update_columns.size(); ++column) {
        const auto index = key_columns.size() + column;
        updates[column] = {update_columns[column].data(), static_cast<uint32_t>(update_columns[column].size()), STREAMFIND_PLUGIN_COLUMN_UTF8, 0, values[index].data(), rows.size(), sizeof(streamfind_plugin_string_view), nullptr};
    }
    uint64_t affected = 0;
    const auto status = host_.update_composite_batch(execution_context_, table_name.data(), static_cast<uint32_t>(table_name.size()), keys.data(), static_cast<uint32_t>(keys.size()), updates.data(), static_cast<uint32_t>(updates.size()), rows.size(), &affected, host_.user_data);
    if (status != STREAMFIND_PLUGIN_OK) throw std::runtime_error("plugin update_composite_batch failed with status " + std::to_string(status));
}

void PluginHostAccess::delete_rows(
    const std::string &table_name, const std::string &key_column,
    const std::vector<std::vector<std::optional<std::string>>> &rows) {
    if (table_name.empty() || key_column.empty())
        throw std::invalid_argument("invalid dynamic project delete request");
    if (rows.empty()) return;
    std::vector<streamfind_plugin_string_view> keys(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].size() != 1 || !rows[i][0])
            throw std::invalid_argument("invalid dynamic project delete row");
        keys[i] = {rows[i][0]->data(), static_cast<uint32_t>(rows[i][0]->size())};
    }
    const streamfind_plugin_batch_column key_batch = {
        key_column.data(), static_cast<uint32_t>(key_column.size()),
        STREAMFIND_PLUGIN_COLUMN_UTF8, 0, keys.data(), rows.size(),
        sizeof(streamfind_plugin_string_view), nullptr};
    uint64_t affected = 0;
    const auto status = host_.delete_batch(
        execution_context_, table_name.data(), static_cast<uint32_t>(table_name.size()),
        key_column.data(), static_cast<uint32_t>(key_column.size()), &key_batch, rows.size(),
        &affected, host_.user_data);
    if (status != STREAMFIND_PLUGIN_OK)
        throw std::runtime_error("dynamic project delete_batch failed with status " +
                                 std::to_string(status));
}

void PluginHostAccess::require_table(const std::string &table_name) {
    uint8_t exists = 0;
    const auto status = host_.has_table(
        execution_context_, table_name.data(), static_cast<uint32_t>(table_name.size()), &exists,
        host_.user_data);
    if (status != STREAMFIND_PLUGIN_OK || exists == 0)
        throw std::runtime_error("plugin table is unavailable: " + table_name);
}
}  // namespace streamfind::sdk
