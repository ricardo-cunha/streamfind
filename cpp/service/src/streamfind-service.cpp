#include "streamfind/service/service_server.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    std::uint16_t port = 8787;
    if (argc > 1) {
        const auto parsed = std::strtoul(argv[1], nullptr, 10);
        if (parsed > 65535) {
            std::cerr << "streamfind_service: invalid port\n";
            return 2;
        }
        port = static_cast<std::uint16_t>(parsed);
    }
    try {
        const auto executable_path = argc > 0
                                         ? std::filesystem::absolute(argv[0])
                                         : std::filesystem::current_path() / "streamfind_service";
        const auto application_root = executable_path.parent_path().parent_path() / "share" / "streamfind" / "app";
        streamfind::service::ServiceServer server(port, executable_path.parent_path() / "streamfind.json", application_root);
        std::cout << "streamfind_service listening on port " << port << '\n' << std::flush;
        server.run();
    } catch (const std::exception &error) {
        std::cerr << "streamfind_service: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
