#include "streamfind/sdk/plugin_host_access.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
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

bool is_integer_column(std::string_view name) {
    return name == "analysis_index" || name.ends_with("_size") ||
           name == "modality" || name == "polarity" || name == "candidate_rank" ||
           name == "id_level" || name == "shared_fragments";
}

bool is_boolean_column(std::string_view name) {
    return name == "filtered" || name == "filled" || name == "component_is_core" ||
           name == "component_bridge_flag" || name == "has_ion_mobility";
}

bool is_string_column(std::string_view name) {
    static const std::unordered_set<std::string_view> names = {
        "analysis", "name", "formula", "SMILES", "InChI", "InChIKey", "database_id", "file_name", "file_path", "file_dir", "file_extension", "format", "type", "time_stamp", "created_at",
        "blank", "replicate", "feature", "feature_component",
        "feature_group", "adduct", "filter", "eic_rt", "eic_mz", "eic_intensity",
        "eic_baseline", "eic_smoothed", "ms1_mz", "ms1_intensity", "ms2_mz",
        "ms2_intensity", "db_ms2_mz", "db_ms2_intensity", "db_ms2_formula", "db_ms2_smiles",
        "exp_ms2_mz", "exp_ms2_intensity", "annotation_category", "annotation_type", "correction",
        "annotation_parent_feature", "annotation_element", "component_best_partner"};
    return names.find(name) != names.end();
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
        const auto type = detail::is_integer_column(names[i])
            ? STREAMFIND_PLUGIN_COLUMN_INT64
            : detail::is_boolean_column(names[i])
            ? STREAMFIND_PLUGIN_COLUMN_BOOL
            : detail::is_string_column(names[i])
            ? (names[i] == "time_stamp" || names[i] == "created_at"
                ? STREAMFIND_PLUGIN_COLUMN_TIMESTAMP : STREAMFIND_PLUGIN_COLUMN_UTF8)
            : STREAMFIND_PLUGIN_COLUMN_FLOAT64;
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
            if (detail::is_integer_column(name)) value = std::stoll(value.get<std::string>());
            else if (detail::is_boolean_column(name)) value = value.get<std::string>() == "true";
            else if (!detail::is_string_column(name)) value = std::stod(value.get<std::string>());
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
        const auto integer = detail::is_integer_column(column_names[column]);
        const auto boolean = detail::is_boolean_column(column_names[column]);
        strings[column].resize(rows.size()); integers[column].resize(rows.size());
        doubles[column].resize(rows.size()); booleans[column].resize(rows.size());
        for (std::size_t row = 0; row < rows.size(); ++row) {
            if (!rows[row][column]) continue;
            text[column][row] = *rows[row][column];
            if (text[column][row].empty() && (integer || boolean || !detail::is_string_column(column_names[column])))
                continue;
            validity[column][row / 8] |= static_cast<uint8_t>(1u << (row % 8));
            try {
                if (integer) integers[column][row] = std::stoll(text[column][row]);
                else if (boolean) booleans[column][row] = text[column][row] == "true" || text[column][row] == "1";
                else if (!detail::is_string_column(column_names[column])) doubles[column][row] = std::stod(text[column][row]);
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
        if (integer) { output.type = STREAMFIND_PLUGIN_COLUMN_INT64; output.data = integers[column].data(); output.element_size = sizeof(int64_t); }
        else if (boolean) { output.type = STREAMFIND_PLUGIN_COLUMN_BOOL; output.data = booleans[column].data(); output.element_size = sizeof(uint8_t); }
        else if (!detail::is_string_column(column_names[column])) { output.type = STREAMFIND_PLUGIN_COLUMN_FLOAT64; output.data = doubles[column].data(); output.element_size = sizeof(double); }
        else { output.type = STREAMFIND_PLUGIN_COLUMN_UTF8; output.data = strings[column].data(); output.element_size = sizeof(streamfind_plugin_string_view); }
    }
    const auto status = host_.append_batch(execution_context_, table_name.data(), static_cast<uint32_t>(table_name.size()),
                                           columns.data(), static_cast<uint32_t>(columns.size()), rows.size(), host_.user_data);
    if (status != STREAMFIND_PLUGIN_OK)
        throw std::runtime_error("plugin append_batch failed with status " + std::to_string(status));
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
