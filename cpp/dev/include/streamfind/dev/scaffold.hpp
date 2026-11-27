#pragma once

#include "streamfind/dev/command_protocol.hpp"

namespace streamfind::dev {

int scaffold_plugin(const Request &request);
int add_plugin_operation(const Request &request);
int build_plugin(const Request &request);
int test_plugin(const Request &request);
int validate_plugin(const Request &request);
int package_plugin(const Request &request);
int install_plugin(const Request &request);
int uninstall_plugin(const Request &request);
int deactivate_plugin(const Request &request);
int activate_plugin(const Request &request);

} // namespace streamfind::dev
