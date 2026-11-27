#include "streamfind/sdk/abi_type_profile.hpp"

#include <set>
#include <stdexcept>
#include <string>

namespace streamfind::sdk {

namespace detail {

void validate_type(const std::string &type, const std::string &path) {
    static const std::set<std::string> supported = {
        "array", "binary", "boolean", "decimal", "integer", "object", "real", "string", "table", "timestamp"};
    if (!supported.contains(type))
        throw std::invalid_argument("ABI type profile: unsupported type '" + type + "' at " + path);
}

void validate_schema(const Json &schema, const std::string &path) {
    if (schema.is_string()) {
        const auto text = schema.get<std::string>();
        const auto marker = text.find("type=");
        if (marker == std::string::npos)
            throw std::invalid_argument("ABI type profile: projected schema has no type at " + path);
        const auto begin = marker + 5;
        const auto end = text.find_first_of(";}", begin);
        validate_type(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin), path);
        return;
    }
    if (!schema.is_object()) throw std::invalid_argument("ABI type profile: schema is not an object at " + path);
    if (schema.contains("type")) {
        if (!schema.at("type").is_string())
            throw std::invalid_argument("ABI type profile: type is not a string at " + path);
        const auto type = schema.at("type").get<std::string>();
        validate_type(type, path);
        if (type == "array" && schema.contains("items") && !schema.at("items").is_null())
            validate_schema(schema.at("items"), path + ".items");
        if (type == "object" && schema.contains("properties")) {
            if (!schema.at("properties").is_object())
                throw std::invalid_argument("ABI type profile: object properties are not an object at " + path);
            for (const auto &[name, child] : schema.at("properties").items())
                validate_schema(child, path + ".properties." + name);
        }
    }
}

void validate_port(const Json &port, const std::string &path) {
    if (!port.is_object()) throw std::invalid_argument("ABI type profile: port is not an object at " + path);
    const auto representations = port.value("representations", Json::array());
    if (!representations.is_array())
        throw std::invalid_argument("ABI type profile: representations are not an array at " + path);
    static const std::set<std::string> supported = {"array", "boolean", "json", "object", "table"};
    for (const auto &representation : representations) {
        if (!representation.is_string() || !supported.contains(representation.get<std::string>()))
            throw std::invalid_argument("ABI type profile: unsupported representation at " + path);
    }
    if (port.contains("schema")) validate_schema(port.at("schema"), path + ".schema");
}

}  // namespace detail

void validate_abi_type_profile(const Json &catalogue_entries) {
    if (!catalogue_entries.is_array())
        throw std::invalid_argument("ABI type profile: catalogue entries are not an array");
    for (const auto &entry : catalogue_entries) {
        if (!entry.is_object()) throw std::invalid_argument("ABI type profile: entry is not an object");
        const auto id = entry.value("canonical_id", "<unknown>");
        for (const auto &field : {std::string("input_ports"), std::string("output_ports")}) {
            if (!entry.contains(field)) continue;
            if (!entry.at(field).is_array())
                throw std::invalid_argument("ABI type profile: " + field + " is not an array at " + id);
            std::size_t index = 0;
            for (const auto &port : entry.at(field))
                detail::validate_port(port, id + "." + field + "[" + std::to_string(index++) + "]");
        }
        if (entry.contains("parameters") && entry.at("parameters").is_array())
            for (const auto &parameter : entry.at("parameters"))
                if (parameter.contains("schema")) detail::validate_schema(parameter.at("schema"), id + ".parameter");
        if (entry.contains("result") && entry.at("result").is_object() && entry.at("result").contains("schema"))
            detail::validate_schema(entry.at("result").at("schema"), id + ".result.schema");
    }
}

}  // namespace streamfind::sdk
