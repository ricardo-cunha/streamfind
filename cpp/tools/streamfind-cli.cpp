// streamfind-cli — minimal command-line interface for the C++ core, mirroring
// the Rust CLI (rust/crates/cli). Commands:
//   streamfind-cli create               --database-path <path> [--domain <d>]
//   streamfind-cli describe             --database-path <path>


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

}  // namespace

int main(int argc, char **argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    try {
        const std::string command = args.empty() ? "describe" : args[0];
        if (command == "tools") {
            if (args.size() != 3 || args[1] != "install")
                throw streamfind::Error(streamfind::ErrorCode::InvalidArgument,
                                        "usage: streamfind-cli tools install <java|metfrag>");
            if (args[2] == "java") {
                std::cout << streamfind::mass_spec::tools::install_java() << '\n';
                return 0;
            }
            if (args[2] == "metfrag") {
                std::cout << streamfind::mass_spec::tools::install_metfrag() << '\n';
                return 0;
            }
            throw streamfind::Error(streamfind::ErrorCode::InvalidArgument,
                                    "unknown tool; expected java or metfrag");
        }
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