// streamfind-cli — minimal command-line interface for the C++ core, mirroring
// the Rust CLI (rust/crates/cli). Commands:
//   streamfind-cli create               --database-path <path> [--domain <d>]
//   streamfind-cli describe             --database-path <path>
//   streamfind-cli tools status|install|install-java|install-metfrag
// The tools subcommands manage the user-scoped ~/.streamfind external-tools
// layout (Java/Temurin JDK 21 + MetFragCL), mirroring bindings/r.

#include "streamfind/api.hpp"
#include "streamfind/project.hpp"
#include "streamfind/external/tools_resolver.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

std::string option(const std::vector<std::string> &args, const std::string &name) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == name) return args[i + 1];
    throw streamfind::Error(streamfind::ErrorCode::InvalidArgument,
                            "missing required option " + name);
}

int run_tools(const std::vector<std::string> &args) {
    const std::string sub = args.size() > 2 ? args[2] : "status";
    try {
        if (sub == "status") {
            std::cout << streamfind::tools::tool_status() << '\n';
        } else if (sub == "install") {
            const auto java = streamfind::tools::install_java();
            const auto metfrag = streamfind::tools::install_metfrag();
            std::cout << "java: " << java << "\nmetfrag: " << metfrag << '\n';
        } else if (sub == "install-java") {
            std::cout << "java: " << streamfind::tools::install_java() << '\n';
        } else if (sub == "install-metfrag") {
            std::cout << "metfrag: " << streamfind::tools::install_metfrag() << '\n';
        } else {
            throw streamfind::Error(streamfind::ErrorCode::InvalidArgument,
                                    "unknown tools subcommand '" + sub +
                                        "' (status | install | install-java | install-metfrag)");
        }
    } catch (const std::exception &error) {
        std::cerr << "tools " << sub << ": " << error.what() << '\n';
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty() && args[0] == "tools") return run_tools(args);
    try {
        const std::string command = args.empty() ? "describe" : args[0];
        streamfind::ProjectOptions options;
        options.database_path = option(args, "--database-path");
                for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "--domain") options.domain = args[i + 1];
                if (command == "create") {
            auto project = streamfind::Project::create(options);
            const auto &info = project.info();
            std::cout << "{\"domain\":\"" << info.domain << "\",\"domain\":\"" << info.domain
                      << "\",\"metadata\":" << info.metadata.dump() << "}\n";
        } else {
            auto project = streamfind::Project::open(options);
            const auto &info = project.info();
            std::cout << "{\"domain\":\"" << info.domain << "\",\"domain\":\"" << info.domain
                      << "\",\"metadata\":" << info.metadata.dump() << "}\n";
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}