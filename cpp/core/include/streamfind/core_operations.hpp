#pragma once

#include "streamfind/project.hpp"

namespace streamfind::core_operations {

/** Register executable operations owned by the core catalogue. */
STREAMFIND_CORE_API void register_operations(const Json &entries,
                                             OperationRegistry &operations);

}  // namespace streamfind::core_operations
