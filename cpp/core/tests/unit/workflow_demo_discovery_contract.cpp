#include "streamfind/service/workflow_demo_discovery.hpp"
#include "../support/tmp_projects.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace streamfind::test::workflow_demo_discovery {

int run() {
    const auto root = tmp_projects_dir() / "workflow-demo-discovery-contract";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    const auto packaged_demo = root / "plugins" / "native" /
        "native.plugin.mass_spec" / "resources" / "workflows" / "example.json";
    std::filesystem::create_directories(packaged_demo.parent_path());
    std::ofstream(packaged_demo) << "{}";

    const auto unrelated_json = root / "plugins" / "native" /
        "native.plugin.mass_spec" / "resources" / "example.json";
    std::ofstream(unrelated_json) << "{}";

    const auto files = streamfind::service::detail::workflow_demo_files(root);
    std::filesystem::remove_all(root, error);
    if (files.size() != 1 || files.front().filename() != "example.json" ||
        files.front().parent_path().filename() != "workflows") {
        std::cerr << "nested release workflow demo was not discovered exclusively\n";
        return 1;
    }
    return 0;
}

}  // namespace streamfind::test::workflow_demo_discovery

int main() { return streamfind::test::workflow_demo_discovery::run(); }
