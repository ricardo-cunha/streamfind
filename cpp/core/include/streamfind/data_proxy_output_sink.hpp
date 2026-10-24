#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "streamfind/data_proxy_protocol.hpp"
#include "streamfind/operation_output.hpp"

namespace streamfind {

/** Operation output sink backed by the service data-proxy protocol. */
class STREAMFIND_CORE_API DataProxyOutputSink final : public OperationOutputSink {
public:
    using Request = std::function<Json(const data_proxy::Envelope &)>;

    DataProxyOutputSink(Request request,
                        std::string session_id,
                        std::string run_id,
                        std::string operation_id,
                        std::string operation_instance,
                        bool persist_outputs = true);

    Json begin(const Json &descriptor) override;
    void write(const std::string &output_handle,
               std::uint64_t sequence,
               const std::vector<unsigned char> &batch) override;
    void write_rows(const std::string &output_handle,
                    std::uint64_t sequence,
                    const Json &rows) override;
    Json finish(const std::string &output_handle,
                const Json &metadata = Json::object()) override;
    void abort(const std::string &output_handle,
               const std::string &reason) override;

private:
    struct Lease {
        std::string lease_id;
        std::string result_id;
        std::string kind;
        std::string format;
        std::uint64_t next_offset{0};
        std::uint64_t next_sequence{0};
        std::uint64_t row_count{0};
        Json schema{Json::array()};
    };

    Json request(const std::string &method,
                 std::string lease_id,
                 Json payload);
    static std::string next_request_id();

    Request request_;
    std::string session_id_;
    std::string run_id_;
    std::string operation_id_;
    std::string operation_instance_;
    bool persist_outputs_{true};
    std::unordered_map<std::string, Lease> leases_;
};

}  // namespace streamfind
