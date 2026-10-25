#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "streamfind/data_proxy_protocol.hpp"
#include "streamfind/export.hpp"
#include "streamfind/operation_output.hpp"

namespace streamfind {

using Json = nlohmann::json;

/**
 * Runtime boundary exposed to operation algorithms.
 *
 * Operation code may use this context for workflow inputs, typed data access,
 * result publication, cache decisions, progress, and cancellation. It must
 * never expose a DuckDB connection, database path, or worker filesystem path.
 * The service and worker implementations will use the same protocol-backed
 * contract once operation migration is complete.
 */
class STREAMFIND_CORE_API OperationExecutionContext {
public:
    using ProgressCallback = std::function<void(double, std::string_view)>;
    using EventCallback = std::function<void(std::string_view, const Json &)>;

    virtual ~OperationExecutionContext() = default;

    virtual std::string operation_instance() const = 0;
    virtual int workflow_revision() const = 0;
    /** Execute a runtime-owned read query without exposing a database handle. */
    virtual Json query(std::string_view sql) = 0;
    /** Execute a runtime-owned write statement without exposing a database handle. */
    virtual void execute(std::string_view sql) = 0;
    /** Prepare the host-owned table contract for direct plugin execution. */
    virtual void prepare_table_domain(std::string_view domain,
                                      std::string_view module_id) = 0;
    /** Forward host/plugin diagnostics into the runtime event stream. */
    virtual void log(std::string_view message) = 0;
    virtual Json resolve_inputs(const Json &provided_inputs) = 0;
    virtual Json request_data(const data_proxy::Envelope &request) = 0;
    /** Routing identity used when the SDK opens a service-owned data-proxy request. */
    virtual std::string data_proxy_session_id() const { return {}; }
    virtual std::string data_proxy_run_id() const { return {}; }
    virtual Json lookup_result_cache(const std::string &computation_key) = 0;
    virtual bool has_output_sink() const = 0;
    virtual std::string computation_key() const = 0;
    virtual Json publish_result(const std::string &contract_id,
                                const Json &descriptor,
                                const Json &provenance) = 0;
    /** Return the runtime-owned output sink for this operation. */
    virtual OperationOutputSink &outputs() = 0;
    virtual void report_progress(double fraction, std::string_view message) = 0;
    virtual void report_event(std::string_view event, const Json &payload) = 0;
    virtual bool cancellation_requested() const = 0;
};

}  // namespace streamfind
