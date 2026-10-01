#include "streamfind/fingerprint.hpp"

#include <iostream>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
    try {
        const streamfind::Json first = {{"alpha", 1}, {"beta", {"x", true}}};
        const streamfind::Json reordered = {{"beta", {"x", true}}, {"alpha", 1}};
        require(streamfind::fingerprint::canonical_json(first) ==
                    streamfind::fingerprint::canonical_json(reordered),
                "object key order changed canonical JSON");
        require(streamfind::fingerprint::operation("node", "node", "1", first, {{"input", "artifact-a"}}) ==
                    streamfind::fingerprint::operation("node", "node", "1", reordered, {{"input", "artifact-a"}}),
                "object key order changed operation fingerprint");
        require(streamfind::fingerprint::operation("node", "node", "1", first, {{"input", "artifact-a"}}) !=
                    streamfind::fingerprint::operation("node", "node", "1", {{"alpha", 2}}, {{"input", "artifact-a"}}),
                "parameter change did not change operation fingerprint");
        require(streamfind::fingerprint::operation("node", "node", "1", first, {{"input", "artifact-a"}}) !=
                    streamfind::fingerprint::operation("node", "node", "2", first, {{"input", "artifact-a"}}),
                "operation version change did not change operation fingerprint");
        require(streamfind::fingerprint::operation("node", "node", "1", first, {{"input", "artifact-a"}}) !=
                    streamfind::fingerprint::operation("node", "node", "1", first, {{"input", "artifact-b"}}),
                "input identity change did not change operation fingerprint");
        require(streamfind::fingerprint::operation("node", "node-1", "1", first, {}) !=
                    streamfind::fingerprint::operation("node", "node-2", "1", first, {}),
                "operation instance change did not change operation fingerprint");
        require(streamfind::fingerprint::canonical_json(streamfind::Json::array({1, 2})) !=
                    streamfind::fingerprint::canonical_json(streamfind::Json::array({2, 1})),
                "array order was incorrectly normalized");
        const auto path = std::filesystem::path(STREAMFIND_TEST_REPO_ROOT) / "tmp" / "projects" / "fingerprint-state.txt";
        std::filesystem::create_directories(path.parent_path());
        {
            std::ofstream output(path, std::ios::trunc);
            output << "before";
        }
        const auto before = streamfind::fingerprint::operation("node", "node", "1", {{"file_path", path.string()}}, {});
        {
            std::ofstream output(path, std::ios::trunc);
            output << "after-content";
        }
        const auto after = streamfind::fingerprint::operation("node", "node", "1", {{"file_path", path.string()}}, {});
        std::filesystem::remove(path);
        require(before != after, "mutable file state did not change operation fingerprint");
        std::cout << "Fingerprint contract passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
