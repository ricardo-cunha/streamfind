#pragma once

#include <string>

#include "streamfind/project.hpp"

namespace streamfind::static_plugins {

/** @brief Compose all statically linked plugins against one catalogue snapshot. */
void register_all(const Json &entries, MethodRegistry &methods, OperationRegistry &operations);

/** @brief Load the core/plugin catalogues and compose all shipped plugins. */
void load_and_register(const std::string &aggregate_path,
                       MethodRegistry &methods,
                       OperationRegistry &operations);

}  // namespace streamfind::static_plugins
