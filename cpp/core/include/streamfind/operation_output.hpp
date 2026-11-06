#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "streamfind/export.hpp"

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

    /** Submit typed table rows; the runtime chooses the transport encoding. */
    virtual void write_rows(const std::string &output_handle,
                            std::uint64_t sequence,
                            const Json &rows) = 0;

    /** Seal the output and return its runtime-owned reference. */
    virtual Json finish(const std::string &output_handle,
                        const Json &metadata = Json::object()) = 0;

    /** Abandon an incomplete output after an operation failure. */
    virtual void abort(const std::string &output_handle,
                       const std::string &reason) = 0;
};

}  // namespace streamfind
