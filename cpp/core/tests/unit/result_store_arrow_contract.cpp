#include "streamfind/data_proxy_output_sink.hpp"
#include "streamfind/data_proxy_protocol.hpp"
#include "streamfind/service/result_store.hpp"

#include <filesystem>
#include <fstream>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static std::intptr_t duplicate_native_handle(std::intptr_t source) {
#ifdef _WIN32
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(source),
            GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
        throw std::runtime_error("could not duplicate published result reader handle");
    return reinterpret_cast<std::intptr_t>(duplicate);
#else
    const auto duplicate = dup(static_cast<int>(source));
    if (duplicate < 0) throw std::runtime_error("could not duplicate published result reader handle");
    return duplicate;
#endif
}

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

static void write_lease_bytes(streamfind::service::ResultLease &lease,
                              const std::string &payload) {
#ifdef _WIN32
    DWORD written = 0;
    if (!WriteFile(reinterpret_cast<HANDLE>(lease.writer->native_handle()),
                   payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) ||
        written != payload.size())
        throw std::runtime_error("could not write lifecycle fixture payload");
#else
    const auto written = ::write(static_cast<int>(lease.writer->native_handle()),
                                 payload.data(), payload.size());
    if (written != static_cast<ssize_t>(payload.size()))
        throw std::runtime_error("could not write lifecycle fixture payload");
#endif
}

static std::string publish_small_table(streamfind::service::ResultStore &store,
                                       const std::string &run_id,
                                       const std::string &result_id) {
    std::unordered_map<std::string, streamfind::service::ResultLease> leases;
    streamfind::DataProxyOutputSink sink(
        [&store, &leases](const streamfind::data_proxy::Envelope &request) {
            const auto method = request.payload.value("method", std::string{});
            if (method == "allocate_result") {
                auto lease = store.allocate(request.payload.value("run_id", std::string{}),
                                            request.payload.value("result_id", std::string{}),
                                            request.payload.value("format", std::string{}),
                                            request.payload.value("extension", std::string{"arrow"}));
                const auto key = lease.run_id + ":" + lease.result_id;
                const auto result_id = lease.result_id;
                leases.emplace(key, std::move(lease));
                return streamfind::data_proxy::response(
                    request, {{"lease_id", key}, {"result_id", result_id}}).to_json();
            }
            const auto found = leases.find(request.lease_id);
            if (found == leases.end()) throw std::runtime_error("unknown concurrent result lease");
            if (method == "publish_result") {
                const auto result = store.publish(found->second,
                    request.payload.value("metadata", streamfind::Json::object()));
                leases.erase(found);
                return streamfind::data_proxy::response(request, result).to_json();
            }
            throw std::runtime_error("unexpected concurrent result method");
        },
        "concurrent-session", run_id, "concurrent-operation", "concurrent-worker",
        streamfind::Json::object(), [&leases]() -> std::intptr_t {
            if (leases.empty()) throw std::runtime_error("concurrent writer lease was not allocated");
            const auto source = leases.begin()->second.writer->native_handle();
#ifdef _WIN32
            HANDLE duplicate = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(source),
                    GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
                throw std::runtime_error("could not duplicate concurrent writer handle");
            return reinterpret_cast<std::intptr_t>(duplicate);
#else
            const auto duplicate = dup(static_cast<int>(source));
            if (duplicate < 0) throw std::runtime_error("could not duplicate concurrent writer handle");
            return duplicate;
#endif
        });
    const auto output = sink.begin({
        {"contract_id", "concurrentTable"}, {"kind", "table"},
        {"format", "arrow-ipc-file"}, {"extension", "arrow"},
        {"media_type", "application/vnd.apache.arrow.file"},
        {"schema", {{{"name", "result_id"}, {"type", "string"}},
                    {{"name", "value"}, {"type", "integer"}}}}});
    const auto output_handle = output.at("output_handle").get<std::string>();
    sink.write_rows(output_handle, 0,
                    {{{"result_id", result_id}, {"value", 42}},
                     {{"result_id", result_id}, {"value", 43}}});
    return sink.finish(output_handle).at("result_id").get<std::string>();
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
            "session-1", "run-1", "operation-1", "instance-1", streamfind::Json::object(),
            [&leases]() -> std::intptr_t {
                if (leases.empty()) throw std::runtime_error("output lease was not allocated before handle request");
                const auto source = leases.begin()->second.writer->native_handle();
#ifdef _WIN32
                HANDLE duplicate = nullptr;
                if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(source),
                    GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
                    throw std::runtime_error("could not duplicate test result writer");
                return reinterpret_cast<std::intptr_t>(duplicate);
#else
                const auto duplicate = dup(static_cast<int>(source));
                if (duplicate < 0) throw std::runtime_error("could not duplicate test result writer");
                return duplicate;
#endif
            });

        const auto handle = sink.begin({
            {"contract_id", "anyTable"}, {"kind", "table"},
            {"format", "arrow-ipc-file"}, {"extension", "arrow"},
            {"media_type", "application/vnd.apache.arrow.file"},
            {"navigation_columns", {"label", "count"}},
            {"schema", {{{"name", "count"}, {"type", "integer"}},
                        {{"name", "enabled"}, {"type", "boolean"}},
                        {{"name", "label"}, {"type", "string"}},
                        {{"name", "trace"}, {"type", "array"}}}}});
        const auto output_handle = handle.at("output_handle").get<std::string>();
        sink.write_rows(output_handle, 0,
                        {{{"count", 7}, {"enabled", true}, {"label", "first"},
                          {"trace", {1.25, 2.5, 3.75}}}});
        sink.write_rows(output_handle, 1,
                        {{{"count", nullptr}, {"enabled", false}, {"label", "second"},
                          {"trace", nullptr}}});
        const auto published = sink.finish(output_handle, {{"cacheable", true}, {"computation_key", "key-1"}});
        const auto result_id = published.at("result_id").get<std::string>();
        const auto descriptor = store.describe_result(result_id);
        require(descriptor.at("format") == "arrow-ipc-file", "published format was not preserved");
        const auto batch = store.read_arrow_ipc_batch(result_id, 0, 10);
        const auto rows = batch.at("rows");
        require(rows.size() == 2, "published Arrow result returned the wrong row count");
        require(rows.at(0).at("count") == 7 && rows.at(0).at("enabled") == true &&
                    rows.at(1).at("count").is_null() && rows.at(1).at("enabled") == false,
                "published Arrow result changed typed values");
        const auto native_numeric = store.scan_arrow_scalar_filters(
            result_id, {{{"column", "count"}, {"operator", "between"}, {"min", 6}, {"max", 8}}}, 0, 4);
        require(native_numeric.value("total_rows", 0) == 1 &&
                    native_numeric.at("offsets") == streamfind::Json::array({0}),
                "Arrow-native numeric range filter returned incorrect row offsets");
        const auto native_string = store.scan_arrow_scalar_filters(
            result_id, {{{"column", "label"}, {"operator", "eq"}, {"value", "second"}}}, 0, 4);
        require(native_string.value("total_rows", 0) == 1 &&
                    native_string.at("offsets") == streamfind::Json::array({1}),
                "Arrow-native string equality filter returned incorrect row offsets");
        const auto native_null = store.scan_arrow_scalar_filters(
            result_id, {{{"column", "count"}, {"operator", "is_null"}}}, 0, 4);
        require(native_null.value("total_rows", 0) == 1 &&
                    native_null.at("offsets") == streamfind::Json::array({1}),
                "Arrow-native null filter returned incorrect row offsets");
        require(store.find_cached_results("key-1").size() == 1,
                "published Arrow result was not available to cache lookup");
        require(store.clear_cache() == 1,
                "result-cache clear did not invalidate the reusable manifest");
        require(store.find_cached_results("key-1").empty(),
                "result-cache clear left a published result reusable");
        require(store.describe_result(result_id).value("state", std::string{}) == "published" &&
                    store.read_arrow_ipc_batch(result_id, 0, 10).at("rows").size() == 2,
                "result-cache clear removed or damaged a published payload");
        const streamfind::Json expected_navigation_index = {
            {"label", {{"\"first\"", {0}}, {"\"second\"", {1}}}}};
        store.write_navigation_index(result_id, expected_navigation_index);
        const streamfind::Json expected_range_statistics = {
            {{"min", 7}, {"max", 7}}, {{"min", nullptr}, {"max", nullptr}}};
        store.write_range_statistics(result_id, "count", expected_range_statistics);
        require(store.read_range_statistics(result_id, "count") == expected_range_statistics,
                "persisted numeric range statistics did not round-trip");
        require(store.read_navigation_index(result_id) == expected_navigation_index,
                "persisted semantic navigation index did not round-trip");
        const auto result_directory = data_root / "results" / result_id;
        const auto index_path = result_directory / "navigation.json";
        {
            std::ofstream corrupt(index_path, std::ios::binary | std::ios::trunc);
            corrupt << "not-json";
        }
        require(store.read_navigation_index(result_id).is_null(),
                "corrupt persisted navigation index was accepted");
        std::filesystem::remove(index_path, error);
        store.write_navigation_index(result_id, expected_navigation_index);

        auto owned_input = store.open_published_result(result_id);
        streamfind::data_proxy::ArrowFileBatchReader input_reader(
            duplicate_native_handle(owned_input->native_handle()));
        const auto first_page = input_reader.read(0, 1, {"count", "label", "trace"});
        const auto second_page = input_reader.read(1, 1, {"count", "label", "trace"});
        require(first_page.size() == 1 && second_page.size() == 1 &&
                    first_page.front().at("count") == 7 &&
                    second_page.front().at("count").is_null() &&
                    second_page.front().at("label") == "second" &&
                    first_page.front().at("trace") == streamfind::Json::array({1.25, 2.5, 3.75}) &&
                    second_page.front().at("trace").is_null(),
                "handle-backed Arrow reader did not return sequential projected pages");

        const auto empty_handle = sink.begin({
            {"contract_id", "emptyTable"}, {"kind", "table"},
            {"format", "arrow-ipc-file"}, {"extension", "arrow"},
            {"media_type", "application/vnd.apache.arrow.file"},
            {"schema", {{{"name", "count"}, {"type", "integer"}}}}});
        const auto empty_published = sink.finish(
            empty_handle.at("output_handle").get<std::string>(), streamfind::Json::object());
        const auto empty_id = empty_published.at("result_id").get<std::string>();
        const auto empty_descriptor = store.describe_result(empty_id);
        require(empty_descriptor.at("row_count") == 0 &&
                    empty_descriptor.at("schema").size() == 1,
                "empty Arrow table did not preserve its schema and zero row count");
        auto empty_input = store.open_published_result(empty_id);
        streamfind::data_proxy::ArrowFileBatchReader empty_reader(
            duplicate_native_handle(empty_input->native_handle()));
        require(empty_reader.read(0, 4, {"count"}).empty(),
                "empty Arrow table returned rows");

        const auto bounded_handle = sink.begin({
            {"contract_id", "boundedTable"}, {"kind", "table"},
            {"format", "arrow-ipc-file"}, {"extension", "arrow"},
            {"media_type", "application/vnd.apache.arrow.file"},
            {"navigation_columns", {"count"}},
            {"schema", {{{"name", "payload"}, {"type", "string"}},
                        {{"name", "count"}, {"type", "integer"}}}}});
        const auto bounded_output = bounded_handle.at("output_handle").get<std::string>();
        // Control bytes expand to six-character JSON escapes in the encoded
        // Arrow string column. The estimated input footprint stays below the
        // cap for both rows together, while the actual Arrow IPC chunk does
        // not; this exercises the post-encoding split path.
        const std::string large_value(1536 * 1024, '\x01');
        sink.write_rows(bounded_output, 0,
                        {{{"payload", large_value}, {"count", 1}},
                         {{"payload", large_value}, {"count", 2}}});
        const auto bounded_published = sink.finish(bounded_output, streamfind::Json::object());
        const auto bounded_id = bounded_published.at("result_id").get<std::string>();
        const auto bounded_manifest = store.describe_result(bounded_id);
        require(bounded_manifest.value("batch_row_counts", streamfind::Json::array()).size() == 2,
                "large encoded Arrow output was not split to the byte budget");
        const auto bounded_page = store.read_arrow_ipc_batch(bounded_id, 1, 1, {"payload"});
        const auto returned_size = bounded_page.at("rows").empty() ? 0 :
            bounded_page.at("rows").front().at("payload").get<std::string>().size();
        require(bounded_page.at("rows").size() == 1 && returned_size == large_value.size(),
                ("byte-bounded output changed a large row value (rows=" +
                 std::to_string(bounded_page.at("rows").size()) + ", bytes=" +
                 std::to_string(returned_size) + ")").c_str());

        const auto oversized_handle = sink.begin({
            {"contract_id", "oversizedRowTable"}, {"output_port", "featuresTable"},
            {"kind", "table"}, {"format", "arrow-ipc-file"}, {"extension", "arrow"},
            {"media_type", "application/vnd.apache.arrow.file"},
            {"schema", {{{"name", "feature"}, {"type", "string"}},
                        {{"name", "analysis"}, {"type", "string"}},
                        {{"name", "trace"}, {"type", "string"}}}}});
        const auto oversized_output = oversized_handle.at("output_handle").get<std::string>();
        const std::string oversized_trace(9 * 1024 * 1024, 'x');
        sink.write_rows(oversized_output, 0,
                        {{{"feature", "feature-17"}, {"analysis", "analysis-2"},
                          {"trace", oversized_trace}}});
        const auto oversized_published = sink.finish(oversized_output, streamfind::Json::object());
        const auto oversized_manifest = store.describe_result(
            oversized_published.at("result_id").get<std::string>());
        const auto diagnostics = oversized_manifest.value("output_diagnostics", streamfind::Json::object());
        require(diagnostics.value("oversized_row_count", std::uint64_t{0}) == 1 &&
                    diagnostics.value("largest_encoded_row_bytes", std::uint64_t{0}) > 8 * 1024 * 1024 &&
                    diagnostics.value("largest_estimated_field", streamfind::Json::object())
                        .value("name", std::string{}) == "trace" &&
                    diagnostics.value("largest_row_identity", streamfind::Json::object())
                        .value("feature", std::string{}) == "feature-17" &&
                    oversized_manifest.value("output_port", std::string{}) == "featuresTable" &&
                    oversized_manifest.value("producer_instance", std::string{}) == "instance-1",
                "oversized Arrow row publication did not retain actionable diagnostics");

        store.write_range_statistics(bounded_id, "count", {
            {{"min", 1}, {"max", 1}}, {{"min", 2}, {"max", 2}}});
        const auto pruned = store.scan_arrow_scalar_filters(
            bounded_id, {{{"column", "count"}, {"operator", "eq"}, {"value", 2}}}, 0, 4);
        require(pruned.value("total_rows", 0) == 1 &&
                    pruned.at("offsets") == streamfind::Json::array({1}) &&
                    pruned.value("batches_skipped", 0) == 1,
                "persisted range statistics did not skip a nonmatching Arrow batch");

        const auto typed_handle = sink.begin({
            {"contract_id", "typedTraceTable"}, {"kind", "table"},
            {"format", "arrow-ipc-file"}, {"extension", "arrow"},
            {"media_type", "application/vnd.apache.arrow.file"},
            {"schema", {{{"name", "trace"}, {"type", "array"},
                         {"items", {{"type", "number"}}}}}}});
        const auto typed_output = typed_handle.at("output_handle").get<std::string>();
        sink.write_rows(typed_output, 0,
                        {{{"trace", {1.25, 2.5}}}, {{"trace", streamfind::Json::array()}},
                         {{"trace", nullptr}}});
        const auto typed_published = sink.finish(typed_output, streamfind::Json::object());
        const auto typed_id = typed_published.at("result_id").get<std::string>();
        const auto typed_rows = store.read_arrow_ipc_batch(typed_id, 0, 4, {"trace"}).at("rows");
        require(typed_rows.size() == 3 && typed_rows.at(0).at("trace") == streamfind::Json({1.25, 2.5}) &&
                    typed_rows.at(1).at("trace").is_array() && typed_rows.at(1).at("trace").empty() &&
                    typed_rows.at(2).at("trace").is_null(),
                "published Arrow list did not preserve numeric values, empty lists, and nulls");

        // Exercise independent store operations concurrently: result publication,
        // projected reads of a stable result, and deletion of another result.
        std::atomic<bool> concurrent_start{false};
        std::atomic<bool> concurrent_failed{false};
        std::mutex concurrent_error_mutex;
        std::string concurrent_error;
        const auto record_concurrent_error = [&](const std::string &message) {
            concurrent_failed.store(true);
            std::lock_guard<std::mutex> lock(concurrent_error_mutex);
            if (concurrent_error.empty()) concurrent_error = message;
        };
        std::string concurrent_published_id;
        std::thread publisher([&] {
            while (!concurrent_start.load()) std::this_thread::yield();
            try {
                concurrent_published_id = publish_small_table(store, "concurrent-run", "concurrent-result");
            } catch (const std::exception &exception) {
                record_concurrent_error(exception.what());
            }
        });
        std::vector<std::thread> readers;
        for (int reader_index = 0; reader_index < 3; ++reader_index) {
            readers.emplace_back([&] {
                while (!concurrent_start.load()) std::this_thread::yield();
                try {
                    for (int read_index = 0; read_index < 20; ++read_index) {
                        const auto page = store.read_arrow_ipc_batch(result_id, 0, 2, {"label", "count"});
                        if (page.at("rows").size() != 2 || page.at("rows").at(0).at("label") != "first")
                            throw std::runtime_error("concurrent projected read returned inconsistent rows");
                    }
                } catch (const std::exception &exception) {
                    record_concurrent_error(exception.what());
                }
            });
        }
        std::thread deleter([&] {
            while (!concurrent_start.load()) std::this_thread::yield();
            try {
                store.remove_published_results({typed_id});
            } catch (const std::exception &exception) {
                record_concurrent_error(exception.what());
            }
        });
        concurrent_start.store(true);
        publisher.join();
        for (auto &reader : readers) reader.join();
        deleter.join();
        require(!concurrent_failed.load(), concurrent_error.empty()
                    ? "concurrent result-store operations failed"
                    : concurrent_error.c_str());
        require(!concurrent_published_id.empty() &&
                    store.read_arrow_ipc_batch(concurrent_published_id, 0, 1).at("rows").size() == 1,
                "concurrent publication was not readable after completion");
        bool deleted_result_hidden = false;
        try { (void)store.describe_result(typed_id); }
        catch (const std::exception &) { deleted_result_hidden = true; }
        require(deleted_result_hidden, "concurrent deletion left a published result visible");

        // An already-issued read handle is a snapshot capability: deleting the
        // registry path must not invalidate that reader halfway through a page
        // sequence on either platform.
        auto concurrent_read_handle = store.open_published_result(concurrent_published_id);
        streamfind::data_proxy::ArrowFileBatchReader concurrent_reader(
            duplicate_native_handle(concurrent_read_handle->native_handle()));
        std::atomic<bool> first_read_complete{false};
        std::atomic<bool> concurrent_delete_complete{false};
        std::thread held_reader([&] {
            try {
                const auto first = concurrent_reader.read(0, 1, {"value"});
                if (first.size() != 1 || first.front().at("value") != 42)
                    throw std::runtime_error("held read handle returned an invalid first page");
                first_read_complete.store(true);
                while (!concurrent_delete_complete.load()) std::this_thread::yield();
                const auto after_delete = concurrent_reader.read(1, 1, {"value"});
                if (after_delete.size() != 1 || after_delete.front().at("value") != 43)
                    throw std::runtime_error("published-result deletion invalidated an open read handle");
            } catch (const std::exception &exception) {
                record_concurrent_error(exception.what());
                first_read_complete.store(true);
            }
        });
        std::thread concurrent_deleter([&] {
            while (!first_read_complete.load()) std::this_thread::yield();
            try { store.remove_published_results({concurrent_published_id}); }
            catch (const std::exception &exception) { record_concurrent_error(exception.what()); }
            concurrent_delete_complete.store(true);
        });
        held_reader.join();
        concurrent_deleter.join();
        require(!concurrent_failed.load(), concurrent_error.empty()
                    ? "same-result concurrent deletion/read failed"
                    : concurrent_error.c_str());
        bool concurrent_result_hidden = false;
        try { (void)store.describe_result(concurrent_published_id); }
        catch (const std::exception &) { concurrent_result_hidden = true; }
        require(concurrent_result_hidden, "concurrently deleted result remained discoverable");

        const auto aborted_handle = sink.begin({
            {"contract_id", "abortedTable"}, {"kind", "table"},
            {"format", "arrow-ipc-file"}, {"extension", "arrow"},
            {"media_type", "application/vnd.apache.arrow.file"},
            {"schema", {{{"name", "count"}, {"type", "integer"}}}}});
        const auto aborted_output = aborted_handle.at("output_handle").get<std::string>();
        sink.write_rows(aborted_output, 0, {{{"count", 99}}});
        const auto aborted_result_id = aborted_handle.at("result_id").get<std::string>();
        sink.abort(aborted_output, "contract abort after a partial write");
        bool aborted_result_is_hidden = false;
        try {
            (void)store.describe_result(aborted_result_id);
        } catch (const std::exception &) {
            aborted_result_is_hidden = true;
        }
        require(aborted_result_is_hidden,
                "partially written aborted Arrow result became visible");

        // A worker can disappear after writing bytes but before requesting
        // publication. Reopening the project must not expose that partial
        // payload as a result; the surviving lease can be explicitly reaped.
        auto crashed_lease = store.allocate("crashed-run", "crashed-result", "binary", "bin");
        write_lease_bytes(crashed_lease, "partial worker output");
        crashed_lease.writer->close();
        crashed_lease.writer.reset();
        streamfind::service::ResultStore restarted_store(database);
        require(restarted_store.initialize().status ==
                    streamfind::service::ResultStoreStatus::ready,
                "result store did not recover after a worker interruption");
        bool crashed_result_hidden = false;
        try { (void)restarted_store.describe_result("crashed-result"); }
        catch (const std::exception &) { crashed_result_hidden = true; }
        require(crashed_result_hidden,
                "an unfinished worker payload became visible after service restart");
        require(std::filesystem::exists(crashed_lease.staging_payload),
                "restart removed non-empty staging data before recovery could inspect it");
        restarted_store.abort(crashed_lease);
        require(!std::filesystem::exists(crashed_lease.staging_directory),
                "recovered abandoned lease was not removed");

        // Race final publication against abort after the producer has closed
        // its handle. Exactly one terminal state may win; a partial result or
        // orphaned staging directory is never acceptable.
        auto racing_lease = store.allocate("race-run", "race-result", "binary", "bin");
        write_lease_bytes(racing_lease, "race payload");
        racing_lease.writer->close();
        racing_lease.writer.reset();
        std::atomic<bool> race_start{false};
        std::atomic<bool> race_published{false};
        std::thread racing_publisher([&] {
            while (!race_start.load()) std::this_thread::yield();
            try {
                (void)store.publish(racing_lease,
                    {{"kind", "binary"}, {"media_type", "application/octet-stream"}});
                race_published.store(true);
            } catch (const std::exception &) {}
        });
        std::thread aborter([&] {
            while (!race_start.load()) std::this_thread::yield();
            try { store.abort(racing_lease); }
            catch (const std::exception &) {}
        });
        race_start.store(true);
        racing_publisher.join();
        aborter.join();
        bool race_result_visible = false;
        try {
            const auto race_descriptor = store.describe_result("race-result");
            race_result_visible = race_descriptor.value("state", std::string{}) == "published";
        } catch (const std::exception &) {}
        require(race_result_visible == race_published.load(),
                "publish/abort race produced a result with inconsistent terminal state");
        if (!race_result_visible)
            require(!std::filesystem::exists(racing_lease.staging_directory),
                    "publish/abort race left an orphaned staging directory");

        store.remove_published_results({result_id});
        require(store.read_navigation_index(result_id).is_null(),
                "result deletion left a stale persisted navigation index");
        require(store.read_range_statistics(result_id, "count").is_null(),
                "result deletion left stale numeric range statistics");
        store.remove_published_results({bounded_id});
        require(store.read_range_statistics(bounded_id, "count").is_null(),
                "deleting a result left its range-statistics sidecar");
        std::filesystem::remove_all(data_root, error);
        return 0;
    } catch (...) {
        std::filesystem::remove_all(data_root, error);
        throw;
    }
}
