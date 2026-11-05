#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 3 || std::string(argv[1]) != "--worker-config") return 2;
    std::string request_line;
    if (!std::getline(std::cin, request_line)) return 3;
    const auto request = nlohmann::json::parse(request_line);
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
    if (mode == "progress")
        std::cout << nlohmann::json{{"type", "operation.log"}, {"message", "fixture progress"}}.dump()
                  << '\n' << std::flush;
    auto result = nlohmann::json{{"ok", true}};
#if defined(__linux__)
    if (mode == "oom-score") {
        std::ifstream score("/proc/self/oom_score_adj");
        int value = -1;
        if (!(score >> value)) return 7;
        result["oom_score_adj"] = value;
    }
#endif
    std::cout << nlohmann::json{{"type", "result"}, {"result", result}}.dump() << '\n' << std::flush;
    return 0;
}
