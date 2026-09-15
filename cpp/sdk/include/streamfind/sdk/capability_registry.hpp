#pragma once

#include <cstddef>
#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "streamfind/sdk/plugin_host_access.hpp"

namespace streamfind::sdk {

enum class CapabilityKind {
    Operation,
    Method,
};

using CapabilityHandler = Json (*)(PluginProjectAccess &, const Json &);

struct CapabilityBinding {
    std::string_view id;
    CapabilityKind kind;
    CapabilityHandler handler;
};

class CapabilityRegistry final {
public:
    CapabilityRegistry(std::initializer_list<CapabilityBinding> bindings)
        : bindings_(bindings) {
        std::sort(bindings_.begin(), bindings_.end(),
                  [](const auto &left, const auto &right) { return left.id < right.id; });
        for (std::size_t index = 1; index < bindings_.size(); ++index) {
            if (bindings_[index - 1].id == bindings_[index].id)
                throw std::invalid_argument("duplicate capability binding: " +
                                            std::string(bindings_[index].id));
        }
    }

    const CapabilityBinding *find(std::string_view id) const noexcept {
        const auto iterator = std::lower_bound(
            bindings_.begin(), bindings_.end(), id,
            [](const auto &binding, std::string_view value) { return binding.id < value; });
        return iterator != bindings_.end() && iterator->id == id ? &*iterator : nullptr;
    }
    const std::vector<CapabilityBinding> &bindings() const noexcept { return bindings_; }

private:
    std::vector<CapabilityBinding> bindings_;
};

}  // namespace streamfind::sdk
