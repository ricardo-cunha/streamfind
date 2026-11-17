#include "streamfind/data_proxy_output_sink.hpp"
#include "streamfind/data_proxy_protocol.hpp"
#include "streamfind/service/result_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <arrow/builder.h>

namespace streamfind::dev
{
    using Clock = std::chrono::steady_clock;

    void check_arrow(const arrow::Status &status)
    {
        if (!status.ok()) throw std::runtime_error(status.ToString());
    }

    std::intptr_t duplicate_handle(std::intptr_t source)
    {
#ifdef _WIN32
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(source),
                GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
            throw std::runtime_error("could not duplicate result-store benchmark handle");
        return reinterpret_cast<std::intptr_t>(duplicate);
#else
        const auto duplicate = ::dup(static_cast<int>(source));
        if (duplicate < 0) throw std::runtime_error("could not duplicate result-store benchmark handle");
        return duplicate;
#endif
    }

    std::uint64_t working_set_bytes()
    {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return 0;
        return static_cast<std::uint64_t>(counters.WorkingSetSize);
#else
        std::ifstream statm("/proc/self/statm");
        std::uint64_t pages = 0;
        std::uint64_t resident = 0;
        if (!(statm >> pages >> resident)) return 0;
        return resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
    }

    class OutputFixture
    {
    public:
        OutputFixture(service::ResultStore &store, const std::string &run_id)
            : store_(store), run_id_(run_id), sink_(
                [this](const data_proxy::Envelope &request) { return dispatch(request); },
                "benchmark-session", run_id, "development.result_store_benchmark", "benchmark-worker",
                Json::object(), [this] { return duplicate_writer(); }) {}

        Json write(const std::string &result_name, const Json &schema,
                   const std::function<void(const std::string &)> &write_batches)
        {
            const auto output = sink_.begin({
                {"contract_id", result_name}, {"output_port", result_name},
                {"result_name", result_name}, {"kind", "table"},
                {"format", "arrow-ipc-file"}, {"extension", "arrow"},
                {"media_type", "application/vnd.apache.arrow.file"},
                {"schema", schema}});
            const auto handle = output.at("output_handle").get<std::string>();
            write_batches(handle);
            return sink_.finish(handle);
        }

        void write_batch(const std::string &handle, std::uint64_t sequence,
                         const std::shared_ptr<arrow::RecordBatch> &batch)
        {
            sink_.write_table_batch(handle, sequence, batch);
        }

    private:
        Json dispatch(const data_proxy::Envelope &request)
        {
            const auto method = request.payload.value("method", std::string{});
            if (method == "allocate_result") {
                auto lease = store_.allocate(request.payload.at("run_id").get<std::string>(),
                    request.payload.at("result_id").get<std::string>(),
                    request.payload.at("format").get<std::string>(),
                    request.payload.value("extension", std::string{"arrow"}));
                const auto key = lease.run_id + ":" + lease.result_id;
                leases_.emplace(key, std::move(lease));
                return data_proxy::response(request,
                    {{"lease_id", key}, {"result_id", request.payload.at("result_id")}}).to_json();
            }
            const auto found = leases_.find(request.lease_id);
            if (found == leases_.end()) throw std::runtime_error("unknown benchmark output lease");
            if (method == "publish_result") {
                const auto result = store_.publish(found->second,
                    request.payload.value("metadata", Json::object()));
                leases_.erase(found);
                return data_proxy::response(request, result).to_json();
            }
            if (method == "abort_result") {
                store_.abort(found->second);
                leases_.erase(found);
                return data_proxy::response(request, {{"state", "aborted"}}).to_json();
            }
            throw std::runtime_error("unsupported benchmark result-store request");
        }

        std::intptr_t duplicate_writer()
        {
            if (leases_.empty()) throw std::runtime_error("benchmark writer lease was not allocated");
            return duplicate_handle(leases_.begin()->second.writer->native_handle());
        }

        service::ResultStore &store_;
        std::string run_id_;
        std::unordered_map<std::string, service::ResultLease> leases_;
        DataProxyOutputSink sink_;
    };

    Json benchmark_page(service::ResultStore &store, const std::string &result_id,
                        const std::vector<std::string> &columns, std::uint64_t limit)
    {
        const auto start = Clock::now();
        auto result = store.read_arrow_ipc_batch(result_id, 0, limit, columns);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        result["latency_us"] = elapsed;
        result["response_json_bytes"] = result.at("rows").dump().size();
        return result;
    }

    void emit_page_profile(service::ResultStore &store, const std::string &label,
                           const std::string &result_id,
                           const std::vector<std::string> &columns, std::uint64_t limit)
    {
        const auto cold = benchmark_page(store, result_id, columns, limit);
        std::vector<std::int64_t> warm_times;
        std::uint64_t response_bytes = 0;
        for (int i = 0; i < 5; ++i) {
            const auto page = benchmark_page(store, result_id, columns, limit);
            warm_times.push_back(page.at("latency_us").get<std::int64_t>());
            response_bytes = page.at("response_json_bytes").get<std::uint64_t>();
        }
        std::sort(warm_times.begin(), warm_times.end());
        std::cout << Json{{"benchmark", label}, {"cold_us", cold.at("latency_us")},
            {"warm_median_us", warm_times[warm_times.size() / 2]},
            {"rows_returned", cold.at("rows").size()}, {"response_json_bytes", response_bytes}}
            .dump() << '\n';
    }

std::shared_ptr<arrow::RecordBatch> make_table_batch(
    std::size_t offset, std::size_t count, const std::string &payload)
{
    arrow::StringBuilder row_keys;
    arrow::StringBuilder payloads;
    for (std::size_t row = 0; row < count; ++row) {
        check_arrow(row_keys.Append("row-" + std::to_string(offset + row)));
        check_arrow(payloads.Append(payload));
    }
    std::shared_ptr<arrow::Array> row_key_array;
    std::shared_ptr<arrow::Array> payload_array;
    check_arrow(row_keys.Finish(&row_key_array));
    check_arrow(payloads.Finish(&payload_array));
    auto schema = arrow::schema({arrow::field("row_key", arrow::utf8()),
                                 arrow::field("payload", arrow::utf8())});
    return arrow::RecordBatch::Make(std::move(schema), static_cast<std::int64_t>(count),
                                    {std::move(row_key_array), std::move(payload_array)});
}

std::shared_ptr<arrow::RecordBatch> make_trace_batch(
    const std::vector<float> &rt, const std::vector<float> &intensity,
    const std::vector<float> &ms2_mz, const std::vector<float> &ms2_intensity)
{
    arrow::StringBuilder feature;
    check_arrow(feature.Append("feature-large"));
    auto make_list = [](const std::vector<float> &values) {
        auto value_builder = std::make_shared<arrow::DoubleBuilder>();
        arrow::ListBuilder builder(arrow::default_memory_pool(), value_builder);
        check_arrow(builder.Append());
        for (const auto value : values) check_arrow(value_builder->Append(static_cast<double>(value)));
        std::shared_ptr<arrow::Array> array;
        check_arrow(builder.Finish(&array));
        return array;
    };
    std::shared_ptr<arrow::Array> feature_array;
    check_arrow(feature.Finish(&feature_array));
    auto schema = arrow::schema({
        arrow::field("feature", arrow::utf8()),
        arrow::field("eic_rt", arrow::list(arrow::float64())),
        arrow::field("eic_intensity", arrow::list(arrow::float64())),
        arrow::field("ms2_mz", arrow::list(arrow::float64())),
        arrow::field("ms2_intensity", arrow::list(arrow::float64()))});
    return arrow::RecordBatch::Make(std::move(schema), 1,
        {std::move(feature_array), make_list(rt), make_list(intensity),
         make_list(ms2_mz), make_list(ms2_intensity)});
}
}

int main()
{
    using namespace streamfind;
    namespace fs = std::filesystem;
    const fs::path root = fs::path(STREAMFIND_TEST_REPO_ROOT) / "tmp" / "projects";
    const fs::path database = root / "result-store-scale-benchmark.duckdb";
    const auto data_root = service::ResultStore::data_path_for_database(database);
    std::error_code error;
    fs::create_directories(root, error);
    fs::remove(database, error);
    fs::remove_all(data_root, error);

    try {
        service::ResultStore store(database);
        if (store.initialize().status != service::ResultStoreStatus::ready)
            throw std::runtime_error("could not initialize benchmark result store");
        streamfind::dev::OutputFixture output(store, "scale-run");

        constexpr std::size_t table_rows = 76800;
        constexpr std::size_t cell_bytes = 1024;
        const auto table_schema = Json::array({
            {{"name", "row_key"}, {"type", "string"}},
            {{"name", "payload"}, {"type", "string"}}});
        const auto table_result = output.write("largeTable", table_schema,
            [&](const std::string &handle) {
                constexpr std::size_t rows_per_batch = 256;
                std::string payload(cell_bytes, 'x');
                for (std::size_t offset = 0; offset < table_rows; offset += rows_per_batch) {
                    const auto count = std::min(rows_per_batch, table_rows - offset);
                    output.write_batch(handle, offset / rows_per_batch,
                                       streamfind::dev::make_table_batch(offset, count, payload));
                }
            });
        const auto table_id = table_result.at("result_id").get<std::string>();
        const auto table_descriptor = store.describe_result(table_id);
        streamfind::dev::emit_page_profile(store, "75MiB table projected page", table_id, {"row_key"}, 250);
        std::cout << Json{{"benchmark", "75MiB table storage"},
            {"payload_bytes", table_descriptor.at("payload_size")},
            {"row_count", table_descriptor.at("row_count")},
            {"working_set_bytes", streamfind::dev::working_set_bytes()}}.dump() << '\n';

        constexpr std::size_t trace_points = 576000;
        constexpr std::size_t ms2_points = 50000;
        std::vector<float> rt(trace_points), intensity(trace_points);
        for (std::size_t i = 0; i < trace_points; ++i) {
            rt[i] = static_cast<float>(i) * 0.05f;
            intensity[i] = static_cast<float>((i * 7919) % 100000) / 100.0f;
        }
        std::vector<float> ms2_mz(ms2_points), ms2_intensity(ms2_points);
        for (std::size_t i = 0; i < ms2_points; ++i) {
            ms2_mz[i] = 50.0f + static_cast<float>(i) * 0.01f;
            ms2_intensity[i] = static_cast<float>((i * 3571) % 10000) / 10.0f;
        }
        const auto trace_schema = Json::array({
            {{"name", "feature"}, {"type", "string"}},
            {{"name", "eic_rt"}, {"type", "array"}, {"items", {{"type", "number"}}}},
            {{"name", "eic_intensity"}, {"type", "array"}, {"items", {{"type", "number"}}}},
            {{"name", "ms2_mz"}, {"type", "array"}, {"items", {{"type", "number"}}}},
            {{"name", "ms2_intensity"}, {"type", "array"}, {"items", {{"type", "number"}}}}});
        const auto trace_result = output.write("largeFeatureTrace", trace_schema,
            [&](const std::string &handle) {
                output.write_batch(handle, 0, streamfind::dev::make_trace_batch(
                    rt, intensity, ms2_mz, ms2_intensity));
            });
        const auto trace_id = trace_result.at("result_id").get<std::string>();
        const auto trace_descriptor = store.describe_result(trace_id);
        streamfind::dev::emit_page_profile(store, "576k-point EIC plus 50k-peak MS2 selected trace",
                          trace_id, {"feature", "eic_rt", "eic_intensity", "ms2_mz", "ms2_intensity"}, 1);
        streamfind::dev::emit_page_profile(store, "50k-peak MS2 selected trace",
                          trace_id, {"feature", "ms2_mz", "ms2_intensity"}, 1);
        std::cout << Json{{"benchmark", "large selected trace storage"},
            {"payload_bytes", trace_descriptor.at("payload_size")},
            {"eic_points", trace_points}, {"ms2_peaks", ms2_points},
            {"working_set_bytes", streamfind::dev::working_set_bytes()}}.dump() << '\n';

        store.remove_published_results({table_id, trace_id});
        fs::remove_all(data_root, error);
        return 0;
    } catch (const std::exception &exception) {
        fs::remove_all(data_root, error);
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
