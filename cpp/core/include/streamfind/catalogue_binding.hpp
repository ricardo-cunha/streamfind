#pragma once

#include "streamfind/sdk/plugin_contract.hpp"
#include "streamfind/project.hpp"

namespace streamfind::catalogue {

/** @brief Install a module schema into the caller-owned project file. */
STREAMFIND_CORE_API void install_module_schema(const DomainModuleBinding &module,
                                               Project &project);

/** @brief Parse a semantic catalogue method entry into a runtime definition. */
STREAMFIND_CORE_API MethodDefinition method_definition(const Json &entry);
/** @brief Parse a semantic catalogue operation entry into a runtime definition. */
STREAMFIND_CORE_API OperationDefinition operation_definition(const Json &entry);

STREAMFIND_CORE_API void register_module(const DomainModuleBinding &module,
                                         const Json &entries,
                                         MethodRegistry &methods,
                                         OperationRegistry &operations);
STREAMFIND_CORE_API void register_module(const DomainModuleBinding &module,
                                         const Json &entries,
                                         MethodRegistry &methods);
STREAMFIND_CORE_API void register_module(const DomainModuleBinding &module,
                                         const Json &entries,
                                         OperationRegistry &operations);

}  // namespace streamfind::catalogue
