#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include "streamfind/catalogue.hpp"
#include "streamfind/mcp.hpp"
#include "static_plugin_composition.hpp"

int main(int argc, char **argv) {
    std::string line;
    streamfind::MethodRegistry registry;
    streamfind::OperationRegistry operations;
    std::unique_ptr<streamfind::static_plugins::DynamicPluginRuntime> dynamic_plugins;
    try {
        const auto executable_path = argc > 0
                                         ? std::filesystem::absolute(argv[0])
                                         : std::filesystem::current_path() / "streamfind_mcp";
        const auto configuration_path = executable_path.parent_path() / "streamfind.json";
        if (!std::filesystem::exists(configuration_path))
            throw std::runtime_error("streamfind.json is required for dynamic plugin loading");
        dynamic_plugins = std::make_unique<streamfind::static_plugins::DynamicPluginRuntime>();
        dynamic_plugins->load_and_register(configuration_path, registry, operations);
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
