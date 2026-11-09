#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace streamfind::mass_spec::fragmentation::detail
{
struct FragmenterLimits
{
    int depth = 2;
    std::uint32_t timeout_seconds = 120;
    std::uint64_t output_limit_bytes = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t maximum_fragment_rows = 250000;
};

inline std::uint64_t require_bounded_integer(
    const nlohmann::json &parameters,
    const char *name,
    std::uint64_t default_value,
    std::uint64_t minimum,
    std::uint64_t maximum)
{
    const auto found = parameters.find(name);
    if (found == parameters.end()) return default_value;
    if (!found->is_number_integer())
        throw std::invalid_argument(std::string(name) + " must be an integer.");
    const auto value = found->get<std::int64_t>();
    if (value < 0 || static_cast<std::uint64_t>(value) < minimum ||
        static_cast<std::uint64_t>(value) > maximum)
        throw std::invalid_argument(std::string(name) + " must be between " +
            std::to_string(minimum) + " and " + std::to_string(maximum) + ".");
    return static_cast<std::uint64_t>(value);
}

inline FragmenterLimits parse_fragmenter_limits(const nlohmann::json &parameters)
{
    FragmenterLimits limits;
    limits.depth = static_cast<int>(require_bounded_integer(
        parameters, "fragmentation_depth", 2, 1, 8));
    limits.timeout_seconds = static_cast<std::uint32_t>(require_bounded_integer(
        parameters, "fragmenter_timeout_seconds", 120, 1, 86400));
    limits.output_limit_bytes = require_bounded_integer(
        parameters, "fragmenter_output_limit_mb", 64, 1, 1024) * 1024ULL * 1024ULL;
    limits.maximum_fragment_rows = require_bounded_integer(
        parameters, "maximum_fragment_rows", 250000, 1, 1000000);
    return limits;
}
} // namespace streamfind::mass_spec::fragmentation::detail
