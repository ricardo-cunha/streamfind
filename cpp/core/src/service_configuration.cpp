#include "streamfind/service_configuration.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace streamfind {

ServiceConfiguration load_service_configuration(const std::filesystem::path &configuration_path) {
    ServiceConfiguration configuration;
    std::ifstream input(configuration_path, std::ios::binary);
    if (!input) return configuration;

    nlohmann::json document;
    input >> document;
    if (!document.is_object()) return configuration;
    if (document.contains("server")) {
        const auto &server = document.at("server");
        if (!server.is_object()) throw std::invalid_argument("streamfind.json server must be an object");
        if (server.contains("port")) {
            const auto &port = server.at("port");
            if (!port.is_number_unsigned() && !port.is_number_integer())
                throw std::invalid_argument("streamfind.json server.port must be an integer");
            const auto value = port.get<std::int64_t>();
            if (value < 1 || value > 65535)
                throw std::invalid_argument("streamfind.json server.port must be between 1 and 65535");
            configuration.port = static_cast<std::uint16_t>(value);
        }
    }
    if (document.contains("tray")) {
        const auto &tray = document.at("tray");
        if (!tray.is_object()) throw std::invalid_argument("streamfind.json tray must be an object");
        if (tray.contains("enabled")) configuration.tray_enabled = tray.at("enabled").get<bool>();
        if (tray.contains("start_with_app")) configuration.tray_start_with_app = tray.at("start_with_app").get<bool>();
        if (tray.contains("start_with_service")) configuration.tray_start_with_service = tray.at("start_with_service").get<bool>();
        if (tray.contains("notifications")) configuration.tray_notifications = tray.at("notifications").get<bool>();
        if (tray.contains("status_interval_ms")) {
            const auto value = tray.at("status_interval_ms").get<std::int64_t>();
            if (value < 250 || value > 60000)
                throw std::invalid_argument("streamfind.json tray.status_interval_ms must be between 250 and 60000");
            configuration.tray_status_interval_ms = static_cast<std::uint32_t>(value);
        }
    }
    return configuration;
}

}  // namespace streamfind
