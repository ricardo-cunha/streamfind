#include "streamfind/plugin_abi.h"
#include "streamfind/sdk/capability_registry.hpp"

#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

namespace streamfind::sensors::dynamic_detail {

using Json = nlohmann::json;

void report_error(const streamfind_plugin_host_api *host, const std::string &message) {
    if (host != nullptr && host->report_error != nullptr)
        host->report_error(message.data(), static_cast<uint32_t>(message.size()), host->user_data);
}

const sdk::CapabilityRegistry &capabilities() {
    static const sdk::CapabilityRegistry registry{};
    return registry;
}

streamfind_plugin_status invoke(
    void *, const char *request_json, uint32_t request_size,
    streamfind_plugin_buffer *result_json, void *user_data) {
    if (request_json == nullptr || result_json == nullptr || user_data == nullptr)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    const auto *host = static_cast<const streamfind_plugin_host_api *>(user_data);
    try {
        const auto request = Json::parse(std::string(request_json, request_size));
        const auto capability = request.at("capability_id").get<std::string>();
        return capabilities().find(capability) == nullptr
                   ? STREAMFIND_PLUGIN_INVALID_ARGUMENT
                   : STREAMFIND_PLUGIN_ERROR;
    } catch (const std::exception &error) {
        report_error(host, error.what());
        return STREAMFIND_PLUGIN_ERROR;
    }
}

void release_buffer(streamfind_plugin_buffer *, void *) {}

streamfind_plugin_status register_plugin(
    const streamfind_plugin_host_api *host, streamfind_plugin_api *plugin, void *) {
    if (host == nullptr || plugin == nullptr) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    plugin->module_id = "sensors.base";
    plugin->domain_id = "sensors";
    plugin->invoke = &invoke;
    plugin->release_buffer = &release_buffer;
    plugin->user_data = const_cast<streamfind_plugin_host_api *>(host);
    return STREAMFIND_PLUGIN_OK;
}

void shutdown_plugin(streamfind_plugin_api *, void *) {}

}  // namespace streamfind::sensors::dynamic_detail

extern "C" STREAMFIND_PLUGIN_EXPORT streamfind_plugin_status
streamfind_plugin_get_descriptor(
    uint32_t requested_abi_major, uint32_t requested_abi_minor,
    streamfind_plugin_descriptor *descriptor) {
    if (descriptor == nullptr || requested_abi_major != STREAMFIND_PLUGIN_ABI_MAJOR ||
        requested_abi_minor > STREAMFIND_PLUGIN_ABI_MINOR)
        return STREAMFIND_PLUGIN_INCOMPATIBLE_ABI;
    *descriptor = {};
    descriptor->struct_size = sizeof(*descriptor);
    descriptor->abi_major = STREAMFIND_PLUGIN_ABI_MAJOR;
    descriptor->abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
    descriptor->plugin_id = "sensors";
    descriptor->plugin_version = "0.2.0";
    descriptor->register_plugin = &streamfind::sensors::dynamic_detail::register_plugin;
    descriptor->shutdown_plugin = &streamfind::sensors::dynamic_detail::shutdown_plugin;
    return STREAMFIND_PLUGIN_OK;
}
