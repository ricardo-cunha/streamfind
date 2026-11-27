#pragma once

#include "streamfind/project.hpp"

#include <string>

namespace streamfind::sdk {

/** Validate catalogue schemas against the ABI 3.0 logical type profile. */
STREAMFIND_SDK_API void validate_abi_type_profile(const Json &catalogue_entries);

}  // namespace streamfind::sdk
