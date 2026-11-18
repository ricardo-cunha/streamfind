#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include "streamfind/catalogue.hpp"
#include "streamfind/core_operations.hpp"
#include "streamfind/data_proxy_protocol.hpp"
#include "streamfind/data_proxy_output_sink.hpp"
#include "streamfind/operation_execution_context.hpp"
#include "streamfind/project.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace streamfind::table_column_binding_contract_detail {

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void require_arrow(const arrow::Status &status) {
    if (!status.ok()) throw std::runtime_error(status.ToString());
}

void write_arrow_file(const std::filesystem::path &path,
                      const std::shared_ptr<arrow::Schema> &schema,
                      const std::vector<std::shared_ptr<arrow::RecordBatch>> &batches) {
    auto output_result = arrow::io::FileOutputStream::Open(path.string());
    if (!output_result.ok()) throw std::runtime_error(output_result.status().ToString());
    const auto output = *output_result;
    auto writer_result = arrow::ipc::MakeFileWriter(output, schema);
    if (!writer_result.ok()) throw std::runtime_error(writer_result.status().ToString());
    const auto writer = *writer_result;
    for (const auto &batch : batches) require_arrow(writer->WriteRecordBatch(*batch));
    require_arrow(writer->Close());
    require_arrow(output->Close());
}

std::intptr_t open_read_handle(const std::filesystem::path &path) {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open Arrow input fixture");
    return reinterpret_cast<std::intptr_t>(handle);
#else
    const auto handle = ::open(path.c_str(), O_RDONLY);
    if (handle < 0) throw std::runtime_error("cannot open Arrow input fixture");
    return handle;
#endif
}

std::shared_ptr<arrow::RecordBatch> string_batch(
    const std::shared_ptr<arrow::Schema> &schema,
    const std::vector<std::string> &values) {
    arrow::StringBuilder builder;
    for (const auto &value : values) require_arrow(builder.Append(value));
    std::shared_ptr<arrow::Array> array;
    require_arrow(builder.Finish(&array));
    return arrow::RecordBatch::Make(schema, static_cast<std::int64_t>(values.size()), {array});
}

std::shared_ptr<arrow::RecordBatch> integer_batch(
    const std::shared_ptr<arrow::Schema> &schema,
    const std::vector<std::int64_t> &values) {
    arrow::Int64Builder builder;
    for (const auto value : values) require_arrow(builder.Append(value));
    std::shared_ptr<arrow::Array> array;
    require_arrow(builder.Finish(&array));
    return arrow::RecordBatch::Make(schema, static_cast<std::int64_t>(values.size()), {array});
}

class CapturingOutput final : public OperationOutputSink {
public:
    Json begin(const Json &descriptor) override {
        if (descriptor.value("kind", std::string{}) == "table") output_descriptor = descriptor;
        const auto handle = "column-binding-output-" + std::to_string(++next_handle);
        descriptors.emplace(handle, descriptor);
        return {{"output_handle", handle}};
    }
    void write(const std::string &handle, std::uint64_t,
               const std::vector<unsigned char> &bytes) override {
        require(descriptors.contains(handle), "operation wrote an unknown output handle");
        require(descriptors.at(handle).value("kind", std::string{}) == "json",
                "column binder encoded table rows instead of writing Arrow batches");
        json_bytes.insert(json_bytes.end(), bytes.begin(), bytes.end());
    }
    void write_table_batch(const std::string &handle, std::uint64_t sequence,
                           const std::shared_ptr<arrow::RecordBatch> &batch) override {
        require(descriptors.contains(handle) &&
                    descriptors.at(handle).value("kind", std::string{}) == "table",
                "column binder used an unknown table output handle");
        require(sequence == batches.size(), "column binder emitted an invalid batch sequence");
        batches.push_back(batch);
    }
    Json finish(const std::string &handle, const Json &metadata) override {
        require(descriptors.contains(handle), "operation finished an unknown output handle");
        finish_metadata = metadata;
        const auto kind = descriptors.at(handle).value("kind", std::string{});
        return {{"result_id", kind == "table" ? "bound-result" : "success-result"}};
    }
    void abort(const std::string &, const std::string &reason) override {
        abort_reason = reason;
    }

    Json output_descriptor = Json::object();
    Json finish_metadata = Json::object();
    std::unordered_map<std::string, Json> descriptors;
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    std::vector<unsigned char> json_bytes;
    std::string abort_reason;
    std::size_t next_handle{0};
};

class BindingContext final : public OperationExecutionContext {
public:
    BindingContext(Json descriptors, std::unordered_map<std::string, std::filesystem::path> paths)
        : descriptors_(std::move(descriptors)), paths_(std::move(paths)) {}

    std::string operation_instance() const override { return "column-binding-test"; }
    int workflow_revision() const override { return 4; }
    Json query(std::string_view) override { throw std::runtime_error("unexpected SQL query"); }
    void execute(std::string_view) override { throw std::runtime_error("unexpected SQL statement"); }
    void log(std::string_view) override {}
    Json resolve_inputs(const Json &inputs) override { return inputs; }
    Json request_data(const data_proxy::Envelope &request) override {
        const auto method = request.payload.value("method", std::string{});
        const auto result_id = request.payload.value("result_id", std::string{});
        if (method == "describe_result") {
            const auto found = descriptors_.find(result_id);
            if (found == descriptors_.end()) throw std::runtime_error("unknown Arrow result descriptor");
            return data_proxy::response(request, found.value()).to_json();
        }
        if (method == "lease_result_input") {
            if (!paths_.contains(result_id)) throw std::runtime_error("unknown Arrow result file");
            pending_result_id_ = result_id;
            return data_proxy::response(request, {{"lease_id", "read-" + result_id}}).to_json();
        }
        throw std::runtime_error("unexpected result-store request: " + method);
    }
    std::intptr_t receive_result_input_handle() override {
        if (pending_result_id_.empty()) throw std::runtime_error("no input read was granted");
        const auto id = std::exchange(pending_result_id_, {});
        return open_read_handle(paths_.at(id));
    }
    std::string data_proxy_session_id() const override { return "column-binding-session"; }
    std::string data_proxy_run_id() const override { return "column-binding-run"; }
    bool has_output_sink() const override { return true; }
    std::string computation_key() const override { return {}; }
    OperationOutputSink &outputs() override { return output; }
    void report_progress(double, std::string_view) override {}
    void report_event(std::string_view, const Json &) override {}
    bool cancellation_requested() const override { return false; }

    CapturingOutput output;

private:
    Json descriptors_;
    std::unordered_map<std::string, std::filesystem::path> paths_;
    std::string pending_result_id_;
};

Json descriptor(std::uint64_t rows, const std::string &name, const std::string &type) {
    return {{"state", "published"}, {"kind", "table"}, {"format", "arrow-ipc-file"},
            {"row_count", rows}, {"schema", {{{"name", name}, {"type", type}}}}};
}

void run_case(const Operation &binding,
              const std::filesystem::path &left_path,
              const std::filesystem::path &right_path,
              std::uint64_t row_count,
              const std::vector<std::string> &left_values,
              const std::vector<std::int64_t> &right_values) {
    BindingContext context(
        {{"left", descriptor(row_count, "left_name", "string")},
         {"right", descriptor(row_count, "right_value", "integer")}},
        {{"left", left_path}, {"right", right_path}});
    const auto result = binding.run_context(context, Json::object(), "3", {
        {"leftTable", {{"result_id", "left"}, {"output_port_id", "anyTable"}}},
        {"rightTable", {{"result_id", "right"}, {"output_port_id", "anyTable"}}}});
    require(result.at("emitted_results").at("anyTable").at("result_id") == "bound-result",
            "column binder did not return the published table result");
    require(context.output.abort_reason.empty(), "column binder aborted a valid result");
    const auto expected_schema = arrow::schema({arrow::field("left_name", arrow::utf8()),
                                                arrow::field("right_value", arrow::int64())});
    require(context.output.output_descriptor.at("row_count") == row_count,
            "column binder changed the declared row count");
    require(context.output.batches.size() == (row_count == 0 ? 0 : 1),
            "column binder emitted an unexpected number of logical windows");
    std::vector<std::string> observed_left;
    std::vector<std::int64_t> observed_right;
    for (const auto &batch : context.output.batches) {
        require(batch->schema()->Equals(*expected_schema), "column binder changed schema order or Arrow types");
        const auto names = std::static_pointer_cast<arrow::StringArray>(batch->column(0));
        const auto values = std::static_pointer_cast<arrow::Int64Array>(batch->column(1));
        for (std::int64_t row = 0; row < batch->num_rows(); ++row) {
            observed_left.push_back(names->GetString(row));
            observed_right.push_back(values->Value(row));
        }
    }
    require(observed_left == left_values && observed_right == right_values,
            "column binder did not concatenate values by row position");
}

}  // namespace streamfind::table_column_binding_contract_detail

int main() {
    using namespace streamfind::table_column_binding_contract_detail;
    const auto project_directory = std::filesystem::path(STREAMFIND_TEST_REPO_ROOT) / "tmp" / "projects";
    std::filesystem::create_directories(project_directory);
    const auto left_path = project_directory / "column-binding-left.arrow";
    const auto right_path = project_directory / "column-binding-right.arrow";
    const auto empty_left_path = project_directory / "column-binding-empty-left.arrow";
    const auto empty_right_path = project_directory / "column-binding-empty-right.arrow";
    try {
        const auto entries = streamfind::catalogue::load(STREAMFIND_CATALOGUE_PATH);
        require(entries.has_value(), "runtime catalogue could not be loaded");
        streamfind::OperationRegistry operations;
        streamfind::core_operations::register_operations(*entries, operations);
        const auto *binding = operations.find("streamfind.bind_table_columns");
        require(binding != nullptr, "generic column-binding operation is not registered");

        const auto left_schema = arrow::schema({arrow::field("left_name", arrow::utf8())});
        const auto right_schema = arrow::schema({arrow::field("right_value", arrow::int64())});
        write_arrow_file(left_path, left_schema, {string_batch(left_schema, {"row-a"}),
                                                  string_batch(left_schema, {"row-b", "row-c"})});
        write_arrow_file(right_path, right_schema, {integer_batch(right_schema, {10, 20}),
                                                    integer_batch(right_schema, {30})});
        run_case(*binding, left_path, right_path, 3,
                 {"row-a", "row-b", "row-c"}, {10, 20, 30});

        write_arrow_file(empty_left_path, left_schema, {});
        write_arrow_file(empty_right_path, right_schema, {});
        run_case(*binding, empty_left_path, empty_right_path, 0, {}, {});

        std::error_code ignored;
        for (const auto &path : {left_path, right_path, empty_left_path, empty_right_path})
            std::filesystem::remove(path, ignored);
        return 0;
    } catch (const std::exception &error) {
        std::error_code ignored;
        for (const auto &path : {left_path, right_path, empty_left_path, empty_right_path})
            std::filesystem::remove(path, ignored);
        std::cerr << "failure: " << error.what() << '\n';
        return 3;
    }
}
