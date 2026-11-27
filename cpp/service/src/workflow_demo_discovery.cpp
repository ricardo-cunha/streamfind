#include "streamfind/service/workflow_demo_discovery.hpp"

#include <algorithm>

namespace streamfind::service::detail {

std::vector<std::filesystem::path> workflow_demo_files(
    const std::filesystem::path &package_root) {
    std::vector<std::filesystem::path> files;
    const auto plugins = package_root / "plugins";
    if (!std::filesystem::is_directory(plugins)) return files;

    for (const auto &group : std::filesystem::directory_iterator(plugins)) {
        if (!group.is_directory()) continue;
        for (const auto &plugin : std::filesystem::directory_iterator(group.path())) {
            if (!plugin.is_directory()) continue;
            const auto workflows = plugin.path() / "resources" / "workflows";
            if (!std::filesystem::is_directory(workflows)) continue;
            for (const auto &entry : std::filesystem::directory_iterator(workflows)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json")
                    files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace streamfind::service::detail
