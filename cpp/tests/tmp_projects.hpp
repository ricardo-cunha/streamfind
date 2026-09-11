#pragma once

// Test-output location policy (AGENTS.md "Repository Scratch, Build, and Log
// Locations (tmp/)") — every transient artifact must land under the
// repository-local tmp/ folder, never in the CWD, the source tree, or the
// system temp directory.
//
// DuckDB project files created by tests belong in <repo-root>/tmp/projects/.

#include <filesystem>
#include <string>

namespace streamfind::test {

// Returns <repo-root>/tmp/projects, creating it (and any missing parents) on
// first use. CMake supplies the absolute repository root so this remains
// independent of the executable's working directory and __FILE__ spelling.
inline std::filesystem::path tmp_projects_dir() {
    std::filesystem::path dir = std::filesystem::path(STREAMFIND_TEST_REPO_ROOT) / "tmp" / "projects";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    return dir;
}

}  // namespace streamfind::test