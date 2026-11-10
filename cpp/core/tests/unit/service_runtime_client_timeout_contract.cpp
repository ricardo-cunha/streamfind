#include "streamfind/service_runtime_client.hpp"

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace streamfind::service_runtime_test {

void close_socket(std::intptr_t socket) {
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(socket));
#else
    close(static_cast<int>(socket));
#endif
}

void set_environment(const char *name, const std::string &value) {
#ifdef _WIN32
    if (_putenv_s(name, value.c_str()) != 0) throw std::runtime_error("cannot set test environment");
#else
    if (setenv(name, value.c_str(), 1) != 0) throw std::runtime_error("cannot set test environment");
#endif
}

void unset_environment(const char *name) {
#ifdef _WIN32
    if (_putenv_s(name, "") != 0) throw std::runtime_error("cannot clear test environment");
#else
    if (unsetenv(name) != 0) throw std::runtime_error("cannot clear test environment");
#endif
}

}  // namespace streamfind::service_runtime_test

int main() {
#ifdef _WIN32
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
#endif
    using namespace streamfind::service_runtime_test;
#ifdef _WIN32
    const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return 2;
#else
    const auto listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 2;
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        close_socket(static_cast<std::intptr_t>(listener));
#ifdef _WIN32
        WSACleanup();
#endif
        return 3;
    }
#ifdef _WIN32
    int address_length = sizeof(address);
#else
    socklen_t address_length = sizeof(address);
#endif
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &address_length) != 0) {
        close_socket(static_cast<std::intptr_t>(listener));
#ifdef _WIN32
        WSACleanup();
#endif
        return 4;
    }
    const auto endpoint = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    std::thread unresponsive_service([listener] {
#ifdef _WIN32
        const auto client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1600));
        closesocket(client);
#else
        const auto client = accept(listener, nullptr, nullptr);
        if (client < 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1600));
        close(client);
#endif
    });

    int result = 0;
    try {
        set_environment("STREAMFIND_SERVICE_ENDPOINT", endpoint);
        set_environment("STREAMFIND_SERVICE_REQUEST_TIMEOUT_SECONDS", "1");
        const auto started = std::chrono::steady_clock::now();
        try {
            (void)streamfind::service::create_project("timeout-contract.duckdb");
            result = 5;
        } catch (const std::exception &error) {
            const auto elapsed = std::chrono::steady_clock::now() - started;
            if (std::string(error.what()).find("timed out after 1 seconds") == std::string::npos ||
                elapsed >= std::chrono::seconds(2)) result = 6;
        }
    } catch (...) {
        result = 7;
    }

    try {
        unset_environment("STREAMFIND_SERVICE_ENDPOINT");
        unset_environment("STREAMFIND_SERVICE_REQUEST_TIMEOUT_SECONDS");
    } catch (...) {
        result = 8;
    }
    close_socket(static_cast<std::intptr_t>(listener));
    unresponsive_service.join();
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}
