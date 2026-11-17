#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "streamfind/export.hpp"
#include "streamfind/plugin_abi.h"
#include "streamfind/sdk/plugin_project_access.hpp"

namespace streamfind::sdk {

class STREAMFIND_SDK_API TableBatchBuilder {
public:
    using ListCell = std::variant<std::monostate, std::string, std::int64_t, double>;
    using Cell = std::variant<std::monostate, std::string, std::int64_t, double, bool,
                              std::vector<ListCell>>;

    struct Column {
        std::string name;
        streamfind_plugin_column_type type;
        bool is_list = false;
        streamfind_plugin_column_type child_type = STREAMFIND_PLUGIN_COLUMN_UTF8;
    };

    explicit TableBatchBuilder(std::vector<Column> schema);

    std::uint64_t row_count() const noexcept;
    void append_row(const std::vector<Cell> &values);
    void append_row(const PluginProjectAccess::BatchView &batch, std::uint64_t source_row,
                    const std::map<std::string, Cell> &overrides = {});
    void emit(PluginProjectAccess &access, const std::string &output_contract_id) const;
    void clear();

private:
    struct ColumnValues {
        Column schema;
        std::vector<std::string> strings;
        std::vector<std::int64_t> integers;
        std::vector<double> reals;
        std::vector<std::uint8_t> booleans;
        std::vector<std::uint8_t> validity;
        std::vector<std::uint64_t> offsets;
        std::vector<std::string> child_strings;
        std::vector<std::int64_t> child_integers;
        std::vector<double> child_reals;
        std::vector<std::uint8_t> child_validity;
    };

    std::vector<ColumnValues> columns_;
    std::uint64_t row_count_ = 0;
};

}  // namespace streamfind::sdk
