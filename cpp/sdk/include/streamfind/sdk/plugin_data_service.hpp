#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "streamfind/project_table_store.hpp"
#include "streamfind/plugin_abi.h"
#include "streamfind/export.hpp"

namespace streamfind::sdk {

struct STREAMFIND_SDK_API PluginDataServiceContext {
    ProjectTableStore *tables{nullptr};
    std::atomic_bool *cancelled{nullptr};
    std::function<void(double, std::string_view)> progress;
    std::vector<std::string> allowed_tables;
    std::unordered_map<std::string, std::unordered_set<std::string>> readable_columns;
    std::unordered_map<std::string, std::unordered_set<std::string>> writable_columns;
};

STREAMFIND_SDK_API streamfind_plugin_status plugin_has_table(void *, const char *, uint32_t, uint8_t *, void *);
STREAMFIND_SDK_API streamfind_plugin_status plugin_clear_table(void *, const char *, uint32_t, void *);
STREAMFIND_SDK_API streamfind_plugin_status plugin_read_batch(void *, const char *, uint32_t, const streamfind_plugin_batch_column *, uint32_t, uint64_t, uint64_t, streamfind_plugin_consume_batch_fn, void *, void *);
STREAMFIND_SDK_API streamfind_plugin_status plugin_append_batch(void *, const char *, uint32_t, const streamfind_plugin_batch_column *, uint32_t, uint64_t, void *);
STREAMFIND_SDK_API streamfind_plugin_status plugin_update_batch(void *, const char *, uint32_t, const char *, uint32_t, const streamfind_plugin_batch_column *, uint32_t, uint64_t, uint64_t *, void *);
STREAMFIND_SDK_API streamfind_plugin_status plugin_update_composite_batch(void *, const char *, uint32_t, const streamfind_plugin_batch_column *, uint32_t, const streamfind_plugin_batch_column *, uint32_t, uint64_t, uint64_t *, void *);
STREAMFIND_SDK_API streamfind_plugin_status plugin_delete_batch(void *, const char *, uint32_t, const char *, uint32_t, const streamfind_plugin_batch_column *, uint64_t, uint64_t *, void *);
STREAMFIND_SDK_API streamfind_plugin_status plugin_report_progress(void *, double, const char *, uint32_t, void *);
STREAMFIND_SDK_API uint8_t plugin_is_cancelled(void *, void *);

}  // namespace streamfind::sdk
