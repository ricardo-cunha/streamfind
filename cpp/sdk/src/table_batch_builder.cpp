#include "streamfind/sdk/table_batch_builder.hpp"

#include <limits>
#include <stdexcept>
#include <utility>
#include <arrow/api.h>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace streamfind::sdk {

TableBatchBuilder::TableBatchBuilder(std::vector<Column> schema) {
    if (schema.empty()) throw std::invalid_argument("typed table batch schema must not be empty");
    columns_.reserve(schema.size());
    for (auto &column : schema) {
        if (column.name.empty()) throw std::invalid_argument("typed table batch column name must not be empty");
        if (column.name.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("typed table batch column name exceeds the ABI limit");
        switch (column.type) {
        case STREAMFIND_PLUGIN_COLUMN_UTF8:
        case STREAMFIND_PLUGIN_COLUMN_INT64:
        case STREAMFIND_PLUGIN_COLUMN_FLOAT64:
        case STREAMFIND_PLUGIN_COLUMN_BOOL:
        case STREAMFIND_PLUGIN_COLUMN_TIMESTAMP:
        case STREAMFIND_PLUGIN_COLUMN_DECIMAL:
        case STREAMFIND_PLUGIN_COLUMN_BINARY:
            break;
        default:
            throw std::invalid_argument("typed table batch builder received an unsupported scalar type");
        }
        if (column.is_list && column.type != STREAMFIND_PLUGIN_COLUMN_UTF8)
            throw std::invalid_argument("typed table list root must use the UTF-8 ABI type");
        if (column.is_list && column.child_type != STREAMFIND_PLUGIN_COLUMN_UTF8 &&
            column.child_type != STREAMFIND_PLUGIN_COLUMN_INT64 &&
            column.child_type != STREAMFIND_PLUGIN_COLUMN_FLOAT64)
            throw std::invalid_argument("typed table list has an unsupported child type");
        ColumnValues values;
        values.schema = std::move(column);
        if (values.schema.is_list) values.offsets.push_back(0);
        columns_.push_back(std::move(values));
    }
}

std::uint64_t TableBatchBuilder::row_count() const noexcept { return row_count_; }

void TableBatchBuilder::append_row(const PluginProjectAccess::BatchView &batch,
                                   std::uint64_t source_row,
                                   const std::map<std::string, Cell> &overrides) {
    if (source_row >= batch.row_count())
        throw std::out_of_range("typed table source row is out of range");
    std::vector<Cell> values;
    values.reserve(columns_.size());
    for (const auto &column : columns_) {
        const auto override_value = overrides.find(column.schema.name);
        if (override_value != overrides.end()) {
            values.push_back(override_value->second);
            continue;
        }
        const auto index = batch.column_index(column.schema.name);
        if (batch.is_null(index, source_row)) {
            values.emplace_back(std::monostate{});
            continue;
        }
        if (column.schema.is_list != batch.is_list(index))
            throw std::invalid_argument("typed table copy list shape does not match its output schema");
        if (column.schema.is_list) {
            if (batch.list_child_type(index) != column.schema.child_type)
                throw std::invalid_argument("typed table copy child type does not match its output schema");
            std::vector<ListCell> items;
            const auto size = batch.list_size(index, source_row);
            if (size > std::numeric_limits<std::size_t>::max())
                throw std::length_error("typed table source list exceeds addressable memory");
            items.reserve(static_cast<std::size_t>(size));
            for (std::uint64_t item = 0; item < size; ++item) {
                if (batch.list_is_null(index, source_row, item)) {
                    items.emplace_back(std::monostate{});
                    continue;
                }
                switch (column.schema.child_type) {
                case STREAMFIND_PLUGIN_COLUMN_UTF8:
                    items.emplace_back(std::string(batch.list_text(index, source_row, item)));
                    break;
                case STREAMFIND_PLUGIN_COLUMN_INT64:
                    items.emplace_back(batch.list_integer(index, source_row, item));
                    break;
                case STREAMFIND_PLUGIN_COLUMN_FLOAT64:
                    items.emplace_back(batch.list_real(index, source_row, item));
                    break;
                default: throw std::invalid_argument("typed table copy has an unsupported child type");
                }
            }
            values.emplace_back(std::move(items));
            continue;
        }
        if (batch.type(index) != column.schema.type)
            throw std::invalid_argument("typed table copy scalar type does not match its output schema");
        switch (column.schema.type) {
        case STREAMFIND_PLUGIN_COLUMN_UTF8:
        case STREAMFIND_PLUGIN_COLUMN_TIMESTAMP:
        case STREAMFIND_PLUGIN_COLUMN_DECIMAL:
        case STREAMFIND_PLUGIN_COLUMN_BINARY:
            values.emplace_back(std::string(batch.text(index, source_row))); break;
        case STREAMFIND_PLUGIN_COLUMN_INT64: values.emplace_back(batch.integer(index, source_row)); break;
        case STREAMFIND_PLUGIN_COLUMN_FLOAT64: values.emplace_back(batch.real(index, source_row)); break;
        case STREAMFIND_PLUGIN_COLUMN_BOOL: values.emplace_back(batch.boolean(index, source_row)); break;
        default: throw std::invalid_argument("typed table copy has an unsupported scalar type");
        }
    }
    append_row(values);
}

void TableBatchBuilder::append_row(const std::vector<Cell> &values) {
    if (values.size() != columns_.size())
        throw std::invalid_argument("typed table batch row does not match its schema");
    if (row_count_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("typed table batch row count overflow");
    for (std::size_t index = 0; index < columns_.size(); ++index) {
        if (std::holds_alternative<std::monostate>(values[index])) continue;
        const auto valid_type = columns_[index].schema.is_list
            ? std::holds_alternative<std::vector<ListCell>>(values[index])
            :
            (columns_[index].schema.type == STREAMFIND_PLUGIN_COLUMN_UTF8 &&
             std::holds_alternative<std::string>(values[index])) ||
            ((columns_[index].schema.type == STREAMFIND_PLUGIN_COLUMN_TIMESTAMP ||
              columns_[index].schema.type == STREAMFIND_PLUGIN_COLUMN_DECIMAL ||
              columns_[index].schema.type == STREAMFIND_PLUGIN_COLUMN_BINARY) &&
             std::holds_alternative<std::string>(values[index])) ||
            (columns_[index].schema.type == STREAMFIND_PLUGIN_COLUMN_INT64 &&
             std::holds_alternative<std::int64_t>(values[index])) ||
            (columns_[index].schema.type == STREAMFIND_PLUGIN_COLUMN_FLOAT64 &&
             std::holds_alternative<double>(values[index])) ||
            (columns_[index].schema.type == STREAMFIND_PLUGIN_COLUMN_BOOL &&
             std::holds_alternative<bool>(values[index]));
        if (!valid_type)
            throw std::invalid_argument("typed table batch cell type does not match its column schema: " +
                                        columns_[index].schema.name);
        if (const auto *text = std::get_if<std::string>(&values[index]);
            text != nullptr && text->size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("typed table batch text cell exceeds the ABI limit");
        if (columns_[index].schema.is_list &&
            std::holds_alternative<std::vector<ListCell>>(values[index])) {
            const auto &items = std::get<std::vector<ListCell>>(values[index]);
            const auto child_type = columns_[index].schema.child_type;
            for (const auto &item : items) {
                if (std::holds_alternative<std::monostate>(item)) continue;
                const bool matches =
                    (child_type == STREAMFIND_PLUGIN_COLUMN_UTF8 && std::holds_alternative<std::string>(item)) ||
                    (child_type == STREAMFIND_PLUGIN_COLUMN_INT64 && std::holds_alternative<std::int64_t>(item)) ||
                    (child_type == STREAMFIND_PLUGIN_COLUMN_FLOAT64 && std::holds_alternative<double>(item));
                if (!matches) throw std::invalid_argument("typed table list item does not match its child type");
                if (const auto *text = std::get_if<std::string>(&item);
                    text != nullptr && text->size() > std::numeric_limits<std::uint32_t>::max())
                    throw std::length_error("typed table list text item exceeds the ABI limit");
            }
            const auto existing_children = child_type == STREAMFIND_PLUGIN_COLUMN_UTF8
                ? columns_[index].child_strings.size()
                : child_type == STREAMFIND_PLUGIN_COLUMN_INT64
                    ? columns_[index].child_integers.size() : columns_[index].child_reals.size();
            if (items.size() > std::numeric_limits<std::uint64_t>::max() - existing_children)
                throw std::overflow_error("typed table list child count overflow");
        }
    }
    const auto next_row = row_count_;
    for (std::size_t index = 0; index < columns_.size(); ++index) {
        auto &column = columns_[index];
        const auto &value = values[index];
        const bool valid = !std::holds_alternative<std::monostate>(value);
        if (column.schema.is_list) {
            if (valid) {
                for (const auto &item : std::get<std::vector<ListCell>>(value)) {
                    const bool child_valid = !std::holds_alternative<std::monostate>(item);
                    const auto child_index = column.schema.child_type == STREAMFIND_PLUGIN_COLUMN_UTF8
                        ? column.child_strings.size()
                        : column.schema.child_type == STREAMFIND_PLUGIN_COLUMN_INT64
                            ? column.child_integers.size() : column.child_reals.size();
                    const auto child_byte = static_cast<std::size_t>(child_index / 8);
                    if (child_byte == column.child_validity.size()) column.child_validity.push_back(0);
                    if (child_valid) column.child_validity[child_byte] |= static_cast<std::uint8_t>(1u << (child_index % 8));
                    switch (column.schema.child_type) {
                    case STREAMFIND_PLUGIN_COLUMN_UTF8:
                        column.child_strings.push_back(child_valid ? std::get<std::string>(item) : std::string{});
                        break;
                    case STREAMFIND_PLUGIN_COLUMN_INT64:
                        column.child_integers.push_back(child_valid ? std::get<std::int64_t>(item) : 0);
                        break;
                    case STREAMFIND_PLUGIN_COLUMN_FLOAT64:
                        column.child_reals.push_back(child_valid ? std::get<double>(item) : 0.0);
                        break;
                    default: throw std::logic_error("typed table list child type changed after validation");
                    }
                }
            }
            const auto child_count = column.schema.child_type == STREAMFIND_PLUGIN_COLUMN_UTF8
                ? column.child_strings.size()
                : column.schema.child_type == STREAMFIND_PLUGIN_COLUMN_INT64
                    ? column.child_integers.size() : column.child_reals.size();
            column.offsets.push_back(static_cast<std::uint64_t>(child_count));
            const auto byte = static_cast<std::size_t>(next_row / 8);
            if (byte == column.validity.size()) column.validity.push_back(0);
            if (valid) column.validity[byte] |= static_cast<std::uint8_t>(1u << (next_row % 8));
            continue;
        }
        const auto byte = static_cast<std::size_t>(next_row / 8);
        if (byte == column.validity.size()) column.validity.push_back(0);
        if (valid) column.validity[byte] |= static_cast<std::uint8_t>(1u << (next_row % 8));
        switch (column.schema.type) {
        case STREAMFIND_PLUGIN_COLUMN_UTF8:
        case STREAMFIND_PLUGIN_COLUMN_TIMESTAMP:
        case STREAMFIND_PLUGIN_COLUMN_DECIMAL:
        case STREAMFIND_PLUGIN_COLUMN_BINARY:
            column.strings.push_back(valid ? std::get<std::string>(value) : std::string{});
            break;
        case STREAMFIND_PLUGIN_COLUMN_INT64:
            column.integers.push_back(valid ? std::get<std::int64_t>(value) : 0);
            break;
        case STREAMFIND_PLUGIN_COLUMN_FLOAT64:
            column.reals.push_back(valid ? std::get<double>(value) : 0.0);
            break;
        case STREAMFIND_PLUGIN_COLUMN_BOOL:
            column.booleans.push_back(valid && std::get<bool>(value) ? 1 : 0);
            break;
        default:
            throw std::logic_error("typed table batch schema changed after validation");
        }
    }
    ++row_count_;
}

void TableBatchBuilder::emit(PluginProjectAccess &access,
                             const std::string &output_contract_id) const {
    auto require_arrow = [](const arrow::Status &status) {
        if (!status.ok()) throw std::runtime_error(status.ToString());
    };
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    fields.reserve(columns_.size());
    arrays.reserve(columns_.size());
    for (const auto &column : columns_) {
        std::shared_ptr<arrow::DataType> type;
        if (column.schema.is_list) {
            switch (column.schema.child_type) {
            case STREAMFIND_PLUGIN_COLUMN_UTF8: type = arrow::list(arrow::utf8()); break;
            case STREAMFIND_PLUGIN_COLUMN_INT64: type = arrow::list(arrow::int64()); break;
            case STREAMFIND_PLUGIN_COLUMN_FLOAT64: type = arrow::list(arrow::float64()); break;
            default: throw std::invalid_argument("unsupported Arrow list child type");
            }
        } else {
            switch (column.schema.type) {
            case STREAMFIND_PLUGIN_COLUMN_INT64: type = arrow::int64(); break;
            case STREAMFIND_PLUGIN_COLUMN_FLOAT64: type = arrow::float64(); break;
            case STREAMFIND_PLUGIN_COLUMN_BOOL: type = arrow::boolean(); break;
            case STREAMFIND_PLUGIN_COLUMN_TIMESTAMP: type = arrow::timestamp(arrow::TimeUnit::MICRO); break;
            default: type = arrow::utf8(); break;
            }
        }
        fields.push_back(arrow::field(column.schema.name, type, true));
        std::shared_ptr<arrow::Array> output;
        if (column.schema.is_list) {
            std::shared_ptr<arrow::ArrayBuilder> values;
            if (column.schema.child_type == STREAMFIND_PLUGIN_COLUMN_UTF8)
                values = std::make_shared<arrow::StringBuilder>();
            else if (column.schema.child_type == STREAMFIND_PLUGIN_COLUMN_INT64)
                values = std::make_shared<arrow::Int64Builder>();
            else values = std::make_shared<arrow::DoubleBuilder>();
            arrow::ListBuilder builder(arrow::default_memory_pool(), values);
            const auto child_count = static_cast<std::size_t>(column.offsets.empty() ? 0 : column.offsets.back());
            for (std::uint64_t row = 0; row < row_count_; ++row) {
                const bool valid = column.validity.empty() ||
                    (column.validity[row / 8] & static_cast<std::uint8_t>(1u << (row % 8))) != 0;
                if (!valid) { require_arrow(builder.AppendNull()); continue; }
                require_arrow(builder.Append());
                const auto begin = static_cast<std::size_t>(column.offsets[row]);
                const auto end = static_cast<std::size_t>(column.offsets[row + 1]);
                if (end > child_count || begin > end) throw std::invalid_argument("invalid typed list offsets");
                for (std::size_t item = begin; item < end; ++item) {
                    const bool child_valid = column.child_validity.empty() ||
                        (column.child_validity[item / 8] & static_cast<std::uint8_t>(1u << (item % 8))) != 0;
                    if (!child_valid) { require_arrow(values->AppendNull()); continue; }
                    switch (column.schema.child_type) {
                    case STREAMFIND_PLUGIN_COLUMN_UTF8:
                        require_arrow(std::static_pointer_cast<arrow::StringBuilder>(values)->Append(column.child_strings[item])); break;
                    case STREAMFIND_PLUGIN_COLUMN_INT64:
                        require_arrow(std::static_pointer_cast<arrow::Int64Builder>(values)->Append(column.child_integers[item])); break;
                    case STREAMFIND_PLUGIN_COLUMN_FLOAT64:
                        require_arrow(std::static_pointer_cast<arrow::DoubleBuilder>(values)->Append(column.child_reals[item])); break;
                    default: throw std::invalid_argument("unsupported Arrow list child type");
                    }
                }
            }
            require_arrow(builder.Finish(&output));
        } else if (column.schema.type == STREAMFIND_PLUGIN_COLUMN_INT64) {
            arrow::Int64Builder builder;
            for (std::uint64_t row = 0; row < row_count_; ++row) {
                const bool valid = column.validity.empty() || (column.validity[row / 8] & (1u << (row % 8))) != 0;
                require_arrow(valid ? builder.Append(column.integers[row]) : builder.AppendNull());
            }
            require_arrow(builder.Finish(&output));
        } else if (column.schema.type == STREAMFIND_PLUGIN_COLUMN_FLOAT64) {
            arrow::DoubleBuilder builder;
            for (std::uint64_t row = 0; row < row_count_; ++row) {
                const bool valid = column.validity.empty() || (column.validity[row / 8] & (1u << (row % 8))) != 0;
                require_arrow(valid ? builder.Append(column.reals[row]) : builder.AppendNull());
            }
            require_arrow(builder.Finish(&output));
        } else if (column.schema.type == STREAMFIND_PLUGIN_COLUMN_BOOL) {
            arrow::BooleanBuilder builder;
            for (std::uint64_t row = 0; row < row_count_; ++row) {
                const bool valid = column.validity.empty() || (column.validity[row / 8] & (1u << (row % 8))) != 0;
                require_arrow(valid ? builder.Append(column.booleans[row] != 0) : builder.AppendNull());
            }
            require_arrow(builder.Finish(&output));
        } else if (column.schema.type == STREAMFIND_PLUGIN_COLUMN_TIMESTAMP) {
            arrow::TimestampBuilder builder(arrow::timestamp(arrow::TimeUnit::MICRO), arrow::default_memory_pool());
            for (std::uint64_t row = 0; row < row_count_; ++row) {
                const bool valid = column.validity.empty() || (column.validity[row / 8] & (1u << (row % 8))) != 0;
                if (!valid) { require_arrow(builder.AppendNull()); continue; }
                const auto &value = column.strings[row];
                std::tm parsed{};
                std::istringstream input(value);
                input >> std::get_time(&parsed, value.find('T') == std::string::npos ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%dT%H:%M:%S");
                if (input.fail()) throw std::invalid_argument("typed timestamp must use ISO-8601 date-time text");
#ifdef _WIN32
                const auto seconds = _mkgmtime(&parsed);
#else
                const auto seconds = timegm(&parsed);
#endif
                if (seconds < 0) throw std::invalid_argument("typed timestamp is outside supported range");
                require_arrow(builder.Append(static_cast<std::int64_t>(seconds) * 1000000));
            }
            require_arrow(builder.Finish(&output));
        } else {
            arrow::StringBuilder builder;
            for (std::uint64_t row = 0; row < row_count_; ++row) {
                const bool valid = column.validity.empty() || (column.validity[row / 8] & (1u << (row % 8))) != 0;
                require_arrow(valid ? builder.Append(column.strings[row]) : builder.AppendNull());
            }
            require_arrow(builder.Finish(&output));
        }
        arrays.push_back(std::move(output));
    }
    auto batch = arrow::RecordBatch::Make(arrow::schema(std::move(fields)),
        static_cast<std::int64_t>(row_count_), std::move(arrays));
    access.emit_table_batch(output_contract_id, batch);
}
void TableBatchBuilder::clear() {
    row_count_ = 0;
    for (auto &column : columns_) {
        column.strings.clear();
        column.integers.clear();
        column.reals.clear();
        column.booleans.clear();
        column.validity.clear();
        column.offsets.clear();
        if (column.schema.is_list) column.offsets.push_back(0);
        column.child_strings.clear();
        column.child_integers.clear();
        column.child_reals.clear();
        column.child_validity.clear();
    }
}

}  // namespace streamfind::sdk
