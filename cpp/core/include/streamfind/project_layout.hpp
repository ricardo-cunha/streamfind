#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace streamfind {

/** Canonical paths for one user-visible StreamFind project container. */
class ProjectLayout {
public:
    static constexpr const char *kDirectoryExtension = ".sf";
    static constexpr const char *kDatabaseFilename = "project.duckdb";
    static constexpr const char *kDataDirectory = "data";

    static ProjectLayout from_container(const std::filesystem::path &path) {
        if (path.empty()) throw std::invalid_argument("Project folder path is required");
        auto extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension != kDirectoryExtension) {
            if (extension == ".duckdb")
                throw std::invalid_argument(
                    "A legacy DuckDB project file cannot be opened as a current project. "
                    "Open it with its matching streamfind release, export its workflow JSON, "
                    "then create a new .sf project and import and rerun that workflow.");
            throw std::invalid_argument("Project path must name a .sf folder");
        }
        std::error_code error;
        auto canonical = std::filesystem::weakly_canonical(path, error);
        if (error) {
            error.clear();
            canonical = std::filesystem::absolute(path, error).lexically_normal();
        }
        if (error) throw std::invalid_argument("Cannot resolve project folder path: " + error.message());
        return ProjectLayout(std::move(canonical));
    }

    const std::filesystem::path &container() const noexcept { return container_; }
    std::filesystem::path database() const { return container_ / kDatabaseFilename; }
    std::filesystem::path data() const { return container_ / kDataDirectory; }

private:
    explicit ProjectLayout(std::filesystem::path container) : container_(std::move(container)) {}
    std::filesystem::path container_;
};

inline std::filesystem::path path_from_utf8(const std::string &value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char byte : value) encoded.push_back(static_cast<char8_t>(byte));
    return std::filesystem::path(encoded);
}

inline std::string path_to_utf8(const std::filesystem::path &value) {
    const auto encoded = value.u8string();
    return std::string(encoded.begin(), encoded.end());
}

}  // namespace streamfind
