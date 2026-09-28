#include "streamfind/core_operations.hpp"

#include "streamfind/catalogue_binding.hpp"

#include <stdexcept>
#include <utility>

namespace streamfind::core_operations {
namespace detail {

void validate_paths(const Json &parameters) {
    const auto &paths = parameters.at("paths");
    if (!paths.is_array())
        throw std::invalid_argument("paths must be an array");
    for (const auto &path : paths) {
        if (!path.is_string() || path.get<std::string>().empty())
            throw std::invalid_argument("paths must contain non-empty strings");
    }
}

Json select_paths(Project &, const Json &parameters,
                  const std::string &operation_instance, const Json &) {
    validate_paths(parameters);
    (void)operation_instance;
    return parameters.at("paths");
}

}  // namespace detail

void register_operations(const Json &entries, OperationRegistry &operations) {
    for (const auto &entry : entries) {
        if (entry.value("kind", "") != "operation" ||
            entry.value("canonical_id", "") != "streamfind.select_paths")
            continue;
        auto definition = catalogue::operation_definition(entry);
        operations.register_operation(
            Operation(std::move(definition), &detail::select_paths, &detail::validate_paths));
        return;
    }
    throw std::invalid_argument("catalogue: core operation is missing streamfind.select_paths");
}

}  // namespace streamfind::core_operations
