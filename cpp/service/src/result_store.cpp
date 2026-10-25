#include "streamfind/service/result_store.hpp"
#include "streamfind/data_proxy_protocol.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <deque>

namespace streamfind::service {

namespace detail {

struct ArrowFrameIndexEntry {
    std::uintmax_t file_offset{0};
    std::uintmax_t row_offset{0};
    std::uintmax_t row_count{0};
};

std::mutex arrow_index_mutex;
std::unordered_map<std::string, std::vector<ArrowFrameIndexEntry>> arrow_frame_indexes;

// Visualization tabs commonly request the same Arrow frame more than once.
// Cache decoded frames so EIC/MS/Raman/statistics views share the parse work.
std::mutex arrow_frame_cache_mutex;
std::unordered_map<std::string, Json> arrow_frame_rows_cache;
std::deque<std::string> arrow_frame_cache_order;
std::size_t arrow_frame_cache_bytes = 0;
constexpr std::size_t arrow_frame_cache_limit = 64 * 1024 * 1024;

std::string arrow_frame_cache_key(const std::filesystem::path &payload_path,
                                  std::uintmax_t file_offset) {
    return payload_path.lexically_normal().string() + "\n" + std::to_string(file_offset);
}

bool cached_arrow_frame(const std::filesystem::path &payload_path,
                        std::uintmax_t file_offset,
                        Json &rows) {
    const auto key = arrow_frame_cache_key(payload_path, file_offset);
    std::lock_guard lock(arrow_frame_cache_mutex);
    const auto found = arrow_frame_rows_cache.find(key);
    if (found == arrow_frame_rows_cache.end()) return false;
    rows = found->second;
    return true;
}

void cache_arrow_frame(const std::filesystem::path &payload_path,
                       std::uintmax_t file_offset,
                       const Json &rows) {
    const auto key = arrow_frame_cache_key(payload_path, file_offset);
    const auto bytes = rows.dump().size();
    if (bytes > arrow_frame_cache_limit) return;
    std::lock_guard lock(arrow_frame_cache_mutex);
    if (arrow_frame_rows_cache.contains(key)) return;
    while (arrow_frame_cache_bytes + bytes > arrow_frame_cache_limit && !arrow_frame_cache_order.empty()) {
        const auto oldest = arrow_frame_cache_order.front();
        arrow_frame_cache_order.pop_front();
        const auto found = arrow_frame_rows_cache.find(oldest);
        if (found != arrow_frame_rows_cache.end()) {
            arrow_frame_cache_bytes -= found->second.dump().size();
            arrow_frame_rows_cache.erase(found);
        }
    }
    arrow_frame_cache_order.push_back(key);
    arrow_frame_rows_cache.emplace(key, rows);
    arrow_frame_cache_bytes += bytes;
}

const std::vector<ArrowFrameIndexEntry> &arrow_frame_index(const std::filesystem::path &payload_path) {
    const auto key = payload_path.lexically_normal().string();
    std::lock_guard lock(arrow_index_mutex);
    const auto cached = arrow_frame_indexes.find(key);
    if (cached != arrow_frame_indexes.end()) return cached->second;

    std::ifstream input(payload_path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read Arrow IPC result payload");
    std::vector<ArrowFrameIndexEntry> index;
    std::uintmax_t row_offset = 0;
    while (input.peek() != std::char_traits<char>::eof()) {
        const auto file_offset = static_cast<std::uintmax_t>(input.tellg());
        std::uint64_t size = 0;
        input.read(reinterpret_cast<char *>(&size), sizeof(size));
        if (!input) throw std::runtime_error("Arrow IPC result contains an incomplete frame");
        if (size == 0 || size > 64 * 1024 * 1024)
            throw std::runtime_error("Arrow IPC result frame size is invalid");
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) throw std::runtime_error("Arrow IPC result contains an incomplete batch");
        const auto rows = data_proxy::arrow_ipc_decode_rows(bytes);
        index.push_back({file_offset, row_offset, rows.size()});
        row_offset += rows.size();
    }
    return arrow_frame_indexes.emplace(key, std::move(index)).first->second;
}

void clear_arrow_frame_indexes() {
    std::lock_guard lock(arrow_index_mutex);
    arrow_frame_indexes.clear();
    std::lock_guard frame_lock(arrow_frame_cache_mutex);
    arrow_frame_rows_cache.clear();
    arrow_frame_cache_order.clear();
    arrow_frame_cache_bytes = 0;
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

std::vector<unsigned char> decode_base64(const std::string &value) {
    static constexpr unsigned char invalid = 0xff;
    static constexpr std::array<unsigned char, 256> lookup = [] {
        std::array<unsigned char, 256> table{};
        table.fill(invalid);
        for (unsigned char index = 0; index < 26; ++index) {
            table[static_cast<unsigned char>('A' + index)] = index;
            table[static_cast<unsigned char>('a' + index)] = static_cast<unsigned char>(26 + index);
        }
        for (unsigned char index = 0; index < 10; ++index)
            table[static_cast<unsigned char>('0' + index)] = static_cast<unsigned char>(52 + index);
        table[static_cast<unsigned char>('+')] = 62;
        table[static_cast<unsigned char>('/')] = 63;
        return table;
    }();
    if (value.size() % 4 != 0) throw std::invalid_argument("base64 payload has invalid length");
    std::vector<unsigned char> output;
    output.reserve((value.size() / 4) * 3);
    for (std::size_t index = 0; index < value.size(); index += 4) {
        const auto first = lookup[static_cast<unsigned char>(value[index])];
        const auto second = lookup[static_cast<unsigned char>(value[index + 1])];
        if (first == invalid || second == invalid)
            throw std::invalid_argument("base64 payload contains an invalid character");
        const auto third_character = value[index + 2];
        const auto fourth_character = value[index + 3];
        const auto third = third_character == '=' ? 0 : lookup[static_cast<unsigned char>(third_character)];
        const auto fourth = fourth_character == '=' ? 0 : lookup[static_cast<unsigned char>(fourth_character)];
        if (third == invalid || fourth == invalid ||
            (third_character == '=' && fourth_character != '=') ||
            (index + 4 != value.size() && (third_character == '=' || fourth_character == '=')))
            throw std::invalid_argument("base64 payload has invalid padding");
        output.push_back(static_cast<unsigned char>((first << 2) | (second >> 4)));
        if (third_character != '=') {
            output.push_back(static_cast<unsigned char>((second << 4) | (third >> 2)));
            if (fourth_character != '=') output.push_back(static_cast<unsigned char>((third << 6) | fourth));
        }
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
    std::uintmax_t size = 0;
    const auto digest = payload_digest(payload_path, size);
    if (manifest.value("state", std::string{}) != "published" ||
        manifest.value("payload_size", std::uintmax_t{0}) != size ||
        manifest.value("payload_digest", std::string{}) != digest)
        throw std::runtime_error("result payload failed integrity validation: " + result_id);
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
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root_, error)) {
            if (error) break;
            if (entry.is_regular_file(error) && !error) result.size_bytes += entry.file_size(error);
            if (error) break;
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
        if (std::filesystem::exists(results, error)) {
            for (const auto &entry : std::filesystem::directory_iterator(results, error)) {
                if (error) break;
                if (!entry.is_directory()) continue;
                const auto manifest_path = entry.path() / "manifest.json";
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
                    ++result.invalid_count;
                }
            }
        }
        const auto runs = root_ / "runs";
        if (std::filesystem::exists(runs, error)) {
            for (const auto &entry : std::filesystem::directory_iterator(runs, error)) {
                if (error) break;
                if (entry.is_directory()) ++result.staging_run_count;
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
    const auto existing = inspect(verify_payloads);
    if (existing.status == ResultStoreStatus::ready) return existing;
    if (existing.status == ResultStoreStatus::invalid)
        throw std::runtime_error(existing.error);

    std::error_code error;
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
            if (descriptor.value("format", std::string{}) == "jsonl" ||
                descriptor.value("format", std::string{}) == "arrow-ipc-batches")
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
    detail::clear_arrow_frame_indexes();
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
    Json descriptor = manifest;
    descriptor.erase("payload");
    descriptor["payload_ref"] = "result:" + result_id;
    descriptor["capabilities"] = {"describe", "read_range"};
    if (descriptor.value("format", std::string{}) == "parquet" ||
        descriptor.value("format", std::string{}) == "arrow-ipc-batches")
        descriptor["capabilities"].push_back("read_batches");
    return descriptor;
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

Json ResultStore::read_jsonl_batch(const std::string &result_id,
                                   std::uintmax_t offset,
                                   std::uintmax_t limit) const {
    if (limit == 0 || limit > 65536) throw std::invalid_argument("result batch limit is invalid");
    const auto manifest = detail::validated_manifest(root_, result_id);
    if (manifest.value("format", std::string{}) != "jsonl")
        throw std::invalid_argument("result is not a JSONL table result: " + result_id);
    const auto payload_path = root_ / std::filesystem::path(manifest.at("payload").get<std::string>());
    std::ifstream input(payload_path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read JSONL result payload");
    Json rows = Json::array();
    std::string line;
    std::uintmax_t skipped = 0;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto batch = Json::parse(line, nullptr, false);
        if (batch.is_discarded() || !batch.is_array())
            throw std::runtime_error("JSONL result contains an invalid table batch");
        for (const auto &row : batch) {
            if (skipped < offset) { ++skipped; continue; }
            if (rows.size() >= limit)
                return Json{{"rows", rows}, {"offset", offset}, {"limit", limit}, {"has_more", true}};
            rows.push_back(row);
        }
    }
    return Json{{"rows", rows}, {"offset", offset}, {"limit", limit}, {"has_more", false}};
}

Json ResultStore::read_arrow_ipc_batch(const std::string &result_id,
                                       std::uintmax_t offset,
                                       std::uintmax_t limit,
                                       const std::vector<std::string> &columns) const {
    if (limit == 0 || limit > 65536) throw std::invalid_argument("result batch limit is invalid");
    const auto manifest = detail::validated_manifest(root_, result_id);
    if (manifest.value("format", std::string{}) != "arrow-ipc-batches")
        throw std::invalid_argument("result is not an Arrow IPC table result: " + result_id);
    const auto payload_path = root_ / std::filesystem::path(manifest.at("payload").get<std::string>());
    std::ifstream input(payload_path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read Arrow IPC result payload");
    const auto &index = detail::arrow_frame_index(payload_path);
    Json rows = Json::array();
    for (const auto &frame : index) {
        if (frame.row_offset + frame.row_count <= offset) continue;
        if (frame.row_offset >= offset + limit && !rows.empty()) break;
        input.clear();
        input.seekg(static_cast<std::streamoff>(frame.file_offset));
        if (!input) throw std::runtime_error("cannot seek Arrow IPC result payload");
        std::uint64_t size = 0;
        input.read(reinterpret_cast<char *>(&size), sizeof(size));
        if (!input) throw std::runtime_error("Arrow IPC result contains an incomplete frame");
        if (size == 0 || size > 64 * 1024 * 1024)
            throw std::runtime_error("Arrow IPC result frame size is invalid");
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) throw std::runtime_error("Arrow IPC result contains an incomplete batch");
        Json batch;
        if (!columns.empty()) {
            batch = data_proxy::arrow_ipc_decode_rows(bytes, columns);
        } else if (!detail::cached_arrow_frame(payload_path, frame.file_offset, batch)) {
            batch = data_proxy::arrow_ipc_decode_rows(bytes);
            detail::cache_arrow_frame(payload_path, frame.file_offset, batch);
        }
        for (std::size_t row_index = 0; row_index < batch.size(); ++row_index) {
            const auto &row = batch.at(row_index);
            const auto absolute_offset = frame.row_offset + row_index;
            if (absolute_offset < offset) continue;
            if (rows.size() >= limit)
                return Json{{"rows", rows}, {"offset", offset}, {"limit", limit}, {"has_more", true}};
            rows.push_back(row);
        }
    }
    const auto returned = static_cast<std::uintmax_t>(rows.size());
    return Json{{"rows", rows}, {"offset", offset}, {"limit", limit},
                {"has_more", offset + returned < (index.empty() ? 0 : index.back().row_offset + index.back().row_count)}};
}

void ResultStore::append_staging_chunk(const ResultLease &lease,
                                       std::uintmax_t offset,
                                       const std::vector<unsigned char> &bytes) const {
    constexpr std::size_t max_chunk = 1024 * 1024;
    if (bytes.size() > max_chunk) throw std::invalid_argument("result chunk is too large");
    if (!std::filesystem::is_directory(lease.staging_directory))
        throw std::invalid_argument("result lease staging directory is unavailable");
    std::error_code error;
    const auto current_size = std::filesystem::exists(lease.staging_payload, error)
                                  ? std::filesystem::file_size(lease.staging_payload, error)
                                  : std::uintmax_t{0};
    if (error || offset != current_size)
        throw std::invalid_argument("result chunks must be appended sequentially");
    std::ofstream output(lease.staging_payload, std::ios::binary | std::ios::app);
    if (!output) throw std::runtime_error("cannot open result staging payload");
    if (!bytes.empty())
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("cannot append result staging payload");
}

ResultLease ResultStore::allocate(const std::string &run_id,
                                      const std::string &result_id,
                                      const std::string &format,
                                      const std::string &extension) {
    require_component(run_id, "run_id");
    require_component(result_id, "result_id");
    require_component(format, "format");
    if (format == "jsonl")
        throw std::invalid_argument("new table results must use arrow-ipc-batches; JSONL is legacy input only");
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
    return ResultLease{result_id, run_id, format, staging_directory,
                         staging_directory / ("payload." + normalized_extension),
                         published_directory,
                         published_directory / ("payload." + normalized_extension)};
}

Json ResultStore::publish(const ResultLease &lease, const Json &metadata) {
    if (!std::filesystem::is_regular_file(lease.staging_payload))
        throw std::runtime_error("result payload is missing or is not a regular file");
    std::uintmax_t payload_size = 0;
    const auto payload_digest_value = detail::payload_digest(lease.staging_payload, payload_size);
    std::error_code error;
    if (std::filesystem::exists(lease.published_directory, error))
        throw std::runtime_error("result has already been published: " + lease.result_id);
    std::filesystem::rename(lease.staging_directory, lease.published_directory, error);
    if (error) throw std::runtime_error("cannot publish result payload: " + error.message());
    Json manifest = metadata;
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
    write_json_file(lease.published_directory / "manifest.json", manifest);
    cleanup_empty_runs();
    return manifest;
}

void ResultStore::abort(const ResultLease &lease) const {
    std::error_code error;
    if (std::filesystem::exists(lease.staging_directory, error))
        std::filesystem::remove_all(lease.staging_directory, error);
    if (error) throw std::runtime_error("cannot remove aborted result staging payload: " + error.message());
    cleanup_empty_runs();
}

}  // namespace streamfind::service
