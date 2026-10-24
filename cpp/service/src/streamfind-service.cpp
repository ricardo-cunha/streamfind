#include "streamfind/service/service_server.hpp"
#include "streamfind/service/service_discovery.hpp"
#include "streamfind/service_configuration.hpp"
#include "streamfind/vendor_runtime.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    streamfind::configure_vendor_runtime_paths();
    const auto executable_path = argc > 0
                                     ? std::filesystem::absolute(argv[0])
                                     : std::filesystem::current_path() / "streamfind-service";
    std::uint16_t port = 0;
    try {
        port = streamfind::load_service_configuration(executable_path.parent_path() / "streamfind.json").port;
    } catch (const std::exception &error) {
        std::cerr << "streamfind-service: " << error.what() << '\n';
        return 2;
    }
    if (argc > 1) {
        const auto parsed = std::strtoul(argv[1], nullptr, 10);
        if (parsed > 65535) {
            std::cerr << "streamfind-service: invalid port\n";
            return 2;
        }
        port = static_cast<std::uint16_t>(parsed);
    }
    streamfind::service::ServiceDiscovery discovery;
    try {
        const auto application_root = executable_path.parent_path().parent_path() / "app";
        const auto record = discovery.publish(port, streamfind::service::kProtocolVersion);
        auto service_identity = record.to_json();
        service_identity.erase("token");
        streamfind::service::ServiceServer server(
            port, executable_path.parent_path() / "streamfind.json", application_root,
            service_identity, record.token);
        std::cout << "streamfind-service listening on port " << port << '\n' << std::flush;
        std::cout << "streamfind service instance " << record.service_instance_id
                  << " (PID " << record.process_id << ")\n" << std::flush;
        server.run();
        discovery.update_state("stopped");
        discovery.remove();
    } catch (const std::exception &error) {
        discovery.update_state("failed");
        discovery.remove();
        std::cerr << "streamfind-service: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
