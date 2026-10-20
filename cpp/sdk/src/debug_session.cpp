#include "streamfind/sdk/debug_session.hpp"

#include <cmath>

namespace streamfind::sdk {

DebugSession DebugSession::open(const std::filesystem::path &database_path,
                                std::string_view operation_id,
                                std::string_view operation_instance,
                                const DebugOptions &options) {
    DebugSession session;
    session.options_ = options;
    session.enabled_ = options.enabled();
    if (!session.enabled_) return session;

    try {
        const auto instance = operation_instance.empty() ? "direct" : operation_instance;
        (void)database_path;
        session.write_line("StreamFind debug session");
        session.write_line("operation: " + std::string(operation_id));
        session.write_line("operation_instance: " + std::string(instance));
        session.write_line("database: " + database_path.string());
        if (options.include_generic_selectors) {
            session.write_line("analysis_selector: " + options.analysis);
            session.write_line("mz_selector: " + std::to_string(options.mz));
            session.write_line("spectrum_index_selector: " + std::to_string(options.spectrum_index));
        }
        session.write_line("");
    } catch (...) {
        session.enabled_ = false;
    }
    return session;
}

DebugSession::DebugSession(DebugSession &&other) noexcept {
    std::lock_guard lock(other.mutex_);
    enabled_ = other.enabled_;
    options_ = std::move(other.options_);
    text_ = std::move(other.text_);
    other.enabled_ = false;
}

DebugSession &DebugSession::operator=(DebugSession &&other) noexcept {
    if (this == &other) return *this;
    std::scoped_lock lock(mutex_, other.mutex_);
    enabled_ = other.enabled_;
    options_ = std::move(other.options_);
    text_ = std::move(other.text_);
    other.enabled_ = false;
    return *this;
}

DebugSession::~DebugSession() = default;

bool DebugSession::matches_analysis(std::string_view analysis) const noexcept {
    return options_.analysis.empty() || options_.analysis == analysis;
}

bool DebugSession::matches_mz(double mz, double tolerance) const noexcept {
    return has_mz_selector() && std::abs(options_.mz - mz) <= tolerance;
}

bool DebugSession::matches_spectrum(int spectrum_index) const noexcept {
    return has_spectrum_selector() && options_.spectrum_index == spectrum_index;
}

void DebugSession::write_line(std::string_view message) {
    if (!enabled_) return;
    std::lock_guard lock(mutex_);
    text_.append(message);
    text_.push_back('\n');
}

std::string DebugSession::text() const {
    std::lock_guard lock(mutex_);
    return text_;
}

} // namespace streamfind::sdk
