#include "streamfind/sdk/debug_session.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <thread>

namespace streamfind::sdk {
namespace detail {

std::string timestamp_utc() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream result;
    result << std::put_time(&utc, "%Y%m%dT%H%M%SZ");
    return result.str();
}

std::string safe_component(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_') {
            result.push_back(character);
        } else {
            result.push_back('_');
        }
    }
    return result.empty() ? "operation" : result;
}

} // namespace detail

DebugSession DebugSession::open(const std::filesystem::path &database_path,
                                std::string_view operation_id,
                                std::string_view operation_instance,
                                const DebugOptions &options) {
    DebugSession session;
    session.options_ = options;
    session.enabled_ = options.enabled();
    if (!session.enabled_) return session;

    try {
        const auto directory = database_path.parent_path() /
                               (database_path.stem().string() + ".streamfind-debug");
        std::filesystem::create_directories(directory);
        const auto instance = operation_instance.empty() ? "direct" : operation_instance;
        session.path_ = directory /
            (detail::safe_component(operation_id) + "_" +
             detail::timestamp_utc() + "_" + detail::safe_component(instance) + ".txt");
        session.stream_.open(session.path_, std::ios::out | std::ios::trunc);
        if (!session.stream_) {
            session.enabled_ = false;
            session.path_.clear();
            return session;
        }
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
        session.path_.clear();
    }
    return session;
}

DebugSession::DebugSession(DebugSession &&other) noexcept {
    std::lock_guard lock(other.mutex_);
    enabled_ = other.enabled_;
    options_ = std::move(other.options_);
    path_ = std::move(other.path_);
    stream_ = std::move(other.stream_);
    other.enabled_ = false;
}

DebugSession &DebugSession::operator=(DebugSession &&other) noexcept {
    if (this == &other) return *this;
    std::scoped_lock lock(mutex_, other.mutex_);
    enabled_ = other.enabled_;
    options_ = std::move(other.options_);
    path_ = std::move(other.path_);
    stream_ = std::move(other.stream_);
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
    if (!enabled_ || !stream_) return;
    std::lock_guard lock(mutex_);
    stream_ << message << '\n';
    stream_.flush();
}

} // namespace streamfind::sdk
