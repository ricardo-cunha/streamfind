#include "streamfind/sdk/plugin_data_service.hpp"

#include <cmath>
#include <cctype>
#include <limits>

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace streamfind::sdk::detail {

PluginDataServiceContext *context_from(void *execution_context) {
    return static_cast<PluginDataServiceContext *>(execution_context);
}

std::optional<std::string> column_value(const streamfind_plugin_batch_column &column, uint64_t row) {
    if (column.validity_bitmap != nullptr &&
        (column.validity_bitmap[row / 8] & static_cast<uint8_t>(1u << (row % 8))) == 0) {
        return std::nullopt;
    }
    if (column.data == nullptr || column.element_size == 0) {
        throw std::invalid_argument("batch column has no data");
    }
    switch (column.type) {
    case STREAMFIND_PLUGIN_COLUMN_UTF8:
    case STREAMFIND_PLUGIN_COLUMN_TIMESTAMP:
    case STREAMFIND_PLUGIN_COLUMN_DECIMAL:
    case STREAMFIND_PLUGIN_COLUMN_BINARY: {
        if (column.element_size != sizeof(streamfind_plugin_string_view)) {
            throw std::invalid_argument("UTF-8 column has invalid element size");
        }
        const auto *values = static_cast<const streamfind_plugin_string_view *>(column.data);
        if (values[row].data == nullptr && values[row].size != 0)
            throw std::invalid_argument("UTF-8 column contains a null value pointer");
        const std::string value(values[row].data == nullptr ? "" : values[row].data, values[row].size);
        if ((column.flags & STREAMFIND_PLUGIN_COLUMN_FLAG_ARRAY) != 0) {
            const auto parsed = Json::parse(value, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array())
                throw std::invalid_argument("array column value is not a JSON array");
        }
        return value;
    }
    case STREAMFIND_PLUGIN_COLUMN_INT64: {
        if (column.element_size != sizeof(int64_t)) {
            throw std::invalid_argument("INT64 column has invalid element size");
        }
        return std::to_string(static_cast<const int64_t *>(column.data)[row]);
    }
    case STREAMFIND_PLUGIN_COLUMN_FLOAT64: {
        if (column.element_size != sizeof(double)) {
            throw std::invalid_argument("FLOAT64 column has invalid element size");
        }
        return std::to_string(static_cast<const double *>(column.data)[row]);
    }
    case STREAMFIND_PLUGIN_COLUMN_BOOL: {
        if (column.element_size != sizeof(uint8_t)) {
            throw std::invalid_argument("BOOL column has invalid element size");
        }
        return static_cast<const uint8_t *>(column.data)[row] != 0 ? "true" : "false";
    }
    default:
        throw std::invalid_argument("batch column type is unsupported");
    }
}

std::string quoted_identifier(const char *value, uint32_t size) {
    if (value == nullptr || size == 0) throw std::invalid_argument("empty identifier");
    std::string name(value, size);
    if (!(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_'))
        throw std::invalid_argument("invalid identifier");
    for (const char character : name)
        if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_'))
            throw std::invalid_argument("invalid identifier");
    return "\"" + name + "\"";
}

std::string sql_literal(const std::optional<std::string> &value) {
    if (!value) return "NULL";
    std::string escaped = "'";
    for (const char character : *value) escaped += character == '\'' ? "''" : std::string(1, character);
    return escaped + "'";
}

bool allowed_table(const PluginDataServiceContext &context, const std::string &table) {
    return context.allowed_tables.empty() ||
           std::find(context.allowed_tables.begin(), context.allowed_tables.end(), table) != context.allowed_tables.end();
}

bool allowed_column(const PluginDataServiceContext &context, const std::string &table,
                    const std::string &column, bool writing) {
    const auto &scopes = writing ? context.writable_columns : context.readable_columns;
    const auto found = scopes.find(table);
    return found != scopes.end() && found->second.find(column) != found->second.end();
}

}  // namespace streamfind::sdk::detail

namespace streamfind::sdk {

streamfind_plugin_status plugin_has_table(
    void *execution_context,
    const char *table_name,
    uint32_t table_name_size,
    uint8_t *exists,
    void *) {
    if (execution_context == nullptr || table_name == nullptr || exists == nullptr || table_name_size == 0) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    }
    auto *context = detail::context_from(execution_context);
    if (context->tables == nullptr) {
        return STREAMFIND_PLUGIN_ERROR;
    }
    try {
        const std::string table(table_name, table_name_size);
        if (!detail::allowed_table(*context, table)) return STREAMFIND_PLUGIN_NOT_ALLOWED;
        *exists = context->tables->has_table(table) ? 1 : 0;
        return STREAMFIND_PLUGIN_OK;
    } catch (...) {
        return STREAMFIND_PLUGIN_ERROR;
    }
}

streamfind_plugin_status plugin_clear_table(
    void *execution_context, const char *table_name, uint32_t table_name_size, void *) {
    if (execution_context == nullptr || table_name == nullptr || table_name_size == 0)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    if (context->tables == nullptr) return STREAMFIND_PLUGIN_ERROR;
    try {
        const std::string table(table_name, table_name_size);
        if (!detail::allowed_table(*context, table)) return STREAMFIND_PLUGIN_NOT_ALLOWED;
        context->tables->execute("DELETE FROM " + detail::quoted_identifier(table_name, table_name_size));
        return STREAMFIND_PLUGIN_OK;
    } catch (...) {
        return STREAMFIND_PLUGIN_ERROR;
    }
}

streamfind_plugin_status plugin_read_batch(
    void *execution_context,
    const char *table_name,
    uint32_t table_name_size,
    const streamfind_plugin_batch_column *requested_columns,
    uint32_t column_count,
    uint64_t offset,
    uint64_t limit,
    streamfind_plugin_consume_batch_fn consume,
    void *consumer_context,
    void *) {
    if (execution_context == nullptr || table_name == nullptr || table_name_size == 0 ||
        requested_columns == nullptr || column_count == 0 || consume == nullptr)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    if (context->tables == nullptr) return STREAMFIND_PLUGIN_ERROR;
    try {
        const std::string table(table_name, table_name_size);
        if (!detail::allowed_table(*context, table)) return STREAMFIND_PLUGIN_NOT_ALLOWED;
        std::string sql = "SELECT ";
        for (uint32_t index = 0; index < column_count; ++index) {
            const auto &column = requested_columns[index];
            if (column.data != nullptr || column.row_count != 0 || column.validity_bitmap != nullptr)
                return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            if (column.name == nullptr || column.name_size == 0)
                return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            const std::string name(column.name, column.name_size);
            if (!detail::allowed_column(*context, table, name, false)) return STREAMFIND_PLUGIN_NOT_ALLOWED;
            if (index != 0) sql += ", ";
            sql += detail::quoted_identifier(column.name, column.name_size);
        }
        sql += " FROM " + detail::quoted_identifier(table_name, table_name_size);
        if (limit != std::numeric_limits<uint64_t>::max()) sql += " LIMIT " + std::to_string(limit);
        sql += " OFFSET " + std::to_string(offset);
        const auto rows = context->tables->query(sql);
        constexpr std::size_t batch_size = 4096;
        for (std::size_t begin = 0; begin < rows.size(); begin += batch_size) {
            if (context->cancelled != nullptr && context->cancelled->load())
                return STREAMFIND_PLUGIN_CANCELLED;
            const auto count = std::min(batch_size, rows.size() - begin);
            std::vector<std::vector<std::string>> text(column_count, std::vector<std::string>(count));
            std::vector<std::vector<streamfind_plugin_string_view>> strings(column_count);
            std::vector<std::vector<int64_t>> integers(column_count);
            std::vector<std::vector<double>> doubles(column_count);
            std::vector<std::vector<uint8_t>> booleans(column_count);
            std::vector<std::vector<uint8_t>> validity(column_count, std::vector<uint8_t>((count + 7) / 8, 0));
            std::vector<streamfind_plugin_batch_column> columns(column_count);
            for (uint32_t column_index = 0; column_index < column_count; ++column_index) {
                const auto &request = requested_columns[column_index];
                strings[column_index].resize(count);
                integers[column_index].resize(count);
                doubles[column_index].resize(count);
                booleans[column_index].resize(count);
                for (std::size_t row = 0; row < count; ++row) {
                    const auto &value = rows[begin + row].at(std::string(request.name, request.name_size));
                    if (value.is_null()) continue;
                    validity[column_index][row / 8] |= static_cast<uint8_t>(1u << (row % 8));
                    text[column_index][row] = value.is_string() ? value.get<std::string>() : value.dump();
                    switch (request.type) {
                    case STREAMFIND_PLUGIN_COLUMN_UTF8:
                    case STREAMFIND_PLUGIN_COLUMN_TIMESTAMP:
                    case STREAMFIND_PLUGIN_COLUMN_DECIMAL:
                    case STREAMFIND_PLUGIN_COLUMN_BINARY:
                        if ((request.flags & STREAMFIND_PLUGIN_COLUMN_FLAG_ARRAY) != 0) {
                            const auto parsed = Json::parse(text[column_index][row], nullptr, false);
                            if (parsed.is_discarded() || !parsed.is_array()) return STREAMFIND_PLUGIN_SCHEMA_ERROR;
                        }
                        strings[column_index][row] = {text[column_index][row].data(),
                                                       static_cast<uint32_t>(text[column_index][row].size())};
                        break;
                    case STREAMFIND_PLUGIN_COLUMN_INT64:
                        integers[column_index][row] = std::stoll(text[column_index][row]);
                        break;
                    case STREAMFIND_PLUGIN_COLUMN_FLOAT64:
                        doubles[column_index][row] = std::stod(text[column_index][row]);
                        break;
                    case STREAMFIND_PLUGIN_COLUMN_BOOL:
                        if (text[column_index][row] != "true" && text[column_index][row] != "false" &&
                            text[column_index][row] != "0" && text[column_index][row] != "1")
                            return STREAMFIND_PLUGIN_SCHEMA_ERROR;
                        booleans[column_index][row] = text[column_index][row] == "true" || text[column_index][row] == "1";
                        break;
                    default: return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
                    }
                }
                auto &output = columns[column_index];
                output = request;
                output.data = (request.type == STREAMFIND_PLUGIN_COLUMN_UTF8 ||
                               request.type == STREAMFIND_PLUGIN_COLUMN_TIMESTAMP ||
                               request.type == STREAMFIND_PLUGIN_COLUMN_DECIMAL ||
                               request.type == STREAMFIND_PLUGIN_COLUMN_BINARY) ? static_cast<const void *>(strings[column_index].data())
                           : request.type == STREAMFIND_PLUGIN_COLUMN_INT64 ? static_cast<const void *>(integers[column_index].data())
                           : request.type == STREAMFIND_PLUGIN_COLUMN_FLOAT64 ? static_cast<const void *>(doubles[column_index].data())
                           : static_cast<const void *>(booleans[column_index].data());
                output.row_count = count;
                output.element_size = (request.type == STREAMFIND_PLUGIN_COLUMN_UTF8 ||
                                       request.type == STREAMFIND_PLUGIN_COLUMN_TIMESTAMP ||
                                       request.type == STREAMFIND_PLUGIN_COLUMN_DECIMAL ||
                                       request.type == STREAMFIND_PLUGIN_COLUMN_BINARY) ? sizeof(streamfind_plugin_string_view)
                                     : request.type == STREAMFIND_PLUGIN_COLUMN_INT64 ? sizeof(int64_t)
                                     : request.type == STREAMFIND_PLUGIN_COLUMN_FLOAT64 ? sizeof(double)
                                     : sizeof(uint8_t);
                output.validity_bitmap = validity[column_index].data();
            }
            const auto status = consume(consumer_context, columns.data(), column_count, count);
            if (status != STREAMFIND_PLUGIN_OK) return status;
        }
        return STREAMFIND_PLUGIN_OK;
    } catch (const std::invalid_argument &) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    } catch (...) {
        return STREAMFIND_PLUGIN_ERROR;
    }
}

streamfind_plugin_status plugin_append_batch(
    void *execution_context,
    const char *table_name,
    uint32_t table_name_size,
    const streamfind_plugin_batch_column *columns,
    uint32_t column_count,
    uint64_t row_count,
    void *) {
    if (execution_context == nullptr || table_name == nullptr || table_name_size == 0 ||
        columns == nullptr || column_count == 0) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    }
    auto *context = detail::context_from(execution_context);
    if (context->tables == nullptr) {
        return STREAMFIND_PLUGIN_ERROR;
    }
    try {
        const std::string table(table_name, table_name_size);
        if (!detail::allowed_table(*context, table)) return STREAMFIND_PLUGIN_NOT_ALLOWED;
        std::vector<std::string> names;
        names.reserve(column_count);
        for (uint32_t column_index = 0; column_index < column_count; ++column_index) {
            const auto &column = columns[column_index];
            if (column.name == nullptr || column.name_size == 0 || column.row_count != row_count) {
                return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            }
            if (!detail::allowed_column(*context, table,
                                        std::string(column.name, column.name_size), true))
                return STREAMFIND_PLUGIN_NOT_ALLOWED;
            names.emplace_back(column.name, column.name_size);
        }
        std::vector<std::vector<std::optional<std::string>>> rows(
            static_cast<std::size_t>(row_count),
            std::vector<std::optional<std::string>>(column_count));
        for (uint32_t column_index = 0; column_index < column_count; ++column_index) {
            for (uint64_t row = 0; row < row_count; ++row) {
                rows[static_cast<std::size_t>(row)][column_index] =
                    detail::column_value(columns[column_index], row);
            }
        }
        context->tables->append(
            std::string(table_name, table_name_size), names, rows);
        return STREAMFIND_PLUGIN_OK;
    } catch (...) {
        return STREAMFIND_PLUGIN_ERROR;
    }
}

streamfind_plugin_status plugin_update_batch(
    void *execution_context, const char *table_name, uint32_t table_name_size,
    const char *key_column, uint32_t key_column_size,
    const streamfind_plugin_batch_column *columns, uint32_t column_count,
    uint64_t row_count, uint64_t *affected_row_count, void *) {
    if (execution_context == nullptr || table_name == nullptr || key_column == nullptr || columns == nullptr ||
        affected_row_count == nullptr || table_name_size == 0 || key_column_size == 0 || column_count < 2 || row_count == 0)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    if (context->tables == nullptr) return STREAMFIND_PLUGIN_ERROR;
    try {
        const std::string table_name_text(table_name, table_name_size);
        if (!detail::allowed_table(*context, table_name_text)) return STREAMFIND_PLUGIN_NOT_ALLOWED;
        const std::string key_name(key_column, key_column_size);
        if (!detail::allowed_column(*context, table_name_text, key_name, false))
            return STREAMFIND_PLUGIN_NOT_ALLOWED;
        const auto table = detail::quoted_identifier(table_name, table_name_size);
        const auto key = detail::quoted_identifier(key_column, key_column_size);
        int key_index = -1;
        std::unordered_set<std::string> names;
        for (uint32_t index = 0; index < column_count; ++index) {
            if (columns[index].name == nullptr || columns[index].name_size == 0)
                return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            if (columns[index].row_count != row_count) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            const std::string name(columns[index].name, columns[index].name_size);
            if (!names.insert(name).second) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            if (name == key_name) key_index = static_cast<int>(index);
            if (name != key_name && !detail::allowed_column(*context, table_name_text, name, true))
                return STREAMFIND_PLUGIN_NOT_ALLOWED;
        }
        if (key_index < 0) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
        std::vector<std::optional<std::string>> keys;
        std::unordered_set<std::string> unique_keys;
        for (uint64_t row = 0; row < row_count; ++row) {
            auto value = detail::column_value(columns[key_index], row);
            if (!value || !unique_keys.insert(*value).second) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            keys.push_back(std::move(value));
        }
        std::string predicate = key + " IN (";
        for (std::size_t row = 0; row < keys.size(); ++row)
            predicate += (row == 0 ? "" : ",") + detail::sql_literal(keys[row]);
        predicate += ")";
        const auto count = context->tables->scalar("SELECT COUNT(*) FROM " + table + " WHERE " + predicate);
        *affected_row_count = count ? std::stoull(*count) : 0;
        std::string sql = "UPDATE " + table + " SET ";
        bool first = true;
        for (uint32_t index = 0; index < column_count; ++index) {
            if (static_cast<int>(index) == key_index) continue;
            if (!first) sql += ", ";
            first = false;
            const auto column = detail::quoted_identifier(columns[index].name, columns[index].name_size);
            sql += column + " = CASE " + key;
            for (uint64_t row = 0; row < row_count; ++row)
                sql += " WHEN " + detail::sql_literal(keys[static_cast<std::size_t>(row)]) +
                       " THEN " + detail::sql_literal(detail::column_value(columns[index], row));
            sql += " ELSE " + column + " END";
        }
        sql += " WHERE " + predicate;
        context->tables->execute(sql);
        return STREAMFIND_PLUGIN_OK;
    } catch (const std::invalid_argument &) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    } catch (...) {
        return STREAMFIND_PLUGIN_ERROR;
    }
}

streamfind_plugin_status plugin_update_composite_batch(
    void *execution_context, const char *table_name, uint32_t table_name_size,
    const streamfind_plugin_batch_column *key_columns, uint32_t key_column_count,
    const streamfind_plugin_batch_column *columns, uint32_t column_count,
    uint64_t row_count, uint64_t *affected_row_count, void *) {
    if (execution_context == nullptr || table_name == nullptr || key_columns == nullptr || columns == nullptr ||
        affected_row_count == nullptr || table_name_size == 0 || key_column_count == 0 || column_count == 0 || row_count == 0)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    if (context->tables == nullptr) return STREAMFIND_PLUGIN_ERROR;
    try {
        const std::string table_name_text(table_name, table_name_size);
        if (!detail::allowed_table(*context, table_name_text)) {
            return STREAMFIND_PLUGIN_NOT_ALLOWED;
        }
        std::unordered_set<std::string> all_names;
        for (uint32_t index = 0; index < key_column_count; ++index) {
            const auto &column = key_columns[index];
            if (column.name == nullptr || column.name_size == 0 || column.row_count != row_count)
                return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            const std::string name(column.name, column.name_size);
            if (!all_names.insert(name).second || !detail::allowed_column(*context, table_name_text, name, false)) {
                return STREAMFIND_PLUGIN_NOT_ALLOWED;
            }
        }
        for (uint32_t index = 0; index < column_count; ++index) {
            const auto &column = columns[index];
            if (column.name == nullptr || column.name_size == 0 || column.row_count != row_count)
                return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            const std::string name(column.name, column.name_size);
            if (!all_names.insert(name).second || !detail::allowed_column(*context, table_name_text, name, true)) {
                return STREAMFIND_PLUGIN_NOT_ALLOWED;
            }
        }
        const auto table = detail::quoted_identifier(table_name, table_name_size);
        std::vector<std::string> predicates;
        std::unordered_set<std::string> unique_keys;
        for (uint64_t row = 0; row < row_count; ++row) {
            std::string predicate = "(";
            std::string identity;
            for (uint32_t key = 0; key < key_column_count; ++key) {
                const auto value = detail::column_value(key_columns[key], row);
                if (!value) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
                if (key != 0) { predicate += " AND "; identity += '\x1f'; }
                predicate += detail::quoted_identifier(key_columns[key].name, key_columns[key].name_size) +
                             " = " + detail::sql_literal(value);
                identity += *value;
            }
            predicate += ")";
            if (!unique_keys.insert(identity).second) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            predicates.push_back(std::move(predicate));
        }
        std::string where;
        for (std::size_t row = 0; row < predicates.size(); ++row)
            where += (row == 0 ? "" : " OR ") + predicates[row];
        const auto count = context->tables->scalar("SELECT COUNT(*) FROM " + table + " WHERE " + where);
        *affected_row_count = count ? std::stoull(*count) : 0;

        std::string sql = "UPDATE " + table + " SET ";
        for (uint32_t column = 0; column < column_count; ++column) {
            if (column != 0) sql += ", ";
            const auto name = detail::quoted_identifier(columns[column].name, columns[column].name_size);
            sql += name + " = CASE";
            for (uint64_t row = 0; row < row_count; ++row)
                sql += " WHEN " + predicates[static_cast<std::size_t>(row)] + " THEN " +
                       detail::sql_literal(detail::column_value(columns[column], row));
            sql += " ELSE " + name + " END";
        }
        context->tables->execute(sql + " WHERE " + where);
        return STREAMFIND_PLUGIN_OK;
    } catch (const std::invalid_argument &) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    } catch (...) {
        return STREAMFIND_PLUGIN_ERROR;
    }
}

streamfind_plugin_status plugin_delete_batch(
    void *execution_context, const char *table_name, uint32_t table_name_size,
    const char *key_column, uint32_t key_column_size,
    const streamfind_plugin_batch_column *keys, uint64_t row_count,
    uint64_t *affected_row_count, void *) {
    if (execution_context == nullptr || table_name == nullptr || key_column == nullptr || keys == nullptr ||
        affected_row_count == nullptr || table_name_size == 0 || key_column_size == 0 || row_count == 0)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    auto *context = detail::context_from(execution_context);
    if (context->tables == nullptr) return STREAMFIND_PLUGIN_ERROR;
    try {
        const std::string table_name_text(table_name, table_name_size);
        if (!detail::allowed_table(*context, table_name_text) ||
            !detail::allowed_column(*context, table_name_text,
                                    std::string(key_column, key_column_size), false))
            return STREAMFIND_PLUGIN_NOT_ALLOWED;
        if (keys->name == nullptr || keys->name_size == 0)
            return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
        if (keys->row_count != row_count || std::string(keys->name, keys->name_size) !=
            std::string(key_column, key_column_size)) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
        std::unordered_set<std::string> unique;
        std::string predicate = detail::quoted_identifier(key_column, key_column_size) + " IN (";
        for (uint64_t row = 0; row < row_count; ++row) {
            const auto value = detail::column_value(*keys, row);
            if (!value || !unique.insert(*value).second) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
            if (row != 0) predicate += ",";
            predicate += detail::sql_literal(value);
        }
        predicate += ")";
        const auto table = detail::quoted_identifier(table_name, table_name_size);
        const auto count = context->tables->scalar("SELECT COUNT(*) FROM " + table + " WHERE " + predicate);
        *affected_row_count = count ? std::stoull(*count) : 0;
        context->tables->execute("DELETE FROM " + table + " WHERE " + predicate);
        return STREAMFIND_PLUGIN_OK;
    } catch (const std::invalid_argument &) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    } catch (...) {
        return STREAMFIND_PLUGIN_ERROR;
    }
}

streamfind_plugin_status plugin_report_progress(
    void *execution_context,
    double fraction,
    const char *message,
    uint32_t message_size,
    void *) {
    if (execution_context == nullptr || !std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    }
    auto *context = detail::context_from(execution_context);
    if (context->progress) {
        context->progress(fraction, message == nullptr ? std::string_view{} :
                                                   std::string_view(message, message_size));
    }
    return STREAMFIND_PLUGIN_OK;
}

uint8_t plugin_is_cancelled(void *execution_context, void *) {
    if (execution_context == nullptr) {
        return 1;
    }
    const auto *context = detail::context_from(execution_context);
    return context->cancelled != nullptr && context->cancelled->load() ? 1 : 0;
}

}  // namespace streamfind::sdk
