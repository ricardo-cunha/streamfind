#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "streamfind/export.hpp"
#include "streamfind/plugin_abi.h"
#include <arrow/record_batch.h>

namespace streamfind {

using Json = nlohmann::json;

/**
 * Runtime-owned destination for one operation output.
 *
 * Operation code submits typed descriptors and batches. It does not select a
 * file, persistence policy, cache location, or downstream transport. The
 * runtime grants a scoped destination and publishes the completed result.
 */
class STREAMFIND_CORE_API OperationOutputSink {
public:
    virtual ~OperationOutputSink() = default;

    /** Describe and allocate one output; returns an opaque output handle. */
    virtual Json begin(const Json &descriptor) = 0;

    /** Submit one bounded encoded batch for the opaque output handle. */
    virtual void write(const std::string &output_handle,
                       std::uint64_t sequence,
                       const std::vector<unsigned char> &batch) = 0;

    /** Submit a callback-scoped typed batch without materializing row JSON. */
    virtual void write_table_batch(
        const std::string &output_handle,
        std::uint64_t sequence,
        const std::shared_ptr<arrow::RecordBatch> &batch) {
        (void)output_handle;
        (void)sequence;
        (void)batch;
        throw std::logic_error("this output sink does not accept typed table batches");
    }

    /** Execute a runtime-owned SELECT directly into this table output. */
    virtual std::uint64_t write_query_result(const std::string &output_handle,
                                             std::uint64_t sequence,
                                             std::string_view sql) {
        (void)output_handle;
        (void)sequence;
        (void)sql;
        throw std::logic_error("this output sink does not support direct SQL-to-Arrow output");
    }

    /** Seal the output and return its runtime-owned reference. */
    virtual Json finish(const std::string &output_handle,
                        const Json &metadata = Json::object()) = 0;

    /** Abandon an incomplete output after an operation failure. */
    virtual void abort(const std::string &output_handle,
                       const std::string &reason) = 0;
};

}  // namespace streamfind
