#include "streamfind/data_proxy_output_sink.hpp"
#include "streamfind/result_table_schema.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <ctime>
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
        schema_ = schema_from_descriptor(schema);
    }

    void append(const std::shared_ptr<arrow::RecordBatch> &batch) {
        if (!batch || !schema_ || !batch->schema()->Equals(*schema_))
            throw std::invalid_argument("Arrow record batch does not match its declared result schema");
        if (!writer_) {
            auto created = arrow::ipc::MakeFileWriter(sink_, schema_);
            if (!created.ok()) throw std::runtime_error(created.status().ToString());
            writer_ = *created;
        }
        require_arrow(writer_->WriteRecordBatch(*batch));
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
    static std::shared_ptr<arrow::Schema> schema_from_descriptor(const Json &columns) {
        return result_table_arrow_schema(columns);
    }

    std::shared_ptr<arrow::io::FileOutputStream> sink_;
    std::shared_ptr<arrow::Schema> schema_;
    std::shared_ptr<arrow::ipc::RecordBatchWriter> writer_;
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
    if (!payload.contains("extension")) {
        const auto kind = payload.value("kind", std::string{});
        const auto format = payload.value("format", std::string{"binary"});
        payload["extension"] = kind == "table" && format == "arrow-ipc-file" ? "arrow" : format;
    }
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
    auto descriptor_metadata = payload;
    for (const auto *field : {"result_id", "run_id", "extension", "storage"})
        descriptor_metadata.erase(field);
    const auto response = request("allocate_result", {}, std::move(payload));
    const auto lease_id = response.value("lease_id", std::string{});
    const auto result_id = response.value("result_id", std::string{});
    if (lease_id.empty() || result_id.empty()) throw std::runtime_error("data-proxy did not return an output lease");
    std::intptr_t writer = -1;
    try {
        writer = receive_writer_();
        if (writer == -1) throw std::runtime_error("service did not provide the leased output handle");
    } catch (...) {
        try {
            request("abort_result", lease_id,
                    {{"reason", "worker did not receive the leased output handle"},
                     {"lease_id", lease_id}});
        } catch (...) {
            // Preserve the original handle-receive failure. The service also
            // expires abandoned leases when the owning worker exits.
        }
        throw;
    }
    Lease lease{lease_id, result_id, kind, format, 0, 0, 0, writer, nullptr, schema,
                std::move(descriptor_metadata), navigation_columns, contract_id, output_port, result_name};
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

void DataProxyOutputSink::write_table_batch(
    const std::string &output_handle, std::uint64_t sequence,
    const std::shared_ptr<arrow::RecordBatch> &batch) {
    auto iterator = leases_.find(output_handle);
    if (iterator == leases_.end()) throw std::invalid_argument("output handle is not active");
    if (sequence != iterator->second.next_sequence)
        throw std::invalid_argument("typed table batches must be written sequentially");
    if (iterator->second.format != "arrow-ipc-file" || !iterator->second.arrow_writer)
        throw std::invalid_argument("typed table batch requires an Arrow IPC result");
    if (!batch) throw std::invalid_argument("Arrow table batch is null");
    iterator->second.arrow_writer->append(batch);
    iterator->second.row_count += static_cast<std::uint64_t>(batch->num_rows());
    ++iterator->second.next_sequence;
}

std::uint64_t DataProxyOutputSink::write_query_result(
    const std::string &output_handle, std::uint64_t sequence, std::string_view sql) {
    auto iterator = leases_.find(output_handle);
    if (iterator == leases_.end()) throw std::invalid_argument("output handle is not active");
    if (sequence != iterator->second.next_sequence)
        throw std::invalid_argument("typed SQL result batches must be written sequentially");
    if (iterator->second.kind != "table" || iterator->second.format != "arrow-ipc-file")
        throw std::invalid_argument("direct SQL output requires an Arrow table lease");
    if (iterator->second.row_count != 0 || iterator->second.written_by_service)
        throw std::invalid_argument("direct SQL output must be the first and only output batch");
    if (sql.empty()) throw std::invalid_argument("direct SQL output query is empty");
    const auto response = request("write_query_result", iterator->second.lease_id,
                                  {{"sql", std::string(sql)}});
    const auto rows = response.value("row_count", std::uint64_t{0});
    iterator->second.row_count = rows;
    iterator->second.written_by_service = true;
    ++iterator->second.next_sequence;
    return rows;
}

Json DataProxyOutputSink::finish(const std::string &output_handle, const Json &metadata) {
    const auto iterator = leases_.find(output_handle);
    if (iterator == leases_.end()) throw std::invalid_argument("output handle is not active");
    if (!metadata.is_object()) throw std::invalid_argument("output metadata must be an object");
    auto publish_metadata = iterator->second.descriptor_metadata;
    for (const auto &[name, value] : metadata.items()) publish_metadata[name] = value;
    if (!publish_metadata.contains("kind")) publish_metadata["kind"] = iterator->second.kind;
    if (iterator->second.format == "arrow-ipc-file") {
        if (iterator->second.written_by_service) {
            iterator->second.arrow_writer.reset();
            iterator->second.writer = -1;
        } else {
            if (!iterator->second.arrow_writer)
                throw std::runtime_error("Arrow result writer is not active");
            iterator->second.arrow_writer->finish();
            iterator->second.arrow_writer.reset();
            iterator->second.writer = -1;
        }
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
