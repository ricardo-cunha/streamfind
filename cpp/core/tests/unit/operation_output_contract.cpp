#include "streamfind/data_proxy_output_sink.hpp"
#include "streamfind/data_proxy_protocol.hpp"
#include "streamfind/operation_output.hpp"

#include <iostream>
#include <stdexcept>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

class RecordingOutputSink final : public streamfind::OperationOutputSink {
public:
    streamfind::Json begin(const streamfind::Json &descriptor) override {
        require(descriptor.value("format", std::string{}) == "arrow-ipc",
                "output descriptor format was not received");
        return { {"output_handle", "output-1"}, {"delivery", "transient"} };
    }

    void write(const std::string &output_handle, std::uint64_t sequence,
               const std::vector<unsigned char> &batch) override {
        require(output_handle == "output-1", "output handle was not preserved");
        require(sequence == batches, "output batches were not ordered");
        bytes += batch.size();
        ++batches;
    }

    void write_rows(const std::string &output_handle, std::uint64_t sequence,
                    const streamfind::Json &rows) override {
        const auto bytes = streamfind::data_proxy::arrow_ipc_encode_rows(rows);
        write(output_handle, sequence, bytes);
    }

    streamfind::Json finish(const std::string &output_handle,
                            const streamfind::Json &metadata) override {
        require(output_handle == "output-1", "finish used the wrong output handle");
        return { {"output_handle", output_handle}, {"bytes", bytes}, {"metadata", metadata} };
    }

    void abort(const std::string &output_handle, const std::string &reason) override {
        aborted_handle = output_handle;
        abort_reason = reason;
    }

    std::uint64_t batches{0};
    std::size_t bytes{0};
    std::string aborted_handle;
    std::string abort_reason;
};

int main() {
    try {
        RecordingOutputSink sink;
        const auto handle = sink.begin({{"kind", "table"}, {"format", "arrow-ipc"},
                                        {"media_type", "application/vnd.apache.arrow.stream"}});
        sink.write(handle.at("output_handle"), 0, {1, 2, 3});
        sink.write(handle.at("output_handle"), 1, {4, 5});
        const auto result = sink.finish(handle.at("output_handle"), {{"persistent", false}});
        require(result.at("bytes") == 5, "output sink did not receive all bytes");
        require(result.at("metadata").at("persistent") == false,
                "output metadata was not preserved");
        sink.abort("output-2", "test cleanup");
        require(sink.aborted_handle == "output-2", "output abort was not routed");

        streamfind::DataProxyOutputSink proxy_sink(
            [](const streamfind::data_proxy::Envelope &request) {
                const auto method = request.payload.value("method", std::string{});
                if (method == "allocate_result") {
                    return streamfind::data_proxy::response(request,
                        {{"lease_id", "lease-1"}, {"result_id", "result-1"}}).to_json();
                }
                return streamfind::data_proxy::response(request, {{"state", "unexpected"}}).to_json();
            },
            "session-1", "run-1", "operation-1", "instance-1", streamfind::Json::object(),
            [] { return std::intptr_t{-1}; });
        bool rejected_missing_handle = false;
        try { static_cast<void>(proxy_sink.begin({{"format", "arrow-ipc"}})); }
        catch (const std::runtime_error &) { rejected_missing_handle = true; }
        require(rejected_missing_handle, "output sink accepted a result without its leased writer handle");
        std::cout << "operation output contract passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
