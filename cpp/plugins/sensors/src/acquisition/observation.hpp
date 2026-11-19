#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace streamfind::sensors::acquisition {

enum class SensorValueKind {
    Missing,
    Real,
    Integer,
    Boolean,
    Text,
    Json,
};

struct SensorValue {
    SensorValueKind kind = SensorValueKind::Missing;
    std::optional<double> real;
    std::optional<std::int64_t> integer;
    std::optional<bool> boolean;
    std::optional<std::string> text;
};

struct SensorObservation {
    std::string source_id;
    std::string signal_id;
    std::string event_time;
    std::string received_at;
    std::string time_origin;
    SensorValue value;
    std::optional<std::string> unit;
    std::string quality;
    std::optional<std::string> status_code;
    std::string capture_id;
    std::string event_id;
};

struct CaptureRequest {
    std::chrono::milliseconds duration{1000};
    std::chrono::milliseconds sample_interval{1000};
    std::size_t max_samples = 1000;
    std::size_t max_buffer_bytes = 64 * 1024 * 1024;
};

}  // namespace streamfind::sensors::acquisition
