#include "streamfind/service/result_store.hpp"
#include "streamfind/data_proxy_protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <deque>
#include <arrow/buffer.h>
#include <arrow/array.h>
#include <arrow/scalar.h>
#include <arrow/io/file.h>
#include <arrow/ipc/api.h>

namespace streamfind::service {

ResultStagingHandle::~ResultStagingHandle() { close(); }

void ResultStagingHandle::close() noexcept {
    if (native_handle_ == -1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(native_handle_));
#else
    ::close(static_cast<int>(native_handle_));
#endif
    native_handle_ = -1;
}

namespace detail {

Json arrow_record_batch_rows(const std::shared_ptr<arrow::RecordBatch> &batch,
                             const std::vector<std::string> &columns) {
    const auto value_at = [](const std::shared_ptr<arrow::Array> &array,
                             const std::int64_t row, const auto &self) -> Json {
        if (array->IsNull(row)) return nullptr;
        switch (array->type_id()) {
        case arrow::Type::BOOL:
            return std::static_pointer_cast<arrow::BooleanArray>(array)->Value(row);
        case arrow::Type::INT8:
            return std::static_pointer_cast<arrow::Int8Array>(array)->Value(row);
        case arrow::Type::INT16:
            return std::static_pointer_cast<arrow::Int16Array>(array)->Value(row);
        case arrow::Type::INT32:
            return std::static_pointer_cast<arrow::Int32Array>(array)->Value(row);
        case arrow::Type::INT64:
            return std::static_pointer_cast<arrow::Int64Array>(array)->Value(row);
        case arrow::Type::UINT8:
            return std::static_pointer_cast<arrow::UInt8Array>(array)->Value(row);
        case arrow::Type::UINT16:
            return std::static_pointer_cast<arrow::UInt16Array>(array)->Value(row);
        case arrow::Type::UINT32:
            return std::static_pointer_cast<arrow::UInt32Array>(array)->Value(row);
        case arrow::Type::UINT64:
            return std::static_pointer_cast<arrow::UInt64Array>(array)->Value(row);
        case arrow::Type::FLOAT:
            return std::static_pointer_cast<arrow::FloatArray>(array)->Value(row);
        case arrow::Type::DOUBLE:
            return std::static_pointer_cast<arrow::DoubleArray>(array)->Value(row);
        case arrow::Type::STRING:
            return Json::parse(std::static_pointer_cast<arrow::StringArray>(array)->GetView(row));
        case arrow::Type::LARGE_STRING:
            return Json::parse(std::static_pointer_cast<arrow::LargeStringArray>(array)->GetView(row));
        case arrow::Type::LIST: {
            const auto list = std::static_pointer_cast<arrow::ListArray>(array);
            const auto begin = list->value_offset(row);
            const auto end = begin + list->value_length(row);
            Json values = Json::array();
            for (auto index = begin; index < end; ++index) {
                if (list->values()->type_id() == arrow::Type::STRING)
                    values.push_back(std::static_pointer_cast<arrow::StringArray>(list->values())->GetString(index));
                else values.push_back(self(list->values(), index, self));
            }
            return values;
        }
        case arrow::Type::LARGE_LIST: {
            const auto list = std::static_pointer_cast<arrow::LargeListArray>(array);
            const auto begin = list->value_offset(row);
            const auto end = begin + list->value_length(row);
            Json values = Json::array();
            for (auto index = begin; index < end; ++index) {
                if (list->values()->type_id() == arrow::Type::STRING)
                    values.push_back(std::static_pointer_cast<arrow::StringArray>(list->values())->GetString(index));
                else values.push_back(self(list->values(), index, self));
            }
            return values;
        }
        default: {
            auto scalar = array->GetScalar(row);
            if (!scalar.ok()) throw std::runtime_error(scalar.status().ToString());
            return (*scalar)->ToString();
        }
        }
    };
    Json rows = Json::array();
    rows.get_ref<Json::array_t &>().reserve(static_cast<std::size_t>(batch->num_rows()));
    std::vector<int> selected;
    selected.reserve(columns.empty() ? static_cast<std::size_t>(batch->num_columns()) : columns.size());
    if (columns.empty()) {
        for (int index = 0; index < batch->num_columns(); ++index) selected.push_back(index);
    } else {
        for (const auto &name : columns) {
            const auto index = batch->schema()->GetFieldIndex(name);
            if (index >= 0) selected.push_back(index);
        }
    }
    for (std::int64_t row_index = 0; row_index < batch->num_rows(); ++row_index) {
        Json row = Json::object();
        for (const auto column_index : selected) {
            const auto &field = batch->schema()->field(column_index);
            row[field->name()] = value_at(batch->column(column_index), row_index, value_at);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string payload_digest(const std::filesystem::path &path, std::uintmax_t &size) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read result payload: " + path.string());

    // Keep the first implementation dependency-free and deterministic across
    // the native Windows and Linux builds. The manifest records the algorithm
    // so cache validation never mistakes this for a cryptographic signature.
    std::uint64_t hash = 14695981039346656037ULL;
    constexpr std::size_t buffer_size = 64 * 1024;
    std::array<char, buffer_size> buffer{};
    size = 0;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) break;
        size += static_cast<std::uintmax_t>(count);
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= 1099511628211ULL;
        }
    }
    if (!input.eof()) throw std::runtime_error("cannot read result payload: " + path.string());

    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

std::string result_store_status_name(ResultStoreStatus status) {
    switch (status) {
    case ResultStoreStatus::missing: return "missing";
    case ResultStoreStatus::ready: return "ready";
    case ResultStoreStatus::invalid: return "invalid";
    }
    return "invalid";
}

std::string base64_encode(const std::vector<unsigned char> &bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto first = bytes[index];
        const auto second = index + 1 < bytes.size() ? bytes[index + 1] : 0;
        const auto third = index + 2 < bytes.size() ? bytes[index + 2] : 0;
        output.push_back(alphabet[first >> 2]);
        output.push_back(alphabet[((first & 0x03) << 4) | (second >> 4)]);
        output.push_back(index + 1 < bytes.size() ? alphabet[((second & 0x0f) << 2) | (third >> 6)] : '=');
        output.push_back(index + 2 < bytes.size() ? alphabet[third & 0x3f] : '=');
    }
    return output;
}

Json validated_manifest(const std::filesystem::path &root,
                        const std::string &result_id) {
    if (result_id.empty() || result_id == "." || result_id == ".." ||
        result_id.find('/') != std::string::npos || result_id.find('\\') != std::string::npos ||
        result_id.find(':') != std::string::npos)
        throw std::invalid_argument("result_id must be a single safe path component");
    const auto directory = root / "results" / result_id;
    std::ifstream input(directory / "manifest.json", std::ios::binary);
    if (!input) throw std::runtime_error("cannot read result manifest: " + result_id);
    Json manifest;
    input >> manifest;
    const auto payload_value = manifest.value("payload", std::string{});
    const auto payload_path = root / std::filesystem::path(payload_value);
    std::error_code error;
    const auto relative = std::filesystem::relative(payload_path, root, error);
    if (error || relative.empty() || relative.begin()->string() == ".." ||
        !std::filesystem::is_regular_file(payload_path))
        throw std::runtime_error("result payload is unavailable: " + result_id);
    const auto size = std::filesystem::file_size(payload_path, error);
    if (error || manifest.value("state", std::string{}) != "published" ||
        manifest.value("payload_size", std::uintmax_t{0}) != size ||
        manifest.value("payload_digest", std::string{}).empty())
        throw std::runtime_error("result payload metadata failed validation: " + result_id);
    if (manifest.value("format", std::string{}) == "arrow-ipc-file") {
        const auto row_batches = manifest.value("batch_row_counts", Json{});
        if (manifest.value("row_batch_directory_version", 0) != 1 || !row_batches.is_array())
            throw std::runtime_error("Arrow result is missing its row-batch directory; regenerate this result");
        std::uint64_t indexed_rows = 0;
        for (const auto &count : row_batches) {
            if (!count.is_number_unsigned() && !count.is_number_integer())
                throw std::runtime_error("Arrow result row-batch directory is invalid; regenerate this result");
            const auto batch_rows = count.get<std::uint64_t>();
            if (batch_rows > std::numeric_limits<std::uint64_t>::max() - indexed_rows)
                throw std::runtime_error("Arrow result row-batch directory exceeds its supported range");
            indexed_rows += batch_rows;
        }
        if (indexed_rows != manifest.value("row_count", std::uint64_t{0}))
            throw std::runtime_error("Arrow result row-batch directory does not match its manifest");
    }
    return manifest;
}

}  // namespace detail

Json ResultStoreInspection::to_json() const {
    Json result{{"status", detail::result_store_status_name(status)},
                {"root", root.string()},
                {"published_count", published_count},
                {"invalid_count", invalid_count},
                {"staging_run_count", staging_run_count},
                {"size_bytes", size_bytes}};
    if (!store_id.empty()) result["store_id"] = store_id;
    if (!error.empty()) result["error"] = error;
    return result;
}

ResultStore::ResultStore(std::filesystem::path database_path)
    : database_path_(std::filesystem::weakly_canonical(database_path)),
      root_(data_path_for_database(database_path_)) {}

std::filesystem::path ResultStore::data_path_for_database(const std::filesystem::path &database_path) {
    auto canonical = database_path;
    std::error_code error;
    canonical = std::filesystem::weakly_canonical(canonical, error);
    if (error) canonical = std::filesystem::absolute(database_path, error).lexically_normal();
    canonical.replace_extension(".data");
    return canonical.lexically_normal();
}

void ResultStore::require_component(const std::string &value, const char *name) {
    if (value.empty() || value == "." || value == ".." || value.find('/') != std::string::npos ||
        value.find('\\') != std::string::npos || value.find(':') != std::string::npos)
        throw std::invalid_argument(std::string(name) + " must be a single safe path component");
}

std::string ResultStore::create_store_id() {
    std::random_device device;
    std::mt19937_64 generator(device());
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream output;
    output << "store-" << std::hex << now << '-' << generator();
    return output.str();
}

std::string ResultStore::normalize_extension(const std::string &extension) {
    if (extension.empty()) return "bin";
    const auto first = extension.front() == '.' ? extension.substr(1) : extension;
    require_component(first, "extension");
    return first;
}

void ResultStore::write_json_file(const std::filesystem::path &path, const Json &value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create result store metadata: " + path.string());
    output << value.dump(2) << '\n';
    if (!output) throw std::runtime_error("cannot write result store metadata: " + path.string());
}

Json ResultStore::read_json_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read result store metadata: " + path.string());
    Json value;
    input >> value;
    return value;
}

ResultStoreInspection ResultStore::inspect(bool verify_payloads) const {
    ResultStoreInspection result;
    result.root = root_;
    const auto manifest = root_ / "store.json";
    std::error_code error;
    if (!std::filesystem::exists(root_, error) || !std::filesystem::exists(manifest, error)) {
        result.status = ResultStoreStatus::missing;
        return result;
    }
    try {
        // Results are immutable, but cleanup may remove a result directory
        // while an inventory/allocate path is inspecting the store. Use the
        // error-code iterator overloads and treat a vanished entry as a normal
        // concurrent delete instead of turning a healthy store into invalid.
        error.clear();
        for (std::filesystem::recursive_directory_iterator iterator(
                 root_, std::filesystem::directory_options::skip_permission_denied, error), end;
             iterator != end;) {
            if (error) {
                if (error == std::errc::no_such_file_or_directory ||
                    error == std::errc::not_a_directory) {
                    error.clear();
                    continue;
                }
                break;
            }
            const auto path = iterator->path();
            error.clear();
            if (std::filesystem::is_regular_file(path, error) && !error) {
                const auto bytes = std::filesystem::file_size(path, error);
                if (!error) result.size_bytes += bytes;
            }
            error.clear();
            iterator.increment(error);
        }
        const auto value = read_json_file(manifest);
        if (value.value("layout_version", std::string{}) != kLayoutVersion ||
            value.value("database_filename", std::string{}) != database_path_.filename().string() ||
            value.value("store_id", std::string{}).empty()) {
            result.status = ResultStoreStatus::invalid;
            result.error = "result store identity or layout version does not match the project";
            return result;
        }
        result.store_id = value.at("store_id").get<std::string>();
        const auto results = root_ / "results";
        error.clear();
        if (std::filesystem::exists(results, error) && !error) {
            for (std::filesystem::directory_iterator iterator(
                     results, std::filesystem::directory_options::skip_permission_denied, error), end;
                 iterator != end;) {
                if (error) {
                    if (error == std::errc::no_such_file_or_directory ||
                        error == std::errc::not_a_directory) {
                        error.clear();
                        continue;
                    }
                    break;
                }
                const auto result_directory = iterator->path();
                error.clear();
                const bool is_directory = std::filesystem::is_directory(result_directory, error);
                error.clear();
                iterator.increment(error);
                if (!is_directory || error == std::errc::no_such_file_or_directory) {
                    error.clear();
                    continue;
                }
                const auto manifest_path = result_directory / "manifest.json";
                try {
                    const auto manifest = read_json_file(manifest_path);
                    const auto payload_value = manifest.value("payload", std::string{});
                    const auto payload_path = root_ / std::filesystem::path(payload_value);
                    const auto relative = std::filesystem::relative(payload_path, root_, error);
                    const bool outside_root = error || relative.empty() || relative.begin()->string() == "..";
                    const auto payload_exists = !outside_root && std::filesystem::is_regular_file(payload_path);
                    const auto declared_size = manifest.value("payload_size", std::uintmax_t{0});
                    std::uintmax_t actual_size = payload_exists ? std::filesystem::file_size(payload_path) : 0;
                    const auto digest = verify_payloads && payload_exists
                        ? detail::payload_digest(payload_path, actual_size) : std::string{};
                    const auto valid = manifest.value("state", std::string{}) == "published" && payload_exists &&
                        (verify_payloads
                            ? (declared_size == actual_size && manifest.value("payload_digest", std::string{}) == digest)
                            : (declared_size == 0 || declared_size == actual_size));
                    if (!valid) {
                        ++result.invalid_count;
                    } else {
                        ++result.published_count;
                    }
                } catch (...) {
                    error.clear();
                    if (std::filesystem::exists(result_directory, error) && !error)
                        ++result.invalid_count;
                    error.clear();
                }
            }
        }
        const auto runs = root_ / "runs";
        error.clear();
        if (std::filesystem::exists(runs, error) && !error) {
            for (std::filesystem::directory_iterator iterator(
                     runs, std::filesystem::directory_options::skip_permission_denied, error), end;
                 iterator != end;) {
                if (error) {
                    if (error == std::errc::no_such_file_or_directory ||
                        error == std::errc::not_a_directory) {
                        error.clear();
                        continue;
                    }
                    break;
                }
                error.clear();
                if (iterator->is_directory(error) && !error) ++result.staging_run_count;
                error.clear();
                iterator.increment(error);
            }
        }
        result.status = result.invalid_count == 0 ? ResultStoreStatus::ready : ResultStoreStatus::invalid;
        if (result.invalid_count != 0)
            result.error = "one or more published result payloads failed manifest validation";
        return result;
    } catch (const std::exception &exception) {
        result.status = ResultStoreStatus::invalid;
        result.error = exception.what();
        return result;
    }
}

ResultStoreInspection ResultStore::initialize(bool verify_payloads) {
    cleanup_empty_runs();
    std::error_code error;
    const auto existing = inspect(verify_payloads);
    if (existing.status == ResultStoreStatus::ready) return existing;
    if (existing.status == ResultStoreStatus::invalid)
        throw std::runtime_error(existing.error);

    std::filesystem::create_directories(root_ / "results", error);
    if (error) throw std::runtime_error("cannot create result store: " + error.message());
    std::filesystem::create_directories(root_ / "runs", error);
    if (error) throw std::runtime_error("cannot create result run store: " + error.message());
    const Json manifest{{"layout_version", kLayoutVersion},
                        {"database_filename", database_path_.filename().string()},
                        {"store_id", create_store_id()}};
    write_json_file(root_ / "store.json", manifest);
    return inspect(verify_payloads);
}

void ResultStore::cleanup_empty_runs() const {
    const auto runs = root_ / "runs";
    std::error_code error;
    if (!std::filesystem::is_directory(runs, error) || error) return;
    std::vector<std::filesystem::path> directories;
    for (std::filesystem::recursive_directory_iterator iterator(runs, error), end;
         iterator != end && !error; iterator.increment(error)) {
        if (iterator->is_directory(error) && !error) directories.push_back(iterator->path());
    }
    std::sort(directories.rbegin(), directories.rend());
    for (const auto &directory : directories) {
        error.clear();
        if (std::filesystem::is_empty(directory, error) && !error)
            std::filesystem::remove(directory, error);
    }
}

Json ResultStore::published_results() const {
    Json results = Json::array();
    std::error_code error;
    const auto directory = root_ / "results";
    if (!std::filesystem::exists(directory, error)) return results;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
        if (error || !entry.is_directory()) continue;
        try {
            const auto result_id = entry.path().filename().string();
            auto descriptor = detail::validated_manifest(root_, result_id);
            descriptor.erase("payload");
            descriptor["payload_ref"] = "result:" + result_id;
            descriptor["capabilities"] = {"describe", "read_range"};
            if (descriptor.value("kind", std::string{}) == "table" &&
                descriptor.value("format", std::string{}) == "arrow-ipc-file")
                descriptor["capabilities"].push_back("read_batches");
            results.push_back(std::move(descriptor));
        } catch (...) {
            // Invalid or incomplete results are excluded from the published inventory.
        }
    }
    std::sort(results.begin(), results.end(), [](const Json &left, const Json &right) {
        return left.value("result_id", std::string{}) < right.value("result_id", std::string{});
    });
    return results;
}

Json ResultStore::find_cached_result(const std::string &computation_key) const {
    const auto results = find_cached_results(computation_key);
    return results.empty() ? Json(nullptr) : results.front();
}

Json ResultStore::find_cached_results(const std::string &computation_key) const {
    if (computation_key.empty()) throw std::invalid_argument("computation_key must not be empty");
    const auto inspection = inspect();
    if (inspection.status != ResultStoreStatus::ready) return Json::array();

    Json matches = Json::array();
    std::error_code error;
    const auto results_directory = root_ / "results";
    if (!std::filesystem::exists(results_directory, error)) return Json::array();
    for (const auto &entry : std::filesystem::directory_iterator(results_directory, error)) {
        if (error || !entry.is_directory()) continue;
        try {
            const auto manifest = read_json_file(entry.path() / "manifest.json");
            if (!manifest.value("cacheable", false) ||
                manifest.value("computation_key", std::string{}) != computation_key)
                continue;
            const auto payload_value = manifest.value("payload", std::string{});
            const auto payload_path = root_ / std::filesystem::path(payload_value);
            const auto relative = std::filesystem::relative(payload_path, root_, error);
            if (error || relative.empty() || relative.begin()->string() == ".." ||
                !std::filesystem::is_regular_file(payload_path)) continue;
            std::uintmax_t payload_size = 0;
            const auto digest = detail::payload_digest(payload_path, payload_size);
            if (manifest.value("payload_size", std::uintmax_t{0}) != payload_size ||
                manifest.value("payload_digest", std::string{}) != digest) continue;
            Json result = manifest;
            result["cache_hit"] = true;
            result.erase("payload");
            result["payload_ref"] = "result:" + result.value("result_id", std::string{});
            matches.push_back(std::move(result));
        } catch (...) {
            // A malformed or incomplete result is a cache miss, never a hit.
        }
    }
    std::sort(matches.begin(), matches.end(), [](const Json &left, const Json &right) {
        return left.value("result_id", std::string{}) < right.value("result_id", std::string{});
    });
    return matches;
}

void ResultStore::remove_published_results(const std::vector<std::string> &result_ids) const {
    for (const auto &result_id : result_ids) {
        require_component(result_id, "result_id");
        const auto directory = root_ / "results" / result_id;
        std::error_code error;
        if (!std::filesystem::exists(directory, error)) {
            if (error) throw std::runtime_error("cannot inspect result payload: " + error.message());
            continue;
        }
        if (!std::filesystem::is_directory(directory, error) || error)
            throw std::runtime_error("published result path is not a directory: " + result_id);
        std::filesystem::remove_all(directory, error);
        if (error) throw std::runtime_error("cannot remove published result payload: " + error.message());
    }
}

Json ResultStore::describe_result(const std::string &result_id) const {
    const auto manifest = detail::validated_manifest(root_, result_id);
    if (manifest.value("kind", std::string{}) == "table" &&
        manifest.value("format", std::string{}) != "arrow-ipc-file")
        throw std::runtime_error("table result uses an unsupported format; clear and regenerate this result");
    Json descriptor = manifest;
    descriptor.erase("payload");
    descriptor["payload_ref"] = "result:" + result_id;
    descriptor["capabilities"] = {"describe", "read_range"};
    if (descriptor.value("format", std::string{}) == "parquet" ||
        descriptor.value("format", std::string{}) == "arrow-ipc-file")
        descriptor["capabilities"].push_back("read_batches");
    return descriptor;
}

Json ResultStore::read_navigation_index(const std::string &result_id) const {
    require_component(result_id, "result_id");
    constexpr std::uintmax_t max_index_bytes = 32 * 1024 * 1024;
    Json manifest;
    try {
        manifest = detail::validated_manifest(root_, result_id);
    } catch (...) {
        return nullptr;
    }
    const auto path = root_ / "results" / result_id / "navigation.json";
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return nullptr;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > max_index_bytes) return nullptr;
    try {
        auto index = read_json_file(path);
        if (!index.is_object() || index.value("result_id", std::string{}) != result_id ||
            index.value("payload_digest", std::string{}) != manifest.value("payload_digest", std::string{}) ||
            !index.value("postings", Json{}).is_object()) return nullptr;
        const auto declared = manifest.value("navigation_columns", Json::array());
        const auto indexed = index.value("navigation_columns", Json::array());
        if (!declared.is_array() || !indexed.is_array() || declared != indexed) return nullptr;
        return index.at("postings");
    } catch (...) {
        return nullptr;
    }
}

void ResultStore::write_navigation_index(const std::string &result_id, const Json &index) const {
    require_component(result_id, "result_id");
    constexpr std::size_t max_index_bytes = 32 * 1024 * 1024;
    if (!index.is_object()) throw std::invalid_argument("navigation index must be an object");
    const auto manifest = detail::validated_manifest(root_, result_id);
    const auto navigation_columns = manifest.value("navigation_columns", Json::array());
    if (!navigation_columns.is_array() || navigation_columns.empty()) return;
    for (const auto &[column, postings] : index.items()) {
        if (std::none_of(navigation_columns.begin(), navigation_columns.end(), [&](const Json &name) {
                return name.is_string() && name.get<std::string>() == column;
            }) || !postings.is_object())
            throw std::invalid_argument("navigation index contains an undeclared column or invalid postings");
    }
    Json payload{{"result_id", result_id},
                 {"payload_digest", manifest.value("payload_digest", std::string{})},
                 {"navigation_columns", navigation_columns},
                 {"postings", index}};
    const auto serialized = payload.dump();
    if (serialized.size() > max_index_bytes) return;
    const auto directory = root_ / "results" / result_id;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error)
        throw std::runtime_error("cannot locate published result directory for navigation index");
    const auto path = directory / "navigation.json";
    if (std::filesystem::exists(path, error)) return;
    if (error) throw std::runtime_error("cannot inspect result navigation index: " + error.message());
    const auto temporary = directory / ("navigation." + create_store_id() + ".tmp");
    write_json_file(temporary, payload);
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        if (!std::filesystem::exists(path))
            throw std::runtime_error("cannot publish result navigation index: " + error.message());
    }
}

Json ResultStore::read_range_statistics(const std::string &result_id, const std::string &column) const {
    require_component(result_id, "result_id");
    Json manifest;
    try {
        manifest = detail::validated_manifest(root_, result_id);
    } catch (...) {
        return nullptr;
    }
    const auto declared = manifest.value("navigation_columns", Json::array());
    if (!declared.is_array() ||
        std::none_of(declared.begin(), declared.end(), [&](const Json &value) {
            return value.is_string() && value.get<std::string>() == column;
        })) return nullptr;
    std::string encoded_column;
    static constexpr char hex[] = "0123456789abcdef";
    for (const auto ch : column) {
        const auto byte = static_cast<unsigned char>(ch);
        encoded_column.push_back(hex[byte >> 4]);
        encoded_column.push_back(hex[byte & 0x0f]);
    }
    const auto path = root_ / "results" / result_id / ("range-" + encoded_column + ".json");
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return nullptr;
    constexpr std::uintmax_t max_range_index_bytes = 8 * 1024 * 1024;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > max_range_index_bytes) return nullptr;
    try {
        const auto index = read_json_file(path);
        if (index.value("result_id", std::string{}) != result_id ||
            index.value("column", std::string{}) != column ||
            index.value("payload_digest", std::string{}) != manifest.value("payload_digest", std::string{}) ||
            index.value("navigation_columns", Json::array()) != declared ||
            !index.value("batches", Json{}).is_array() ||
            index.at("batches").size() != manifest.value("batch_row_counts", Json::array()).size()) return nullptr;
        return index.at("batches");
    } catch (...) {
        return nullptr;
    }
}

void ResultStore::write_range_statistics(const std::string &result_id,
                                         const std::string &column,
                                         const Json &batches) const {
    require_component(result_id, "result_id");
    if (!batches.is_array()) throw std::invalid_argument("range statistics batches must be an array");
    const auto manifest = detail::validated_manifest(root_, result_id);
    const auto declared = manifest.value("navigation_columns", Json::array());
    if (!declared.is_array() ||
        std::none_of(declared.begin(), declared.end(), [&](const Json &value) {
            return value.is_string() && value.get<std::string>() == column;
        })) return;
    if (batches.size() != manifest.value("batch_row_counts", Json::array()).size())
        throw std::invalid_argument("range statistics batch count does not match the Arrow result");
    Json index{{"result_id", result_id}, {"column", column},
               {"payload_digest", manifest.value("payload_digest", std::string{})},
               {"navigation_columns", declared}, {"batches", batches}};
    constexpr std::size_t max_range_index_bytes = 8 * 1024 * 1024;
    const auto serialized = index.dump();
    if (serialized.size() > max_range_index_bytes) return;
    std::string encoded_column;
    static constexpr char hex[] = "0123456789abcdef";
    for (const auto ch : column) {
        const auto byte = static_cast<unsigned char>(ch);
        encoded_column.push_back(hex[byte >> 4]);
        encoded_column.push_back(hex[byte & 0x0f]);
    }
    const auto directory = root_ / "results" / result_id;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error)
        throw std::runtime_error("cannot locate published result directory for range index");
    const auto path = directory / ("range-" + encoded_column + ".json");
    if (std::filesystem::exists(path, error)) return;
    if (error) throw std::runtime_error("cannot inspect result range index: " + error.message());
    const auto temporary = directory / ("range-" + encoded_column + "." + create_store_id() + ".tmp");
    write_json_file(temporary, index);
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        if (!std::filesystem::exists(path))
            throw std::runtime_error("cannot publish result range index: " + error.message());
    }
}

Json ResultStore::read_result_range(const std::string &result_id,
                                    std::uintmax_t offset,
                                    std::uintmax_t length) const {
    constexpr std::uintmax_t max_range = 1024 * 1024;
    if (length > max_range) throw std::invalid_argument("result range is too large");
    const auto manifest = detail::validated_manifest(root_, result_id);
    const auto payload_path = root_ / std::filesystem::path(manifest.at("payload").get<std::string>());
    const auto size = manifest.at("payload_size").get<std::uintmax_t>();
    if (offset > size) throw std::invalid_argument("result range offset is outside the payload");
    const auto available = size - offset;
    const auto actual_length = std::min(length, available);
    std::ifstream input(payload_path, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(offset));
    std::vector<unsigned char> bytes(static_cast<std::size_t>(actual_length));
    if (actual_length != 0)
        input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(actual_length));
    if (!input && !input.eof()) throw std::runtime_error("cannot read result payload range");
    return Json{{"result_id", result_id}, {"payload_ref", "result:" + result_id},
                {"offset", offset}, {"length", actual_length}, {"total_size", size},
                {"encoding", "base64"}, {"data", detail::base64_encode(bytes)}};
}

Json ResultStore::read_json_payload(const std::string &result_id) const {
    // Visualization specifications can legitimately contain many sampled
    // traces (for example a TIC across many analyses). Keep JSON bounded, but
    // do not reject valid canvas payloads just above the table chunk limit.
    constexpr std::uintmax_t max_json_payload = 16 * 1024 * 1024;
    const auto manifest = detail::validated_manifest(root_, result_id);
    if (manifest.value("format", std::string{}) != "json")
        throw std::invalid_argument("result is not a JSON payload: " + result_id);
    const auto size = manifest.at("payload_size").get<std::uintmax_t>();
    if (size > max_json_payload)
        throw std::invalid_argument("JSON result payload is too large for inline reuse");
    const auto payload_path = root_ / std::filesystem::path(manifest.at("payload").get<std::string>());
    std::ifstream input(payload_path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read JSON result payload");
    std::string text(static_cast<std::size_t>(size), '\0');
    if (size != 0) input.read(text.data(), static_cast<std::streamsize>(size));
    if (!input && !input.eof()) throw std::runtime_error("cannot read JSON result payload");
    const auto payload = Json::parse(text, nullptr, false);
    if (payload.is_discarded()) throw std::runtime_error("JSON result payload is invalid");
    return payload;
}

Json ResultStore::read_arrow_ipc_batch(const std::string &result_id,
                                       std::uintmax_t offset,
                                       std::uintmax_t limit,
                                       const std::vector<std::string> &columns) const {
    if (limit == 0 || limit > 65536) throw std::invalid_argument("result batch limit is invalid");
    const auto manifest = detail::validated_manifest(root_, result_id);
    if (manifest.value("format", std::string{}) != "arrow-ipc-file")
        throw std::invalid_argument("result is not an Arrow IPC table result: " + result_id);
    const auto batch_row_counts = manifest.value("batch_row_counts", Json::array());
    if (!batch_row_counts.is_array())
        throw std::runtime_error("Arrow result is missing its row-batch directory; regenerate this result");
    std::vector<std::uint64_t> batch_offsets;
    batch_offsets.reserve(batch_row_counts.size() + 1);
    std::uint64_t indexed_rows = 0;
    batch_offsets.push_back(0);
    for (const auto &count : batch_row_counts) {
        if (!count.is_number_unsigned() && !count.is_number_integer())
            throw std::runtime_error("Arrow result row-batch directory is invalid; regenerate this result");
        const auto batch_count = count.get<std::uint64_t>();
        if (batch_count > std::numeric_limits<std::uint64_t>::max() - indexed_rows)
            throw std::runtime_error("Arrow result row-batch directory overflows its row count");
        indexed_rows += batch_count;
        batch_offsets.push_back(indexed_rows);
    }
    if (indexed_rows != manifest.value("row_count", std::uint64_t{0}) ||
        batch_row_counts.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Arrow result row-batch directory does not match its manifest; regenerate this result");
    const auto payload_path = root_ / std::filesystem::path(manifest.at("payload").get<std::string>());
    auto file_result = arrow::io::ReadableFile::Open(payload_path.string());
    if (!file_result.ok()) throw std::runtime_error(file_result.status().ToString());
    const auto file = *file_result;
    auto reader_result = arrow::ipc::RecordBatchFileReader::Open(file);
    if (!reader_result.ok()) throw std::runtime_error(reader_result.status().ToString());
    auto reader = *reader_result;
    if (!columns.empty()) {
        auto read_options = arrow::ipc::IpcReadOptions::Defaults();
        const auto schema = reader->schema();
        for (const auto &name : columns) {
            const auto field_index = schema->GetFieldIndex(name);
            // row_key is a virtual query key added by ProjectRuntimeManager,
            // so it has no physical Arrow field to include here.
            if (field_index >= 0 &&
                std::find(read_options.included_fields.begin(), read_options.included_fields.end(), field_index) ==
                    read_options.included_fields.end())
                read_options.included_fields.push_back(field_index);
        }
        if (!read_options.included_fields.empty()) {
            auto projected_reader_result = arrow::ipc::RecordBatchFileReader::Open(file, read_options);
            if (!projected_reader_result.ok())
                throw std::runtime_error(projected_reader_result.status().ToString());
            reader = *projected_reader_result;
        }
    }
    Json rows = Json::array();
    if (reader->num_record_batches() != static_cast<int>(batch_row_counts.size()))
        throw std::runtime_error("Arrow result footer does not match its row-batch directory");
    const auto requested_end = offset > std::numeric_limits<std::uint64_t>::max() - limit
        ? std::numeric_limits<std::uint64_t>::max() : offset + limit;
    for (int batch_index = 0; batch_index < reader->num_record_batches(); ++batch_index) {
        const auto row_offset = batch_offsets[static_cast<std::size_t>(batch_index)];
        const auto batch_end = batch_offsets[static_cast<std::size_t>(batch_index) + 1];
        if (batch_end <= offset) continue;
        if (row_offset >= requested_end) break;
        auto batch_result = reader->ReadRecordBatch(batch_index);
        if (!batch_result.ok()) throw std::runtime_error(batch_result.status().ToString());
        const auto record_batch = *batch_result;
        if (static_cast<std::uint64_t>(record_batch->num_rows()) !=
            batch_offsets[static_cast<std::size_t>(batch_index) + 1] - row_offset)
            throw std::runtime_error("Arrow result record batch does not match its row-batch directory");
        const auto batch = detail::arrow_record_batch_rows(record_batch, columns);
        for (std::size_t row_index = 0; row_index < batch.size(); ++row_index) {
            const auto &row = batch.at(row_index);
            const auto absolute_offset = row_offset + row_index;
            if (absolute_offset < offset) continue;
            if (rows.size() >= limit)
                return Json{{"rows", rows}, {"offset", offset}, {"limit", limit}, {"has_more", true}};
            rows.push_back(row);
        }
    }
    return Json{{"rows", rows}, {"offset", offset}, {"limit", limit},
                {"has_more", offset + rows.size() < manifest.value("row_count", std::uint64_t{0})}};
}

Json ResultStore::scan_arrow_scalar_filters(const std::string &result_id,
                                            const Json &filters,
                                            std::uintmax_t offset,
                                            std::uintmax_t limit) const {
    if (!filters.is_array() || filters.empty() || limit == 0) return nullptr;
    const auto manifest = detail::validated_manifest(root_, result_id);
    if (manifest.value("format", std::string{}) != "arrow-ipc-file") return nullptr;
    const auto batch_counts = manifest.value("batch_row_counts", Json::array());
    if (!batch_counts.is_array()) throw std::runtime_error("Arrow result has no row-batch directory");
    std::vector<std::string> filter_columns;
    for (const auto &filter : filters) {
        if (!filter.is_object() || !filter.value("column", Json{}).is_string()) return nullptr;
        const auto operation = filter.value("operator", std::string{"eq"});
        if (operation != "eq" && operation != "neq" && operation != "in" &&
            operation != "gt" && operation != "gte" && operation != "lt" &&
            operation != "lte" && operation != "between" &&
            operation != "is_null" && operation != "is_not_null") return nullptr;
        if (operation == "in" && !filter.value("values", Json{}).is_array()) return nullptr;
        if ((operation == "eq" || operation == "neq" || operation == "gt" || operation == "gte" ||
             operation == "lt" || operation == "lte") && !filter.contains("value")) return nullptr;
        const auto name = filter.at("column").get<std::string>();
        if (std::find(filter_columns.begin(), filter_columns.end(), name) == filter_columns.end())
            filter_columns.push_back(name);
    }
    const auto payload_path = root_ / std::filesystem::path(manifest.at("payload").get<std::string>());
    auto file_result = arrow::io::ReadableFile::Open(payload_path.string());
    if (!file_result.ok()) throw std::runtime_error(file_result.status().ToString());
    const auto file = *file_result;
    auto reader_result = arrow::ipc::RecordBatchFileReader::Open(file);
    if (!reader_result.ok()) throw std::runtime_error(reader_result.status().ToString());
    auto reader = *reader_result;
    auto options = arrow::ipc::IpcReadOptions::Defaults();
    std::unordered_map<std::string, int> field_indexes;
    for (const auto &name : filter_columns) {
        const auto index = reader->schema()->GetFieldIndex(name);
        if (index < 0) return nullptr;
        const auto type = reader->schema()->field(index)->type()->id();
        if (type != arrow::Type::STRING && type != arrow::Type::BOOL &&
            type != arrow::Type::INT8 && type != arrow::Type::INT16 && type != arrow::Type::INT32 &&
            type != arrow::Type::INT64 && type != arrow::Type::UINT8 && type != arrow::Type::UINT16 &&
            type != arrow::Type::UINT32 && type != arrow::Type::UINT64 &&
            type != arrow::Type::FLOAT && type != arrow::Type::DOUBLE) return nullptr;
        options.included_fields.push_back(index);
    }
    auto projected_result = arrow::ipc::RecordBatchFileReader::Open(file, options);
    if (!projected_result.ok()) throw std::runtime_error(projected_result.status().ToString());
    reader = *projected_result;
    // Arrow keeps projected fields in source-schema order. Resolve each
    // predicate column by name in that projected schema instead of assuming
    // the projection follows request order; otherwise mixed filters such as
    // filtered, rt, mz can inspect the wrong arrays and silently match zero
    // rows.
    for (const auto &name : filter_columns) {
        const auto index = reader->schema()->GetFieldIndex(name);
        if (index < 0) return nullptr;
        field_indexes.emplace(name, index);
    }
    if (reader->num_record_batches() != static_cast<int>(batch_counts.size()))
        throw std::runtime_error("Arrow result footer does not match its row-batch directory");

    const auto numeric_value = [](const std::shared_ptr<arrow::Array> &array, std::int64_t row) -> long double {
        const auto scalar = array->GetScalar(row);
        if (!scalar.ok()) throw std::runtime_error(scalar.status().ToString());
        return std::stold((*scalar)->ToString());
    };
    const auto is_numeric = [](arrow::Type::type type) {
        return type == arrow::Type::INT8 || type == arrow::Type::INT16 || type == arrow::Type::INT32 ||
               type == arrow::Type::INT64 || type == arrow::Type::UINT8 || type == arrow::Type::UINT16 ||
               type == arrow::Type::UINT32 || type == arrow::Type::UINT64 ||
               type == arrow::Type::FLOAT || type == arrow::Type::DOUBLE;
    };
    const auto navigation_columns = manifest.value("navigation_columns", Json::array());
    std::unordered_map<std::string, Json> range_batches;
    std::unordered_map<std::string, bool> collect_ranges;
    for (const auto &name : filter_columns) {
        const auto column_index = field_indexes.at(name);
        const auto type = reader->schema()->field(column_index)->type()->id();
        const bool declared_navigation = navigation_columns.is_array() &&
            std::any_of(navigation_columns.begin(), navigation_columns.end(), [&](const Json &value) {
                return value.is_string() && value.get<std::string>() == name;
            });
        if (!declared_navigation || !is_numeric(type)) continue;
        auto persisted = read_range_statistics(result_id, name);
        if (persisted.is_array()) {
            range_batches.emplace(name, std::move(persisted));
            collect_ranges.emplace(name, false);
        } else {
            range_batches.emplace(name, Json::array());
            collect_ranges.emplace(name, true);
        }
    }
    const auto batch_may_match = [&](int batch_index) {
        if (std::any_of(collect_ranges.begin(), collect_ranges.end(),
                        [](const auto &entry) { return entry.second; })) return true;
        for (const auto &filter : filters) {
            const auto name = filter.at("column").get<std::string>();
            const auto statistics = range_batches.find(name);
            if (statistics == range_batches.end() || collect_ranges.at(name) ||
                static_cast<std::size_t>(batch_index) >= statistics->second.size()) continue;
            const auto &range = statistics->second.at(static_cast<std::size_t>(batch_index));
            if (!range.is_object() || !range.value("min", Json{}).is_number() ||
                !range.value("max", Json{}).is_number()) continue;
            const auto minimum = range.at("min").get<long double>();
            const auto maximum = range.at("max").get<long double>();
            const auto operation = filter.value("operator", std::string{"eq"});
            bool possible = true;
            if (operation == "eq" && filter.value("value", Json{}).is_number()) {
                const auto value = filter.at("value").get<long double>();
                possible = value >= minimum && value <= maximum;
            } else if (operation == "in") {
                possible = false;
                for (const auto &value : filter.at("values"))
                    if (value.is_number() && value.get<long double>() >= minimum && value.get<long double>() <= maximum) {
                        possible = true;
                        break;
                    }
            } else if (operation == "between" && filter.value("min", Json{}).is_number() &&
                       filter.value("max", Json{}).is_number()) {
                possible = maximum >= filter.at("min").get<long double>() &&
                           minimum <= filter.at("max").get<long double>();
            } else if (operation == "gt" && filter.value("value", Json{}).is_number())
                possible = maximum > filter.at("value").get<long double>();
            else if (operation == "gte" && filter.value("value", Json{}).is_number())
                possible = maximum >= filter.at("value").get<long double>();
            else if (operation == "lt" && filter.value("value", Json{}).is_number())
                possible = minimum < filter.at("value").get<long double>();
            else if (operation == "lte" && filter.value("value", Json{}).is_number())
                possible = minimum <= filter.at("value").get<long double>();
            else if (operation == "neq" && filter.value("value", Json{}).is_number()) {
                const auto value = filter.at("value").get<long double>();
                possible = !(minimum == maximum && minimum == value);
            }
            if (!possible) return false;
        }
        return true;
    };
    const auto scalar_matches = [&](const std::shared_ptr<arrow::Array> &array, std::int64_t row,
                                    const Json &filter) {
        const auto operation = filter.value("operator", std::string{"eq"});
        const auto type = array->type_id();
        const bool null_value = array->IsNull(row);
        if (operation == "is_null") return null_value;
        if (operation == "is_not_null") return !null_value;
        if (null_value) return false;
        const auto equals = [&](const Json &expected) {
            if (type == arrow::Type::STRING)
                return std::static_pointer_cast<arrow::StringArray>(array)->GetView(row) == expected.dump();
            if (type == arrow::Type::BOOL)
                return expected.is_boolean() && std::static_pointer_cast<arrow::BooleanArray>(array)->Value(row) == expected.get<bool>();
            return is_numeric(type) && expected.is_number() && numeric_value(array, row) == expected.get<long double>();
        };
        if (operation == "eq") return equals(filter.at("value"));
        if (operation == "neq") return !equals(filter.at("value"));
        if (operation == "in") {
            for (const auto &value : filter.at("values")) if (equals(value)) return true;
            return false;
        }
        if (!is_numeric(type)) return false;
        const auto actual = numeric_value(array, row);
        if (operation == "between") {
            const auto minimum = filter.value("min", Json{});
            const auto maximum = filter.value("max", Json{});
            return minimum.is_number() && maximum.is_number() &&
                   actual >= minimum.get<long double>() && actual <= maximum.get<long double>();
        }
        const auto expected = filter.at("value");
        if (!expected.is_number()) return false;
        const auto target = expected.get<long double>();
        if (operation == "gt") return actual > target;
        if (operation == "gte") return actual >= target;
        if (operation == "lt") return actual < target;
        if (operation == "lte") return actual <= target;
        return false;
    };

    Json selected_offsets = Json::array();
    std::uintmax_t match_count = 0;
    std::uintmax_t batch_offset = 0;
    std::uintmax_t skipped_batches = 0;
    const auto end = offset > std::numeric_limits<std::uintmax_t>::max() - limit
        ? std::numeric_limits<std::uintmax_t>::max() : offset + limit;
    for (int batch_index = 0; batch_index < reader->num_record_batches(); ++batch_index) {
        auto batch_result = reader->ReadRecordBatch(batch_index);
        if (!batch_result.ok()) throw std::runtime_error(batch_result.status().ToString());
        const auto batch = *batch_result;
        const auto expected_rows = batch_counts.at(static_cast<std::size_t>(batch_index)).get<std::uint64_t>();
        if (static_cast<std::uint64_t>(batch->num_rows()) != expected_rows)
            throw std::runtime_error("Arrow result record batch does not match its row-batch directory");
        if (!batch_may_match(batch_index)) {
            ++skipped_batches;
            batch_offset += static_cast<std::uint64_t>(batch->num_rows());
            continue;
        }
        for (auto &[name, should_collect] : collect_ranges) {
            if (!should_collect) continue;
            const auto column_index = field_indexes.at(name);
            const auto array = batch->column(column_index);
            Json minimum = nullptr;
            Json maximum = nullptr;
            long double min_value = std::numeric_limits<long double>::infinity();
            long double max_value = -std::numeric_limits<long double>::infinity();
            for (std::int64_t row = 0; row < batch->num_rows(); ++row) {
                if (array->IsNull(row)) continue;
                const auto value = numeric_value(array, row);
                min_value = std::min(min_value, value);
                max_value = std::max(max_value, value);
            }
            if (min_value <= max_value) {
                minimum = static_cast<double>(min_value);
                maximum = static_cast<double>(max_value);
            }
            range_batches[name].push_back({{"min", minimum}, {"max", maximum}});
        }
        for (std::int64_t row = 0; row < batch->num_rows(); ++row) {
            bool matches = true;
            for (const auto &filter : filters) {
                const auto column = field_indexes.at(filter.at("column").get<std::string>());
                if (!scalar_matches(batch->column(column), row, filter)) { matches = false; break; }
            }
            if (!matches) continue;
            if (match_count >= offset && match_count < end) selected_offsets.push_back(batch_offset + row);
            ++match_count;
        }
        batch_offset += static_cast<std::uint64_t>(batch->num_rows());
    }
    for (const auto &[name, should_collect] : collect_ranges)
        if (should_collect && range_batches.at(name).size() == batch_counts.size())
            write_range_statistics(result_id, name, range_batches.at(name));
    return Json{{"total_rows", match_count}, {"offsets", std::move(selected_offsets)},
                {"batches_skipped", skipped_batches}, {"batches_scanned", batch_counts.size() - skipped_batches}};
}

std::shared_ptr<ResultStagingHandle> ResultStore::open_published_result(
    const std::string &result_id) const {
    const auto manifest = detail::validated_manifest(root_, result_id);
    if (manifest.value("kind", std::string{}) != "table" ||
        manifest.value("format", std::string{}) != "arrow-ipc-file")
        throw std::invalid_argument("worker input must reference a published Arrow table result");
    const auto payload = root_ / std::filesystem::path(manifest.at("payload").get<std::string>());
#ifdef _WIN32
    const auto handle = CreateFileW(payload.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot open published result for worker read");
    return std::make_shared<ResultStagingHandle>(reinterpret_cast<std::intptr_t>(handle));
#else
    const auto descriptor = ::open(payload.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) throw std::runtime_error("cannot open published result for worker read");
    return std::make_shared<ResultStagingHandle>(descriptor);
#endif
}

ResultLease ResultStore::allocate(const std::string &run_id,
                                      const std::string &result_id,
                                      const std::string &format,
                                      const std::string &extension) {
    require_component(run_id, "run_id");
    require_component(result_id, "result_id");
    require_component(format, "format");
    if (format == "arrow-ipc-file" && extension != "arrow")
        throw std::invalid_argument("Arrow IPC table results must use the .arrow payload extension");
    const auto inspection = inspect();
    if (inspection.status != ResultStoreStatus::ready)
        throw std::runtime_error("result store is not ready: " + inspection.error);

    const auto staging_directory = root_ / "runs" / run_id / "staging" / result_id;
    const auto published_directory = root_ / "results" / result_id;
    std::error_code error;
    if (std::filesystem::exists(staging_directory, error) ||
        std::filesystem::exists(published_directory, error))
        throw std::runtime_error("result allocation already exists: " + result_id);
    if (!std::filesystem::create_directories(staging_directory, error) && error)
        throw std::runtime_error("cannot create result staging directory: " + error.message());
    const auto normalized_extension = normalize_extension(extension);
    const auto staging_payload = staging_directory / ("payload." + normalized_extension);
#ifdef _WIN32
    const auto native = CreateFileW(staging_payload.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (native == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot create result staging payload");
    const auto writer = std::make_shared<ResultStagingHandle>(reinterpret_cast<std::intptr_t>(native));
#else
    const auto native = ::open(staging_payload.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (native < 0) throw std::runtime_error("cannot create result staging payload");
    const auto writer = std::make_shared<ResultStagingHandle>(native);
#endif
    ResultLease lease;
    lease.result_id = result_id;
    lease.run_id = run_id;
    lease.format = format;
    lease.staging_directory = staging_directory;
    lease.staging_payload = staging_payload;
    lease.published_directory = published_directory;
    lease.published_payload = published_directory / ("payload." + normalized_extension);
    lease.writer = writer;
    return lease;
}

Json ResultStore::publish(const ResultLease &lease, const Json &metadata) {
    if (lease.writer) lease.writer->close();
    if (!std::filesystem::is_regular_file(lease.staging_payload))
        throw std::runtime_error("result payload is missing or is not a regular file");
    Json batch_row_counts = Json::array();
    if (lease.format == "arrow-ipc-file") {
        if (metadata.value("kind", std::string{}) != "table")
            throw std::invalid_argument("Arrow IPC file publication requires a table result");
        auto file_result = arrow::io::ReadableFile::Open(lease.staging_payload.string());
        if (!file_result.ok()) throw std::runtime_error("cannot validate Arrow result: " + file_result.status().ToString());
        auto reader_result = arrow::ipc::RecordBatchFileReader::Open(*file_result);
        if (!reader_result.ok()) throw std::runtime_error("Arrow result has no valid IPC file footer: " + reader_result.status().ToString());
        const auto reader = *reader_result;
        std::uint64_t rows = 0;
        for (int index = 0; index < reader->num_record_batches(); ++index) {
            auto batch = reader->ReadRecordBatch(index);
            if (!batch.ok()) throw std::runtime_error("Arrow result batch is invalid: " + batch.status().ToString());
            const auto batch_rows = static_cast<std::uint64_t>((*batch)->num_rows());
            if (batch_rows > std::numeric_limits<std::uint64_t>::max() - rows)
                throw std::runtime_error("Arrow result row count exceeds the supported range");
            rows += batch_rows;
            batch_row_counts.push_back(batch_rows);
        }
        if (metadata.value("row_count", std::uint64_t{0}) != rows)
            throw std::runtime_error("Arrow result row count does not match its publication metadata");
        const auto declared_schema = metadata.value("schema", Json::array());
        if (declared_schema.is_array() && !declared_schema.empty()) {
            if (declared_schema.size() != static_cast<std::size_t>(reader->schema()->num_fields()))
                throw std::runtime_error("Arrow result schema does not match its declared output schema");
            for (const auto &field : reader->schema()->fields()) {
                const auto declared = std::find_if(declared_schema.begin(), declared_schema.end(),
                    [&](const Json &column) { return column.value("name", std::string{}) == field->name(); });
                if (declared == declared_schema.end())
                    throw std::runtime_error("Arrow result contains a column outside its declared output schema");
            }
        }
    }
    std::uintmax_t payload_size = 0;
    const auto payload_digest_value = detail::payload_digest(lease.staging_payload, payload_size);
    std::error_code error;
    if (std::filesystem::exists(lease.published_directory, error))
        throw std::runtime_error("result has already been published: " + lease.result_id);
    Json manifest = metadata;
    if (lease.format == "arrow-ipc-file") {
        manifest["batch_row_counts"] = std::move(batch_row_counts);
        manifest["row_batch_directory_version"] = 1;
    }
    manifest["result_id"] = lease.result_id;
    manifest["run_id"] = lease.run_id;
    manifest["format"] = lease.format;
    manifest["state"] = "published";
    manifest["payload"] = std::filesystem::relative(lease.published_payload, root_).generic_string();
    manifest["payload_size"] = payload_size;
    manifest["payload_digest"] = payload_digest_value;
    manifest["digest_algorithm"] = "fnv1a64";
    if (!manifest.contains("kind")) manifest["kind"] = "binary";
    if (!manifest.contains("media_type")) manifest["media_type"] = "application/octet-stream";
    if (!manifest.contains("schema")) manifest["schema"] = Json::object();
    if (!manifest.contains("metadata")) manifest["metadata"] = Json::object();
    manifest["cacheable"] = metadata.value("cacheable", false);
    if (!manifest.contains("computation_key")) manifest["computation_key"] = nullptr;
    if (!manifest.contains("provenance")) manifest["provenance"] = Json::object();
    // Prepare the complete publication unit while it is still private. A
    // process crash must never expose a promoted payload directory without its
    // manifest; the directory rename is the single visibility boundary.
    write_json_file(lease.staging_directory / "manifest.json", manifest);
    std::filesystem::rename(lease.staging_directory, lease.published_directory, error);
    if (error) throw std::runtime_error("cannot publish result payload: " + error.message());
    cleanup_empty_runs();
    return manifest;
}

void ResultStore::abort(const ResultLease &lease) const {
    if (lease.writer) lease.writer->close();
    std::error_code error;
    if (std::filesystem::exists(lease.staging_directory, error))
        std::filesystem::remove_all(lease.staging_directory, error);
    if (error) throw std::runtime_error("cannot remove aborted result staging payload: " + error.message());
    cleanup_empty_runs();
}

}  // namespace streamfind::service
