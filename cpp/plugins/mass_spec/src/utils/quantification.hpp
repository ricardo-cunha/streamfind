#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace streamfind::mass_spec::quantification::utils
{
struct ValidationError
{
    std::string path;
    std::string message;
};

std::vector<ValidationError> validate_method(const nlohmann::json &);
std::vector<ValidationError> validate_assignments(const nlohmann::json &);
std::string format_validation_errors(const std::vector<ValidationError> &);
}
