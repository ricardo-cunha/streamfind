#pragma once

#include <string>

struct streamfind_plugin_host_api;

namespace streamfind::raman::utils {

void report_error(const streamfind_plugin_host_api *host, const std::string &message);

}  // namespace streamfind::raman::utils
