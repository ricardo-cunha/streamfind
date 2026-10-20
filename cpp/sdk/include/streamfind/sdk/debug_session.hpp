#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include "streamfind/export.hpp"

namespace streamfind::sdk {

struct STREAMFIND_SDK_API DebugOptions {
    std::string analysis;
    double mz = 0.0;
    int spectrum_index = -1;
    bool enabled_override = false;
    bool include_generic_selectors = true;

    bool enabled() const noexcept {
        return enabled_override || !analysis.empty() || mz != 0.0 || spectrum_index >= 0;
    }
};

/**
 * In-memory diagnostic log owned by one plugin operation invocation.
 * Debugging must never change the analytical result or depend on a diagnostic
 * file being created beside the project.
 */
class STREAMFIND_SDK_API DebugSession final {
public:
    static DebugSession open(const std::filesystem::path &database_path,
                             std::string_view operation_id,
                             std::string_view operation_instance,
                             const DebugOptions &options);

    DebugSession(DebugSession &&other) noexcept;
    DebugSession &operator=(DebugSession &&other) noexcept;
    ~DebugSession();

    DebugSession(const DebugSession &) = delete;
    DebugSession &operator=(const DebugSession &) = delete;

    bool enabled() const noexcept { return enabled_; }
    bool has_mz_selector() const noexcept { return options_.mz != 0.0; }
    bool has_spectrum_selector() const noexcept { return options_.spectrum_index >= 0; }
    bool matches_analysis(std::string_view analysis) const noexcept;
    bool matches_mz(double mz, double tolerance = 1e-5) const noexcept;
    bool matches_spectrum(int spectrum_index) const noexcept;

    void write_line(std::string_view message);
    std::string text() const;

private:
    DebugSession() = default;

    bool enabled_ = false;
    DebugOptions options_;
    std::string text_;
    mutable std::mutex mutex_;
};

} // namespace streamfind::sdk
