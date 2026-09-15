#include "streamfind/sdk/semantic_resources.hpp"

#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace streamfind::sdk {

namespace detail {

std::filesystem::path executable_dir() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : path.parent_path();
#endif
}

}  // namespace detail

std::vector<std::filesystem::path> semantic_files(const std::filesystem::path &directory) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::exists(directory)) return files;
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".ttl") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::filesystem::path streamfind_home() {
    if (const char *override_home = std::getenv("STREAMFIND_HOME"); override_home && *override_home)
        return override_home;
#ifdef _WIN32
    if (const char *profile = std::getenv("USERPROFILE"); profile && *profile)
        return std::filesystem::path(profile) / ".streamfind";
#else
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".streamfind";
#endif
    return {};
}

std::filesystem::path jena_home() {
#ifdef STREAMFIND_JENA_VENDOR_DIR
    const std::filesystem::path vendored(STREAMFIND_JENA_VENDOR_DIR);
    if (std::filesystem::exists(vendored)) return vendored;
#endif
    const auto installed = detail::executable_dir().parent_path() / "share" / "streamfind" / "tools" / "jena";
    if (std::filesystem::exists(installed)) return installed;
#ifdef _WIN32
    if (const char *profile = std::getenv("USERPROFILE"); profile && *profile)
        return std::filesystem::path(profile) / ".streamfind" / "tools" / "jena";
#else
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".streamfind" / "tools" / "jena";
#endif
    return {};
}

}  // namespace streamfind::sdk
