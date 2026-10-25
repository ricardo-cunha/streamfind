#include "streamfind/data_proxy_output_sink.hpp"
#include "streamfind/data_proxy_protocol.hpp"
#include "streamfind/service/result_store.hpp"

#include <filesystem>
#include <stdexcept>
#include <unordered_map>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    const std::filesystem::path database =
        std::filesystem::path(STREAMFIND_TEST_REPO_ROOT) /
        "tmp" / "projects" / "result-store-arrow-contract.duckdb";
    const auto data_root = streamfind::service::ResultStore::data_path_for_database(database);
    std::error_code error;
    std::filesystem::create_directories(database.parent_path(), error);
    std::filesystem::remove(database, error);
    std::filesystem::remove_all(data_root, error);

    try {
        streamfind::service::ResultStore store(database);
        require(store.initialize().status == streamfind::service::ResultStoreStatus::ready,
                "result store did not initialize");
        bool rejected_legacy_jsonl = false;
        try {
            (void)store.allocate("run-jsonl", "result-jsonl", "jsonl", "jsonl");
        } catch (const std::invalid_argument &) {
            rejected_legacy_jsonl = true;
        }
        require(rejected_legacy_jsonl,
                "new result allocations must reject legacy JSONL table payloads");
        std::unordered_map<std::string, streamfind::service::ResultLease> leases;
        streamfind::DataProxyOutputSink sink(
            [&store, &leases](const streamfind::data_proxy::Envelope &request) {
                const auto method = request.payload.value("method", std::string{});
                if (method == "allocate_result") {
                    const auto result_id = request.payload.value("result_id", std::string{});
                    const auto run_id = request.payload.value("run_id", std::string{});
                    const auto format = request.payload.value("format", std::string{});
                    const auto extension = request.payload.value("extension", std::string{"arrow"});
                    auto lease = store.allocate(run_id, result_id, format, extension);
                    const auto lease_id = lease.run_id + ":" + lease.result_id;
                    leases.emplace(lease_id, std::move(lease));
                    return streamfind::data_proxy::response(
                        request, {{"lease_id", lease_id}, {"result_id", result_id}}).to_json();
                }
                if (request.lease_id.empty() || !leases.contains(request.lease_id))
                    throw std::runtime_error("service test received an unknown result lease");
                auto &lease = leases.at(request.lease_id);
                if (method == "write_result_chunk") {
                    const auto bytes = streamfind::data_proxy::base64_decode(
                        request.payload.value("data", std::string{}));
                    store.append_staging_chunk(lease, request.payload.value("offset", std::uintmax_t{0}), bytes);
                    return streamfind::data_proxy::response(request, {{"state", "written"}}).to_json();
                }
                if (method == "publish_result") {
                    auto result = store.publish(lease, request.payload.value("metadata", streamfind::Json::object()));
                    leases.erase(request.lease_id);
                    return streamfind::data_proxy::response(request, std::move(result)).to_json();
                }
                if (method == "abort_result") {
                    store.abort(lease);
                    leases.erase(request.lease_id);
                    return streamfind::data_proxy::response(request, {{"state", "aborted"}}).to_json();
                }
                throw std::runtime_error("service test received an unsupported result method");
            },
            "session-1", "run-1", "operation-1", "instance-1");

        const auto handle = sink.begin({
            {"contract_id", "anyTable"}, {"kind", "table"},
            {"format", "arrow-ipc-batches"}, {"extension", "arrow"},
            {"media_type", "application/vnd.apache.arrow.stream"},
            {"schema", {{{"name", "count"}, {"type", "integer"}},
                        {{"name", "enabled"}, {"type", "boolean"}},
                        {{"name", "label"}, {"type", "string"}}}}});
        const auto output_handle = handle.at("output_handle").get<std::string>();
        sink.write_rows(output_handle, 0,
                        {{{"count", 7}, {"enabled", true}, {"label", "first"}},
                         {{"count", nullptr}, {"enabled", false}, {"label", "second"}}});
        const auto published = sink.finish(output_handle, {{"cacheable", true}, {"computation_key", "key-1"}});
        const auto result_id = published.at("result_id").get<std::string>();
        const auto descriptor = store.describe_result(result_id);
        require(descriptor.at("format") == "arrow-ipc-batches", "published format was not preserved");
        const auto batch = store.read_arrow_ipc_batch(result_id, 0, 10);
        const auto rows = batch.at("rows");
        require(rows.size() == 2, "published Arrow result returned the wrong row count");
        require(rows.at(0).at("count") == 7 && rows.at(0).at("enabled") == true &&
                    rows.at(1).at("count").is_null() && rows.at(1).at("enabled") == false,
                "published Arrow result changed typed values");
        require(store.find_cached_results("key-1").size() == 1,
                "published Arrow result was not available to cache lookup");
        std::filesystem::remove_all(data_root, error);
        return 0;
    } catch (...) {
        std::filesystem::remove_all(data_root, error);
        throw;
    }
}
