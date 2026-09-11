#include <iostream>
#include <string>
#include "streamfind/catalogue.hpp"
#include "streamfind/mcp.hpp"
#include "static_plugin_composition.hpp"

int main() {
    // The runtime knowledge base is a required installation artifact; refuse
    // to start when it cannot be located via the search chain.
    if (!streamfind::catalogue::load()) {
        std::cerr << "streamfind-mcp: fatal: " << streamfind::catalogue::load_error() << '\n';
        return 2;
    }
    std::string line;
    streamfind::MethodRegistry registry;
    streamfind::OperationRegistry operations;
    try {
        const auto aggregate_path = streamfind::catalogue::find_path();
        if (!aggregate_path) throw std::runtime_error("aggregate catalogue path is unavailable");
        streamfind::static_plugins::load_and_register(*aggregate_path, registry, operations);
    } catch (const std::exception &error) {
        std::cerr << "streamfind-mcp: registration failed: " << error.what() << '\n';
        return 3;
    }
    streamfind::mcp::Session session(registry, operations);
    while (std::getline(std::cin, line)) {
            try { std::cout << session.handle(streamfind::Json::parse(line)).dump() << '\n' << std::flush; }
            catch (const std::exception &error) { std::cout << streamfind::Json{{"jsonrpc", "2.0"}, {"error", {{"code", -32700}, {"message", error.what()}}}}.dump() << '\n' << std::flush; }
        }
}
