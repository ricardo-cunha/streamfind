#include "streamfind/data_proxy_protocol.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace streamfind::data_proxy {

namespace detail {

enum class ArrowColumnKind { Boolean, Integer, FloatingPoint, IntegerList, FloatingPointList, StringList, String };

ArrowColumnKind classify_numeric_list(const Json &value) {
    if (!value.is_array()) return ArrowColumnKind::String;
    auto kind = ArrowColumnKind::IntegerList;
    bool saw_string = false;
    bool saw_number = false;
    for (const auto &item : value) {
        if (item.is_null()) continue;
        if (item.is_string()) {
            if (saw_number) return ArrowColumnKind::String;
            saw_string = true;
            continue;
        }
        if (!item.is_number() || saw_string) return ArrowColumnKind::String;
        saw_number = true;
        if (item.is_number_float()) kind = ArrowColumnKind::FloatingPointList;
    }
    if (saw_string) return ArrowColumnKind::StringList;
    return kind;
}

ArrowColumnKind classify_arrow_value(const Json &value) {
    if (value.is_boolean()) return ArrowColumnKind::Boolean;
    if (value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<std::uint64_t>() <=
         static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        return ArrowColumnKind::Integer;
    if (value.is_number_float()) return ArrowColumnKind::FloatingPoint;
    if (value.is_array()) return classify_numeric_list(value);
    return ArrowColumnKind::String;
}

ArrowColumnKind merge_arrow_kinds(ArrowColumnKind current, ArrowColumnKind value) {
    if (current == value) return current;
    if ((current == ArrowColumnKind::Integer && value == ArrowColumnKind::FloatingPoint) ||
        (current == ArrowColumnKind::FloatingPoint && value == ArrowColumnKind::Integer))
        return ArrowColumnKind::FloatingPoint;
    if ((current == ArrowColumnKind::IntegerList && value == ArrowColumnKind::FloatingPointList) ||
        (current == ArrowColumnKind::FloatingPointList && value == ArrowColumnKind::IntegerList))
        return ArrowColumnKind::FloatingPointList;
    return ArrowColumnKind::String;
}

void append_arrow_error(const arrow::Status &status) {
    if (!status.ok()) throw std::runtime_error(status.ToString());
}

}  // namespace detail

std::vector<unsigned char> arrow_ipc_encode_rows(const Json &rows) {
    return arrow_ipc_encode_rows(rows, Json::array());
}

std::vector<unsigned char> arrow_ipc_encode_rows(const Json &rows, const Json &schema) {
    if (!rows.is_array()) throw std::invalid_argument("Arrow IPC rows must be an array");

    std::vector<std::string> names;
    std::map<std::string, std::size_t> column_indexes;
    if (schema.is_array()) {
        for (const auto &column : schema) {
            if (!column.is_object()) continue;
            const auto name = column.value("name", std::string{});
            if (name.empty() || column_indexes.contains(name)) continue;
            column_indexes.emplace(name, names.size());
            names.push_back(name);
        }
    }
    for (const auto &row : rows) {
        if (!row.is_object()) throw std::invalid_argument("Arrow IPC rows must contain objects");
        for (const auto &[name, value] : row.items()) {
            if (!column_indexes.contains(name)) {
                column_indexes.emplace(name, names.size());
                names.push_back(name);
            }
        }
    }
    std::vector<detail::ArrowColumnKind> kinds(names.size(), detail::ArrowColumnKind::String);
    std::vector<bool> seen(names.size(), false);
    for (const auto &row : rows) {
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (!row.contains(names[index]) || row.at(names[index]).is_null()) continue;
            const auto value_kind = detail::classify_arrow_value(row.at(names[index]));
            kinds[index] = seen[index] ? detail::merge_arrow_kinds(kinds[index], value_kind) : value_kind;
            seen[index] = true;
        }
    }
    if (schema.is_array()) {
        for (const auto &column : schema) {
            if (!column.is_object()) continue;
            const auto name = column.value("name", std::string{});
            const auto found = column_indexes.find(name);
            if (name.empty() || found == column_indexes.end()) continue;
            const auto type = column.value("type", std::string{});
            if (type == "boolean") kinds[found->second] = detail::ArrowColumnKind::Boolean;
            else if (type == "integer") kinds[found->second] = detail::ArrowColumnKind::Integer;
            else if (type == "number" || type == "real") kinds[found->second] = detail::ArrowColumnKind::FloatingPoint;
            else if (type == "array") {
                const auto items = column.value("items", Json::object());
                const auto item_type = items.value("type", std::string{"number"});
                if (item_type == "integer") kinds[found->second] = detail::ArrowColumnKind::IntegerList;
                else if (item_type == "number" || item_type == "real")
                    kinds[found->second] = detail::ArrowColumnKind::FloatingPointList;
                else if (item_type == "string") kinds[found->second] = detail::ArrowColumnKind::StringList;
                else throw std::invalid_argument("unsupported Arrow list item type for column " + name);
            } else if (type == "string" || type == "object") {
                kinds[found->second] = detail::ArrowColumnKind::String;
            }
        }
    }

    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    fields.reserve(names.size());
    arrays.reserve(names.size());
    for (std::size_t column = 0; column < names.size(); ++column) {
        std::shared_ptr<arrow::Array> array;
        std::shared_ptr<arrow::DataType> type;
        switch (kinds[column]) {
        case detail::ArrowColumnKind::Boolean: {
            arrow::BooleanBuilder builder;
            for (const auto &row : rows) {
                if (!row.contains(names[column]) || row.at(names[column]).is_null())
                    detail::append_arrow_error(builder.AppendNull());
                else detail::append_arrow_error(builder.Append(row.at(names[column]).get<bool>()));
            }
            detail::append_arrow_error(builder.Finish(&array));
            type = arrow::boolean();
            break;
        }
        case detail::ArrowColumnKind::Integer: {
            arrow::Int64Builder builder;
            for (const auto &row : rows) {
                if (!row.contains(names[column]) || row.at(names[column]).is_null())
                    detail::append_arrow_error(builder.AppendNull());
                else detail::append_arrow_error(builder.Append(row.at(names[column]).get<std::int64_t>()));
            }
            detail::append_arrow_error(builder.Finish(&array));
            type = arrow::int64();
            break;
        }
        case detail::ArrowColumnKind::FloatingPoint: {
            arrow::DoubleBuilder builder;
            for (const auto &row : rows) {
                if (!row.contains(names[column]) || row.at(names[column]).is_null())
                    detail::append_arrow_error(builder.AppendNull());
                else detail::append_arrow_error(builder.Append(row.at(names[column]).get<double>()));
            }
            detail::append_arrow_error(builder.Finish(&array));
            type = arrow::float64();
            break;
        }
        case detail::ArrowColumnKind::IntegerList: {
            auto child_builder = std::make_shared<arrow::Int64Builder>();
            arrow::ListBuilder builder(arrow::default_memory_pool(), child_builder);
            for (const auto &row : rows) {
                if (!row.contains(names[column]) || row.at(names[column]).is_null()) {
                    detail::append_arrow_error(builder.AppendNull());
                    continue;
                }
                const auto &items = row.at(names[column]);
                if (!items.is_array()) throw std::invalid_argument("Arrow list column changed type within a batch");
                detail::append_arrow_error(builder.Append());
                for (const auto &item : items) {
                    if (item.is_null()) detail::append_arrow_error(child_builder->AppendNull());
                    else if (item.is_number_integer()) detail::append_arrow_error(child_builder->Append(item.get<std::int64_t>()));
                    else throw std::invalid_argument("integer Arrow list contains a non-integer value");
                }
            }
            detail::append_arrow_error(builder.Finish(&array));
            type = arrow::list(arrow::int64());
            break;
        }
        case detail::ArrowColumnKind::FloatingPointList: {
            auto child_builder = std::make_shared<arrow::DoubleBuilder>();
            arrow::ListBuilder builder(arrow::default_memory_pool(), child_builder);
            for (const auto &row : rows) {
                if (!row.contains(names[column]) || row.at(names[column]).is_null()) {
                    detail::append_arrow_error(builder.AppendNull());
                    continue;
                }
                const auto &items = row.at(names[column]);
                if (!items.is_array()) throw std::invalid_argument("Arrow list column changed type within a batch");
                detail::append_arrow_error(builder.Append());
                for (const auto &item : items) {
                    if (item.is_null()) detail::append_arrow_error(child_builder->AppendNull());
                    else if (item.is_number()) detail::append_arrow_error(child_builder->Append(item.get<double>()));
                    else throw std::invalid_argument("numeric Arrow list contains a non-numeric value");
                }
            }
            detail::append_arrow_error(builder.Finish(&array));
            type = arrow::list(arrow::float64());
            break;
        }
        case detail::ArrowColumnKind::StringList: {
            auto child_builder = std::make_shared<arrow::StringBuilder>();
            arrow::ListBuilder builder(arrow::default_memory_pool(), child_builder);
            for (const auto &row : rows) {
                if (!row.contains(names[column]) || row.at(names[column]).is_null()) {
                    detail::append_arrow_error(builder.AppendNull());
                    continue;
                }
                const auto &items = row.at(names[column]);
                if (!items.is_array()) throw std::invalid_argument("Arrow list column changed type within a batch");
                detail::append_arrow_error(builder.Append());
                for (const auto &item : items) {
                    if (item.is_null()) detail::append_arrow_error(child_builder->AppendNull());
                    else if (item.is_string()) detail::append_arrow_error(child_builder->Append(item.get<std::string>()));
                    else throw std::invalid_argument("string Arrow list contains a non-string value");
                }
            }
            detail::append_arrow_error(builder.Finish(&array));
            type = arrow::list(arrow::utf8());
            break;
        }
        case detail::ArrowColumnKind::String: {
            arrow::StringBuilder builder;
            for (const auto &row : rows) {
                if (!row.contains(names[column]) || row.at(names[column]).is_null())
                    detail::append_arrow_error(builder.AppendNull());
                else detail::append_arrow_error(builder.Append(row.at(names[column]).dump()));
            }
            detail::append_arrow_error(builder.Finish(&array));
            type = arrow::utf8();
            break;
        }
        }
        fields.push_back(arrow::field(names[column], type));
        arrays.push_back(std::move(array));
    }
    const auto arrow_schema = arrow::schema(fields);
    const auto batch = arrow::RecordBatch::Make(arrow_schema,
                                                static_cast<int64_t>(rows.size()),
                                                arrays);
    auto sink_result = arrow::io::BufferOutputStream::Create();
    if (!sink_result.ok()) throw std::runtime_error(sink_result.status().ToString());
    const auto sink = *sink_result;
    auto writer_result = arrow::ipc::MakeStreamWriter(sink, arrow_schema);
    if (!writer_result.ok()) throw std::runtime_error(writer_result.status().ToString());
    const auto writer = *writer_result;
    auto status = writer->WriteRecordBatch(*batch);
    if (!status.ok()) throw std::runtime_error(status.ToString());
    status = writer->Close();
    if (!status.ok()) throw std::runtime_error(status.ToString());
    auto buffer_result = sink->Finish();
    if (!buffer_result.ok()) throw std::runtime_error(buffer_result.status().ToString());
    const auto buffer = *buffer_result;
    return std::vector<unsigned char>(buffer->data(), buffer->data() + buffer->size());
}

Json arrow_ipc_decode_rows(const std::vector<unsigned char> &bytes) {
    return arrow_ipc_decode_rows(bytes, {});
}

Json arrow_ipc_decode_rows(const std::vector<unsigned char> &bytes,
                           const std::vector<std::string> &columns) {
    auto buffer = std::make_shared<arrow::Buffer>(bytes.data(), bytes.size());
    auto reader = std::make_shared<arrow::io::BufferReader>(buffer);
    auto stream_result = arrow::ipc::RecordBatchStreamReader::Open(reader);
    if (!stream_result.ok()) throw std::runtime_error(stream_result.status().ToString());
    const auto stream = *stream_result;
    Json rows = Json::array();
    const auto include_column = [&columns](const std::string &name) {
        return columns.empty() || std::find(columns.begin(), columns.end(), name) != columns.end();
    };
    while (true) {
        auto batch_result = stream->Next();
        if (!batch_result.ok()) throw std::runtime_error(batch_result.status().ToString());
        const auto batch = *batch_result;
        if (!batch) break;
        if (rows.empty()) {
            for (int64_t index = 0; index < batch->num_rows(); ++index)
                rows.push_back(Json::object());
        }
        else if (rows.size() != static_cast<std::size_t>(batch->num_rows()))
            throw std::runtime_error("inconsistent Arrow IPC row batch sizes");
        for (int column = 0; column < batch->num_columns(); ++column) {
            const auto &field = batch->schema()->field(column);
            if (!include_column(field->name())) continue;
            const auto values = batch->column(column);
            for (int64_t index = 0; index < values->length(); ++index) {
                if (values->IsNull(index)) {
                    rows[static_cast<std::size_t>(index)][field->name()] = nullptr;
                    continue;
                }
                Json value;
                switch (values->type_id()) {
                case arrow::Type::BOOL:
                    value = std::static_pointer_cast<arrow::BooleanArray>(values)->Value(index);
                    break;
                case arrow::Type::INT64:
                    value = std::static_pointer_cast<arrow::Int64Array>(values)->Value(index);
                    break;
                case arrow::Type::DOUBLE:
                    value = std::static_pointer_cast<arrow::DoubleArray>(values)->Value(index);
                    break;
                case arrow::Type::STRING:
                    value = Json::parse(std::static_pointer_cast<arrow::StringArray>(values)->GetView(index));
                    break;
                case arrow::Type::LIST: {
                    const auto list = std::static_pointer_cast<arrow::ListArray>(values);
                    const auto begin = list->value_offset(index);
                    const auto end = list->value_offset(index + 1);
                    const auto child = list->values();
                    value = Json::array();
                    for (auto child_index = begin; child_index < end; ++child_index) {
                        if (child->IsNull(child_index)) value.push_back(nullptr);
                        else if (child->type_id() == arrow::Type::INT64)
                            value.push_back(std::static_pointer_cast<arrow::Int64Array>(child)->Value(child_index));
                        else if (child->type_id() == arrow::Type::DOUBLE)
                            value.push_back(std::static_pointer_cast<arrow::DoubleArray>(child)->Value(child_index));
                        else if (child->type_id() == arrow::Type::STRING)
                            value.push_back(std::static_pointer_cast<arrow::StringArray>(child)->GetString(child_index));
                        else throw std::runtime_error("unsupported StreamFind Arrow list child type");
                    }
                    break;
                }
                default:
                    throw std::runtime_error("unsupported StreamFind Arrow IPC column type");
                }
                rows[static_cast<std::size_t>(index)][field->name()] = std::move(value);
            }
        }
    }
    return rows;
}

struct ArrowFileBatchReader::Impl {
    std::shared_ptr<arrow::io::ReadableFile> file;
    std::shared_ptr<arrow::ipc::RecordBatchFileReader> reader;
    std::shared_ptr<arrow::RecordBatch> batch;
    int next_batch{0};
    std::int64_t batch_row{0};
    std::uint64_t row_cursor{0};
};

ArrowFileBatchReader::ArrowFileBatchReader(std::intptr_t owned_native_handle)
    : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    const auto descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(owned_native_handle), _O_BINARY);
    if (descriptor < 0) {
        CloseHandle(reinterpret_cast<HANDLE>(owned_native_handle));
        throw std::runtime_error("cannot adapt worker result handle for Arrow reading");
    }
    auto file_result = arrow::io::ReadableFile::Open(descriptor);
#else
    auto file_result = arrow::io::ReadableFile::Open(static_cast<int>(owned_native_handle));
#endif
    if (!file_result.ok()) throw std::runtime_error(file_result.status().ToString());
    impl_->file = *file_result;
    auto reader_result = arrow::ipc::RecordBatchFileReader::Open(impl_->file);
    if (!reader_result.ok()) throw std::runtime_error(reader_result.status().ToString());
    impl_->reader = *reader_result;
}

ArrowFileBatchReader::~ArrowFileBatchReader() = default;

Json ArrowFileBatchReader::read(std::uint64_t offset, std::uint64_t limit,
                                const std::vector<std::string> &columns) {
    if (limit == 0 || limit > 16384)
        throw std::invalid_argument("Arrow file batch limit must be between 1 and 16384 rows");
    if (offset < impl_->row_cursor)
        throw std::invalid_argument("Arrow file reader only supports forward sequential reads");
    Json rows = Json::array();
    while (rows.size() < limit) {
        if (!impl_->batch || impl_->batch_row >= impl_->batch->num_rows()) {
            if (impl_->next_batch >= impl_->reader->num_record_batches()) break;
            auto result = impl_->reader->ReadRecordBatch(impl_->next_batch++);
            if (!result.ok()) throw std::runtime_error(result.status().ToString());
            impl_->batch = *result;
            impl_->batch_row = 0;
        }
        while (impl_->batch_row < impl_->batch->num_rows() && rows.size() < limit) {
            if (impl_->row_cursor++ < offset) { ++impl_->batch_row; continue; }
            Json row = Json::object();
            for (int column = 0; column < impl_->batch->num_columns(); ++column) {
                const auto &field = impl_->batch->schema()->field(column);
                if (!columns.empty() && std::find(columns.begin(), columns.end(), field->name()) == columns.end())
                    continue;
                const auto &array = impl_->batch->column(column);
                if (array->IsNull(impl_->batch_row)) { row[field->name()] = nullptr; continue; }
                switch (array->type_id()) {
                case arrow::Type::BOOL:
                    row[field->name()] = std::static_pointer_cast<arrow::BooleanArray>(array)->Value(impl_->batch_row);
                    break;
                case arrow::Type::INT64:
                    row[field->name()] = std::static_pointer_cast<arrow::Int64Array>(array)->Value(impl_->batch_row);
                    break;
                case arrow::Type::DOUBLE:
                    row[field->name()] = std::static_pointer_cast<arrow::DoubleArray>(array)->Value(impl_->batch_row);
                    break;
                case arrow::Type::STRING: {
                    const auto text = std::static_pointer_cast<arrow::StringArray>(array)->GetView(impl_->batch_row);
                    auto value = Json::parse(text, nullptr, false);
                    row[field->name()] = value.is_discarded() ? Json(std::string(text)) : std::move(value);
                    break;
                }
                default:
                    throw std::runtime_error("unsupported StreamFind Arrow file column type");
                }
            }
            ++impl_->batch_row;
            rows.push_back(std::move(row));
        }
    }
    impl_->row_cursor = std::max<std::uint64_t>(impl_->row_cursor, offset + rows.size());
    return rows;
}

Json Envelope::to_json() const {
    Json value{{"protocol", std::string(kProtocol)},
               {"type", type},
               {"request_id", request_id},
               {"session_id", session_id},
               {"run_id", run_id},
               {"operation_id", operation_id},
               {"operation_instance", operation_instance},
               {"lease_id", lease_id},
               {"payload", payload}};
    if (!error_code.empty()) value["error_code"] = error_code;
    if (!error_message.empty()) value["error_message"] = error_message;
    return value;
}

Envelope Envelope::from_json(const Json &value) {
    if (!value.is_object()) throw std::invalid_argument("data-proxy envelope must be an object");
    if (value.value("protocol", std::string{}) != kProtocol)
        throw std::invalid_argument("unsupported data-proxy protocol");
    Envelope result;
    result.type = value.value("type", std::string{});
    result.request_id = value.value("request_id", std::string{});
    result.session_id = value.value("session_id", std::string{});
    result.run_id = value.value("run_id", std::string{});
    result.operation_id = value.value("operation_id", std::string{});
    result.operation_instance = value.value("operation_instance", std::string{});
    result.lease_id = value.value("lease_id", std::string{});
    result.payload = value.value("payload", Json::object());
    result.error_code = value.value("error_code", std::string{});
    result.error_message = value.value("error_message", std::string{});
    result.validate();
    return result;
}

void Envelope::validate() const {
    if (type != "request" && type != "response" && type != "event")
        throw std::invalid_argument("data-proxy envelope has an invalid type");
    if (request_id.empty()) throw std::invalid_argument("data-proxy envelope requires request_id");
    if (type == "request" && session_id.empty())
        throw std::invalid_argument("data-proxy request requires session_id");
    if (!payload.is_object()) throw std::invalid_argument("data-proxy payload must be an object");
    if (type == "response" && error_code.empty() != error_message.empty())
        throw std::invalid_argument("data-proxy failure must include both error fields");
}

Envelope request(std::string request_id, std::string session_id,
                 std::string run_id, std::string operation_id,
                 std::string operation_instance, std::string lease_id,
                 Json payload) {
    Envelope result{"request", std::move(request_id), std::move(session_id),
                    std::move(run_id), std::move(operation_id),
                    std::move(operation_instance), std::move(lease_id),
                    std::move(payload), {}, {}};
    result.validate();
    return result;
}

Envelope response(const Envelope &request_value, Json payload) {
    request_value.validate();
    Envelope result{"response", request_value.request_id, request_value.session_id,
                    request_value.run_id, request_value.operation_id,
                    request_value.operation_instance, request_value.lease_id,
                    std::move(payload), {}, {}};
    result.validate();
    return result;
}

Envelope failure(const Envelope &request_value, std::string error_code,
                 std::string error_message) {
    request_value.validate();
    if (error_code.empty() || error_message.empty())
        throw std::invalid_argument("data-proxy failure requires error details");
    Envelope result{"response", request_value.request_id, request_value.session_id,
                    request_value.run_id, request_value.operation_id,
                    request_value.operation_instance, request_value.lease_id,
                    Json::object(), std::move(error_code), std::move(error_message)};
    result.validate();
    return result;
}

}  // namespace streamfind::data_proxy
