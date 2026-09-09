#pragma once

#include <functional>
#include <string>
#include <vector>

#include "streamfind/catalogue.hpp"
#include "streamfind/project.hpp"

namespace streamfind::catalogue {

/** @brief Resolve an executable method for a catalogue identifier. */
using MethodResolver = std::function<MethodExecutor(const std::string &)>;
using MethodValidatorResolver = std::function<MethodValidator(const std::string &)>;
/** @brief Resolve an executable operation for a catalogue identifier. */
using OperationResolver = std::function<OperationExecutor(const std::string &, const Json &)>;

/** @brief Parse a semantic catalogue method entry into a runtime definition. */
STREAMFIND_CORE_API MethodDefinition method_definition(const Json &entry);
/** @brief Parse a semantic catalogue operation entry into a runtime definition. */
STREAMFIND_CORE_API OperationDefinition operation_definition(const Json &entry);

/** @brief Register all executable methods for one domain from the catalogue. */
STREAMFIND_CORE_API void register_methods(const std::string &domain,
                                          MethodRegistry &registry,
                                          const MethodResolver &resolver,
                                          const MethodValidatorResolver &validator = {});
/** @brief Register all executable operations for one domain from the catalogue. */
STREAMFIND_CORE_API void register_operations(const std::string &domain,
                                             OperationRegistry &registry,
                                             const OperationResolver &resolver);

}  // namespace streamfind::catalogue
