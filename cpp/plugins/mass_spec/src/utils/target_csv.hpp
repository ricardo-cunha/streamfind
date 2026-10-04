#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace streamfind::mass_spec::target_csv
{
    nlohmann::json read_targets_csv(const std::string &path, bool suspect_targets);
}
