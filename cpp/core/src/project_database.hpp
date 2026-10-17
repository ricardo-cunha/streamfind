#pragma once

#include <duckdb.h>

#include <memory>
#include <mutex>
#include <string>

namespace streamfind::detail {

/**
 * Owns the single DuckDB database handle for a Project.
 *
 * Sessions serialize access to that handle and keep their connection alive for
 * the complete scope of the caller. This avoids opening the same project file
 * repeatedly from independent DuckDB database handles.
 */
class ProjectDatabase : public std::enable_shared_from_this<ProjectDatabase> {
public:
    class Session {
    public:
        Session(std::shared_ptr<const ProjectDatabase> database, duckdb_connection connection,
                std::unique_lock<std::mutex> lock) noexcept
            : database_(std::move(database)), connection_(connection), lock_(std::move(lock)) {}
        ~Session();

        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;

        duckdb_connection get() const noexcept { return connection_; }

    private:
        std::shared_ptr<const ProjectDatabase> database_;
        duckdb_connection connection_;
        std::unique_lock<std::mutex> lock_;
    };

    static std::shared_ptr<ProjectDatabase> open(const std::string &path);
    ~ProjectDatabase();

    ProjectDatabase(const ProjectDatabase &) = delete;
    ProjectDatabase &operator=(const ProjectDatabase &) = delete;

    std::unique_ptr<Session> connect() const;

private:
    explicit ProjectDatabase(duckdb_database database) noexcept : database_(database) {}

    duckdb_database database_;
    mutable std::mutex mutex_;
};

}  // namespace streamfind::detail
