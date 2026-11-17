#include "streamfind/data_proxy_protocol.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
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

std::shared_ptr<arrow::RecordBatch> ArrowFileBatchReader::read_typed(
    std::uint64_t offset, std::uint64_t limit,
    const std::vector<std::string> &columns) {
    if (limit == 0 || limit > 16384)
        throw std::invalid_argument("Arrow file batch limit must be between 1 and 16384 rows");
    if (offset < impl_->row_cursor)
        throw std::invalid_argument("Arrow file reader only supports forward sequential reads");
    while (!impl_->batch || impl_->batch_row >= impl_->batch->num_rows()) {
        if (impl_->next_batch >= impl_->reader->num_record_batches()) return nullptr;
        auto next = impl_->reader->ReadRecordBatch(impl_->next_batch++);
        if (!next.ok()) throw std::runtime_error(next.status().ToString());
        impl_->batch = *next;
        impl_->batch_row = 0;
    }
    while (impl_->row_cursor < offset) {
        const auto skipped = std::min<std::uint64_t>(
            offset - impl_->row_cursor,
            static_cast<std::uint64_t>(impl_->batch->num_rows() - impl_->batch_row));
        impl_->batch_row += static_cast<std::int64_t>(skipped);
        impl_->row_cursor += skipped;
        if (impl_->batch_row >= impl_->batch->num_rows()) {
            while (impl_->next_batch < impl_->reader->num_record_batches()) {
                auto next = impl_->reader->ReadRecordBatch(impl_->next_batch++);
                if (!next.ok()) throw std::runtime_error(next.status().ToString());
                impl_->batch = *next;
                impl_->batch_row = 0;
                if (impl_->batch->num_rows() != 0) break;
            }
            if (impl_->batch_row >= impl_->batch->num_rows() &&
                impl_->next_batch >= impl_->reader->num_record_batches()) return nullptr;
        }
    }
    if (!impl_->batch || impl_->batch_row >= impl_->batch->num_rows()) return nullptr;
    const auto count = std::min<std::uint64_t>(
        limit, static_cast<std::uint64_t>(impl_->batch->num_rows() - impl_->batch_row));
    std::shared_ptr<arrow::RecordBatch> projected = impl_->batch;
    if (!columns.empty()) {
        std::vector<int> indices;
        indices.reserve(columns.size());
        for (const auto &name : columns) {
            const auto index = impl_->batch->schema()->GetFieldIndex(name);
            if (index < 0) throw std::invalid_argument("Arrow result has no requested column: " + name);
            indices.push_back(index);
        }
        auto selected = impl_->batch->SelectColumns(indices);
        if (!selected.ok()) throw std::runtime_error(selected.status().ToString());
        projected = *selected;
    }
    auto result = projected->Slice(impl_->batch_row, static_cast<std::int64_t>(count));
    impl_->batch_row += static_cast<std::int64_t>(count);
    impl_->row_cursor += count;
    return result;
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
