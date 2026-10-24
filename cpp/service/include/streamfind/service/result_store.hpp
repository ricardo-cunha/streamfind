#pragma once

#include "streamfind/project.hpp"

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

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

    Json to_json() const;
};

struct ResultLease {
    std::string result_id;
    std::string run_id;
    std::string format;
    std::filesystem::path staging_directory;
    std::filesystem::path staging_payload;
    std::filesystem::path published_directory;
    std::filesystem::path published_payload;
};

namespace detail {
/** Decode one canonical base64 payload chunk received over the proxy. */
std::vector<unsigned char> decode_base64(const std::string &value);
}

/**
 * Service-owned filesystem boundary for external result payloads.
 *
 * This class does not register results in DuckDB yet. It provides the
 * filesystem half of the publication protocol: isolated staging and atomic
 * promotion of a closed payload. Callers must perform metadata registration
 * only after publish() succeeds.
 */
class ResultStore {
public:
    static constexpr const char *kLayoutVersion = "1";

    explicit ResultStore(std::filesystem::path database_path);

    static std::filesystem::path data_path_for_database(const std::filesystem::path &database_path);

    const std::filesystem::path &database_path() const noexcept { return database_path_; }
    const std::filesystem::path &root() const noexcept { return root_; }

    ResultStoreInspection inspect() const;
    ResultStoreInspection initialize();
    /** Return integrity-validated published result descriptors. */
    Json published_results() const;
    /** Return one checksum-validated reusable result, or null on a miss. */
    Json find_cached_result(const std::string &computation_key) const;
    /** Return all checksum-validated results for a computation key. */
    Json find_cached_results(const std::string &computation_key) const;

    /** Return a format-neutral descriptor without exposing the payload path. */
    Json describe_result(const std::string &result_id) const;

    /** Read a bounded byte range for generic payload consumers. */
    Json read_result_range(const std::string &result_id,
                           std::uintmax_t offset,
                           std::uintmax_t length) const;
    /** Read and parse a bounded JSON result payload. */
    Json read_json_payload(const std::string &result_id) const;
    Json read_jsonl_batch(const std::string &result_id,
                          std::uintmax_t offset,
                          std::uintmax_t limit) const;
    Json read_arrow_ipc_batch(const std::string &result_id,
                              std::uintmax_t offset,
                              std::uintmax_t limit) const;

    /** Append one bounded chunk to an unpublished result lease. */
    void append_staging_chunk(const ResultLease &lease,
                              std::uintmax_t offset,
                              const std::vector<unsigned char> &bytes) const;

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
