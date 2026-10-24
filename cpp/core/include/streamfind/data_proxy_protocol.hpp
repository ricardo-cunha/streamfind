#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <string_view>

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

/** Encode bounded binary proxy payloads for transport inside the JSON envelope. */
STREAMFIND_CORE_API std::string base64_encode(const std::vector<unsigned char> &bytes);
/** Decode a bounded binary proxy payload and reject malformed input. */
STREAMFIND_CORE_API std::vector<unsigned char> base64_decode(std::string_view value);

/** Encode a bounded JSON row batch as an Arrow IPC stream. */
STREAMFIND_CORE_API std::vector<unsigned char> arrow_ipc_encode_rows(const Json &rows);
/** Decode an Arrow IPC row batch produced by arrow_ipc_encode_rows. */
STREAMFIND_CORE_API Json arrow_ipc_decode_rows(const std::vector<unsigned char> &bytes);

}  // namespace streamfind::data_proxy
