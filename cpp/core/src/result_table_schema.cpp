#include "streamfind/result_table_schema.hpp"

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <arrow/api.h>

namespace streamfind {

std::shared_ptr<arrow::Schema> result_table_arrow_schema(const data_proxy::Json &columns) {
    if (!columns.is_array() || columns.empty())
        throw std::invalid_argument("Arrow table schema must be a non-empty column array");
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(columns.size());
    std::set<std::string> names;
    for (const auto &column : columns) {
        if (!column.is_object() || !column.value("name", data_proxy::Json{}).is_string())
            throw std::invalid_argument("Arrow table schema contains an invalid column descriptor");
        const auto name = column.at("name").get<std::string>();
        if (name.empty() || !names.insert(name).second)
            throw std::invalid_argument("Arrow table schema contains an empty or duplicate column name: " + name);
        const auto type = column.value("type", std::string{});
        std::shared_ptr<arrow::DataType> arrow_type;
        if (type == "string" || type == "object") arrow_type = arrow::utf8();
        else if (type == "integer") arrow_type = arrow::int64();
        else if (type == "real" || type == "number") arrow_type = arrow::float64();
        else if (type == "boolean") arrow_type = arrow::boolean();
        else if (type == "timestamp") arrow_type = arrow::timestamp(arrow::TimeUnit::MICRO);
        else if (type == "binary") arrow_type = arrow::binary();
        else if (type == "decimal") {
            const auto precision = column.value("precision", 0);
            const auto scale = column.value("scale", -1);
            if (precision < 1 || precision > 38 || scale < 0 || scale > precision)
                throw std::invalid_argument("decimal column requires valid precision and scale: " + name);
            arrow_type = arrow::decimal128(precision, scale);
        }
        else if (type == "array") {
            const auto item = column.value("items", data_proxy::Json::object());
            if (!item.is_object())
                throw std::invalid_argument("Arrow table list column has invalid item schema: " + name);
            const auto item_type = item.value("type", std::string{});
            if (item_type == "integer") arrow_type = arrow::list(arrow::int64());
            else if (item_type == "number" || item_type == "real") arrow_type = arrow::list(arrow::float64());
            else if (item_type == "string") arrow_type = arrow::list(arrow::utf8());
            else throw std::invalid_argument("unsupported Arrow table list item type '" + item_type + "' in column " + name);
        } else {
            throw std::invalid_argument("unsupported Arrow table type '" + type + "' in column " + name);
        }
        fields.push_back(arrow::field(name, std::move(arrow_type), column.value("nullable", true)));
    }
    return arrow::schema(std::move(fields));
}

}  // namespace streamfind
