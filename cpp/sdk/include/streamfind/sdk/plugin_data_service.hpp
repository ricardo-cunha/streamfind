#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "streamfind/project.hpp"
#include "streamfind/plugin_abi.h"
#include "streamfind/export.hpp"

namespace streamfind::sdk {

struct STREAMFIND_SDK_API PluginDataServiceContext {
    using DataProxyRequest = std::function<Json(const data_proxy::Envelope &)>;
    using TableBatchEmitter = std::function<streamfind_plugin_status(
        void *, const char *, uint32_t, struct ArrowArray *, struct ArrowSchema *, void *)>;
    using ResultEmitter = std::function<streamfind_plugin_status(
        void *, const char *, uint32_t, const char *, uint64_t, void *)>;
    std::string operation_instance;
    std::string data_proxy_operation_id;
    std::function<bool()> cancellation_requested;
    std::function<void(double, std::string_view)> progress;
    std::function<void(std::string_view)> report_error;
    std::string data_proxy_session_id;
    std::string data_proxy_run_id;
    // The service-owned proxy is the only execution boundary for table and
    // typed-result access.
    DataProxyRequest data_proxy_request;
    std::function<std::intptr_t()> receive_result_input_handle;
    std::vector<std::string> allowed_result_refs;
    std::unordered_map<std::string, std::unordered_set<std::string>> readable_columns;
    TableBatchEmitter emit_table_batch;
    ResultEmitter emit_result;
};

STREAMFIND_SDK_API streamfind_plugin_status plugin_has_table(void *, const char *, uint32_t, uint8_t *, void *);
STREAMFIND_SDK_API streamfind_plugin_status plugin_read_batch(void *, const char *, uint32_t, const char *const *, uint32_t, uint64_t, uint64_t, streamfind_plugin_consume_batch_fn, void *, void *);
STREAMFIND_SDK_API streamfind_plugin_status plugin_count_rows(void *, const char *, uint32_t, uint64_t *, void *);
STREAMFIND_SDK_API streamfind_plugin_status plugin_emit_table_batch(void *, const char *, uint32_t, struct ArrowArray *, struct ArrowSchema *, void *);
STREAMFIND_SDK_API streamfind_plugin_status plugin_emit_result(void *, const char *, uint32_t, const char *, uint64_t, void *);
STREAMFIND_SDK_API streamfind_plugin_status plugin_report_progress(void *, double, const char *, uint32_t, void *);
STREAMFIND_SDK_API uint8_t plugin_is_cancelled(void *, void *);

}  // namespace streamfind::sdk
