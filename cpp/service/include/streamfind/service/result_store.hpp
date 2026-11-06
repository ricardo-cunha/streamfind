#pragma once

#include "streamfind/project.hpp"

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace streamfind::service {

enum class ResultStoreStatus {
    missing,
    ready,
    invalid,
};

struct ResultStoreInspection {
    ResultStoreStatus status{ResultStoreStatus::missing};
    std::filesystem::path root;
    std::string store_id;
    std::string error;
    std::size_t published_count{0};
    std::size_t invalid_count{0};
    std::size_t staging_run_count{0};
    std::uintmax_t size_bytes{0};

    Json to_json() const;
};

struct ResultLease {
    std::string result_id;
    std::string run_id;
    std::string format;
    std::string owner_session_id;
    std::string owner_operation_id;
    std::string owner_worker_id;
    std::filesystem::path staging_directory;
    std::filesystem::path staging_payload;
    std::filesystem::path published_directory;
    std::filesystem::path published_payload;
    std::shared_ptr<class ResultStagingHandle> writer;
};

class ResultStagingHandle {
public:
    explicit ResultStagingHandle(std::intptr_t native_handle) : native_handle_(native_handle) {}
    ~ResultStagingHandle();
    ResultStagingHandle(const ResultStagingHandle &) = delete;
    ResultStagingHandle &operator=(const ResultStagingHandle &) = delete;
    std::intptr_t native_handle() const noexcept { return native_handle_; }
    void close() noexcept;
private:
    std::intptr_t native_handle_{-1};
};

/**
 * Service-owned filesystem boundary for external result payloads.
 *
 * It provides isolated staging, validation, and atomic promotion of closed
 * payloads. Project metadata registration is committed only after publish()
 * succeeds.
 */
class ResultStore {
public:
    static constexpr const char *kLayoutVersion = "1";

    explicit ResultStore(std::filesystem::path database_path);

    static std::filesystem::path data_path_for_database(const std::filesystem::path &database_path);

    const std::filesystem::path &database_path() const noexcept { return database_path_; }
    const std::filesystem::path &root() const noexcept { return root_; }

    ResultStoreInspection inspect(bool verify_payloads = true) const;
    ResultStoreInspection initialize(bool verify_payloads = false);
    /** Remove only empty run/staging directories left by completed leases. */
    void cleanup_empty_runs() const;
    /** Return integrity-validated published result descriptors. */
    Json published_results() const;
    /** Return one checksum-validated reusable result, or null on a miss. */
    Json find_cached_result(const std::string &computation_key) const;
    /** Return all checksum-validated results for a computation key. */
    Json find_cached_results(const std::string &computation_key) const;

    /** Remove published payload directories after their registry entries are cleared. */
    void remove_published_results(const std::vector<std::string> &result_ids) const;

    /** Return a format-neutral descriptor without exposing the payload path. */
    Json describe_result(const std::string &result_id) const;

    /** Load/store a derived equality index for semantics-declared navigation columns. */
    Json read_navigation_index(const std::string &result_id) const;
    void write_navigation_index(const std::string &result_id, const Json &index) const;
    /** Read/write one digest-bound per-batch numeric range index for a declared navigation column. */
    Json read_range_statistics(const std::string &result_id, const std::string &column) const;
    void write_range_statistics(const std::string &result_id,
                                const std::string &column,
                                const Json &batches) const;

    /** Read a bounded byte range for generic payload consumers. */
    Json read_result_range(const std::string &result_id,
                           std::uintmax_t offset,
                           std::uintmax_t length) const;
    /** Read and parse a bounded JSON result payload. */
    Json read_json_payload(const std::string &result_id) const;
    Json read_arrow_ipc_batch(const std::string &result_id,
                              std::uintmax_t offset,
                              std::uintmax_t limit,
                              const std::vector<std::string> &columns = {}) const;
    /** Evaluate supported scalar predicates against Arrow arrays and return matching row offsets. */
    Json scan_arrow_scalar_filters(const std::string &result_id,
                                   const Json &filters,
                                   std::uintmax_t offset,
                                   std::uintmax_t limit) const;
    /** Open an immutable published payload for a scoped worker read grant. */
    std::shared_ptr<ResultStagingHandle> open_published_result(const std::string &result_id) const;

    ResultLease allocate(const std::string &run_id,
                           const std::string &result_id,
                           const std::string &format,
                           const std::string &extension);

    /** Promote a closed staging payload without replacing an existing result. */
    Json publish(const ResultLease &lease, const Json &metadata);

    /** Remove an unpublished lease payload after an operation aborts. */
    void abort(const ResultLease &lease) const;

private:
    static void require_component(const std::string &value, const char *name);
    static std::string create_store_id();
    static std::string normalize_extension(const std::string &extension);
    static void write_json_file(const std::filesystem::path &path, const Json &value);
    static Json read_json_file(const std::filesystem::path &path);

    std::filesystem::path database_path_;
    std::filesystem::path root_;
};

}  // namespace streamfind::service
