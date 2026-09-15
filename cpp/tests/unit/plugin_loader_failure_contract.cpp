#include <filesystem>
#include <string>

#include "streamfind/plugin_loader.hpp"

int main() {
    const auto missing = streamfind::DynamicLibrary::load(
        std::filesystem::path("tmp") / "missing-stage9-plugin");
    if (missing.loaded || missing.diagnostics.empty()) {
        return 1;
    }
    return 0;
}
