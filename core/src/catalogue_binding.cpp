#include "streamfind/catalogue_binding.hpp"

namespace streamfind::catalogue {
namespace {

ParameterDefinition parameter_definition(const Json &item) {
    ParameterDefinition parameter;
    parameter.name = item.at("name").get<std::string>();
    parameter.description = item.value("description", item.value("definition", ""));
    parameter.type = TypeDescriptor::from_json(item.at("schema"));
    parameter.required = item.value("required", false);
    parameter.default_value = item.value("default", Json(nullptr));
    parameter.example = item.value("example", Json(nullptr));
    parameter.constraints = item.value("constraints", Json::object());
    return parameter;
}

ParameterSchema parameter_schema(const Json &entry) {
    ParameterSchema schema;
    for (const auto &item : entry.value("parameters", Json::array()))
        schema.definitions.push_back(parameter_definition(item));
    return schema;
}

const Json &entries_or_empty() {
    static const Json entries = entries_json().value_or(Json::array());
    return entries;
}

}  // namespace

MethodDefinition method_definition(const Json &entry) {
    MethodDefinition definition;
    definition.id = entry.at("canonical_id").get<std::string>();
    definition.name = entry.value("label", definition.id);
    definition.description = entry.value("definition", entry.value("label", ""));
    definition.domain = entry.value("domain", "");
    definition.cacheable = entry.value("cacheable", false);
    definition.writes = entry.value("effects", Json::object()).value("writes", std::vector<std::string>{});
    definition.required_methods = entry.value("required_methods", std::vector<std::string>{});
    definition.single_occurrence = entry.value("single_occurrence", false);
    definition.parameters = parameter_schema(entry);
    return definition;
}

OperationDefinition operation_definition(const Json &entry) {
    OperationDefinition definition;
    definition.id = entry.at("canonical_id").get<std::string>();
    definition.name = entry.value("label", definition.id);
    definition.description = entry.value("definition", entry.value("label", ""));
    definition.domain = entry.value("domain", "");
    definition.parameters = parameter_schema(entry);
    return definition;
}

void register_methods(const std::string &domain, MethodRegistry &registry,
                      const MethodResolver &resolver, const MethodValidatorResolver &validator) {
    for (const auto &entry : entries_or_empty()) {
        if (entry.value("domain", "") != domain || entry.value("kind", "") != "method") continue;
        const auto id = entry.at("canonical_id").get<std::string>();
        auto executor = resolver(id);
        if (!executor) continue;
        registry.register_method(Method(method_definition(entry), std::move(executor),
                                        validator ? validator(id) : MethodValidator{}));
    }
}

void register_operations(const std::string &domain, OperationRegistry &registry,
                         const OperationResolver &resolver) {
    for (const auto &entry : entries_or_empty()) {
        if (entry.value("domain", "") != domain || entry.value("kind", "") != "operation") continue;
        const auto id = entry.at("canonical_id").get<std::string>();
        auto executor = resolver(id, entry.value("result", Json::object()).value("schema", Json::object()));
        if (!executor) continue;
        registry.register_operation(Operation(operation_definition(entry), std::move(executor)));
    }
}

}  // namespace streamfind::catalogue
