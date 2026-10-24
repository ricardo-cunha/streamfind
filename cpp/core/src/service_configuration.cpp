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
    if (!document.is_object() || !document.contains("server")) return configuration;
    const auto &server = document.at("server");
    if (!server.is_object()) throw std::invalid_argument("streamfind.json server must be an object");
    if (!server.contains("port")) return configuration;
    const auto &port = server.at("port");
    if (!port.is_number_unsigned() && !port.is_number_integer())
        throw std::invalid_argument("streamfind.json server.port must be an integer");
    const auto value = port.get<std::int64_t>();
    if (value < 1 || value > 65535)
        throw std::invalid_argument("streamfind.json server.port must be between 1 and 65535");
    configuration.port = static_cast<std::uint16_t>(value);
    return configuration;
}

}  // namespace streamfind
