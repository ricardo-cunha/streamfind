#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "streamfind/export.hpp"
#include "streamfind/sdk/method_execution_context.hpp"

namespace streamfind {

class Project;
class ProjectTableStore;

namespace catalogue {

/** @brief Stable in-process method binding supplied by a domain plugin. */
struct STREAMFIND_SDK_API MethodBinding {
    std::string_view id;
    std::function<nlohmann::json(Project &, const nlohmann::json &)> executor;
    std::function<void(const nlohmann::json &)> validator;
    sdk::ContextMethodExecutor context_executor{};
};

/** @brief Stable in-process operation binding supplied by a domain plugin. */
struct STREAMFIND_SDK_API OperationBinding {
    std::string_view id;
    std::function<nlohmann::json(Project &, const nlohmann::json &)> executor;
    std::function<void(const nlohmann::json &)> validator;
};

/** @brief Module descriptor and executable bindings supplied by a domain plugin. */
struct STREAMFIND_SDK_API DomainModuleBinding {
    std::string module_id;
    std::string domain_id;
    std::string module_version;
    std::vector<std::string> required_modules;
    std::vector<MethodBinding> methods;
    std::vector<OperationBinding> operations;
    std::vector<std::string> tables;
    std::function<void(ProjectTableStore &)> schema_binding;
    int schema_version{1};
    std::function<void(ProjectTableStore &, int)> schema_migration;
};

}  // namespace catalogue
}  // namespace streamfind
