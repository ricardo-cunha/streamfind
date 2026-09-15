#include "streamfind/project_table_store.hpp"
#include "streamfind/catalogue.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <duckdb.h>
#include <limits>
#include <sstream>
#include <set>
#include <string_view>

namespace streamfind {

namespace detail {

std::string manifest_sql_type(const Json &column) {
    const auto type = column.at("type").get<std::string>();
    if (type == "string") return "VARCHAR";
    if (type == "integer") return "INTEGER";
    if (type == "real") return "DOUBLE";
    if (type == "boolean") return "BOOLEAN";
    if (type == "timestamp") return "TIMESTAMP";
    if (type == "binary") return "BLOB";
    if (type == "decimal") {
        const auto width = column.value("precision", 18);
        const auto scale = column.value("scale", 3);
        if (width < 1 || width > 38 || scale < 0 || scale > width)
            throw std::invalid_argument("catalogue: invalid decimal precision/scale");
        return "DECIMAL(" + std::to_string(width) + "," + std::to_string(scale) + ")";
    }
    throw std::invalid_argument("catalogue: unsupported manifest column type " + type);
}

duckdb_timestamp parse_timestamp(std::string_view text) {
    if (text.size() < 19 || (text[10] != ' ' && text[10] != 'T') || text[4] != '-' ||
        text[7] != '-' || text[13] != ':' || text[16] != ':')
        throw std::invalid_argument("invalid timestamp");
    auto number = [&](std::size_t begin, std::size_t count) -> int64_t {
        int64_t value = 0;
        for (std::size_t i = begin; i < begin + count; ++i) {
            if (text[i] < '0' || text[i] > '9') throw std::invalid_argument("invalid timestamp");
            value = value * 10 + (text[i] - '0');
        }
        return value;
    };
    const int64_t year = number(0, 4), month = number(5, 2), day = number(8, 2);
    const int64_t hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59)
        throw std::invalid_argument("invalid timestamp");
    int64_t micros = 0;
    if (text.size() > 19) {
        if (text[19] != '.') throw std::invalid_argument("invalid timestamp fraction");
        std::size_t digits = 0;
        for (std::size_t i = 20; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9' || ++digits > 6)
                throw std::invalid_argument("timestamp precision exceeds microseconds");
            micros = micros * 10 + (text[i] - '0');
        }
        while (digits++ < 6) micros *= 10;
    }
    int64_t y = year - (month <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
    const uint32_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
    return {((days * 24 + hour) * 60 + minute) * 60 * 1000000 + second * 1000000 + micros};
}

duckdb_hugeint parse_hugeint(std::string_view digits, bool negative) {
    uint64_t lower = 0;
    uint64_t upper = 0;
    for (const char digit : digits) {
        if (digit < '0' || digit > '9') throw std::invalid_argument("invalid decimal");
        const uint64_t value = static_cast<uint64_t>(digit - '0');
        const uint64_t carry = lower > (std::numeric_limits<uint64_t>::max() - value) / 10 ? 1 : 0;
        lower = lower * 10 + value;
        if (upper > (std::numeric_limits<uint64_t>::max() - carry) / 10)
            throw std::invalid_argument("decimal overflow");
        upper = upper * 10 + carry;
    }
    if ((!negative && upper > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) ||
        (negative && upper > 0x8000000000000000ULL))
        throw std::invalid_argument("decimal overflow");
    if (negative) {
        lower = ~lower + 1;
        upper = ~upper + (lower == 0 ? 1 : 0);
    }
    return {lower, static_cast<int64_t>(upper)};
}

duckdb_decimal parse_decimal(std::string_view text, uint8_t width, uint8_t scale) {
    const bool negative = !text.empty() && text.front() == '-';
    const std::size_t start = negative || (!text.empty() && text.front() == '+') ? 1 : 0;
    const auto point = text.find('.', start);
    const std::size_t fractional = point == std::string_view::npos ? 0 : text.size() - point - 1;
    if (start == text.size() || fractional > scale || (point != std::string_view::npos && point == start))
        throw std::invalid_argument("invalid decimal scale");
    std::string digits(text.substr(start, point == std::string_view::npos ? text.size() - start : point - start));
    if (point != std::string_view::npos) digits += text.substr(point + 1);
    digits.append(scale - fractional, '0');
    return {width, scale, parse_hugeint(digits, negative)};
}

std::string manifest_identifier(const std::string &value) {
    if (value.empty()) throw std::invalid_argument("catalogue: empty manifest identifier");
    for (const char ch : value)
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
            throw std::invalid_argument("catalogue: invalid manifest identifier " + value);
    return value;
}

}  // namespace detail

struct ProjectTableStore::Impl {
    duckdb_database database{nullptr};
    duckdb_connection connection{nullptr};
    std::vector<std::string> owned_tables;
};

namespace detail {

void close_store_connection(ProjectTableStore::Impl &impl) noexcept {
    if (impl.connection) duckdb_disconnect(&impl.connection);
    if (impl.database) duckdb_close(&impl.database);
}

void store_query(ProjectTableStore::Impl &impl, const std::string &sql) {
    duckdb_result result{};
    if (duckdb_query(impl.connection, sql.c_str(), &result) == DuckDBError) {
        const std::string message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "DuckDB query failed";
        duckdb_destroy_result(&result);
        throw Error(ErrorCode::DatabaseError, message);
    }
    duckdb_destroy_result(&result);
}

Json store_query_json(ProjectTableStore::Impl &impl, const std::string &sql) {
    duckdb_result result{};
    if (duckdb_query(impl.connection, sql.c_str(), &result) == DuckDBError) {
        const std::string message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "DuckDB query failed";
        duckdb_destroy_result(&result);
        throw Error(ErrorCode::DatabaseError, message);
    }
    Json rows = Json::array();
    for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
        Json object = Json::object();
        for (idx_t column = 0; column < duckdb_column_count(&result); ++column) {
            const auto name = duckdb_column_name(&result, column);
            if (duckdb_value_is_null(&result, column, row)) object[name] = nullptr;
            else {
                char *value = duckdb_value_varchar(&result, column, row);
                object[name] = value ? value : "";
                if (value) duckdb_free(value);
            }
        }
        rows.push_back(std::move(object));
    }
    duckdb_destroy_result(&result);
    return rows;
}

bool owns(const ProjectTableStore::Impl &impl, const std::string &table_name) {
    return impl.owned_tables.empty() ||
           std::find(impl.owned_tables.begin(), impl.owned_tables.end(), table_name) != impl.owned_tables.end();
}

void require_owned(const ProjectTableStore::Impl &impl, const std::string &table_name) {
    if (!owns(impl, table_name))
        throw Error(ErrorCode::SchemaMismatch, "table is not owned by module: " + table_name);
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

void require_sql_ownership(const ProjectTableStore::Impl &impl, const std::string &sql) {
    if (impl.owned_tables.empty()) return;
    const auto statement = lower_copy(sql);
    if (statement.find("begin ") == 0 || statement.find("commit") == 0 ||
        statement.find("rollback") == 0 || statement.find("pragma") == 0 ||
        statement.find("module_schema") != std::string::npos) return;
    for (const auto &table : impl.owned_tables) {
        if (statement.find(lower_copy(table)) != std::string::npos) return;
    }
    throw Error(ErrorCode::SchemaMismatch, "SQL does not reference a table owned by module");
}

void append_on_connection(ProjectTableStore::Impl &impl, const std::string &table_name,
                          const std::vector<std::string> &column_names,
                          const std::vector<std::vector<std::optional<std::string>>> &rows) {
    duckdb_appender appender = nullptr;
    if (duckdb_appender_create(impl.connection, nullptr, table_name.c_str(), &appender) == DuckDBError)
        throw Error(ErrorCode::DatabaseError, "create DuckDB appender failed for " + table_name);
    const auto close = [&]() {
        if (appender) duckdb_appender_close(appender);
    };
    try {
        for (const auto &name : column_names) {
            if (duckdb_appender_add_column(appender, name.c_str()) == DuckDBError)
                throw Error(ErrorCode::DatabaseError, "add appender column failed: " + name);
        }
        std::vector<duckdb_type> types;
        std::vector<uint8_t> decimal_widths;
        std::vector<uint8_t> decimal_scales;
        for (idx_t index = 0; index < duckdb_appender_column_count(appender); ++index) {
            auto logical = duckdb_appender_column_type(appender, index);
            const auto type = duckdb_get_type_id(logical);
            types.push_back(type);
            decimal_widths.push_back(type == DUCKDB_TYPE_DECIMAL ? duckdb_decimal_width(logical) : 0);
            decimal_scales.push_back(type == DUCKDB_TYPE_DECIMAL ? duckdb_decimal_scale(logical) : 0);
            duckdb_destroy_logical_type(&logical);
        }
        for (const auto &row : rows) {
            if (row.size() != column_names.size())
                throw Error(ErrorCode::InvalidArgument, "append row/column count mismatch for " + table_name);
            if (duckdb_appender_begin_row(appender) == DuckDBError) throw Error(ErrorCode::DatabaseError, "begin appender row failed");
            for (idx_t index = 0; index < types.size(); ++index) {
                const auto &cell = row[static_cast<std::size_t>(index)];
                duckdb_state state = DuckDBSuccess;
                if (!cell) state = duckdb_append_null(appender);
                else switch (types[static_cast<std::size_t>(index)]) {
                    case DUCKDB_TYPE_VARCHAR: state = duckdb_append_varchar(appender, cell->c_str()); break;
                    case DUCKDB_TYPE_DOUBLE: state = duckdb_append_double(appender, std::stod(*cell)); break;
                    case DUCKDB_TYPE_FLOAT: state = duckdb_append_float(appender, std::stof(*cell)); break;
                    case DUCKDB_TYPE_INTEGER: state = duckdb_append_int32(appender, static_cast<int32_t>(std::stoll(*cell))); break;
                    case DUCKDB_TYPE_BIGINT: state = duckdb_append_int64(appender, std::stoll(*cell)); break;
                    case DUCKDB_TYPE_SMALLINT: state = duckdb_append_int16(appender, static_cast<int16_t>(std::stoll(*cell))); break;
                    case DUCKDB_TYPE_TINYINT: state = duckdb_append_int8(appender, static_cast<int8_t>(std::stoll(*cell))); break;
                    case DUCKDB_TYPE_BOOLEAN: state = duckdb_append_bool(appender, *cell == "true" || *cell == "TRUE" || *cell == "1"); break;
                    case DUCKDB_TYPE_TIMESTAMP: state = duckdb_append_timestamp(appender, parse_timestamp(*cell)); break;
                    case DUCKDB_TYPE_DECIMAL: {
                        const auto decimal = parse_decimal(*cell, decimal_widths[index], decimal_scales[index]);
                        duckdb_value value = duckdb_create_decimal(decimal);
                        if (!value) throw Error(ErrorCode::DatabaseError, "create DuckDB decimal failed");
                        state = duckdb_append_value(appender, value);
                        duckdb_destroy_value(&value);
                        break;
                    }
                    case DUCKDB_TYPE_BLOB: state = duckdb_append_blob(appender, cell->data(), cell->size()); break;
                    default: throw Error(ErrorCode::DatabaseError, "unsupported appender column type");
                }
                if (state == DuckDBError) throw Error(ErrorCode::DatabaseError, "append appender value failed");
            }
            if (duckdb_appender_end_row(appender) == DuckDBError) throw Error(ErrorCode::DatabaseError, "end appender row failed");
        }
        if (duckdb_appender_close(appender) == DuckDBError) throw Error(ErrorCode::DatabaseError, "close appender failed");
        appender = nullptr;
    } catch (...) {
        close();
        throw;
    }
}

}  // namespace detail

ProjectTableStore::ProjectTableStore(Project &project) noexcept : project_(&project) {}

ProjectTableStore::ProjectTableStore(Project &project, std::vector<std::string> owned_tables)
    : project_(&project), impl_(std::make_unique<Impl>()) {
    impl_->owned_tables = std::move(owned_tables);
    if (duckdb_open(project.get_database_path().string().c_str(), &impl_->database) == DuckDBError)
        throw Error(ErrorCode::DatabaseError, "open transaction project database failed");
    if (duckdb_connect(impl_->database, &impl_->connection) == DuckDBError) {
        duckdb_close(&impl_->database);
        throw Error(ErrorCode::DatabaseError, "connect transaction project database failed");
    }

}

ProjectTableStore::~ProjectTableStore() {
    if (impl_) detail::close_store_connection(*impl_);
}

void ProjectTableStore::transaction(Project &project,
                                    const std::vector<std::string> &owned_tables,
                                    const std::function<void(ProjectTableStore &)> &callback) {
    ProjectTableStore store(project, owned_tables);
    detail::store_query(*store.impl_, "BEGIN TRANSACTION");
    try {
        callback(store);
        detail::store_query(*store.impl_, "COMMIT");
    } catch (...) {
        try { detail::store_query(*store.impl_, "ROLLBACK"); } catch (...) {}
        throw;
    }
}

void ProjectTableStore::install_manifest_schema(Project &project,
                                                 const std::string &domain,
                                                 const std::string &module_id) {
    const auto manifest = catalogue::table_manifest_json(domain, "");
    if (!manifest) throw Error(ErrorCode::SchemaMismatch, "module table manifest unavailable: " + module_id);
    std::vector<std::string> owned_tables;
    std::set<std::string> modules;
    for (const auto &table : *manifest)
    {
        owned_tables.push_back(detail::manifest_identifier(table.at("table_name").get<std::string>()));
        modules.insert(table.value("module_id", std::string{}));
    }
    ProjectTableStore::transaction(project, owned_tables, [&](ProjectTableStore &tables) {
        tables.execute(
            "CREATE TABLE IF NOT EXISTS MODULE_SCHEMA (domain_id VARCHAR NOT NULL, module_id VARCHAR NOT NULL, "
            "schema_version INTEGER NOT NULL, installed_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
            "PRIMARY KEY (domain_id, module_id))");
        for (const auto &table : *manifest) {
            const auto table_name = detail::manifest_identifier(table.at("table_name").get<std::string>());
            std::vector<std::string> definitions;
            for (const auto &column : table.value("columns", Json::array())) {
                const auto name = detail::manifest_identifier(column.at("name").get<std::string>());
                definitions.push_back(name + " " + detail::manifest_sql_type(column));
            }
            tables.ensure_table(table_name, "CREATE TABLE IF NOT EXISTS " + table_name + " (" +
                                             [&] { std::string text; for (size_t i = 0; i < definitions.size(); ++i) { if (i) text += ", "; text += definitions[i]; } return text; }() + ")");
        }
        for (const auto &installed_module : modules) {
            if (installed_module.empty()) continue;
            tables.execute("DELETE FROM MODULE_SCHEMA WHERE domain_id = '" + domain + "' AND module_id = '" + installed_module + "'");
            tables.execute("INSERT INTO MODULE_SCHEMA (domain_id, module_id, schema_version) VALUES ('" + domain + "', '" + installed_module + "', 1)");
        }
    });
}

bool ProjectTableStore::has_table(const std::string &table_name) const {
    if (impl_) {
        detail::require_owned(*impl_, table_name);
        duckdb_result result{};
        const std::string sql = "SELECT 1 FROM information_schema.tables WHERE table_schema = 'main' AND table_name = '" +
                                table_name + "' LIMIT 1";
        if (duckdb_query(impl_->connection, sql.c_str(), &result) == DuckDBError) {
            const std::string message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "inspect tables failed";
            duckdb_destroy_result(&result);
            throw Error(ErrorCode::DatabaseError, message);
        }
        const bool found = duckdb_row_count(&result) != 0;
        duckdb_destroy_result(&result);
        return found;
    }
    const auto tables = project_->list_tables();
    return std::find(tables.begin(), tables.end(), table_name) != tables.end();
}

void ProjectTableStore::require(const std::vector<TableRequirement> &requirements) const {
    for (const auto &requirement : requirements) {
        if (!has_table(requirement.name))
            throw Error(ErrorCode::SchemaMismatch, "missing domain table: " + requirement.name);
        if (requirement.required_columns.empty() && requirement.required_column_types.empty()) continue;
        const auto rows = impl_
            ? detail::store_query_json(*impl_, "DESCRIBE \"" + requirement.name + "\"")
            : project_->query_json("DESCRIBE \"" + requirement.name + "\"");
        for (const auto &column : requirement.required_columns) {
            const auto found = std::find_if(rows.begin(), rows.end(), [&column](const auto &row) {
                return row.value("column_name", "") == column;
            });
            if (found == rows.end())
                throw Error(ErrorCode::SchemaMismatch,
                            "missing column " + column + " in domain table " + requirement.name);
        }
        for (const auto &[column, semantic_type] : requirement.required_column_types) {
            const auto found = std::find_if(rows.begin(), rows.end(), [&column](const auto &row) {
                return row.value("column_name", "") == column;
            });
            if (found == rows.end())
                throw Error(ErrorCode::SchemaMismatch,
                            "missing column " + column + " in domain table " + requirement.name);
            const std::string actual = found->value("column_type", "");
            const auto hash = semantic_type.rfind('#');
            const std::string expected = hash == std::string::npos ? semantic_type : semantic_type.substr(hash + 1);
            const bool compatible = expected == "string" ? (actual == "VARCHAR" || actual == "TEXT")
                : expected == "integer" ? actual.find("INT") != std::string::npos
                : expected == "real" ? (actual == "DOUBLE" || actual == "FLOAT" || actual == "DECIMAL")
                : expected == "boolean" ? actual == "BOOLEAN"
                : expected == "timestamp" ? actual.find("TIMESTAMP") != std::string::npos
                : true;
            if (!compatible)
                throw Error(ErrorCode::SchemaMismatch, "incompatible type for " + column + " in domain table " +
                                                        requirement.name + ": expected " + expected + ", got " + actual);
        }
    }
}

void ProjectTableStore::require_manifest(const std::string &domain, const std::string &module_id) const {
    const auto manifest = catalogue::table_manifest_json(domain, module_id);
    if (!manifest) throw Error(ErrorCode::DatabaseError, "generated table manifest unavailable");
    std::vector<TableRequirement> requirements;
    for (const auto &table : *manifest) {
        TableRequirement requirement;
        requirement.name = table.value("table_name", "");
        for (const auto &column : table.value("columns", Json::array()))
            requirement.required_column_types.emplace_back(column.value("name", ""), column.value("type", ""));
        requirements.push_back(std::move(requirement));
    }
    require(requirements);
}

void ProjectTableStore::require_installed_manifest(const std::string &domain,
                                                   const std::string &module_id,
                                                   int minimum_version) const {
    try {
        const auto rows = impl_
            ? detail::store_query_json(*impl_, "SELECT schema_version FROM MODULE_SCHEMA WHERE domain_id = '" +
                                       domain + "' AND module_id = '" + module_id + "' LIMIT 1")
            : project_->query_json("SELECT schema_version FROM MODULE_SCHEMA WHERE domain_id = '" +
                                   domain + "' AND module_id = '" + module_id + "' LIMIT 1");
        if (rows.empty() || rows.at(0).value("schema_version", "") < std::to_string(minimum_version))
            throw Error(ErrorCode::SchemaMismatch, "module schema is not installed: " + module_id);
    } catch (const Error &) {
        throw;
    } catch (const std::exception &) {
        throw Error(ErrorCode::SchemaMismatch, "module schema is not installed: " + module_id);
    }
    require_manifest(domain, module_id);
}

void ProjectTableStore::ensure_table(const std::string &table_name, const std::string &ddl) const {
    if (impl_) {
        detail::require_owned(*impl_, table_name);
        detail::store_query(*impl_, ddl);
        return;
    }
    project_->execute_sql(ddl);
}

Json ProjectTableStore::query(const std::string &sql) const {
    if (impl_) {
        detail::require_sql_ownership(*impl_, sql);
        return detail::store_query_json(*impl_, sql);
    }
    return project_->query_json(sql);
}

std::optional<std::string> ProjectTableStore::scalar(const std::string &sql) const {
    if (impl_) detail::require_sql_ownership(*impl_, sql);
    if (!impl_) {
        const auto rows = project_->query_json(sql);
        if (rows.empty() || rows.at(0).empty()) return std::nullopt;
        return rows.at(0).begin().value().dump();
    }
    duckdb_result result{};
    if (duckdb_query(impl_->connection, sql.c_str(), &result) == DuckDBError) {
        const std::string message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "scalar query failed";
        duckdb_destroy_result(&result);
        throw Error(ErrorCode::DatabaseError, message);
    }
    if (duckdb_row_count(&result) == 0) {
        duckdb_destroy_result(&result);
        return std::nullopt;
    }
    char *value = duckdb_value_varchar(&result, 0, 0);
    std::optional<std::string> output = value ? std::optional<std::string>(value) : std::nullopt;
    if (value) duckdb_free(value);
    duckdb_destroy_result(&result);
    return output;
}

void ProjectTableStore::execute(const std::string &sql) const {
    if (impl_) {
        detail::require_sql_ownership(*impl_, sql);
        detail::store_query(*impl_, sql);
        return;
    }
    project_->execute_sql(sql);
}

void ProjectTableStore::append(
    const std::string &table_name, const std::vector<std::string> &column_names,
    const std::vector<std::vector<std::optional<std::string>>> &rows) const {
    if (impl_) {
        detail::require_owned(*impl_, table_name);
        detail::append_on_connection(*impl_, table_name, column_names, rows);
        return;
    }
    project_->append_rows(table_name, column_names, rows);
}

}  // namespace streamfind
