#include "streamfind/fingerprint.hpp"

#include <iomanip>
#include <map>
#include <sstream>
#include <filesystem>
#include <string_view>

namespace streamfind::fingerprint {
namespace detail {

std::string canonical(const Json &value) {
    if (value.is_object()) {
        std::map<std::string, std::string> members;
        for (const auto &[key, child] : value.items()) members.emplace(key, canonical(child));
        std::string output = "{";
        bool first = true;
        for (const auto &[key, child] : members) {
            if (!first) output += ",";
            first = false;
            output += Json(key).dump() + ":" + child;
        }
        return output + "}";
    }
    if (value.is_array()) {
        std::string output = "[";
        for (std::size_t index = 0; index < value.size(); ++index) {
            if (index) output += ",";
            output += canonical(value.at(index));
        }
        return output + "]";
    }
    return value.dump();
}

void collect_external_state(const Json &value, std::string_view key, Json &state) {
    if (value.is_string() && (key.find("path") != std::string_view::npos ||
                              key.find("file") != std::string_view::npos)) {
        const auto path = std::filesystem::path(value.get<std::string>());
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        Json record = {{"exists", exists}};
        if (exists && !error) {
            const bool directory = std::filesystem::is_directory(path, error);
            record["directory"] = directory;
            if (!directory && !error) record["size"] = std::filesystem::file_size(path, error);
            const auto modified = std::filesystem::last_write_time(path, error);
            if (!error) record["modified"] = modified.time_since_epoch().count();
        }
        state[value.get<std::string>()] = std::move(record);
        return;
    }
    if (value.is_object())
        for (const auto &[child_key, child] : value.items()) collect_external_state(child, child_key, state);
    else if (value.is_array())
        for (const auto &child : value) collect_external_state(child, key, state);
}

}  // namespace detail

std::string canonical_json(const Json &value) { return detail::canonical(value); }

std::string hash(const std::string &value) {
    std::uint64_t result = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        result ^= byte;
        result *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << result;
    return output.str();
}

std::string operation(const std::string &operation_id,
                      const std::string &operation_instance,
                      const std::string &operation_version,
                      const Json &parameters,
                      const Json &inputs) {
    Json external_state = Json::object();
    detail::collect_external_state(parameters, "", external_state);
    detail::collect_external_state(inputs, "", external_state);
    return hash(canonical_json(Json{{"operation_id", operation_id},
                                    {"operation_instance", operation_instance},
                                    {"operation_version", operation_version},
                                    {"parameters", parameters},
                                    {"inputs", inputs},
                                    {"external_state", external_state}}));
}

}  // namespace streamfind::fingerprint
