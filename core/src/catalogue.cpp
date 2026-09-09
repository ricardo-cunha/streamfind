/**
 * @file catalogue.cpp
 * @brief Read-only catalogue.duckdb reader (see streamfind/catalogue.hpp).
 */

#include "streamfind/catalogue.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

#include <duckdb.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef STREAMFIND_INSTALL_PREFIX
#define STREAMFIND_INSTALL_PREFIX ""
#endif

namespace streamfind::catalogue {
namespace detail {

std::optional<std::string> env_path() {
    const char *value = std::getenv("STREAMFIND_CATALOGUE");
    if (!value || !*value) return std::nullopt;
    return std::string(value);
}

std::filesystem::path executable_dir() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#else
    return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
}

std::optional<std::string> binary_relative() {
    const auto candidate = executable_dir() / "catalogue.duckdb";
    if (std::filesystem::exists(candidate)) return candidate.string();
    return std::nullopt;
}

/// Release/install layout: <prefix>/bin/<exe> + <prefix>/share/streamfind/.
/// Resolving relative to the executable makes unpacked release archives
/// relocatable (no compile-time prefix dependency).
std::optional<std::string> binary_relative_share() {
    const auto candidate = executable_dir().parent_path() / "share" / "streamfind" / "catalogue.duckdb";
    if (std::filesystem::exists(candidate)) return candidate.string();
    return std::nullopt;
}

std::optional<std::string> install_data_dir() {
    const std::string prefix = STREAMFIND_INSTALL_PREFIX;
    if (prefix.empty()) return std::nullopt;
    const auto candidate = std::filesystem::path(prefix) / "share" / "streamfind" / "catalogue.duckdb";
    if (std::filesystem::exists(candidate)) return candidate.string();
    return std::nullopt;
}

class CatalogueReader {
public:
    explicit CatalogueReader(const std::string &path) {
        duckdb_config config = nullptr;
        char *error = nullptr;
        if (duckdb_create_config(&config) == DuckDBError) {
            throw Error(ErrorCode::DatabaseError, "catalogue: failed to create DuckDB config");
        }
        duckdb_set_config(config, "access_mode", "READ_ONLY");
        const duckdb_state state = duckdb_open_ext(path.c_str(), &database_, config, &error);
        duckdb_destroy_config(&config);
        if (state == DuckDBError) {
            std::string message = error ? error : "failed to open catalogue.duckdb";
            if (error) duckdb_free(error);
            throw Error(ErrorCode::DatabaseError, "catalogue: " + message);
        }
        if (duckdb_connect(database_, &connection_) == DuckDBError) {
            duckdb_close(&database_);
            throw Error(ErrorCode::DatabaseError, "catalogue: failed to connect");
        }
    }

    ~CatalogueReader() {
        if (connection_) duckdb_disconnect(&connection_);
        if (database_) duckdb_close(&database_);
    }

    CatalogueReader(const CatalogueReader &) = delete;
    CatalogueReader &operator=(const CatalogueReader &) = delete;

    Json entries() const {
        duckdb_result result{};
        const char *sql =
            "SELECT canonical_id, kind, domain, label, definition, category, invocation_model, "
            "requires_connection, guidance, next_operations, interface_guidance, executable, "
            "exposed, mcp_name, input_schema, parameters, result_schema, reads_tables, "
            "writes_tables, cacheable, single_occurrence, mutates_project, required_methods "
            "FROM catalogue_entries ORDER BY canonical_id";
        if (duckdb_query(connection_, sql, &result) == DuckDBError) {
            std::string message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "query failed";
            duckdb_destroy_result(&result);
            throw Error(ErrorCode::DatabaseError, "catalogue: " + message);
        }
        Json entries = Json::array();
        const idx_t rows = duckdb_row_count(&result);
        for (idx_t row = 0; row < rows; ++row) {
            entries.push_back(read_entry(result, row));
        }
        duckdb_destroy_result(&result);
        return entries;
    }

    Json tables(const std::string &domain) const {
        duckdb_result result{};
        const std::string sql = "SELECT table_name, CAST(columns AS VARCHAR) FROM catalogue_tables WHERE domain = '" +
                                domain + "' ORDER BY table_name";
        if (duckdb_query(connection_, sql.c_str(), &result) == DuckDBError) {
            const std::string message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "query failed";
            duckdb_destroy_result(&result);
            throw Error(ErrorCode::DatabaseError, "catalogue: " + message);
        }
        Json output = Json::array();
        for (idx_t row = 0; row < duckdb_row_count(&result); ++row)
            output.push_back(Json{{"table_name", text(result, 0, row)}, {"columns", value(result, 1, row)}});
        duckdb_destroy_result(&result);
        return output;
    }

private:
    static bool is_null(duckdb_result &result, idx_t column, idx_t row) {
        return duckdb_value_is_null(&result, column, row) != 0;
    }

    static std::string text(duckdb_result &result, idx_t column, idx_t row) {
        char *value = duckdb_value_varchar(&result, column, row);
        if (!value) return {};
        std::string output(value);
        duckdb_free(value);
        return output;
    }

    static Json value(duckdb_result &result, idx_t column, idx_t row) {
        if (is_null(result, column, row)) return Json(nullptr);
        const std::string raw = text(result, column, row);
        if (raw.empty()) return Json(nullptr);
        try {
            return Json::parse(raw);
        } catch (const std::exception &error) {
            throw Error(ErrorCode::SchemaMismatch,
                        std::string("catalogue: invalid JSON value: ") + error.what());
        }
    }

    static Json boolean(duckdb_result &result, idx_t column, idx_t row) {
        if (is_null(result, column, row)) return Json(nullptr);
        return duckdb_value_boolean(&result, column, row) != 0;
    }

    Json read_entry(duckdb_result &result, idx_t row) const {
        const std::string kind = text(result, 1, row);
        Json entry = {
            {"kind", kind},
            {"canonical_id", text(result, 0, row)},
            {"domain", text(result, 2, row)},
            {"label", text(result, 3, row)},
            {"definition", text(result, 4, row)},
            {"interface", Json{
                {"category", text(result, 5, row)},
                {"invocation_model", text(result, 6, row)},
                {"requires_connection", boolean(result, 7, row)},
                {"guidance", text(result, 8, row)},
                {"next_operations", value(result, 9, row)},
            }},
            {"interface_guidance", text(result, 10, row)},
            {"executable", boolean(result, 11, row)},
            {"exposed", boolean(result, 12, row)},
        };
        const auto input_schema = value(result, 14, row);
        if (!is_null(result, 13, row)) {
            entry["mcp"] = Json{{"name", text(result, 13, row)}, {"input_schema", value(result, 14, row)}};
        } else {
            entry["method_schema"] = input_schema;
        }
        entry["parameters"] = value(result, 15, row);
        entry["result"] = Json{{"schema", value(result, 16, row)}};
        entry["effects"] = Json{
            {"mutates_project", boolean(result, 21, row)},
            {"reads", value(result, 17, row)},
            {"writes", value(result, 18, row)},
        };
        if (kind == "method") {
            entry["cacheable"] = boolean(result, 19, row);
            entry["single_occurrence"] = boolean(result, 20, row);
            entry["required_methods"] = value(result, 22, row);
        }
        return entry;
    }

    duckdb_database database_{nullptr};
    duckdb_connection connection_{nullptr};
};

}  // namespace detail

std::optional<std::string> find_path() {
    if (auto path = detail::env_path()) return path;
    if (auto path = detail::binary_relative()) return path;
    if (auto path = detail::binary_relative_share()) return path;
    if (auto path = detail::install_data_dir()) return path;
    return std::nullopt;
}

namespace {

struct Catalogue {
    std::optional<Json> entries;
    std::string error;
};

/** @brief Whole-process catalogue, loaded once via the default search chain. */
const Catalogue &catalogue() {
    static const Catalogue value = [] {
        Catalogue result;
        const auto path = find_path();
        if (!path) {
            result.error =
                "catalogue.duckdb not found; searched STREAMFIND_CATALOGUE, the executable directory, "
                "and the install data directory (share/streamfind). Ensure the runtime knowledge base "
                "is installed alongside the binaries.";
            return result;
        }
        try {
            result.entries = detail::CatalogueReader(*path).entries();
        } catch (const std::exception &exception) {
            result.error = exception.what();
        }
        return result;
    }();
    return value;
}

}  // namespace

std::optional<Json> load(const std::optional<std::string> &path) {
    if (path) {
        try {
            return detail::CatalogueReader(*path).entries();
        } catch (const std::exception &) {
            return std::nullopt;
        }
    }
    return catalogue().entries;
}

std::optional<Json> table_manifest_json(const std::string &domain) {
    const auto path = find_path();
    if (!path) return std::nullopt;
    try {
        return detail::CatalogueReader(*path).tables(domain);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

std::optional<Json> tools_json() {
    const auto &entries = catalogue().entries;
    if (!entries) return std::nullopt;
    Json tools = Json::array();
    for (const auto &entry : *entries) {
        if (entry.value("kind", "") != "operation" || entry.value("domain", "") != "streamfind" ||
            !entry.value("exposed", false)) {
            continue;
        }
        std::string description = entry.value("label", "") + ": " + entry.value("definition", "");
        const auto guidance = entry.at("interface").value("guidance", "");
        if (!guidance.empty()) description += " Guidance: " + guidance;
        const auto model = entry.at("interface").value("invocation_model", "");
        if (!model.empty()) description += " Invocation model: " + model + ".";
        const bool read_only = !entry.at("effects").value("mutates_project", false);
        tools.push_back(Json{
                            {"name", entry.at("mcp").at("name")},
                            {"description", description},
                            {"inputSchema", entry.at("mcp").at("input_schema")},
                            {"annotations", {{"title", entry.value("label", "")},
                                              {"readOnlyHint", read_only},
                                              {"destructiveHint", !read_only}}},
                            {"_meta", {{"streamfind", entry.at("interface")}}},
                            // No outputSchema: MCP requires it to be a JSON-Schema
                            // object AND that every result carries matching
                            // structuredContent. streamfind results are table-like
                            // documents returned as text content, so omitting
                            // outputSchema is the spec-correct shape (it is optional
                            // in MCP). The semantic result schema remains available
                            // via the catalogue query operations.
                            {"effects", entry.at("effects")},
                        });
    }
    return tools;
}

std::optional<Json> entries_json() {
    return catalogue().entries;
}

std::optional<Json> methods_json() {
    const auto &entries = catalogue().entries;
    if (!entries) return std::nullopt;
    Json methods = Json::array();
    for (const auto &entry : *entries) {
        if (entry.value("kind", "") == "method") methods.push_back(entry);
    }
    return methods;
}

std::string load_error() {
    return catalogue().error;
}

}  // namespace streamfind::catalogue