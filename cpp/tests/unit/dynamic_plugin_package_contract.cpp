#include <cstdlib>
#include <filesystem>
#include <string>

#include "streamfind/sdk/dynamic_plugin_manager.hpp"

namespace streamfind::stage9_test {

void report_error(const char *, uint32_t, void *) {}

void *allocate(uint64_t size, uint64_t, void *) {
    return std::malloc(static_cast<std::size_t>(size));
}

void deallocate(void *pointer, uint64_t, uint64_t, void *) {
    std::free(pointer);
}

}  // namespace streamfind::stage9_test

int main() {
    auto plugin = streamfind::sdk::load_dynamic_plugin_package(
        std::filesystem::path(STREAMFIND_STAGE9_PACKAGE_DIR));
    if (!plugin.loaded || plugin.manifest.plugin_id != "stage9.fixture") {
        return 1;
    }

    streamfind_plugin_host_api host{};
    host.struct_size = sizeof(host);
    host.abi_major = STREAMFIND_PLUGIN_ABI_MAJOR;
    host.abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
    host.report_error = &streamfind::stage9_test::report_error;
    host.allocate = &streamfind::stage9_test::allocate;
    host.deallocate = &streamfind::stage9_test::deallocate;

    if (streamfind::sdk::register_dynamic_plugin(plugin, host) != STREAMFIND_PLUGIN_OK) {
        return 2;
    }

    streamfind::MethodRegistry methods;
    streamfind::OperationRegistry operations;
    streamfind::sdk::register_dynamic_plugin_capabilities(
        plugin,
        streamfind::Json::array({streamfind::Json{
            {"kind", "operation"},
            {"canonical_id", "stage9.ping"},
            {"label", "Ping"},
            {"definition", "A dynamic plugin ping operation."},
            {"domain", "stage9"},
            {"executable", true},
            {"parameters", streamfind::Json::array()},
            {"effects", streamfind::Json{{"reads", streamfind::Json::array()},
                                           {"writes", streamfind::Json::array()}}}}}),
        methods, operations);
    if (operations.find("stage9.ping") == nullptr) {
        return 3;
    }
    if (plugin.plugin.invoke == nullptr || plugin.plugin.release_buffer == nullptr) {
        return 4;
    }

    streamfind_plugin_buffer response{};
    const std::string request = R"({"action":"ping"})";
    if (plugin.plugin.invoke(nullptr, request.data(), static_cast<uint32_t>(request.size()),
                             &response, plugin.plugin.user_data) != STREAMFIND_PLUGIN_OK) {
        return 5;
    }
    const std::string body(response.data, response.size);
    if (body.find("stage9.fixture") == std::string::npos) {
        return 6;
    }
    plugin.plugin.release_buffer(&response, plugin.plugin.user_data);
    if (response.data != nullptr || response.size != 0) {
        return 7;
    }
    return 0;
}
