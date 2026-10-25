#ifndef _WIN32
#include <cstdlib>
#include "streamfind/service_configuration.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace streamfind::service::launcher_detail {

std::filesystem::path package_root(const char *executable) {
    auto root = std::filesystem::absolute(executable).parent_path();
    if (root.filename() == "bin") root = root.parent_path();
    return root;
}

std::filesystem::path cli_path(const char *executable) {
    const auto cli = package_root(executable) / "bin" / "streamfind";
    if (!std::filesystem::exists(cli)) throw std::runtime_error("streamfind CLI is missing");
    return cli;
}

int run_service_command(int argc, char **argv) {
    const auto cli = cli_path(argv[0]);
    const auto child = fork();
    if (child < 0) throw std::runtime_error("unable to start streamfind CLI");
    if (child == 0) {
        std::vector<char *> arguments;
        arguments.reserve(static_cast<std::size_t>(argc) + 1);
        arguments.push_back(const_cast<char *>(cli.c_str()));
        for (int index = 1; index < argc; ++index) arguments.push_back(argv[index]);
        arguments.push_back(nullptr);
        execv(cli.c_str(), arguments.data());
        _exit(127);
    }
    int status = 1;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

void start_service(const char *executable) {
    const auto cli = cli_path(executable);
    const auto child = fork();
    if (child < 0) throw std::runtime_error("unable to start streamfind service command");
    if (child == 0) {
        execl(cli.c_str(), cli.c_str(), "service", "start", static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 1;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("streamfind service did not become ready");
    const auto configuration = streamfind::load_service_configuration(package_root(executable) / "bin" / "streamfind.json");
    if (configuration.tray_enabled && configuration.tray_start_with_app) {
        auto tray = package_root(executable) / "bin" / "streamfind-tray";
        if (!std::filesystem::exists(tray)) tray = package_root(executable) / "streamfind-tray";
        if (std::filesystem::exists(tray)) {
            const auto child = fork();
            if (child == 0) {
                setsid();
                execl(tray.c_str(), tray.c_str(), static_cast<char *>(nullptr));
                _exit(127);
            }
        }
    }
}

std::uint16_t service_port(const char *executable) {
    return streamfind::load_service_configuration(package_root(executable) / "bin" / "streamfind.json").port;
}

}  // namespace streamfind::service::launcher_detail

int main(int argc, char **argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "service")
            return streamfind::service::launcher_detail::run_service_command(argc, argv);
        streamfind::service::launcher_detail::start_service(argv[0]);
        const auto url = "http://127.0.0.1:" + std::to_string(streamfind::service::launcher_detail::service_port(argv[0])) + "/";
        std::cout << "\nstreamfind is ready.\n"
                  << "The background service remains running when this window or browser closes.\n"
                  << "Open the application: " << url << "\n"
                  << "Stop the service explicitly with: streamfind service stop --instance <id>\n"
                  << std::flush;
        const auto browser = fork();
        if (browser == 0) {
            execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "streamfind-app: " << error.what() << '\n';
        return 1;
    }
}
#endif
