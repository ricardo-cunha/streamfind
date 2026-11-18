#include <iostream>
#include <stdexcept>

#include "streamfind/catalogue.hpp"
#include "streamfind/core_operations.hpp"
#include "streamfind/project.hpp"

namespace streamfind::table_contract_test_detail {

class BindingOutputSink final : public streamfind::OperationOutputSink {
public:
    int begin_calls{0};

    streamfind::Json begin(const streamfind::Json &) override {
        ++begin_calls;
        throw std::runtime_error("binding validation must finish before output allocation");
    }
    void write(const std::string &, std::uint64_t,
               const std::vector<unsigned char> &) override {
        throw std::runtime_error("unexpected output write");
    }
    streamfind::Json finish(const std::string &, const streamfind::Json &) override {
        throw std::runtime_error("unexpected output finish");
    }
    void abort(const std::string &, const std::string &) override {}
};

class BindingExecutionContext final : public streamfind::OperationExecutionContext {
public:
    explicit BindingExecutionContext(streamfind::Json descriptors)
        : descriptors_(std::move(descriptors)) {}

    std::string operation_instance() const override { return "column-binding-contract"; }
    int workflow_revision() const override { return 1; }
    streamfind::Json query(std::string_view) override { throw std::runtime_error("unexpected query"); }
    void execute(std::string_view) override { throw std::runtime_error("unexpected execute"); }
    void log(std::string_view) override {}
    streamfind::Json resolve_inputs(const streamfind::Json &inputs) override { return inputs; }
    streamfind::Json request_data(const streamfind::data_proxy::Envelope &request) override {
        ++descriptor_requests;
        const auto result_id = request.payload.value("result_id", std::string{});
        const auto found = descriptors_.find(result_id);
        if (request.payload.value("method", std::string{}) != "describe_result" ||
            found == descriptors_.end())
            throw std::runtime_error("unexpected result-store request");
        return streamfind::data_proxy::response(request, found.value()).to_json();
    }
    std::string data_proxy_session_id() const override { return "contract-session"; }
    std::string data_proxy_run_id() const override { return "contract-run"; }
    bool has_output_sink() const override { return true; }
    std::string computation_key() const override { return {}; }
    streamfind::OperationOutputSink &outputs() override { return sink; }
    void report_progress(double, std::string_view) override {}
    void report_event(std::string_view, const streamfind::Json &) override {}
    bool cancellation_requested() const override { return false; }

    int descriptor_requests{0};
    BindingOutputSink sink;

private:
    streamfind::Json descriptors_;
};

void require_binding_rejected(const streamfind::Operation &binding,
                              const streamfind::Json &descriptors,
                              const std::string &message_fragment) {
    BindingExecutionContext context(descriptors);
    bool rejected = false;
    std::string diagnostic;
    try {
        binding.run_context(context, streamfind::Json::object(), "3", {
            {"leftTable", {{"result_id", "left"}, {"output_port_id", "anyTable"}}},
            {"rightTable", {{"result_id", "right"}, {"output_port_id", "anyTable"}}}
        });
    } catch (const std::exception &error) {
        diagnostic = error.what();
        rejected = diagnostic.find(message_fragment) != std::string::npos;
    }
    if (!rejected)
        throw std::runtime_error("column binding did not reject invalid Arrow table inputs: " + message_fragment +
                                 "; observed: " + diagnostic);
    if (context.sink.begin_calls != 0)
        throw std::runtime_error("column binding allocated an output before validating its input schemas");
}

streamfind::Json table_descriptor(std::uint64_t row_count,
                                  std::initializer_list<std::string> column_names) {
    streamfind::Json schema = streamfind::Json::array();
    for (const auto &name : column_names)
        schema.push_back({{"name", name}, {"type", "string"}});
    return {{"state", "published"}, {"kind", "table"}, {"format", "arrow-ipc-file"},
            {"row_count", row_count}, {"schema", std::move(schema)}};
}

} // namespace streamfind::table_contract_test_detail

int main()
{
    try
    {
        streamfind::ParameterDefinition optional_table_parameter;
        optional_table_parameter.name = "optional_table";
        optional_table_parameter.type.kind = streamfind::ParameterType::table;
        optional_table_parameter.required = false;
        streamfind::ParameterSchema optional_table_schema;
        optional_table_schema.definitions.push_back(optional_table_parameter);
        if (!optional_table_schema.resolve_and_validate(streamfind::Json::object()).empty())
            throw std::runtime_error("omitted optional table parameter was materialized as a default value");

        if (!streamfind::table_contracts_compatible("featuresTable", "anyTable", "table", "table") ||
            !streamfind::table_contracts_compatible("anyTable", "featuresTable", "table", "table") ||
            streamfind::table_contracts_compatible("featuresTable", "anyTable", "json", "table"))
            throw std::runtime_error("central compatibility rule is incorrect");

        const auto entries = streamfind::catalogue::load(STREAMFIND_CATALOGUE_PATH);
        if (!entries)
            throw std::runtime_error("runtime catalogue could not be loaded");
        streamfind::OperationRegistry operations;
        streamfind::core_operations::register_operations(*entries, operations);
        const auto *writer = operations.find("streamfind.write_table_csv");
        if (!writer || writer->definition().input_ports.size() != 1 ||
            writer->definition().input_ports.front().semantic_contract != "anyTable")
            throw std::runtime_error("CSV writer does not expose the AnyTable contract");
        const auto *reader = operations.find("streamfind.read_csv");
        if (!reader || reader->definition().output_ports.empty() ||
            reader->definition().output_ports.front().data_kind != "table")
            throw std::runtime_error("CSV reader does not expose an AnyTable output");
        const auto *row_binding = operations.find("streamfind.bind_table_rows");
        const auto *column_binding = operations.find("streamfind.bind_table_columns");
        if (!row_binding || !column_binding || row_binding->definition().input_ports.size() != 2 ||
            column_binding->definition().input_ports.size() != 2 ||
            row_binding->definition().input_ports.at(0).semantic_contract != "anyTable" ||
            column_binding->definition().input_ports.at(0).semantic_contract != "anyTable" ||
            !row_binding->definition().parameters.definitions.empty() ||
            !column_binding->definition().parameters.definitions.empty())
            throw std::runtime_error("generic table binding operations are not registered correctly");

        streamfind::table_contract_test_detail::require_binding_rejected(*column_binding,
            {{"left", streamfind::table_contract_test_detail::table_descriptor(2, {"left_value"})},
             {"right", streamfind::table_contract_test_detail::table_descriptor(1, {"right_value"})}},
            "same number of rows");
        streamfind::table_contract_test_detail::require_binding_rejected(*column_binding,
            {{"left", streamfind::table_contract_test_detail::table_descriptor(2, {"sample", "value"})},
             {"right", streamfind::table_contract_test_detail::table_descriptor(2, {"value", "category"})}},
            "unique across both inputs");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "failure: " << error.what() << '\n';
        return 3;
    }
}
