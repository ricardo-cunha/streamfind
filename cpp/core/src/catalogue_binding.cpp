#include "streamfind/catalogue_binding.hpp"
#include "streamfind/catalogue.hpp"
#include "streamfind/project_table_store.hpp"

#include <stdexcept>

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


Json shape_table_result(Json result, const Json &schema) {
    if (schema.value("type", "") != "table") return result;
    Json columns = Json::object();
    const auto properties = schema.value("properties", Json::object());
    for (auto it = properties.begin(); it != properties.end(); ++it)
        columns[it.key()] = Json::array();
    for (const auto &row : result)
        for (auto it = columns.begin(); it != columns.end(); ++it)
            it.value().push_back(row.contains(it.key()) ? row.at(it.key()) : Json(nullptr));
    return {{"row_count", result.size()}, {"columns", std::move(columns)}};
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


void register_module(const DomainModuleBinding &module,
                     const Json &entries,
                     MethodRegistry &method_registry,
                     OperationRegistry &operation_registry) {
    for (const auto &dependency : module.required_modules) {
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) {
            return entry.value("module_id", "") == dependency;
        });
        if (found == entries.end())
            throw std::invalid_argument("catalogue: missing module dependency " + dependency +
                                        " for module " + module.module_id);
    }
    if (!module.tables.empty()) {
        const auto manifest = table_manifest_json(module.domain_id, module.module_id);
        if (!manifest)
            throw std::invalid_argument("catalogue: table manifest unavailable for module " +
                                        module.module_id);
        for (const auto &table : module.tables) {
            const auto found = std::find_if(manifest->begin(), manifest->end(), [&](const auto &entry) {
                return entry.value("table_name", "") == table;
            });
            if (found == manifest->end())
                throw std::invalid_argument("catalogue: missing owned table " + table +
                                            " for module " + module.module_id);
        }
    }
    for (const auto &binding : module.methods) {
        bool found = false;
        for (const auto &entry : entries) {
            if (entry.value("kind", "") == "method" &&
                entry.value("canonical_id", "") == binding.id &&
                entry.value("domain", "") == module.domain_id &&
                entry.value("module_id", "") == module.module_id) {
                method_registry.register_method(
                    Method(method_definition(entry), binding.executor, binding.validator,
                           binding.context_executor));
                found = true;
                break;
            }
        }
        if (!found) throw std::invalid_argument("catalogue: invalid method binding " + std::string(binding.id) + " for module " + module.module_id);
    }
    for (const auto &binding : module.operations) {
        bool found = false;
        for (const auto &entry : entries) {
            if (entry.value("kind", "") == "operation" &&
                entry.value("canonical_id", "") == binding.id &&
                entry.value("domain", "") == module.domain_id &&
                entry.value("module_id", "") == module.module_id) {
                const auto result_schema = entry.value("result", Json::object()).value("schema", Json::object());
                const auto executor = binding.executor;
                auto shaped_executor = [executor, result_schema](Project &project, const Json &parameters) {
                    return shape_table_result(executor(project, parameters), result_schema);
                };
                operation_registry.register_operation(
                    Operation(operation_definition(entry), std::move(shaped_executor), binding.validator));
                found = true;
                break;
            }
        }
        if (!found) throw std::invalid_argument("catalogue: invalid operation binding " + std::string(binding.id) + " for module " + module.module_id);
    }
}


void register_module(const DomainModuleBinding &module,
                     const Json &entries,
                     MethodRegistry &method_registry) {
    OperationRegistry operations;
    register_module(module, entries, method_registry, operations);
}

void register_module(const DomainModuleBinding &module,
                     const Json &entries,
                     OperationRegistry &operation_registry) {
    MethodRegistry methods;
    register_module(module, entries, methods, operation_registry);
}

void install_module_schema(const DomainModuleBinding &module, Project &project) {
    if (module.schema_version < 1)
        throw std::invalid_argument("catalogue: invalid schema version for module " + module.module_id);
    if (!module.tables.empty()) {
        const auto manifest = table_manifest_json(module.domain_id, module.module_id);
        if (!manifest)
            throw std::invalid_argument("catalogue: table manifest unavailable for module " + module.module_id);
        for (const auto &table : module.tables) {
            const auto found = std::find_if(manifest->begin(), manifest->end(), [&](const auto &entry) {
                return entry.value("table_name", "") == table;
            });
            if (found == manifest->end())
                throw std::invalid_argument("catalogue: missing owned table " + table +
                                            " for module " + module.module_id);
        }
    }

    ProjectTableStore::transaction(project, module.tables, [&](ProjectTableStore &tables) {
        tables.execute(
            "CREATE TABLE IF NOT EXISTS MODULE_SCHEMA (domain_id VARCHAR NOT NULL, module_id VARCHAR NOT NULL, "
            "schema_version INTEGER NOT NULL, installed_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
            "PRIMARY KEY (domain_id, module_id))");
        const auto installed_value = tables.scalar("SELECT schema_version FROM MODULE_SCHEMA WHERE domain_id = '" +
                                                   module.domain_id + "' AND module_id = '" + module.module_id + "' LIMIT 1");
        const int installed_version = installed_value ? std::stoi(*installed_value) : 0;
        if (installed_version > module.schema_version)
            throw std::invalid_argument("catalogue: schema downgrade for module " + module.module_id);
        if (installed_version == 0) {
            if (module.schema_binding) module.schema_binding(tables);
        } else if (installed_version < module.schema_version) {
            if (!module.schema_migration)
                throw std::invalid_argument("catalogue: migration unavailable for module " + module.module_id);
            module.schema_migration(tables, installed_version);
        }
        for (const auto &table : module.tables)
            if (!tables.has_table(table))
                throw Error(ErrorCode::SchemaMismatch, "schema installer did not create module table: " + table);

        const auto quote = [](const std::string &value) {
            std::string escaped;
            escaped.reserve(value.size() + 2);
            for (const char ch : value) {
                escaped += ch;
                if (ch == '\'') escaped += '\'';
            }
            return "'" + escaped + "'";
        };
        tables.execute("DELETE FROM MODULE_SCHEMA WHERE domain_id = " + quote(module.domain_id) +
                       " AND module_id = " + quote(module.module_id));
        tables.execute("INSERT INTO MODULE_SCHEMA (domain_id, module_id, schema_version) VALUES (" +
                       quote(module.domain_id) + ", " + quote(module.module_id) + ", " +
                       std::to_string(module.schema_version) + ")");
    });
}

}  // namespace streamfind::catalogue
