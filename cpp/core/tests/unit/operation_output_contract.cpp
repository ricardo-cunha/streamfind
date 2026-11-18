#include "streamfind/data_proxy_output_sink.hpp"
#include "streamfind/data_proxy_protocol.hpp"
#include "streamfind/operation_output.hpp"

#include <iostream>
#include <filesystem>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

static std::intptr_t create_test_writer(const std::filesystem::path &path) {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("could not create output contract fixture");
    return reinterpret_cast<std::intptr_t>(handle);
#else
    const auto descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) throw std::runtime_error("could not create output contract fixture");
    return descriptor;
#endif
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

        streamfind::Json allocation_payload;
        streamfind::Json published_metadata;
        const auto output_path = std::filesystem::current_path() / "streamfind-output-contract.bin";
        streamfind::DataProxyOutputSink metadata_sink(
            [&](const streamfind::data_proxy::Envelope &request) {
                const auto method = request.payload.value("method", std::string{});
                if (method == "allocate_result") {
                    allocation_payload = request.payload;
                    return streamfind::data_proxy::response(request,
                        {{"lease_id", "lease-metadata"}, {"result_id", "result-metadata"}}).to_json();
                }
                if (method == "publish_result") {
                    published_metadata = request.payload.at("metadata");
                    return streamfind::data_proxy::response(request, {{"state", "published"}}).to_json();
                }
                return streamfind::data_proxy::failure(request, "unexpected", "unexpected request").to_json();
            },
            "session-metadata", "run-metadata", "operation-metadata", "instance-metadata",
            streamfind::Json::object(), [&] { return create_test_writer(output_path); });
        const streamfind::Json provenance{{"lineage_inputs", streamfind::Json::array({
            {{"result_id", "source-1"}, {"source_port", "anyTable"}, {"target_port", "leftTable"}}})}};
        const auto metadata_handle = metadata_sink.begin({
            {"kind", "binary"}, {"format", "binary"}, {"provenance", provenance},
            {"workflow_revision", 7}, {"producer_operation", "streamfind.test"}});
        require(allocation_payload.value("extension", std::string{}) == "binary",
                "binary output did not retain its format extension");
        static_cast<void>(metadata_sink.finish(metadata_handle.at("output_handle")));
        require(published_metadata.at("provenance") == provenance,
                "output descriptor lineage was lost before publication");
        require(published_metadata.at("workflow_revision") == 7,
                "output descriptor workflow revision was lost before publication");
        std::filesystem::remove(output_path);

        streamfind::DataProxyOutputSink arrow_extension_sink(
            [&](const streamfind::data_proxy::Envelope &request) {
                if (request.payload.value("method", std::string{}) == "allocate_result")
                    allocation_payload = request.payload;
                return streamfind::data_proxy::response(request,
                    {{"lease_id", "lease-arrow"}, {"result_id", "result-arrow"}}).to_json();
            },
            "session-arrow", "run-arrow", "operation-arrow", "instance-arrow",
            streamfind::Json::object(), [] { return std::intptr_t{-1}; });
        bool arrow_handle_rejected = false;
        try {
            static_cast<void>(arrow_extension_sink.begin({{"kind", "table"},
                {"format", "arrow-ipc-file"}, {"schema", streamfind::Json::array({
                    {{"name", "value"}, {"type", "real"}}})}}));
        } catch (const std::runtime_error &) { arrow_handle_rejected = true; }
        require(arrow_handle_rejected, "Arrow output accepted a missing writer handle");
        require(allocation_payload.value("extension", std::string{}) == "arrow",
                "Arrow table output did not default to the .arrow extension");
        std::cout << "operation output contract passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
