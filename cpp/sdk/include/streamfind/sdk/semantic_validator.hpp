#pragma once

#include <filesystem>
#include <string>

#include "streamfind/export.hpp"
#include "streamfind/sdk/semantic_resources.hpp"

namespace streamfind::sdk {

struct STREAMFIND_SDK_API SemanticValidationResult {
    bool valid{false};
    std::string diagnostics;
};

STREAMFIND_SDK_API SemanticValidationResult
validate_semantics_with_jena(const SemanticResourceSet &resources,
                             const std::filesystem::path &jena_home);

}  // namespace streamfind::sdk
