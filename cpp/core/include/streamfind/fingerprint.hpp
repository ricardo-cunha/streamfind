#pragma once

#include "streamfind/project.hpp"

#include <string>

namespace streamfind::fingerprint {

/** @brief Return deterministic JSON text independent of object insertion order. */
STREAMFIND_CORE_API std::string canonical_json(const Json &value);

/** @brief Hash canonical text with the framework's stable fingerprint hash. */
STREAMFIND_CORE_API std::string hash(const std::string &value);

/** @brief Compute a versioned operation/input fingerprint. */
STREAMFIND_CORE_API std::string operation(const std::string &operation_id,
                                          const std::string &operation_instance,
                                          const std::string &operation_version,
                                          const Json &parameters,
                                          const Json &inputs);

}  // namespace streamfind::fingerprint
