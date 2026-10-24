#include "streamfind/data_proxy_protocol.hpp"

#include <array>
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

namespace streamfind::data_proxy {

std::string base64_encode(const std::vector<unsigned char> &bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto first = bytes[index];
        const auto second = index + 1 < bytes.size() ? bytes[index + 1] : 0;
        const auto third = index + 2 < bytes.size() ? bytes[index + 2] : 0;
        result.push_back(alphabet[first >> 2]);
        result.push_back(alphabet[((first & 0x03u) << 4) | (second >> 4)]);
        result.push_back(index + 1 < bytes.size() ? alphabet[((second & 0x0fu) << 2) | (third >> 6)] : '=');
        result.push_back(index + 2 < bytes.size() ? alphabet[third & 0x3fu] : '=');
    }
    return result;
}

std::vector<unsigned char> base64_decode(std::string_view value) {
    if (value.size() % 4 != 0) throw std::invalid_argument("invalid base64 length");
    std::array<int, 256> lookup{};
    lookup.fill(-1);
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t index = 0; index < alphabet.size(); ++index)
        lookup[static_cast<unsigned char>(alphabet[index])] = static_cast<int>(index);
    std::vector<unsigned char> result;
    result.reserve((value.size() / 4) * 3);
    for (std::size_t index = 0; index < value.size(); index += 4) {
        const auto a = lookup[static_cast<unsigned char>(value[index])];
        const auto b = lookup[static_cast<unsigned char>(value[index + 1])];
        const auto c = value[index + 2] == '=' ? 0 : lookup[static_cast<unsigned char>(value[index + 2])];
        const auto d = value[index + 3] == '=' ? 0 : lookup[static_cast<unsigned char>(value[index + 3])];
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (value[index + 2] == '=' && value[index + 3] != '=') ||
            (index + 4 != value.size() && (value[index + 2] == '=' || value[index + 3] == '=')))
            throw std::invalid_argument("invalid base64 payload");
        result.push_back(static_cast<unsigned char>((a << 2) | (b >> 4)));
        if (value[index + 2] != '=') result.push_back(static_cast<unsigned char>((b << 4) | (c >> 2)));
        if (value[index + 3] != '=') result.push_back(static_cast<unsigned char>((c << 6) | d));
    }
    return result;
}

namespace detail {

enum class ArrowColumnKind { Boolean, Integer, FloatingPoint, String };

ArrowColumnKind classify_arrow_value(const Json &value) {
    if (value.is_boolean()) return ArrowColumnKind::Boolean;
    if (value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<std::uint64_t>() <=
         static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        return ArrowColumnKind::Integer;
    if (value.is_number_float()) return ArrowColumnKind::FloatingPoint;
    return ArrowColumnKind::String;
}

ArrowColumnKind merge_arrow_kinds(ArrowColumnKind current, ArrowColumnKind value) {
    if (current == value) return current;
    if ((current == ArrowColumnKind::Integer && value == ArrowColumnKind::FloatingPoint) ||
        (current == ArrowColumnKind::FloatingPoint && value == ArrowColumnKind::Integer))
        return ArrowColumnKind::FloatingPoint;
    return ArrowColumnKind::String;
}

void append_arrow_error(const arrow::Status &status) {
    if (!status.ok()) throw std::runtime_error(status.ToString());
}

}  // namespace detail

std::vector<unsigned char> arrow_ipc_encode_rows(const Json &rows) {
    if (!rows.is_array()) throw std::invalid_argument("Arrow IPC rows must be an array");

    std::map<std::string, std::size_t> column_indexes;
    for (const auto &row : rows) {
        if (!row.is_object()) throw std::invalid_argument("Arrow IPC rows must contain objects");
        for (const auto &[name, value] : row.items()) {
            if (!column_indexes.contains(name)) column_indexes.emplace(name, column_indexes.size());
        }
    }
    std::vector<std::string> names(column_indexes.size());
    for (const auto &[name, index] : column_indexes) names[index] = name;
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
    const auto schema = arrow::schema(fields);
    const auto batch = arrow::RecordBatch::Make(schema,
                                                static_cast<int64_t>(rows.size()),
                                                arrays);
    auto sink_result = arrow::io::BufferOutputStream::Create();
    if (!sink_result.ok()) throw std::runtime_error(sink_result.status().ToString());
    const auto sink = *sink_result;
    auto writer_result = arrow::ipc::MakeStreamWriter(sink, schema);
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
    auto buffer = std::make_shared<arrow::Buffer>(bytes.data(), bytes.size());
    auto reader = std::make_shared<arrow::io::BufferReader>(buffer);
    auto stream_result = arrow::ipc::RecordBatchStreamReader::Open(reader);
    if (!stream_result.ok()) throw std::runtime_error(stream_result.status().ToString());
    const auto stream = *stream_result;
    Json rows = Json::array();
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
                default:
                    throw std::runtime_error("unsupported StreamFind Arrow IPC column type");
                }
                rows[static_cast<std::size_t>(index)][field->name()] = std::move(value);
            }
        }
    }
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
