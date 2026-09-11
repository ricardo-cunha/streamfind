#pragma once

#include "streamfind/catalogue_binding.hpp"
#include "streamfind/export.hpp"

namespace streamfind::mass_spec {

STREAMFIND_DOMAIN_API void register_plugin(const Json &entries,
                                           MethodRegistry &methods,
                                           OperationRegistry &operations);

}
