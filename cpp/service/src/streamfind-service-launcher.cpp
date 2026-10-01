#ifndef _WIN32
#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace streamfind::service::launcher_detail {
pid_t child_pid = -1;

void stop_child(int) {
    if (child_pid > 0) {
        kill(child_pid, SIGTERM);
        waitpid(child_pid, nullptr, 0);
    }
    std::_Exit(0);
}

std::uint16_t choose_port() {
    const auto probe = socket(AF_INET, SOCK_STREAM, 0);
    if (probe < 0) throw std::runtime_error("unable to allocate a local port");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        close(probe);
        throw std::runtime_error("unable to choose a local port");
    }
    socklen_t length = sizeof(address);
    if (getsockname(probe, reinterpret_cast<sockaddr *>(&address), &length) < 0) {
        close(probe);
        throw std::runtime_error("unable to read the chosen local port");
    }
    close(probe);
    return ntohs(address.sin_port);
}

bool backend_reachable(std::uint16_t port) {
    const auto socket_handle = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_handle < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const auto result = connect(socket_handle, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    close(socket_handle);
    return result == 0;
}

void open_browser(const std::string &url) {
    const auto browser_pid = fork();
    if (browser_pid == 0) {
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char *>(nullptr));
        std::_Exit(127);
    }
}
}

int main(int argc, char **argv) {
    using namespace streamfind::service::launcher_detail;
    try {
        if (argc < 1) throw std::runtime_error("unable to resolve launcher path");
        const auto package_root = std::filesystem::absolute(argv[0]).parent_path();
        const auto service = package_root / "bin" / "streamfind_service";
        if (!std::filesystem::exists(service)) throw std::runtime_error("bin/streamfind_service is missing");
        const auto port = choose_port();
        std::signal(SIGINT, stop_child);
        std::signal(SIGTERM, stop_child);
        child_pid = fork();
        if (child_pid < 0) throw std::runtime_error("unable to start streamfind_service");
        if (child_pid == 0) {
            const auto port_text = std::to_string(port);
            execl(service.c_str(), service.c_str(), port_text.c_str(), static_cast<char *>(nullptr));
            std::_Exit(127);
        }
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (backend_reachable(port)) break;
            if (waitpid(child_pid, nullptr, WNOHANG) == child_pid)
                throw std::runtime_error("streamfind_service exited during startup");
            usleep(100000);
            if (attempt == 99) throw std::runtime_error("streamfind_service did not become ready");
        }
        const auto url = std::string("http://127.0.0.1:") + std::to_string(port) + "/";
        std::cout << "streamfind is running\n"
                  << "  app:     " << url << "\n"
                  << "  backend: http://127.0.0.1:" << port << "\n"
                  << "Close this terminal to stop streamfind.\n" << std::flush;
        open_browser(url);
        waitpid(child_pid, nullptr, 0);
        child_pid = -1;
        return 0;
    } catch (const std::exception &error) {
        if (child_pid > 0) kill(child_pid, SIGTERM);
        std::cerr << "streamfind: " << error.what() << '\n';
        return 1;
    }
}
#endif
