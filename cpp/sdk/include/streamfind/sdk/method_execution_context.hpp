#pragma once

#include <functional>
#include <string_view>

#include <nlohmann/json.hpp>

#include "streamfind/export.hpp"

namespace streamfind::sdk {

/**
 * @brief Project-scoped services available to a plugin method.
 *
 * This is an in-process contract for static plugin composition. It deliberately
 * does not expose Project, MCP, catalogue loading, DuckDB handles, or domain
 * headers. The binary-safe ABI version will be defined separately before runtime
 * DLL loading.
 */
class STREAMFIND_SDK_API MethodExecutionContext {
public:
    virtual ~MethodExecutionContext() = default;
    virtual nlohmann::json query(std::string_view sql) const = 0;
    virtual void execute(std::string_view sql) = 0;
    virtual bool has_table(std::string_view table_name) const = 0;
    virtual nlohmann::json metadata() const = 0;
    virtual bool cancellation_requested() const noexcept = 0;
    virtual void report_progress(double fraction, std::string_view message) = 0;
    virtual void log(std::string_view level, std::string_view message) = 0;
};

/** @brief Host-owned live services supplied during one method invocation. */
struct STREAMFIND_SDK_API ExecutionServices {
    std::function<bool()> cancellation_requested;
    std::function<void(double, std::string_view)> report_progress;
    std::function<void(std::string_view, std::string_view)> log;
};

using ContextMethodExecutor =
    std::function<nlohmann::json(MethodExecutionContext &, const nlohmann::json &)>;

}  // namespace streamfind::sdk
