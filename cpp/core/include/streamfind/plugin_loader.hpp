#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "streamfind/export.hpp"

namespace streamfind {

class STREAMFIND_CORE_API DynamicLibrary {
public:
    DynamicLibrary() = default;
    DynamicLibrary(DynamicLibrary &&other) noexcept;
    DynamicLibrary &operator=(DynamicLibrary &&other) noexcept;
    DynamicLibrary(const DynamicLibrary &) = delete;
    DynamicLibrary &operator=(const DynamicLibrary &) = delete;
    ~DynamicLibrary();

    struct LoadResult {
        bool loaded{false};
        std::unique_ptr<DynamicLibrary> library;
        std::string diagnostics;
    };

    static LoadResult load(const std::filesystem::path &path);

    void *symbol(const char *name, std::string &diagnostics) const;
    const std::filesystem::path &path() const noexcept { return path_; }
    bool loaded() const noexcept { return handle_ != nullptr; }

private:
    DynamicLibrary(void *handle, std::filesystem::path path);
    void reset() noexcept;

    void *handle_{nullptr};
    std::filesystem::path path_;
};

}  // namespace streamfind
