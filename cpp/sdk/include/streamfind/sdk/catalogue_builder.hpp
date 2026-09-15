#pragma once

#include <filesystem>
#include <string>

#include "streamfind/export.hpp"
#include "streamfind/sdk/semantic_resources.hpp"
#include "streamfind/sdk/semantic_validator.hpp"

namespace streamfind::sdk {

struct STREAMFIND_SDK_API CatalogueBuildRequest {
    SemanticResourceSet resources;
    std::filesystem::path output_json;
    std::filesystem::path output_matrix;
    std::filesystem::path output_database;
    std::filesystem::path jena_home;
    std::string catalogue_kind{"plugin"};
};

struct STREAMFIND_SDK_API CatalogueBuildResult {
    bool success{false};
    SemanticValidationResult validation;
    std::string diagnostics;
};

STREAMFIND_SDK_API CatalogueBuildResult
build_plugin_catalogue(const CatalogueBuildRequest &request);

}  // namespace streamfind::sdk
