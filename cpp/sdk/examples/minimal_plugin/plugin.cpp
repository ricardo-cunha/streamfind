#include <nlohmann/json.hpp>

#include "streamfind/sdk/plugin_contract.hpp"

streamfind::catalogue::DomainModuleBinding make_example_module() {
    return {"example.base", "example", "1", {}, {}, {}, {}, {}};
}

int main() {
    const auto module = make_example_module();
    return module.module_id == "example.base" ? 0 : 1;
}
