#include "streamfind/sdk/catalogue_builder.hpp"

#include <fstream>



#include <algorithm>
#include <cctype>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#include <duckdb.h>

namespace streamfind::sdk::detail {

using Json = nlohmann::json;
using Values = std::vector<std::string>;
using Graph = std::map<std::string, std::map<std::string, Values>>;

constexpr std::string_view sf = "https://streamfind.dev/vocabulary#";
constexpr std::string_view skos = "http://www.w3.org/2004/02/skos/core#";
constexpr std::string_view rdf_type = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";

std::string unquote(std::string value) {
    if (value.size() >= 2 && value.front() == '"') {
        std::size_t end = 0;
        bool escaped = false;
        for (std::size_t index = 1; index < value.size(); ++index) {
            if (value[index] == '"' && !escaped) end = index;
            escaped = value[index] == '\\' && !escaped;
            if (value[index] != '\\') escaped = false;
        }
        if (end == 0) return value;
        value = value.substr(1, end - 1);
        std::string out;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size()) {
                const char next = value[++i];
                out += next == 'n' ? '\n' : next == 'r' ? '\r' : next == 't' ? '\t' : next;
            } else out += value[i];
        }
        return out;
    }
    return value;
}

std::string expand(std::string_view token, const std::map<std::string, std::string> &prefixes) {
    if (token == "a") return std::string(rdf_type);
    if (token.size() >= 2 && token.front() == '<' && token.back() == '>')
        return std::string(token.substr(1, token.size() - 2));
    const auto split = token.find(':');
    if (split != std::string_view::npos) {
        const auto it = prefixes.find(std::string(token.substr(0, split)));
        if (it != prefixes.end()) return it->second + std::string(token.substr(split + 1));
    }
    return std::string(token);
}

std::vector<std::string> tokens(std::string_view statement) {
    std::vector<std::string> result;
    for (std::size_t i = 0; i < statement.size();) {
        while (i < statement.size() && std::isspace(static_cast<unsigned char>(statement[i]))) ++i;
        if (i == statement.size()) break;
        const char c = statement[i];
        if (c == ';' || c == ',') { result.emplace_back(1, c); ++i; continue; }
        if (c == '(') {
            std::size_t start = i++, depth = 1;
            while (i < statement.size() && depth) { if (statement[i] == '(') ++depth; if (statement[i] == ')') --depth; ++i; }
            result.emplace_back(statement.substr(start, i - start)); continue;
        }
        if (c == '<') {
            const auto end = statement.find('>', i);
            if (end == std::string_view::npos) { ++i; continue; }
            result.emplace_back(statement.substr(i, end - i + 1)); i = end + 1; continue;
        }
        if (c == '"') {
            std::size_t start = i++; bool escaped = false;
            while (i < statement.size()) { if (!escaped && statement[i] == '"') { ++i; break; } escaped = !escaped && statement[i] == '\\'; if (statement[i] != '\\') escaped = false; ++i; }
            while (i < statement.size() && (statement[i] == '@' || std::isalnum(static_cast<unsigned char>(statement[i])) || statement[i] == '-' || statement[i] == ':')) ++i;
            result.emplace_back(statement.substr(start, i - start)); continue;
        }
        const auto start = i++;
        while (i < statement.size() && !std::isspace(static_cast<unsigned char>(statement[i])) && statement[i] != ';' && statement[i] != ',') ++i;
        result.emplace_back(statement.substr(start, i - start));
    }
    return result;
}

Graph parse(const SemanticResourceSet &resources) {
    Graph graph; std::map<std::string, std::string> prefixes;
    prefixes["sf"] = std::string(sf); prefixes["skos"] = std::string(skos);
    const auto parse_file = [&](const std::filesystem::path &path) {
        std::ifstream input(path); if (!input) throw std::runtime_error("cannot read semantic file: " + path.string());
        std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        constexpr std::size_t max_source_bytes = 8U * 1024U * 1024U;
        constexpr std::size_t max_statement_bytes = 1U * 1024U * 1024U;
        if (source.size() > max_source_bytes) throw std::runtime_error("semantic file exceeds parser input limit: " + path.string());
        std::string statement;
        bool quoted = false, escaped = false, angle = false, comment = false;
        const auto parse_statement = [&](const std::string &raw) {
            const auto begin = raw.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) return;
            auto value = raw.substr(begin);
            auto words = tokens(value);
            if (words.size() >= 3 && words[0] == "@prefix") {
                prefixes[words[1].substr(0, words[1].size() - 1)] = expand(words[2], prefixes); return;
            }
            if (words.size() < 3) return;
            const auto subject = expand(words[0], prefixes); std::string predicate;
            for (std::size_t i = 1; i < words.size();) {
                if (words[i] == ";") { ++i; continue; }
                predicate = expand(words[i++], prefixes); if (i == words.size()) break;
                while (i < words.size() && words[i] != ";") {
                    if (words[i] != ",") {
                        if (graph.size() >= 100000U && !graph.contains(subject)) throw std::runtime_error("semantic graph exceeds subject limit: " + path.string());
                        if (graph[subject][predicate].size() >= 100000U) throw std::runtime_error("semantic predicate exceeds value limit: " + path.string());
                        if (words[i].size() >= 2 && words[i].front() == '(' && words[i].back() == ')') {
                            const auto members = tokens(std::string_view(words[i]).substr(1, words[i].size() - 2));
                            for (const auto &member : members) graph[subject][predicate].push_back(expand(member, prefixes));
                        } else graph[subject][predicate].push_back(expand(words[i], prefixes));
                    }
                    ++i;
                }
            }
        };
        for (std::size_t index = 0; index < source.size(); ++index) {
            const char c = source[index];
            if (comment) { if (c == '\n') comment = false; continue; }
            if (!quoted && !angle && c == '#') { comment = true; continue; }
            statement += c;
            if (statement.size() > max_statement_bytes) throw std::runtime_error("semantic statement exceeds parser limit: " + path.string());
            if (c == '"' && !angle && !escaped) quoted = !quoted;
            if (c == '<' && !quoted) angle = true;
            if (c == '>' && !quoted) angle = false;
            escaped = c == '\\' && !escaped;
            if (c != '\\') escaped = false;
            const bool decimal_point = c == '.' && index > 0 && index + 1 < source.size() &&
                                       std::isdigit(static_cast<unsigned char>(source[index - 1])) &&
                                       std::isdigit(static_cast<unsigned char>(source[index + 1]));
            if (c == '.' && !quoted && !angle && !decimal_point) {
                statement.pop_back(); parse_statement(statement); statement.clear();
            }
        }
        parse_statement(statement);
    };
    for (const auto &path : semantic_files(resources.core_directory)) {
        if (path.filename() == "vocabulary.ttl" || path.filename() == "shapes.ttl") continue;
        parse_file(path);
    }
    for (const auto &directory : resources.plugin_directories)
        for (const auto &path : semantic_files(directory)) parse_file(path);
    return graph;
}

std::string value(const Graph &graph, const std::string &subject, const std::string &predicate) {
    const auto s = graph.find(subject); if (s == graph.end()) return {};
    const auto p = s->second.find(predicate); if (p == s->second.end() || p->second.empty()) return {};
    return unquote(p->second.front());
}

std::vector<std::string> values(const Graph &graph, const std::string &subject, const std::string &predicate) {
    std::vector<std::string> result; const auto s = graph.find(subject); if (s == graph.end()) return result;
    const auto p = s->second.find(predicate); if (p == s->second.end()) return result;
    for (const auto &item : p->second) result.push_back(unquote(item)); return result;
}
std::string value(const Graph &graph, const std::string &subject, const std::string &predicate);
std::vector<std::string> rdf_list(const Graph &graph, const std::string &head) {
    std::vector<std::string> result; std::string node = head; std::set<std::string> seen;
    while (!node.empty() && node != "http://www.w3.org/1999/02/22-rdf-syntax-ns#nil" && seen.insert(node).second) {
        const auto first = value(graph, node, "http://www.w3.org/1999/02/22-rdf-syntax-ns#first"); if (first.empty()) break; result.push_back(first);
        node = value(graph, node, "http://www.w3.org/1999/02/22-rdf-syntax-ns#rest");
    }
    return result;
}

bool boolean(const Graph &graph, const std::string &subject, const std::string &predicate, bool fallback = false) {
    const auto item = value(graph, subject, predicate); return item.empty() ? fallback : item == "true";
}

std::string local(std::string value) { const auto hash = value.rfind('#'); return hash == std::string::npos ? value : value.substr(hash + 1); }
std::string json_text(const Json &value) { return value.dump(); }
std::string sql_quote(const std::string &value) { std::string out = "'"; for (char c : value) { if (c == '\'') out += "''"; else out += c; } return out + "'"; }

std::string resource_name(const std::string &resource) {
    auto name = local(resource); if (name.ends_with("Parameter")) name.resize(name.size() - 9); return name;
}
std::string wire_name(std::string name) {
    std::string result; for (char c : name) { if (std::isupper(static_cast<unsigned char>(c)) && !result.empty()) result += '_'; result += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); } return result;
}
std::string local(std::string value);
Json literal_json(const std::string &raw) {
    const auto datatype_marker = raw.find("^^"); const auto lexical = unquote(raw);
    if (datatype_marker != std::string::npos) {
        const auto datatype = local(raw.substr(datatype_marker + 2));
        try { if (datatype == "integer" || datatype == "int") return std::stoll(lexical); if (datatype == "real" || datatype == "double" || datatype == "decimal") return std::stod(lexical); if (datatype == "boolean") return lexical == "true"; } catch (...) { return lexical; }
    }
    try { return Json::parse(lexical); } catch (...) { return lexical; }
}
Json parameter_schema(const Graph &graph, const std::string &resource) {
    Json result = {{"type", value(graph, resource, std::string(sf) + "type")}};
    const auto definition = value(graph, resource, std::string(skos) + "definition"); if (!definition.empty()) result["description"] = definition;
    const auto label = value(graph, resource, std::string(skos) + "prefLabel"); if (!label.empty()) result["title"] = label;
    const auto units = value(graph, resource, std::string(sf) + "units"); if (!units.empty()) result["x-streamfind-units"] = units;
    const auto nullable = value(graph, resource, std::string(sf) + "nullable"); if (!nullable.empty()) result["x-streamfind-nullable"] = nullable == "true";
    const auto constraint = value(graph, resource, std::string(sf) + "constraints"); if (!constraint.empty()) {
        if (constraint.front() == '{') result.update(literal_json(constraint));
        else { result["enum"] = Json::array(); std::stringstream stream(constraint); std::string item; while (std::getline(stream, item, '|')) result["enum"].push_back(item); }
    }
    const auto example = value(graph, resource, std::string(sf) + "example"); if (!example.empty()) result["examples"] = Json::array({literal_json(example)});
    const auto item = value(graph, resource, std::string(sf) + "items"); if (!item.empty()) result["items"] = parameter_schema(graph, item);
    Json properties = Json::object(); Json required = Json::array();
    for (const auto &property : values(graph, resource, std::string(sf) + "hasProperty")) {
        const auto name = value(graph, property, std::string(sf) + "propertyName").empty() ? value(graph, property, std::string(sf) + "columnName") : value(graph, property, std::string(sf) + "propertyName");
        const auto key = name.empty() ? wire_name(local(property)) : name; properties[key] = parameter_schema(graph, property);
        if (boolean(graph, property, std::string(sf) + "required")) required.push_back(key);
    }
    if (!properties.empty()) { result["properties"] = properties; if (!required.empty()) result["required"] = required; result["additionalProperties"] = false; }
    return result;
}
Json mcp_schema(Json schema_value) {
    if (schema_value.value("type", "") == "real") schema_value["type"] = "number";
    if (schema_value.value("type", "") == "table") schema_value["type"] = "object";
    if (schema_value.contains("items")) schema_value["items"] = mcp_schema(schema_value["items"]);
    if (schema_value.contains("properties")) for (auto &[name, item] : schema_value["properties"].items()) item = mcp_schema(item);
    return schema_value;
}
Json column_schema(const Graph &graph, const std::string &resource) {
    Json result = {{"type", value(graph, resource, std::string(sf) + "type")}};
    const auto item = value(graph, resource, std::string(sf) + "items"); if (!item.empty()) result["items"] = column_schema(graph, item);
    Json properties = Json::object(); for (const auto &property : values(graph, resource, std::string(sf) + "hasProperty")) {
        const auto name = value(graph, property, std::string(sf) + "propertyName").empty() ? value(graph, property, std::string(sf) + "columnName") : value(graph, property, std::string(sf) + "propertyName");
        if (!name.empty()) properties[name] = column_schema(graph, property);
    }
    if (!properties.empty()) result["properties"] = properties;
    const auto constraint = value(graph, resource, std::string(sf) + "constraints"); if (!constraint.empty()) { result["enum"] = Json::array(); std::stringstream stream(constraint); std::string item_value; while (std::getline(stream, item_value, '|')) result["enum"].push_back(item_value); }
    return result;
}
Json result_schema(const Graph &graph, const std::string &resource) {
    if (resource.empty()) return {{"type", "object"}};
    Json result = {{"type", value(graph, resource, std::string(sf) + "type")}};
    const auto label = value(graph, resource, std::string(skos) + "prefLabel"); if (!label.empty()) result["title"] = label;
    const auto definition = value(graph, resource, std::string(skos) + "definition"); if (!definition.empty()) result["description"] = definition;
    const auto item = value(graph, resource, std::string(sf) + "items"); if (!item.empty()) result["items"] = result_schema(graph, item);
    Json properties = Json::object();
    for (const auto &property : values(graph, resource, std::string(sf) + "hasProperty")) {
        const auto name = value(graph, property, std::string(sf) + "propertyName").empty() ? value(graph, property, std::string(sf) + "columnName") : value(graph, property, std::string(sf) + "propertyName");
        if (!name.empty()) properties[name] = value(graph, resource, std::string(sf) + "type") == "table" ? Json{{"type", "array"}, {"items", column_schema(graph, property)}} : column_schema(graph, property);
    }
    if (!properties.empty()) result["properties"] = properties;
    return result;
}
std::string module_id(const Graph &graph, const std::string &resource, const std::string &, const std::string &domain, const std::string &) {
    const auto declared = value(graph, resource, std::string(sf) + "providedByModule");
    if (!declared.empty()) {
        const auto name = local(declared);
        if (name == "baseModule") return domain + ".base";
        if (name.ends_with("Module")) return domain + "." + name.substr(0, name.size() - 6);
        return name;
    }
    return domain + ".base";
}

Json project(const Graph &graph, const std::string &domain) {
    Json output = {{"version", 2}, {"entries", Json::array()}, {"tables", Json::array()}};
    for (const auto &[subject, predicates] : graph) {
        const auto operation = value(graph, subject, std::string(sf) + "operationId");
        const auto method = value(graph, subject, std::string(sf) + "methodId");
        if (operation.empty() && method.empty()) continue;
        const auto canonical = method.empty() ? operation : method;
        const auto kind = method.empty() ? "operation" : "method";
        const auto resource_domain = local(value(graph, subject, std::string(sf) + "availableInDomain"));
        if (!domain.empty() && resource_domain != domain) continue;
        const auto effective_domain = resource_domain.empty() ? "streamfind" : resource_domain;
        Json entry = {{"kind", kind}, {"canonical_id", canonical}, {"domain", effective_domain},
                      {"module_id", module_id(graph, subject, kind, effective_domain, canonical)},
                      {"label", value(graph, subject, std::string(skos) + "prefLabel")},
                      {"definition", value(graph, subject, std::string(skos) + "definition")}, {"executable", true}, {"exposed", true}};
        const auto category = value(graph, subject, std::string(sf) + "category"); const auto invocation = value(graph, subject, std::string(sf) + "invocationModel");
        const auto guidance = value(graph, subject, std::string(sf) + "guidance");
        Json next = Json::array(); for (const auto &item : values(graph, subject, std::string(sf) + "nextOperation")) next.push_back(value(graph, item, std::string(sf) + "operationId").empty() ? item : value(graph, item, std::string(sf) + "operationId"));
        entry["interface"] = {{"category", category.empty() ? (kind == std::string("method") ? "workflow-method" : "domain-operation") : category}, {"invocation_model", invocation.empty() ? (kind == std::string("method") ? "workflow" : "stateless") : invocation}, {"requires_connection", boolean(graph, subject, std::string(sf) + "requiresConnection", kind == std::string("method"))}, {"guidance", guidance}, {"next_operations", next}};
        const auto domain_resource = value(graph, subject, std::string(sf) + "availableInDomain"); entry["interface_guidance"] = value(graph, domain_resource, std::string(sf) + "guidance");
        Json defaults = Json::object(); const auto defaults_raw = value(graph, subject, std::string(sf) + "defaults"); if (!defaults_raw.empty()) defaults = literal_json(defaults_raw);
        Json parameters = Json::array(); for (const auto &parameter : values(graph, subject, std::string(sf) + "hasParameter")) { Json item = {{"name", wire_name(resource_name(parameter))}, {"type", value(graph, parameter, std::string(sf) + "type")}, {"required", boolean(graph, parameter, std::string(sf) + "required")}, {"constraints", Json::object()}, {"items", nullptr}, {"extensions", Json::array()}, {"schema", parameter_schema(graph, parameter)}, {"example", nullptr}, {"description", value(graph, parameter, std::string(skos) + "definition")}, {"default", nullptr}}; const auto constraint = value(graph, parameter, std::string(sf) + "constraints"); if (!constraint.empty()) item["constraints"] = constraint; const auto item_ref = value(graph, parameter, std::string(sf) + "items"); if (!item_ref.empty()) item["items"] = resource_name(item_ref); const auto example = value(graph, parameter, std::string(sf) + "example"); if (!example.empty()) item["example"] = literal_json(example); if (defaults.is_object() && defaults.contains(item["name"])) { item["default"] = defaults[item["name"]]; item["schema"]["default"] = defaults[item["name"]]; } parameters.push_back(item); }
        entry["parameters"] = parameters; Json input = {{"type", "object"}, {"title", entry["label"]}, {"description", entry["definition"]}, {"properties", Json::object()}, {"required", Json::array()}}; for (const auto &parameter : parameters) { input["properties"][parameter["name"]] = mcp_schema(parameter["schema"]); if (parameter["required"]) input["required"].push_back(parameter["name"]); } if (std::string(kind) == "operation") entry["mcp"] = {{"name", canonical}, {"input_schema", input}};
        if (std::string(kind) == "method") entry.erase("mcp");
        Json reads = Json::array(); for (const auto &table : values(graph, subject, std::string(sf) + "reads")) reads.push_back(value(graph, table, std::string(sf) + "tableName")); Json writes = Json::array(); for (const auto &table : values(graph, subject, std::string(sf) + "writes")) writes.push_back(value(graph, table, std::string(sf) + "tableName")); entry["effects"] = {{"mutates_project", boolean(graph, subject, std::string(sf) + "mutatesProject")}, {"reads", reads}, {"writes", writes}};
        const auto result = value(graph, subject, std::string(sf) + "returns"); entry["result"] = {{"id", result}, {"schema", result_schema(graph, result)}};
        if (kind == std::string("method")) { entry["cacheable"] = boolean(graph, subject, std::string(sf) + "cacheable"); entry["single_occurrence"] = boolean(graph, subject, std::string(sf) + "singleOccurrence"); entry["required_methods"] = Json::array(); for (const auto &required : values(graph, subject, std::string(sf) + "requiredMethods")) { const auto id = value(graph, required, std::string(sf) + "methodId").empty() ? value(graph, required, std::string(sf) + "operationId") : value(graph, required, std::string(sf) + "methodId"); entry["required_methods"].push_back(id.empty() ? required : id); } }
        output["entries"].push_back(entry);
    }
    for (const auto &[subject, predicates] : graph) {
        const auto table_name = value(graph, subject, std::string(sf) + "tableName");
        if (table_name.empty()) continue;
        const auto domain_resource = value(graph, subject, std::string(sf) + "availableInDomain"); const auto table_domain = domain_resource.empty() ? "streamfind" : local(domain_resource);
        if (!domain.empty() && table_domain != domain) continue;
        Json columns = Json::array();
        for (const auto &column : values(graph, subject, std::string(sf) + "hasColumn")) {
            const auto column_name = value(graph, column, std::string(sf) + "columnName").empty() ? value(graph, column, std::string(sf) + "propertyName") : value(graph, column, std::string(sf) + "columnName");
            if (!column_name.empty()) columns.push_back({{"name", column_name}, {"type", value(graph, column, std::string(sf) + "type")} });
        }
        output["tables"].push_back({{"table_name", table_name}, {"domain", table_domain}, {"module_id", module_id(graph, subject, "table", table_domain, table_name)}, {"columns", columns}});
    }
    std::sort(output["entries"].begin(), output["entries"].end(), [](const Json &a, const Json &b) { return a["canonical_id"] < b["canonical_id"]; });
    std::sort(output["tables"].begin(), output["tables"].end(), [](const Json &a, const Json &b) { return a["table_name"] < b["table_name"]; });
    return output;
}

void write_catalogue(const Json &catalogue, const CatalogueBuildRequest &request) {
    std::filesystem::create_directories(request.output_database.parent_path());
    std::error_code remove_error;
    std::filesystem::remove(request.output_database, remove_error);
    if (remove_error) throw std::runtime_error("cannot replace catalogue database: " + remove_error.message());
    duckdb_database database; duckdb_connection connection; duckdb_result result;
    if (duckdb_open(request.output_database.string().c_str(), &database) != DuckDBSuccess || duckdb_connect(database, &connection) != DuckDBSuccess) throw std::runtime_error("cannot open catalogue database");
    const auto exec = [&](const std::string &sql) { if (duckdb_query(connection, sql.c_str(), &result) != DuckDBSuccess) { const std::string error = duckdb_result_error(&result); duckdb_destroy_result(&result); throw std::runtime_error(error); } duckdb_destroy_result(&result); };
    exec("CREATE TABLE catalogue_entries (canonical_id VARCHAR PRIMARY KEY, kind VARCHAR, domain VARCHAR, label VARCHAR, definition VARCHAR, category VARCHAR, invocation_model VARCHAR, requires_connection BOOLEAN, guidance VARCHAR, next_operations JSON, interface_guidance VARCHAR, executable BOOLEAN, exposed BOOLEAN, mcp_name VARCHAR, input_schema JSON, parameters JSON, result_schema JSON, reads_tables JSON, writes_tables JSON, cacheable BOOLEAN, single_occurrence BOOLEAN, mutates_project BOOLEAN, required_methods JSON, module_id VARCHAR)");
    exec("CREATE TABLE catalogue_tables (table_name VARCHAR PRIMARY KEY, domain VARCHAR, module_id VARCHAR, columns JSON)");
    exec("CREATE TABLE catalogue_metadata (key VARCHAR PRIMARY KEY, value VARCHAR NOT NULL)");
    for (const auto &entry : catalogue["entries"]) {
        const auto &iface = entry["interface"]; const auto &effects = entry["effects"];
        const auto mcp_name = entry["kind"] == "operation" ? (entry["mcp"]["name"] == "None" ? entry["canonical_id"].get<std::string>() : entry["mcp"]["name"].get<std::string>()) : "";
        const auto input_schema = entry.contains("mcp") ? sql_quote(entry["mcp"]["input_schema"].dump()) : "NULL";
        const auto sql = "INSERT INTO catalogue_entries VALUES (" + sql_quote(entry["canonical_id"]) + "," + sql_quote(entry["kind"]) + "," + sql_quote(entry["domain"]) + "," + sql_quote(entry["label"]) + "," + sql_quote(entry["definition"]) + "," + sql_quote(iface["category"]) + "," + sql_quote(iface["invocation_model"]) + "," + (iface["requires_connection"] ? "true" : "false") + "," + sql_quote(iface["guidance"]) + "," + sql_quote(iface["next_operations"].dump()) + "," + sql_quote(entry["interface_guidance"]) + ",true,true," + (mcp_name.empty() ? "NULL" : sql_quote(mcp_name)) + "," + input_schema + "," + sql_quote(entry["parameters"].dump()) + "," + sql_quote(json_text(entry["result"]["schema"])) + "," + sql_quote(effects["reads"].dump()) + "," + sql_quote(effects["writes"].dump()) + "," + ((entry.contains("cacheable") && entry["cacheable"].is_boolean()) ? (entry["cacheable"] ? "true" : "false") : "NULL") + "," + ((entry.contains("single_occurrence") && entry["single_occurrence"].is_boolean()) ? (entry["single_occurrence"] ? "true" : "false") : "NULL") + "," + (effects["mutates_project"] ? "true" : "false") + "," + sql_quote(entry.value("required_methods", Json::array()).dump()) + "," + sql_quote(entry["module_id"]) + ")";
        exec(sql);
    }
    for (const auto &table : catalogue["tables"]) {
        const auto sql = "INSERT INTO catalogue_tables VALUES (" +
            sql_quote(table["table_name"]) + "," +
            sql_quote(table["domain"]) + "," +
            sql_quote(table["module_id"]) + "," +
            sql_quote(table["columns"].dump()) + ")";
        exec(sql);
    }
    exec("INSERT INTO catalogue_metadata VALUES ('schema_version','2'),('catalogue_kind'," + sql_quote(request.catalogue_kind) + "),('domain_id'," + sql_quote(request.resources.domain_id) + "),('generator_version','native-cpp-sdk')");
    duckdb_disconnect(&connection); duckdb_close(&database);
}

}  // namespace streamfind::sdk::detail

namespace streamfind::sdk {
CatalogueBuildResult build_plugin_catalogue(const CatalogueBuildRequest &request) {
    CatalogueBuildResult result; result.validation = validate_semantics_with_jena(request.resources, jena_home());
    if (!result.validation.valid) { result.diagnostics = result.validation.diagnostics; return result; }
    try { const auto graph = detail::parse(request.resources); const auto catalogue = detail::project(graph, request.resources.domain_id); detail::write_catalogue(catalogue, request); if (!request.output_json.empty()) { std::ofstream file(request.output_json); file << catalogue.dump(2) << '\n'; } if (!request.output_matrix.empty()) { std::ofstream file(request.output_matrix); file << "{\"entries\":["; bool first = true; for (const auto &entry : catalogue["entries"]) { if (!first) file << ','; first = false; file << "{\"canonical_id\":" << nlohmann::json(entry["canonical_id"]).dump() << ",\"kind\":" << nlohmann::json(entry["kind"]).dump() << ",\"domain_id\":" << nlohmann::json(entry["domain"]).dump() << ",\"module_id\":" << nlohmann::json(entry["module_id"]).dump() << "}"; } file << "]}\n"; } result.success = true; } catch (const std::exception &error) { result.diagnostics = error.what(); } return result;
}
}  // namespace streamfind::sdk
