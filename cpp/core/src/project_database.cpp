#include "project_database.hpp"

#include "streamfind/project.hpp"

#include <utility>

namespace streamfind::detail {

std::shared_ptr<ProjectDatabase> ProjectDatabase::open(const std::string &path) {
    duckdb_config config = nullptr;
    if (duckdb_create_config(&config) == DuckDBError)
        throw Error(ErrorCode::DatabaseError, "create DuckDB config failed");

    duckdb_database database = nullptr;
    char *error = nullptr;
    const auto state = duckdb_open_ext(path.c_str(), &database, config, &error);
    duckdb_destroy_config(&config);
    if (state == DuckDBError) {
        const std::string message = error ? error : "open DuckDB database failed";
        if (error) duckdb_free(error);
        throw Error(ErrorCode::DatabaseError, message);
    }
    return std::shared_ptr<ProjectDatabase>(new ProjectDatabase(database));
}

ProjectDatabase::~ProjectDatabase() {
    std::lock_guard lock(mutex_);
    if (database_) duckdb_close(&database_);
}

std::unique_ptr<ProjectDatabase::Session> ProjectDatabase::connect() const {
    std::unique_lock lock(mutex_);
    duckdb_connection connection = nullptr;
    if (duckdb_connect(database_, &connection) == DuckDBError)
        throw Error(ErrorCode::DatabaseError, "connect DuckDB database failed");
    return std::make_unique<Session>(shared_from_this(), connection, std::move(lock));
}

ProjectDatabase::Session::~Session() {
    if (connection_) duckdb_disconnect(&connection_);
}

}  // namespace streamfind::detail
