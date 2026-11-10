#include <nlohmann/json.hpp>
#include "streamfind/service/worker_handle_broker.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

int main(int argc, char **argv) {
    if (argc != 3 || std::string(argv[1]) != "--worker-config") return 2;
    std::string request_line;
    if (!std::getline(std::cin, request_line)) return 3;
    const auto request = nlohmann::json::parse(request_line);
    const auto broker_endpoint = request.value("worker_handle_endpoint", std::string{});
    if (broker_endpoint.empty()) return 8;
    const auto broker_channel = streamfind::service::connect_worker_handle_broker(broker_endpoint);
    const auto mode = request.value("parameters", nlohmann::json::object()).value("mode", "success");
    if (mode == "crash") return 23;
    if (mode == "block") {
        for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (mode == "memory") {
        std::vector<std::vector<char>> allocations;
        for (int block = 0; block < 128; ++block) {
            allocations.emplace_back(1024 * 1024, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    char received_payload = 0;
    std::string received_grant_token;
    if (mode == "broker-handle" || mode == "broker-write" || mode == "broker-read") {
        const auto received = streamfind::service::receive_worker_handle(broker_channel);
        received_grant_token = received.grant_token;
#ifdef _WIN32
        DWORD count = 0;
        bool read_ok = false;
        if (mode == "broker-write") {
            const char written = 'r';
            read_ok = WriteFile(received.handle, &written, 1, &count, nullptr) && count == 1;
        } else {
            read_ok = ReadFile(received.handle, &received_payload, 1, &count, nullptr) && count == 1;
        }
        CloseHandle(received.handle);
#else
        bool read_ok = false;
        if (mode == "broker-write") {
            const char written = 'r';
            read_ok = ::write(received.handle, &written, 1) == 1;
        } else {
            read_ok = ::read(received.handle, &received_payload, 1) == 1;
        }
        close(received.handle);
#endif
        if (!read_ok) return 9;
    }
    if (mode == "progress")
        std::cout << nlohmann::json{{"type", "operation.log"}, {"message", "fixture progress"}}.dump()
                  << '\n' << std::flush;
    auto result = nlohmann::json{{"ok", true}};
    if (mode == "broker-handle" || mode == "broker-write" || mode == "broker-read") {
        result["handle_received"] = mode == "broker-write" || received_payload == (mode == "broker-handle" ? 'g' : 'r');
        result["grant_token"] = received_grant_token;
    }
#if defined(__linux__)
    if (mode == "oom-score") {
        std::ifstream score("/proc/self/oom_score_adj");
        int value = -1;
        if (!(score >> value)) return 7;
        result["oom_score_adj"] = value;
    }
#endif
    std::cout << nlohmann::json{{"type", "result"}, {"result", result}}.dump() << '\n' << std::flush;
#ifdef _WIN32
    CloseHandle(broker_channel);
#else
    close(broker_channel);
#endif
    return 0;
}
