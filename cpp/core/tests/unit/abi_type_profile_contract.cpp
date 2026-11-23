#include <iostream>
#include <stdexcept>

#include "streamfind/sdk/abi_type_profile.hpp"

int main() {
    try {
        const streamfind::Json valid = streamfind::Json::array({streamfind::Json{
            {"canonical_id", "example.operation"},
            {"input_ports", streamfind::Json::array({streamfind::Json{
                {"representations", streamfind::Json::array({"table"})},
                {"schema", streamfind::Json{{"type", "object"}, {"properties", streamfind::Json{
                    {"score", streamfind::Json{{"type", "real"}}},
                    {"labels", streamfind::Json{{"type", "array"}, {"items", streamfind::Json{{"type", "string"}}}}}
                }}}}
            }})},
            {"result", streamfind::Json{{"schema", streamfind::Json{{"type", "object"}}}}}
        }});
        streamfind::sdk::validate_abi_type_profile(valid);

        auto unknown = valid;
        unknown[0]["input_ports"][0]["schema"]["properties"]["score"]["type"] = "uuid";
        try {
            streamfind::sdk::validate_abi_type_profile(unknown);
            throw std::runtime_error("unknown ABI type was accepted");
        } catch (const std::invalid_argument &) {
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "failure: " << error.what() << '\n';
        return 3;
    }
}
