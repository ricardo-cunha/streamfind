#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <string_view>
#include <memory>

#include <nlohmann/json.hpp>
#include <arrow/api.h>

#include "streamfind/export.hpp"

namespace streamfind::data_proxy {

using Json = nlohmann::json;

inline constexpr std::string_view kProtocol = "streamfind.data-proxy/v1";

struct STREAMFIND_CORE_API Envelope {
    std::string type;
    std::string request_id;
    std::string session_id;
    std::string run_id;
    std::string operation_id;
    std::string operation_instance;
    std::string lease_id;
    Json payload = Json::object();
    std::string error_code;
    std::string error_message;

    Json to_json() const;
    static Envelope from_json(const Json &value);
    void validate() const;
};

STREAMFIND_CORE_API Envelope request(std::string request_id,
                                     std::string session_id,
                                     std::string run_id,
                                     std::string operation_id,
                                     std::string operation_instance,
                                     std::string lease_id,
                                     Json payload);

STREAMFIND_CORE_API Envelope response(const Envelope &request,
                                      Json payload);

STREAMFIND_CORE_API Envelope failure(const Envelope &request,
                                     std::string error_code,
                                     std::string error_message);

/** Sequential bounded reader for a standard Arrow IPC file on an owned OS handle. */
class STREAMFIND_CORE_API ArrowFileBatchReader {
public:
    explicit ArrowFileBatchReader(std::intptr_t owned_native_handle);
    ~ArrowFileBatchReader();
    ArrowFileBatchReader(const ArrowFileBatchReader &) = delete;
    ArrowFileBatchReader &operator=(const ArrowFileBatchReader &) = delete;
    /** Read one projected native Arrow batch; nullptr means end of file. */
    std::shared_ptr<arrow::RecordBatch> read_typed(
        std::uint64_t offset, std::uint64_t limit,
        const std::vector<std::string> &columns = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace streamfind::data_proxy
