#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <nlohmann/json.hpp>

namespace streamfind::test {

inline void check_arrow_fixture(const arrow::Status &status) {
    if (!status.ok()) throw std::runtime_error(status.ToString());
}

inline std::shared_ptr<arrow::RecordBatch> arrow_batch_fixture(
    const nlohmann::json &schema_json, const nlohmann::json &rows) {
    if (!schema_json.is_array() || !rows.is_array())
        throw std::invalid_argument("Arrow fixture requires schema and row arrays");
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (const auto &column : schema_json) {
        const auto name = column.at("name").get<std::string>();
        const auto type = column.value("type", std::string("string"));
        if (type == "integer") {
            arrow::Int64Builder builder;
            for (const auto &row : rows) {
                const auto value = row.find(name);
                if (value == row.end() || value->is_null()) check_arrow_fixture(builder.AppendNull());
                else check_arrow_fixture(builder.Append(value->get<std::int64_t>()));
            }
            std::shared_ptr<arrow::Array> array; check_arrow_fixture(builder.Finish(&array));
            fields.push_back(arrow::field(name, arrow::int64())); arrays.push_back(std::move(array));
        } else if (type == "real" || type == "number") {
            arrow::DoubleBuilder builder;
            for (const auto &row : rows) {
                const auto value = row.find(name);
                if (value == row.end() || value->is_null()) check_arrow_fixture(builder.AppendNull());
                else check_arrow_fixture(builder.Append(value->get<double>()));
            }
            std::shared_ptr<arrow::Array> array; check_arrow_fixture(builder.Finish(&array));
            fields.push_back(arrow::field(name, arrow::float64())); arrays.push_back(std::move(array));
        } else if (type == "boolean") {
            arrow::BooleanBuilder builder;
            for (const auto &row : rows) {
                const auto value = row.find(name);
                if (value == row.end() || value->is_null()) check_arrow_fixture(builder.AppendNull());
                else check_arrow_fixture(builder.Append(value->get<bool>()));
            }
            std::shared_ptr<arrow::Array> array; check_arrow_fixture(builder.Finish(&array));
            fields.push_back(arrow::field(name, arrow::boolean())); arrays.push_back(std::move(array));
        } else if (type == "array") {
            auto values = std::make_shared<arrow::DoubleBuilder>();
            arrow::ListBuilder builder(arrow::default_memory_pool(), values);
            for (const auto &row : rows) {
                const auto value = row.find(name);
                if (value == row.end() || value->is_null()) { check_arrow_fixture(builder.AppendNull()); continue; }
                check_arrow_fixture(builder.Append());
                for (const auto &item : *value) {
                    if (item.is_null()) check_arrow_fixture(values->AppendNull());
                    else check_arrow_fixture(values->Append(item.get<double>()));
                }
            }
            std::shared_ptr<arrow::Array> array; check_arrow_fixture(builder.Finish(&array));
            fields.push_back(arrow::field(name, arrow::list(arrow::float64()))); arrays.push_back(std::move(array));
        } else {
            arrow::StringBuilder builder;
            for (const auto &row : rows) {
                const auto value = row.find(name);
                if (value == row.end() || value->is_null()) check_arrow_fixture(builder.AppendNull());
                else check_arrow_fixture(builder.Append(value->get<std::string>()));
            }
            std::shared_ptr<arrow::Array> array; check_arrow_fixture(builder.Finish(&array));
            fields.push_back(arrow::field(name, arrow::utf8())); arrays.push_back(std::move(array));
        }
    }
    return arrow::RecordBatch::Make(arrow::schema(std::move(fields)),
        static_cast<std::int64_t>(rows.size()), std::move(arrays));
}

} // namespace streamfind::test
