#include "streamfind/plugin_abi.h"
#include "streamfind/sdk/capability_registry.hpp"
#include "streamfind/sdk/plugin_host_access.hpp"
#include "operations/operations.hpp"

#include <cstring>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace streamfind::sensors::dynamic_detail {

using Json = nlohmann::json;

void report_error(const streamfind_plugin_host_api *host, const std::string &message) {
    if (host != nullptr && host->report_error != nullptr)
        host->report_error(message.data(), static_cast<uint32_t>(message.size()), host->user_data);
}

streamfind_plugin_status invoke(
    void *execution_context, const char *request_json, uint32_t request_size,
    streamfind_plugin_buffer *result_json, void *user_data) {
    if (request_json == nullptr || result_json == nullptr || user_data == nullptr)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    const auto *host = static_cast<const streamfind_plugin_host_api *>(user_data);
    try {
        const auto request = Json::parse(std::string(request_json, request_size));
        const auto capability = request.at("capability_id").get<std::string>();
        const auto *binding = operations::capabilities().find(capability);
        if (binding == nullptr) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
        sdk::PluginHostAccess access(*host, execution_context);
        auto parameters = request.value("parameters", Json::object());
        parameters["_inputs"] = request.value("inputs", Json::object());
        const auto response = binding->handler(access, parameters);
        const auto payload = response.dump();
        if (payload.size() > std::numeric_limits<uint32_t>::max())
            return STREAMFIND_PLUGIN_ERROR;
        if (host->allocate == nullptr || result_json == nullptr) return STREAMFIND_PLUGIN_ERROR;
        auto *buffer = static_cast<char *>(host->allocate(
            payload.size(), alignof(char), host->user_data));
        if (buffer == nullptr) return STREAMFIND_PLUGIN_ERROR;
        std::memcpy(buffer, payload.data(), payload.size());
        result_json->data = buffer;
        result_json->size = static_cast<uint32_t>(payload.size());
        return STREAMFIND_PLUGIN_OK;
    } catch (const std::exception &error) {
        report_error(host, error.what());
        return STREAMFIND_PLUGIN_ERROR;
    }
}

void release_buffer(streamfind_plugin_buffer *buffer, void *user_data) {
    if (buffer == nullptr || buffer->data == nullptr || user_data == nullptr) return;
    const auto *host = static_cast<const streamfind_plugin_host_api *>(user_data);
    if (host->deallocate != nullptr)
        host->deallocate(const_cast<char *>(buffer->data), buffer->size,
                         alignof(char), host->user_data);
    buffer->data = nullptr;
    buffer->size = 0;
}

streamfind_plugin_status register_plugin(
    const streamfind_plugin_host_api *host, streamfind_plugin_api *plugin, void *) {
    if (host == nullptr || plugin == nullptr) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    plugin->module_id = "sensors.base";
    plugin->plugin_id = "native.plugin.sensors";
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
    descriptor->plugin_id = "native.plugin.sensors";
    descriptor->plugin_version = STREAMFIND_BUNDLED_PLUGIN_VERSION;
    descriptor->register_plugin = &streamfind::sensors::dynamic_detail::register_plugin;
    descriptor->shutdown_plugin = &streamfind::sensors::dynamic_detail::shutdown_plugin;
    return STREAMFIND_PLUGIN_OK;
}
