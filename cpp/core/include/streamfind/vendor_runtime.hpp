#pragma once

#include "streamfind/export.hpp"

namespace streamfind {

// Adds package-owned vendor runtime directories to the native DLL search path.
// This must run before the first use of a delay-loaded vendor library.
STREAMFIND_CORE_API void configure_vendor_runtime_paths();

}  // namespace streamfind
