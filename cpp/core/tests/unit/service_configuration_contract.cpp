#include "streamfind/service_configuration.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        const auto missing = streamfind::load_service_configuration(
            std::filesystem::path(STREAMFIND_TEST_CONFIG_DIR) / "missing.json");
        if (missing.worker_memory_limit_mb != 0) return 1;

        const auto configured = streamfind::load_service_configuration(
            std::filesystem::path(STREAMFIND_TEST_CONFIG_DIR) / "worker-memory-valid.json");
        if (configured.worker_memory_limit_mb != 3072) return 2;

        try {
            (void)streamfind::load_service_configuration(
                std::filesystem::path(STREAMFIND_TEST_CONFIG_DIR) / "worker-memory-invalid.json");
            return 3;
        } catch (const std::invalid_argument &) {
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 4;
    }
    return 0;
}
