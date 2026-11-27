#pragma once

#include "streamfind/export.hpp"

#include <cstdint>

namespace streamfind {

// Returns the preferred loopback port when available, otherwise the first
// available port in the bounded range immediately following it.
STREAMFIND_CORE_API std::uint16_t select_service_port(std::uint16_t preferred,
                                                      std::uint16_t attempts = 128);

}  // namespace streamfind
