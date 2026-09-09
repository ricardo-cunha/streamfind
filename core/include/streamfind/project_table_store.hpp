#pragma once

#include <optional>
#include <utility>
#include <string>
#include <vector>

#include "streamfind/export.hpp"
#include "streamfind/project.hpp"

namespace streamfind {

/** @brief Table contract owned by a registered domain module. */
struct STREAMFIND_CORE_API TableRequirement {
    std::string name;
    std::vector<std::string> required_columns;
    std::vector<std::pair<std::string, std::string>> required_column_types;
};

/** @brief Narrow table-access boundary for domain modules. */
class STREAMFIND_CORE_API ProjectTableStore {
public:
    explicit ProjectTableStore(Project &project) noexcept;

    /** @brief Return whether a physical table exists in the open project file. */
    bool has_table(const std::string &table_name) const;
    /** @brief Require tables and columns declared by a domain module. */
    void require(const std::vector<TableRequirement> &requirements) const;
    void require_manifest(const std::string &domain) const;
    /** @brief Install module-owned DDL in the open project file. */
    void ensure_table(const std::string &table_name, const std::string &ddl) const;
    /** @brief Execute a module-owned query. */
    Json query(const std::string &sql) const;
    /** @brief Execute module-owned SQL. */
    void execute(const std::string &sql) const;
    /** @brief Append module-owned rows. */
    void append(const std::string &table_name,
                const std::vector<std::string> &column_names,
                const std::vector<std::vector<std::optional<std::string>>> &rows) const;

private:
    Project *project_;
};

}  // namespace streamfind
