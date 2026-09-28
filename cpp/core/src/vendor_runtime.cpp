#include "streamfind/vendor_runtime.hpp"

#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace streamfind {

void configure_vendor_runtime_paths() {
#if defined(_WIN32)
    wchar_t buffer[32768];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    if (length == 0 || length >= sizeof(buffer) / sizeof(buffer[0]))
        return;

    const std::filesystem::path executable(buffer, buffer + length);
    const auto executable_directory = executable.parent_path();
    const auto package_root = std::filesystem::is_directory(executable_directory / L"core" / L"vendors")
        ? executable_directory
        : executable_directory.parent_path().parent_path();
    const auto vendors_directory = package_root / L"core" / L"vendors";
    const auto duckdb_directory = vendors_directory / L"duckdb";
    const auto mingw_directory = vendors_directory / L"mingw";
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
        LOAD_LIBRARY_SEARCH_USER_DIRS);
    for (const auto& directory : {duckdb_directory, mingw_directory}) {
        if (std::filesystem::is_directory(directory))
            AddDllDirectory(directory.c_str());
    }
    if (std::filesystem::is_directory(duckdb_directory))
        SetDllDirectoryW(duckdb_directory.c_str());
#endif
}

}  // namespace streamfind
