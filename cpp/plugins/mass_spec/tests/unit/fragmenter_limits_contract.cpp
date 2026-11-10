#include "operations/fragmentation/fragmenter_limits.hpp"

#include <stdexcept>

int main()
{
    using streamfind::mass_spec::fragmentation::detail::parse_fragmenter_limits;

    const auto defaults = parse_fragmenter_limits(nlohmann::json::object());
    if (defaults.depth != 2 || defaults.timeout_seconds != 120 ||
        defaults.output_limit_bytes != 64ULL * 1024ULL * 1024ULL ||
        defaults.maximum_fragment_rows != 250000)
        throw std::runtime_error("Fragmenter resource defaults changed unexpectedly.");

    const auto configured = parse_fragmenter_limits({
        {"fragmentation_depth", 8}, {"fragmenter_timeout_seconds", 600},
        {"fragmenter_output_limit_mb", 128}, {"maximum_fragment_rows", 500000}});
    if (configured.depth != 8 || configured.timeout_seconds != 600 ||
        configured.output_limit_bytes != 128ULL * 1024ULL * 1024ULL ||
        configured.maximum_fragment_rows != 500000)
        throw std::runtime_error("Fragmenter resource parameters were not applied.");

    for (const auto &invalid : {
        nlohmann::json{{"fragmentation_depth", 9}},
        nlohmann::json{{"fragmenter_timeout_seconds", 0}},
        nlohmann::json{{"fragmenter_output_limit_mb", 1025}},
        nlohmann::json{{"maximum_fragment_rows", 0}},
        nlohmann::json{{"fragmenter_timeout_seconds", "120"}}})
    {
        bool rejected = false;
        try { (void)parse_fragmenter_limits(invalid); }
        catch (const std::invalid_argument &) { rejected = true; }
        if (!rejected) throw std::runtime_error("Invalid fragmenter limits were accepted.");
    }
    return 0;
}
