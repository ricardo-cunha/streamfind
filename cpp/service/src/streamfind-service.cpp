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
                                     : std::filesystem::current_path() / "streamfind-server";
    std::uint16_t port = 0;
    std::filesystem::path configuration_path;
    streamfind::ServiceConfiguration configuration;
    try {
        configuration_path = executable_path.parent_path() / "streamfind.json";
        configuration = streamfind::load_service_configuration(configuration_path);
        port = configuration.port;
    } catch (const std::exception &error) {
        std::cerr << "streamfind-server: " << error.what() << '\n';
        return 2;
    }
    if (argc > 1) {
        const auto parsed = std::strtoul(argv[1], nullptr, 10);
        if (parsed > 65535) {
            std::cerr << "streamfind-server: invalid port\n";
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
            port, configuration_path, application_root,
            service_identity, record.token);
        std::cout << "streamfind-server listening on port " << port << '\n' << std::flush;
        std::cout << "streamfind service instance " << record.service_instance_id
                  << " (PID " << record.process_id << ")\n" << std::flush;
        server.run();
        discovery.update_state("stopped");
        discovery.remove();
    } catch (const std::exception &error) {
        discovery.update_state("failed");
        discovery.remove();
        std::cerr << "streamfind-server: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
