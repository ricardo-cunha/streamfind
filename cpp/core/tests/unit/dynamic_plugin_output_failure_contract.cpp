#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "streamfind/operation_execution_context.hpp"
#include "streamfind/sdk/dynamic_plugin_manager.hpp"
#include "streamfind/sdk/plugin_data_service.hpp"

namespace streamfind::dynamic_plugin_failure_test {

struct PluginState {
    std::size_t release_calls{0};
};

class RecordingSink final : public OperationOutputSink {
public:
    Json begin(const Json &descriptor) override {
        if (descriptor.value("kind", std::string{}) != "json")
            throw std::runtime_error("test expected JSON output allocation");
        const auto handle = "output-" + std::to_string(next_handle++);
        allocated.push_back(handle);
        return {{"output_handle", handle}, {"result_id", "result-" + std::to_string(next_handle - 1)}};
    }

    void write(const std::string &handle, std::uint64_t,
               const std::vector<unsigned char> &) override {
        if (std::find(allocated.begin(), allocated.end(), handle) == allocated.end())
            throw std::runtime_error("write used an unknown output handle");
    }

    Json finish(const std::string &handle, const Json &) override {
        ++finish_calls;
        if (finish_calls == 2)
            throw std::runtime_error("injected failure finalizing the second output");
        return {{"result_id", "published-first-result"}, {"state", "published"}};
    }

    void abort(const std::string &handle, const std::string &) override {
        aborted.push_back(handle);
    }

    std::size_t next_handle{1};
    std::size_t finish_calls{0};
    std::vector<std::string> allocated;
    std::vector<std::string> aborted;
};

class Context final : public OperationExecutionContext {
public:
    explicit Context(RecordingSink &sink) : sink_(sink) {}
    std::string operation_instance() const override { return "7"; }
    int workflow_revision() const override { return 4; }
    Json query(std::string_view) override { throw std::runtime_error("unexpected query"); }
    void execute(std::string_view) override { throw std::runtime_error("unexpected execute"); }
    void log(std::string_view) override {}
    Json resolve_inputs(const Json &inputs) override { return inputs; }
    Json request_data(const data_proxy::Envelope &) override { throw std::runtime_error("unexpected proxy request"); }
    std::string data_proxy_session_id() const override { return "test-session"; }
    std::string data_proxy_run_id() const override { return "unique-worker-run"; }
    bool has_output_sink() const override { return true; }
    std::string computation_key() const override { return {}; }
    OperationOutputSink &outputs() override { return sink_; }
    void report_progress(double, std::string_view) override {}
    void report_event(std::string_view, const Json &) override {}
    bool cancellation_requested() const override { return false; }

private:
    RecordingSink &sink_;
};

streamfind_plugin_status invoke_twice(void *execution_context, const char *, uint32_t,
                                      streamfind_plugin_buffer *response, void *) {
    auto *context = static_cast<sdk::PluginDataServiceContext *>(execution_context);
    const std::string payload = "{}";
    const auto emit = [&](const char *contract) {
        return context->emit_result(context, contract,
            static_cast<uint32_t>(std::char_traits<char>::length(contract)),
            payload.data(), payload.size(), nullptr);
    };
    const auto first = emit("test.json.first");
    if (first != STREAMFIND_PLUGIN_OK) return first;
    static constexpr char failure_response[] = "{}";
    response->data = failure_response;
    response->size = 2;
    return emit("test.json.second");
}

void release_empty_buffer(streamfind_plugin_buffer *buffer, void *user_data) {
    auto *state = static_cast<PluginState *>(user_data);
    if (state != nullptr) ++state->release_calls;
    if (buffer != nullptr) {
        buffer->data = nullptr;
        buffer->size = 0;
    }
}

void run() {
    sdk::DynamicPluginLoadResult plugin;
    PluginState plugin_state;
    plugin.loaded = true;
    plugin.plugin.invoke = &invoke_twice;
    plugin.plugin.release_buffer = &release_empty_buffer;
    plugin.plugin.user_data = &plugin_state;

    MethodRegistry methods;
    OperationRegistry operations;
    sdk::register_dynamic_plugin_capabilities(plugin, Json::array({Json{
        {"kind", "operation"},
        {"canonical_id", "test.partial_json_outputs"},
        {"label", "Partial JSON outputs"},
        {"plugin", "test"},
        {"executable", true},
        {"parameters", Json::array()},
        {"input_ports", Json::array()},
        {"output_ports", Json::array({
            Json{{"id", "first"}, {"semantic_contract", "test.json.first"},
                 {"cardinality", "one"}, {"data_kind", "json"}, {"representations", {"json"}}},
            Json{{"id", "second"}, {"semantic_contract", "test.json.second"},
                 {"cardinality", "one"}, {"data_kind", "json"}, {"representations", {"json"}}}
        })}
    }}), methods, operations);

    auto *operation = operations.find("test.partial_json_outputs");
    if (operation == nullptr) throw std::runtime_error("dynamic operation was not registered");
    RecordingSink sink;
    Context context(sink);
    bool invocation_failed = false;
    try {
        static_cast<void>(operation->run_context(context, Json::object(), "7", Json::object()));
    } catch (const std::exception &error) {
        invocation_failed = true;
        if (std::string(error.what()).find("injected failure finalizing the second output") == std::string::npos)
            throw std::runtime_error("dynamic invocation discarded the output publication failure cause");
    }
    if (!invocation_failed || sink.finish_calls != 2 || sink.allocated.size() != 2 ||
        sink.aborted.size() != 1 || sink.aborted.front() != "output-2" || plugin_state.release_calls != 1)
        throw std::runtime_error("failure after publishing the first JSON output did not abort the unfinished sibling");
}

}  // namespace streamfind::dynamic_plugin_failure_test

int main() {
    try {
        streamfind::dynamic_plugin_failure_test::run();
        std::cout << "dynamic plugin partial-output failure contract passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
