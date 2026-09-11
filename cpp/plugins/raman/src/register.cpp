#include "streamfind/catalogue_binding.hpp"
#include "streamfind/raman/register.hpp"

#include <algorithm>
#include <limits>

namespace streamfind::raman {

namespace detail {

std::vector<catalogue::OperationBinding> base_operations(const Json &entries) {
    std::vector<catalogue::OperationBinding> bindings;
    for (const auto &id : {"raman.add_analyses", "raman.remove_analyses"}) {
        const auto entry = std::find_if(entries.begin(), entries.end(), [id](const auto &value) {
            return value.value("canonical_id", "") == id;
        });
        if (entry != entries.end()) bindings.push_back({id, {}, {}});
    }
    return bindings;
}

}

void register_plugin(const Json &entries, MethodRegistry &methods, OperationRegistry &operations) {
    catalogue::register_module({"raman.base", "raman", "1", {}, {}, detail::base_operations(entries), {}, {}},
                                entries, methods, operations);
}

}
