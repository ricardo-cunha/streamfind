#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "streamfind/plugin_abi.h"

int main() {
    static_assert(std::is_standard_layout_v<streamfind_plugin_descriptor>);
    static_assert(std::is_standard_layout_v<streamfind_plugin_host_api>);
    static_assert(std::is_standard_layout_v<streamfind_plugin_api>);
    static_assert(std::is_standard_layout_v<streamfind_plugin_buffer>);
    static_assert(sizeof(streamfind_plugin_status) == sizeof(std::int32_t));

    streamfind_plugin_descriptor descriptor{};
    descriptor.abi_major = STREAMFIND_PLUGIN_ABI_MAJOR;
    descriptor.abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
    descriptor.struct_size = sizeof(descriptor);
    descriptor.plugin_id = "stage9.contract";
    descriptor.plugin_version = "0.2.0";
    descriptor.register_plugin = nullptr;
    descriptor.shutdown_plugin = nullptr;

    streamfind_plugin_api plugin{};
    plugin.struct_size = sizeof(plugin);
    plugin.abi_major = STREAMFIND_PLUGIN_ABI_MAJOR;
    plugin.invoke = nullptr;
    plugin.release_buffer = nullptr;

    return descriptor.abi_major == STREAMFIND_PLUGIN_ABI_MAJOR &&
                   descriptor.struct_size == sizeof(descriptor)
               ? 0
               : 1;
}
