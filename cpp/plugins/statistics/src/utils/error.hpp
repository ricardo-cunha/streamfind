#pragma once

#include "streamfind/plugin_abi.h"

#include <string>

namespace streamfind::statistics::utils {

void report_error(const streamfind_plugin_host_api *host, const std::string &message);

}  // namespace streamfind::statistics::utils
