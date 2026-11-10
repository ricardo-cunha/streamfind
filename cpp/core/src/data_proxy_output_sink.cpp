#include "streamfind/data_proxy_output_sink.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/api.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace streamfind::output_detail {
void require_arrow(const arrow::Status &status) {
    if (!status.ok()) throw std::runtime_error(status.ToString());
}

class ArrowFileWriter {
public:
    ArrowFileWriter(std::intptr_t native_handle, const Json &schema) {
#ifdef _WIN32
        const auto descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(native_handle), _O_BINARY);
        if (descriptor < 0) throw std::runtime_error("cannot adapt worker result handle for Arrow");
        auto opened = arrow::io::FileOutputStream::Open(descriptor);
#else
        auto opened = arrow::io::FileOutputStream::Open(static_cast<int>(native_handle));
#endif
        if (!opened.ok()) throw std::runtime_error(opened.status().ToString());
        sink_ = *opened;
        schema_descriptor_ = schema;
        schema_ = schema_from_descriptor(schema);
    }

    void append(const Json &rows) {
        constexpr std::size_t max_estimated_batch_bytes = 8 * 1024 * 1024;
        Json bounded_rows = Json::array();
        std::size_t estimated_bytes = 0;
        for (const auto &row : rows) {
            const auto row_bytes = estimate_json_bytes(row);
            if (!bounded_rows.empty() && row_bytes > max_estimated_batch_bytes -
                    std::min(estimated_bytes, max_estimated_batch_bytes)) {
                append_batch(bounded_rows);
                bounded_rows = Json::array();
                estimated_bytes = 0;
            }
            bounded_rows.push_back(row);
            estimated_bytes = row_bytes > std::numeric_limits<std::size_t>::max() - estimated_bytes
                ? std::numeric_limits<std::size_t>::max() : estimated_bytes + row_bytes;
            if (estimated_bytes >= max_estimated_batch_bytes) {
                // A single unusually large row is emitted alone. The file-backed
                // contract has no message-size rejection; memory is bounded by
                // that row's own payload plus one Arrow batch.
                append_batch(bounded_rows);
                bounded_rows = Json::array();
                estimated_bytes = 0;
            }
        }
        if (!bounded_rows.empty()) append_batch(bounded_rows);
    }

    Json diagnostics() const {
        return {
            {"oversized_row_count", oversized_row_count_},
            {"largest_encoded_row_bytes", largest_encoded_row_bytes_},
            {"largest_estimated_field", largest_estimated_field_},
            {"largest_row_identity", largest_row_identity_}
        };
    }

    void finish() {
        if (!writer_) {
            auto created = arrow::ipc::MakeFileWriter(sink_, schema_);
            if (!created.ok()) throw std::runtime_error(created.status().ToString());
            writer_ = *created;
        }
        require_arrow(writer_->Close());
        writer_.reset();
        require_arrow(sink_->Close());
        sink_.reset();
    }

private:
    static constexpr std::size_t max_arrow_batch_bytes = 8 * 1024 * 1024;

    static std::size_t estimate_json_bytes(const Json &value) {
        if (value.is_string()) return value.get_ref<const std::string &>().size() + 8;
        if (value.is_object()) {
            std::size_t bytes = 16;
            for (const auto &[name, child] : value.items()) {
                const auto child_bytes = estimate_json_bytes(child);
                const auto item_bytes = name.size() + child_bytes + 8;
                bytes = item_bytes > std::numeric_limits<std::size_t>::max() - bytes
                    ? std::numeric_limits<std::size_t>::max() : bytes + item_bytes;
            }
            return bytes;
        }
        if (value.is_array()) {
            std::size_t bytes = 16;
            for (const auto &child : value) {
                const auto child_bytes = estimate_json_bytes(child);
                bytes = child_bytes > std::numeric_limits<std::size_t>::max() - bytes
                    ? std::numeric_limits<std::size_t>::max() : bytes + child_bytes;
            }
            return bytes;
        }
        return 16;
    }

    void append_batch(const Json &rows) {
        const auto encoded = data_proxy::arrow_ipc_encode_rows(rows, schema_descriptor_);
        if (encoded.size() > max_arrow_batch_bytes && rows.size() > 1) {
            // The JSON-size estimate is deliberately conservative, but nested
            // Arrow arrays can still exceed it. Split by the encoded Arrow
            // size before writing so every multi-row record batch respects the
            // byte budget. A single large row remains allowed and is emitted
            // alone because table rows are atomic at the operation API.
            const auto midpoint = rows.size() / 2;
            Json first = Json::array();
            Json second = Json::array();
            first.get_ref<Json::array_t &>().reserve(midpoint);
            second.get_ref<Json::array_t &>().reserve(rows.size() - midpoint);
            for (std::size_t index = 0; index < rows.size(); ++index)
                (index < midpoint ? first : second).push_back(rows.at(index));
            append_batch(first);
            append_batch(second);
            return;
        }
        if (rows.size() == 1 && encoded.size() > max_arrow_batch_bytes) {
            ++oversized_row_count_;
            if (encoded.size() > largest_encoded_row_bytes_) {
                largest_encoded_row_bytes_ = encoded.size();
                std::string largest_field;
                std::size_t largest_field_bytes = 0;
                Json identity = Json::object();
                const auto &row = rows.front();
                if (row.is_object()) {
                    for (const auto &[name, value] : row.items()) {
                        const auto field_bytes = estimate_json_bytes(value);
                        if (field_bytes > largest_field_bytes) {
                            largest_field = name;
                            largest_field_bytes = field_bytes;
                        }
                        if (name == "row_key" || name == "feature" || name == "analysis" ||
                            name == "feature_component")
                            identity[name] = value;
                    }
                }
                largest_estimated_field_ = {
                    {"name", largest_field}, {"estimated_bytes", largest_field_bytes}
                };
                largest_row_identity_ = std::move(identity);
            }
        }
        auto buffer = std::make_shared<arrow::Buffer>(encoded.data(), static_cast<int64_t>(encoded.size()));
        auto source = std::make_shared<arrow::io::BufferReader>(buffer);
        auto opened = arrow::ipc::RecordBatchStreamReader::Open(source);
        if (!opened.ok()) throw std::runtime_error(opened.status().ToString());
        const auto reader = *opened;
        while (true) {
            auto next = reader->Next();
            if (!next.ok()) throw std::runtime_error(next.status().ToString());
            const auto batch = *next;
            if (!batch) break;
            if (!writer_) {
                if (schema_->num_fields() == 0) schema_ = batch->schema();
                else if (!schema_->Equals(*batch->schema()))
                    throw std::runtime_error("table output batch does not match its semantic Arrow schema");
                auto created = arrow::ipc::MakeFileWriter(sink_, schema_);
                if (!created.ok()) throw std::runtime_error(created.status().ToString());
                writer_ = *created;
            }
            if (!schema_->Equals(*batch->schema()))
                throw std::runtime_error("table output batch schema changed within one result");
            require_arrow(writer_->WriteRecordBatch(*batch));
        }
    }
    static std::shared_ptr<arrow::Schema> schema_from_descriptor(const Json &columns) {
        std::vector<std::shared_ptr<arrow::Field>> fields;
        if (columns.is_array()) for (const auto &column : columns) {
            if (!column.is_object()) continue;
            const auto name = column.value("name", std::string{});
            if (name.empty()) continue;
            const auto type = column.value("type", std::string{"string"});
            std::shared_ptr<arrow::DataType> arrow_type = arrow::utf8();
            if (type == "integer") arrow_type = arrow::int64();
            else if (type == "real" || type == "number") arrow_type = arrow::float64();
            else if (type == "boolean") arrow_type = arrow::boolean();
            else if (type == "array") {
                const auto item_type = column.value("items", Json::object()).value("type", std::string{"number"});
                if (item_type == "integer") arrow_type = arrow::list(arrow::int64());
                else if (item_type == "number" || item_type == "real") arrow_type = arrow::list(arrow::float64());
                else if (item_type == "string") arrow_type = arrow::list(arrow::utf8());
                else throw std::invalid_argument("unsupported Arrow table list item type: " + item_type);
            }
            fields.push_back(arrow::field(name, arrow_type, column.value("nullable", true)));
        }
        return arrow::schema(std::move(fields));
    }

    std::shared_ptr<arrow::io::FileOutputStream> sink_;
    std::shared_ptr<arrow::Schema> schema_;
    Json schema_descriptor_{Json::array()};
    std::shared_ptr<arrow::ipc::RecordBatchWriter> writer_;
    std::uint64_t oversized_row_count_{0};
    std::uint64_t largest_encoded_row_bytes_{0};
    Json largest_estimated_field_{Json::object()};
    Json largest_row_identity_{Json::object()};
};

void write_handle_bytes(std::intptr_t handle, std::uint64_t offset,
                        const unsigned char *data, std::size_t size) {
    std::size_t written = 0;
#ifdef _WIN32
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(reinterpret_cast<HANDLE>(handle), position, nullptr, FILE_BEGIN))
        throw std::runtime_error("cannot position worker output handle");
#endif
    while (written < size) {
#ifdef _WIN32
        DWORD count = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size - written, 0x7ffff000));
        if (!WriteFile(reinterpret_cast<HANDLE>(handle), data + written, chunk, &count, nullptr) || count == 0)
            throw std::runtime_error("cannot write worker output handle");
        written += count;
#else
        const auto count = ::pwrite(static_cast<int>(handle), data + written, size - written,
                                     static_cast<off_t>(offset + written));
        if (count <= 0) throw std::runtime_error("cannot write worker output handle");
        written += static_cast<std::size_t>(count);
#endif
    }
}
void close_output_handle(std::intptr_t handle) noexcept {
#ifdef _WIN32
    if (handle != -1) CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    if (handle >= 0) ::close(static_cast<int>(handle));
#endif
}
}  // namespace streamfind::output_detail

namespace streamfind {

DataProxyOutputSink::DataProxyOutputSink(Request request,
                                         std::string session_id,
                                         std::string run_id,
                                         std::string operation_id,
                                         std::string operation_instance,
                                         Json navigation_columns_by_contract,
                                         ReceiveWriter receive_writer)
    : request_(std::move(request)), session_id_(std::move(session_id)),
      run_id_(std::move(run_id)), operation_id_(std::move(operation_id)),
      operation_instance_(std::move(operation_instance)),
      navigation_columns_by_contract_(std::move(navigation_columns_by_contract)),
      receive_writer_(std::move(receive_writer)) {
    if (!request_) throw std::invalid_argument("data-proxy output sink requires a request callback");
    if (!receive_writer_) throw std::invalid_argument("data-proxy output sink requires a leased writer channel");
}

DataProxyOutputSink::~DataProxyOutputSink() {
    for (auto &[_, lease] : leases_) output_detail::close_output_handle(lease.writer);
}

std::string DataProxyOutputSink::next_request_id() {
    static std::atomic_uint64_t sequence{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream output;
    output << "output-request-" << std::hex << now << '-' << sequence.fetch_add(1, std::memory_order_relaxed);
    return output.str();
}

std::string DataProxyOutputSink::create_result_id(const Json &descriptor) const {
    const auto slug = [](std::string value, const std::string &fallback) {
        std::string result;
        result.reserve(value.size());
        for (const auto character : value) {
            if (std::isalnum(static_cast<unsigned char>(character))) result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
            else if (result.empty() || result.back() != '-') result.push_back('-');
        }
        while (!result.empty() && result.back() == '-') result.pop_back();
        if (result.empty()) result = fallback;
        if (result.size() > 48) result.resize(48);
        return result;
    };
    const auto operation = slug(operation_id_, "operation");
    const auto node = slug(operation_instance_, "unknown-node");
    const auto contract = slug(descriptor.value("contract_id", descriptor.value("output_port", std::string{"output"})), "output");
    return "result-" + operation + "-node-" + node + "-" + contract + "-" + next_request_id().substr(std::string("output-request-").size());
}

Json DataProxyOutputSink::request(const std::string &method,
                                  std::string lease_id,
                                  Json payload) {
    payload["method"] = method;
    const auto response_json = request_(data_proxy::request(
        next_request_id(), session_id_, run_id_, operation_id_, operation_instance_,
        std::move(lease_id), std::move(payload)));
    const auto response = data_proxy::Envelope::from_json(response_json);
    if (!response.error_code.empty())
        throw std::runtime_error(response.error_code + ": " + response.error_message);
    if (!response.payload.is_object())
        throw std::runtime_error("data-proxy output response payload is not an object");
    return response.payload;
}

Json DataProxyOutputSink::begin(const Json &descriptor) {
    if (!descriptor.is_object()) throw std::invalid_argument("output descriptor must be an object");
    auto payload = descriptor;
    if (!payload.contains("result_id")) payload["result_id"] = create_result_id(payload);
    if (!payload.contains("run_id")) payload["run_id"] = run_id_;
    if (!payload.contains("format")) payload["format"] = "binary";
    payload["storage"] = "published";
    const auto contract_id = payload.value("contract_id", std::string{});
    if (!payload.contains("navigation_columns") && navigation_columns_by_contract_.contains(contract_id))
        payload["navigation_columns"] = navigation_columns_by_contract_.at(contract_id);
    const auto kind = payload.value("kind", std::string{});
    const auto format = payload.value("format", std::string{"binary"});
    const auto schema = payload.value("schema", Json::array());
    const auto navigation_columns = payload.value("navigation_columns", Json::array());
    const auto output_port = payload.value("output_port", std::string{});
    const auto result_name = payload.value("result_name", operation_id_ + " / node " + operation_instance_);
    payload["result_name"] = result_name;
    const auto response = request("allocate_result", {}, std::move(payload));
    const auto lease_id = response.value("lease_id", std::string{});
    const auto result_id = response.value("result_id", std::string{});
    if (lease_id.empty() || result_id.empty()) throw std::runtime_error("data-proxy did not return an output lease");
    const auto writer = receive_writer_();
    if (writer == -1) throw std::runtime_error("service did not provide the leased output handle");
    Lease lease{lease_id, result_id, kind, format, 0, 0, 0, writer, nullptr, schema,
                navigation_columns, contract_id, output_port, result_name};
    if (kind == "table")
        lease.arrow_writer = std::make_unique<output_detail::ArrowFileWriter>(writer, schema);
    else
        lease.writer = writer;
    leases_.emplace(lease_id, std::move(lease));
    return {{"output_handle", lease_id}, {"result_id", result_id}, {"delivery", "service"}};
}

void DataProxyOutputSink::write(const std::string &output_handle,
                                std::uint64_t sequence,
                                const std::vector<unsigned char> &batch) {
    const auto iterator = leases_.find(output_handle);
    if (iterator == leases_.end()) throw std::invalid_argument("output handle is not active");
    if (sequence != iterator->second.next_sequence)
        throw std::invalid_argument("output batches must be written sequentially");
    if (iterator->second.format == "arrow-ipc-file")
        throw std::invalid_argument("Arrow table outputs must be written as row batches");
    if (!batch.empty()) output_detail::write_handle_bytes(iterator->second.writer,
        iterator->second.next_offset, batch.data(), batch.size());
    iterator->second.next_offset += batch.size();
    ++iterator->second.next_sequence;
}

void DataProxyOutputSink::write_rows(const std::string &output_handle,
                                     std::uint64_t sequence,
                                     const Json &rows) {
    if (!rows.is_array()) throw std::invalid_argument("table output rows must be an array");
    auto iterator = leases_.find(output_handle);
    if (iterator == leases_.end()) throw std::invalid_argument("output handle is not active");
    if (sequence != iterator->second.next_sequence)
        throw std::invalid_argument("output row batches must be written sequentially");

    if (iterator->second.schema.empty() && !rows.empty() && rows.front().is_object()) {
        auto type_name = [](const Json &value) {
            if (value.is_boolean()) return std::string("boolean");
            if (value.is_number_integer()) return std::string("integer");
            if (value.is_number_float()) return std::string("real");
            if (value.is_array()) return std::string("array");
            if (value.is_object()) return std::string("object");
            return std::string("string");
        };
        for (const auto &[name, value] : rows.front().items())
            iterator->second.schema.push_back({{"name", name}, {"type", type_name(value)}});
    }

    if (iterator->second.format != "arrow-ipc-file" || !iterator->second.arrow_writer)
        throw std::invalid_argument("table result does not have an Arrow IPC file writer");
    iterator->second.arrow_writer->append(rows);
    iterator->second.row_count += rows.size();
    ++iterator->second.next_sequence;
}

Json DataProxyOutputSink::finish(const std::string &output_handle, const Json &metadata) {
    const auto iterator = leases_.find(output_handle);
    if (iterator == leases_.end()) throw std::invalid_argument("output handle is not active");
    if (!metadata.is_object()) throw std::invalid_argument("output metadata must be an object");
    auto publish_metadata = metadata;
    if (!publish_metadata.contains("kind")) publish_metadata["kind"] = iterator->second.kind;
    if (iterator->second.format == "arrow-ipc-file") {
        if (!iterator->second.arrow_writer)
            throw std::runtime_error("Arrow result writer is not active");
        iterator->second.arrow_writer->finish();
        const auto writer_diagnostics = iterator->second.arrow_writer->diagnostics();
        if (writer_diagnostics.value("oversized_row_count", std::uint64_t{0}) > 0)
            publish_metadata["output_diagnostics"] = writer_diagnostics;
        iterator->second.arrow_writer.reset();
        iterator->second.writer = -1;
    } else if (iterator->second.next_offset == 0) {
        // An operation may allocate an output and finish it without emitting
        // a batch (for example, an empty input produces an empty table).  The
        // result store still needs a real payload so publication and later
        // reads can distinguish an empty result from a missing result.
        write(output_handle, 0, {});
    }
    if (!publish_metadata.contains("schema") && !iterator->second.schema.empty())
        publish_metadata["schema"] = iterator->second.schema;
    if (!publish_metadata.contains("navigation_columns") && !iterator->second.navigation_columns.empty())
        publish_metadata["navigation_columns"] = iterator->second.navigation_columns;
    if (!publish_metadata.contains("producer_operation")) publish_metadata["producer_operation"] = operation_id_;
    if (!publish_metadata.contains("producer_instance")) publish_metadata["producer_instance"] = operation_instance_;
    if (!publish_metadata.contains("contract_id") && !iterator->second.contract_id.empty())
        publish_metadata["contract_id"] = iterator->second.contract_id;
    if (!publish_metadata.contains("output_port") && !iterator->second.output_port.empty())
        publish_metadata["output_port"] = iterator->second.output_port;
    if (!publish_metadata.contains("result_name")) publish_metadata["result_name"] = iterator->second.result_name;
    if (!publish_metadata.contains("row_count")) publish_metadata["row_count"] = iterator->second.row_count;
    if (iterator->second.arrow_writer) iterator->second.arrow_writer.reset();
    else output_detail::close_output_handle(iterator->second.writer);
    iterator->second.writer = -1;
    const auto result = request("publish_result", iterator->second.lease_id,
                               {{"metadata", publish_metadata}, {"lease_id", iterator->second.lease_id}});
    leases_.erase(iterator);
    return result;
}

void DataProxyOutputSink::abort(const std::string &output_handle, const std::string &reason) {
    const auto iterator = leases_.find(output_handle);
    if (iterator == leases_.end()) return;
    if (iterator->second.arrow_writer) iterator->second.arrow_writer.reset();
    else output_detail::close_output_handle(iterator->second.writer);
    iterator->second.writer = -1;
    request("abort_result", iterator->second.lease_id,
            {{"reason", reason}, {"lease_id", iterator->second.lease_id}});
    leases_.erase(iterator);
}

}  // namespace streamfind
