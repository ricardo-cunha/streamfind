#pragma once

#include <filesystem>
#include <vector>

namespace streamfind::service::detail {

std::vector<std::filesystem::path> workflow_demo_files(
    const std::filesystem::path &package_root);

}  // namespace streamfind::service::detail
