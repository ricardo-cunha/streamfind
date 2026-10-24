#include "streamfind/data_proxy_output_sink.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace streamfind::detail {

std::string output_base64_encode(const std::vector<unsigned char> &bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto first = bytes[index];
        const auto second = index + 1 < bytes.size() ? bytes[index + 1] : 0;
        const auto third = index + 2 < bytes.size() ? bytes[index + 2] : 0;
        output.push_back(alphabet[first >> 2]);
        output.push_back(alphabet[((first & 0x03) << 4) | (second >> 4)]);
        output.push_back(index + 1 < bytes.size() ? alphabet[((second & 0x0f) << 2) | (third >> 6)] : '=');
        output.push_back(index + 2 < bytes.size() ? alphabet[third & 0x3f] : '=');
    }
    return output;
}

}  // namespace streamfind::detail

namespace streamfind {

DataProxyOutputSink::DataProxyOutputSink(Request request,
                                         std::string session_id,
                                         std::string run_id,
                                         std::string operation_id,
                                         std::string operation_instance,
                                         bool persist_outputs)
    : request_(std::move(request)), session_id_(std::move(session_id)),
      run_id_(std::move(run_id)), operation_id_(std::move(operation_id)),
      operation_instance_(std::move(operation_instance)), persist_outputs_(persist_outputs) {
    if (!request_) throw std::invalid_argument("data-proxy output sink requires a request callback");
}

std::string DataProxyOutputSink::next_request_id() {
    static std::atomic_uint64_t sequence{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream output;
    output << "output-request-" << std::hex << now << '-' << sequence.fetch_add(1, std::memory_order_relaxed);
    return output.str();
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
    if (!payload.contains("result_id")) payload["result_id"] = next_request_id();
    if (!payload.contains("run_id")) payload["run_id"] = run_id_;
    if (!payload.contains("format")) payload["format"] = "binary";
    payload["storage"] = persist_outputs_ ? "published" : "transient";
    const auto kind = payload.value("kind", std::string{});
    const auto format = payload.value("format", std::string{"binary"});
    const auto schema = payload.value("schema", Json::array());
    const auto response = request("allocate_result", {}, std::move(payload));
    const auto lease_id = response.value("lease_id", std::string{});
    const auto result_id = response.value("result_id", std::string{});
    if (lease_id.empty() || result_id.empty()) throw std::runtime_error("data-proxy did not return an output lease");
    Lease lease{lease_id, result_id, kind, format, 0, 0, 0, schema};
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
    constexpr std::size_t max_chunk_bytes = 1024 * 1024;
    if (batch.empty()) {
        request("write_result_chunk", iterator->second.lease_id,
                {{"offset", iterator->second.next_offset}, {"data", ""},
                 {"lease_id", iterator->second.lease_id}});
        ++iterator->second.next_sequence;
        return;
    }
    for (std::size_t begin = 0; begin < batch.size(); begin += max_chunk_bytes) {
        const auto count = std::min(max_chunk_bytes, batch.size() - begin);
        const std::vector<unsigned char> chunk(batch.begin() + static_cast<std::ptrdiff_t>(begin),
                                               batch.begin() + static_cast<std::ptrdiff_t>(begin + count));
        request("write_result_chunk", iterator->second.lease_id,
                {{"offset", iterator->second.next_offset},
                 {"data", detail::output_base64_encode(chunk)},
                 {"lease_id", iterator->second.lease_id}});
        iterator->second.next_offset += count;
        ++iterator->second.next_sequence;
    }
}

void DataProxyOutputSink::write_rows(const std::string &output_handle,
                                     std::uint64_t sequence,
                                     const Json &rows) {
    if (!rows.is_array()) throw std::invalid_argument("table output rows must be an array");
    auto iterator = leases_.find(output_handle);
    if (iterator == leases_.end()) throw std::invalid_argument("output handle is not active");

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

    // The result store deliberately bounds each immutable chunk.  Keep that
    // policy in the runtime: operations submit logical row batches and never
    // need to know the transport or filesystem limit.
    constexpr std::size_t max_chunk_bytes = 1024 * 1024;
    if (rows.empty()) {
        // Empty tables are valid outputs.  Still publish an Arrow frame so
        // the result store has a regular payload file to validate and the
        // downstream reader can distinguish an empty table from a missing
        // result.
        const auto encoded = data_proxy::arrow_ipc_encode_rows(Json::array());
        std::vector<unsigned char> framed(sizeof(std::uint64_t) + encoded.size());
        const auto size = static_cast<std::uint64_t>(encoded.size());
        for (std::size_t index = 0; index < sizeof(size); ++index)
            framed[index] = static_cast<unsigned char>((size >> (index * 8)) & 0xffu);
        std::copy(encoded.begin(), encoded.end(), framed.begin() + sizeof(size));
        if (framed.size() > max_chunk_bytes)
            throw std::invalid_argument("an empty table result exceeds the result chunk limit");
        write(output_handle, sequence, framed);
        return;
    }
    std::size_t begin = 0;
    while (begin < rows.size()) {
        std::size_t end = std::min(rows.size(), begin + std::size_t{1024});
        std::vector<unsigned char> framed;
        for (;;) {
            const Json chunk(rows.begin() + static_cast<std::ptrdiff_t>(begin),
                             rows.begin() + static_cast<std::ptrdiff_t>(end));
            const auto encoded = data_proxy::arrow_ipc_encode_rows(chunk);
            framed.resize(sizeof(std::uint64_t) + encoded.size());
            const auto size = static_cast<std::uint64_t>(encoded.size());
            for (std::size_t index = 0; index < sizeof(size); ++index)
                framed[index] = static_cast<unsigned char>((size >> (index * 8)) & 0xffu);
            std::copy(encoded.begin(), encoded.end(), framed.begin() + sizeof(size));
            if (framed.size() <= max_chunk_bytes || end - begin == 1) break;
            end = begin + std::max<std::size_t>(1, (end - begin) / 2);
        }
        if (framed.size() > max_chunk_bytes)
            throw std::invalid_argument("a single table row exceeds the result chunk limit");
        write(output_handle, iterator->second.next_sequence, framed);
        begin = end;
    }
    iterator->second.row_count += rows.size();
    (void)sequence;
}

Json DataProxyOutputSink::finish(const std::string &output_handle, const Json &metadata) {
    const auto iterator = leases_.find(output_handle);
    if (iterator == leases_.end()) throw std::invalid_argument("output handle is not active");
    if (!metadata.is_object()) throw std::invalid_argument("output metadata must be an object");
    auto publish_metadata = metadata;
    if (iterator->second.next_offset == 0) {
        // An operation may allocate an output and finish it without emitting
        // a batch (for example, an empty input produces an empty table).  The
        // result store still needs a real payload so publication and later
        // reads can distinguish an empty result from a missing result.
        if (iterator->second.format == "arrow-ipc-batches") {
            const auto encoded = data_proxy::arrow_ipc_encode_rows(Json::array());
            std::vector<unsigned char> framed(sizeof(std::uint64_t) + encoded.size());
            const auto size = static_cast<std::uint64_t>(encoded.size());
            for (std::size_t index = 0; index < sizeof(size); ++index)
                framed[index] = static_cast<unsigned char>((size >> (index * 8)) & 0xffu);
            std::copy(encoded.begin(), encoded.end(), framed.begin() + sizeof(size));
            write(output_handle, 0, framed);
        } else {
            write(output_handle, 0, {});
        }
    }
    if (!publish_metadata.contains("schema") && !iterator->second.schema.empty())
        publish_metadata["schema"] = iterator->second.schema;
    if (!publish_metadata.contains("row_count")) publish_metadata["row_count"] = iterator->second.row_count;
    const auto result = request("publish_result", iterator->second.lease_id,
                               {{"metadata", publish_metadata}, {"lease_id", iterator->second.lease_id}});
    leases_.erase(iterator);
    return result;
}

void DataProxyOutputSink::abort(const std::string &output_handle, const std::string &reason) {
    const auto iterator = leases_.find(output_handle);
    if (iterator == leases_.end()) return;
    request("abort_result", iterator->second.lease_id,
            {{"reason", reason}, {"lease_id", iterator->second.lease_id}});
    leases_.erase(iterator);
}

}  // namespace streamfind
