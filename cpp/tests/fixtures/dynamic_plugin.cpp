#include <cstring>
#include <string>

#include "streamfind/plugin_abi.h"

namespace streamfind::stage9_fixture {

streamfind_plugin_status invoke(
    void *,
    const char *request_json,
    uint32_t request_size,
    streamfind_plugin_buffer *result_json,
    void *user_data) {
    if (request_json == nullptr || result_json == nullptr || user_data == nullptr) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    }
    const std::string request(request_json, request_size);
    const std::string response =
        request == R"({"action":"ping"})"
            ? R"({"status":"ok","plugin":"stage9.fixture"})"
            : R"({"status":"error","message":"unknown request"})";
    const auto *host = static_cast<const streamfind_plugin_host_api *>(user_data);
    auto *buffer = static_cast<char *>(host->allocate(response.size(), alignof(char), host->user_data));
    if (buffer == nullptr) {
        return STREAMFIND_PLUGIN_ERROR;
    }
    std::memcpy(buffer, response.data(), response.size());
    result_json->data = buffer;
    result_json->size = static_cast<uint32_t>(response.size());
    return STREAMFIND_PLUGIN_OK;
}

void release_buffer(streamfind_plugin_buffer *buffer, void *user_data) {
    if (buffer == nullptr || buffer->data == nullptr || user_data == nullptr) {
        return;
    }
    const auto *host = static_cast<const streamfind_plugin_host_api *>(user_data);
    host->deallocate(const_cast<char *>(buffer->data), buffer->size, alignof(char), host->user_data);
    buffer->data = nullptr;
    buffer->size = 0;
}

streamfind_plugin_status register_plugin(
    const streamfind_plugin_host_api *host,
    streamfind_plugin_api *plugin,
    void *) {
    if (host == nullptr || plugin == nullptr) {
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    }
    plugin->module_id = "stage9.base";
    plugin->domain_id = "stage9";
    plugin->invoke = &invoke;
    plugin->release_buffer = &release_buffer;
    plugin->user_data = const_cast<streamfind_plugin_host_api *>(host);
    return STREAMFIND_PLUGIN_OK;
}

void shutdown_plugin(streamfind_plugin_api *, void *) {}

}  // namespace streamfind::stage9_fixture

extern "C" STREAMFIND_PLUGIN_EXPORT streamfind_plugin_status
streamfind_plugin_get_descriptor(
    uint32_t requested_abi_major,
    uint32_t requested_abi_minor,
    streamfind_plugin_descriptor *descriptor) {
    if (descriptor == nullptr || requested_abi_major != STREAMFIND_PLUGIN_ABI_MAJOR ||
        requested_abi_minor > STREAMFIND_PLUGIN_ABI_MINOR) {
        return STREAMFIND_PLUGIN_INCOMPATIBLE_ABI;
    }
    *descriptor = {};
    descriptor->struct_size = sizeof(*descriptor);
    descriptor->abi_major = STREAMFIND_PLUGIN_ABI_MAJOR;
    descriptor->abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
    descriptor->plugin_id = "stage9.fixture";
    descriptor->plugin_version = "0.2.0";
    descriptor->register_plugin = &streamfind::stage9_fixture::register_plugin;
    descriptor->shutdown_plugin = &streamfind::stage9_fixture::shutdown_plugin;
    return STREAMFIND_PLUGIN_OK;
}
