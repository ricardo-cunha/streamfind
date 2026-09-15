#include "error.hpp"

#include "streamfind/plugin_abi.h"

namespace streamfind::raman::utils {

void report_error(const streamfind_plugin_host_api *host, const std::string &message) {
    if (host != nullptr && host->report_error != nullptr)
        host->report_error(message.data(), static_cast<uint32_t>(message.size()), host->user_data);
}

}  // namespace streamfind::raman::utils
