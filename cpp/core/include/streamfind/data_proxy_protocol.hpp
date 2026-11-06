#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <string_view>
#include <memory>

#include <nlohmann/json.hpp>

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

/** Encode a bounded JSON row batch as an Arrow IPC stream. */
STREAMFIND_CORE_API std::vector<unsigned char> arrow_ipc_encode_rows(const Json &rows);
/** Encode a row batch against its semantic column schema when one is available. */
STREAMFIND_CORE_API std::vector<unsigned char> arrow_ipc_encode_rows(const Json &rows,
                                                                     const Json &schema);
/** Decode an Arrow IPC row batch produced by arrow_ipc_encode_rows. */
STREAMFIND_CORE_API Json arrow_ipc_decode_rows(const std::vector<unsigned char> &bytes);
/** Decode only selected Arrow columns while preserving the row count. */
STREAMFIND_CORE_API Json arrow_ipc_decode_rows(const std::vector<unsigned char> &bytes,
                                               const std::vector<std::string> &columns);

/** Sequential bounded reader for a standard Arrow IPC file on an owned OS handle. */
class STREAMFIND_CORE_API ArrowFileBatchReader {
public:
    explicit ArrowFileBatchReader(std::intptr_t owned_native_handle);
    ~ArrowFileBatchReader();
    ArrowFileBatchReader(const ArrowFileBatchReader &) = delete;
    ArrowFileBatchReader &operator=(const ArrowFileBatchReader &) = delete;
    Json read(std::uint64_t offset, std::uint64_t limit,
              const std::vector<std::string> &columns = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace streamfind::data_proxy
