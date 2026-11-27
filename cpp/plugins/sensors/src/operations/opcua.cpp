#include "streamfind/sdk/plugin_project_access.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"
#include "../acquisition/observation.hpp"

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/nodeids.h>
#include <open62541/plugin/log_stdout.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace streamfind::sensors::operations::detail {

using Json = nlohmann::json;
using Cell = sdk::TableBatchBuilder::Cell;

struct ClientOptions {
    std::string endpoint;
    std::string username;
    std::string password;
    double timeout_seconds = 10.0;
};

struct ClientHolder {
    UA_Client *client = UA_Client_new();

    ClientHolder() = default;

    ~ClientHolder() {
        if (client != nullptr) {
            UA_Client_disconnect(client);
            UA_Client_delete(client);
        }
    }

    ClientHolder(const ClientHolder &) = delete;
    ClientHolder &operator=(const ClientHolder &) = delete;
};

std::string required_string(const Json &parameters, const char *name) {
    const auto value = parameters.find(name);
    if (value == parameters.end() || !value->is_string() || value->get<std::string>().empty())
        throw std::invalid_argument(std::string(name) + " must be a non-empty string");
    return value->get<std::string>();
}

std::string optional_string(const Json &parameters, const char *name) {
    const auto value = parameters.find(name);
    if (value == parameters.end() || value->is_null()) return {};
    if (!value->is_string())
        throw std::invalid_argument(std::string(name) + " must be a string when provided");
    return value->get<std::string>();
}

ClientOptions client_options(const Json &parameters) {
    ClientOptions options;
    options.endpoint = required_string(parameters, "endpoint");
    options.username = optional_string(parameters, "username");
    options.password = optional_string(parameters, "password");
    if (!options.username.empty() || !options.password.empty()) {
        if (options.username.empty())
            throw std::invalid_argument("username must be provided when password is set");
    }
    options.timeout_seconds = parameters.value("timeout_seconds", 10.0);
    if (!std::isfinite(options.timeout_seconds) || options.timeout_seconds <= 0.0 ||
        options.timeout_seconds > 300.0)
        throw std::invalid_argument("timeout_seconds must be finite and between 0 and 300");
    return options;
}

std::string ua_string(const UA_String &value) {
    if (value.data == nullptr || value.length == 0) return {};
    return {reinterpret_cast<const char *>(value.data), value.length};
}

std::string node_id_text(const UA_NodeId &node_id) {
    UA_String printed = UA_STRING_NULL;
    if (UA_NodeId_print(&node_id, &printed) != UA_STATUSCODE_GOOD) return {};
    const auto text = ua_string(printed);
    UA_String_clear(&printed);
    return text;
}

std::string expanded_node_id_text(const UA_ExpandedNodeId &node_id) {
    UA_String printed = UA_STRING_NULL;
    if (UA_ExpandedNodeId_print(&node_id, &printed) != UA_STATUSCODE_GOOD) return {};
    const auto text = ua_string(printed);
    UA_String_clear(&printed);
    return text;
}

std::string date_time_text(UA_DateTime value) {
    const auto date_time = UA_DateTime_toStruct(value);
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << date_time.year << '-'
           << std::setw(2) << date_time.month << '-' << std::setw(2) << date_time.day
           << 'T' << std::setw(2) << date_time.hour << ':' << std::setw(2)
           << date_time.min << ':' << std::setw(2) << date_time.sec << '.'
           << std::setw(6) << (static_cast<unsigned>(date_time.milliSec) * 1000u +
                               static_cast<unsigned>(date_time.microSec)) << 'Z';
    return output.str();
}

Json scalar_value(const UA_Variant &value) {
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_BOOLEAN]))
        return *static_cast<const UA_Boolean *>(value.data) != 0;
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_SBYTE]))
        return *static_cast<const UA_SByte *>(value.data);
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_BYTE]))
        return *static_cast<const UA_Byte *>(value.data);
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_INT16]))
        return *static_cast<const UA_Int16 *>(value.data);
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_UINT16]))
        return *static_cast<const UA_UInt16 *>(value.data);
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_INT32]))
        return *static_cast<const UA_Int32 *>(value.data);
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_UINT32]))
        return *static_cast<const UA_UInt32 *>(value.data);
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_INT64]))
        return *static_cast<const UA_Int64 *>(value.data);
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_UINT64]))
        return *static_cast<const UA_UInt64 *>(value.data);
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_FLOAT]))
        return static_cast<double>(*static_cast<const UA_Float *>(value.data));
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_DOUBLE]))
        return *static_cast<const UA_Double *>(value.data);
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_STRING]))
        return ua_string(*static_cast<const UA_String *>(value.data));
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_DATETIME]))
        return date_time_text(*static_cast<const UA_DateTime *>(value.data));

    UA_String printed = UA_STRING_NULL;
    if (UA_print(value.data, value.type, &printed) == UA_STATUSCODE_GOOD) {
        const auto text = ua_string(printed);
        UA_String_clear(&printed);
        return text;
    }
    return nullptr;
}

Json variant_value(const UA_Variant &value) {
    if (UA_Variant_isEmpty(&value)) return nullptr;
    if (UA_Variant_isScalar(&value)) return scalar_value(value);
    if (UA_Variant_hasArrayType(&value, &UA_TYPES[UA_TYPES_STRING])) {
        Json values = Json::array();
        const auto *items = static_cast<const UA_String *>(value.data);
        for (std::size_t index = 0; index < value.arrayLength; ++index)
            values.push_back(ua_string(items[index]));
        return values;
    }

    UA_String printed = UA_STRING_NULL;
    if (UA_print(value.data, value.type, &printed) == UA_STATUSCODE_GOOD) {
        const auto text = ua_string(printed);
        UA_String_clear(&printed);
        return text;
    }
    return nullptr;
}

std::string node_class_name(UA_NodeClass node_class) {
    switch (node_class) {
    case UA_NODECLASS_OBJECT: return "Object";
    case UA_NODECLASS_VARIABLE: return "Variable";
    case UA_NODECLASS_METHOD: return "Method";
    case UA_NODECLASS_OBJECTTYPE: return "ObjectType";
    case UA_NODECLASS_VARIABLETYPE: return "VariableType";
    case UA_NODECLASS_REFERENCETYPE: return "ReferenceType";
    case UA_NODECLASS_DATATYPE: return "DataType";
    case UA_NODECLASS_VIEW: return "View";
    default: return "Unspecified";
    }
}

Json status_json(UA_StatusCode status) {
    const char *name = UA_StatusCode_name(status);
    std::ostringstream code;
    code << "0x" << std::hex << std::uppercase << status;
    return Json{{"status_code", code.str()},
                {"status_name", name == nullptr ? "UnknownStatus" : name}};
}

UA_StatusCode configure_client(UA_Client *client, double timeout_seconds) {
    if (client == nullptr) return UA_STATUSCODE_BADOUTOFMEMORY;
    auto *config = UA_Client_getConfig(client);
    if (config == nullptr) return UA_STATUSCODE_BADINTERNALERROR;
    const auto status = UA_ClientConfig_setDefault(config);
    if (UA_StatusCode_isBad(status)) return status;
    config->timeout = static_cast<UA_UInt32>(timeout_seconds * 1000.0);
    config->logging = UA_Log_Stdout_new(UA_LOGLEVEL_FATAL);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode connect_client(UA_Client *client, const ClientOptions &options) {
    if (!options.username.empty())
        return UA_Client_connectUsername(client, options.endpoint.c_str(),
                                         options.username.c_str(), options.password.c_str());
    return UA_Client_connect(client, options.endpoint.c_str());
}

Json browse_node(UA_Client *client, const UA_NodeId &node_id,
                 std::size_t depth, std::size_t max_depth, std::size_t max_children) {
    UA_QualifiedName browse_name;
    UA_LocalizedText display_name;
    UA_NodeClass node_class = UA_NODECLASS_UNSPECIFIED;
    UA_QualifiedName_init(&browse_name);
    UA_LocalizedText_init(&display_name);
    (void)UA_Client_readBrowseNameAttribute(client, node_id, &browse_name);
    (void)UA_Client_readDisplayNameAttribute(client, node_id, &display_name);
    (void)UA_Client_readNodeClassAttribute(client, node_id, &node_class);

    Json result{{"node_id", node_id_text(node_id)},
                {"browse_name", ua_string(browse_name.name)},
                {"display_name", ua_string(display_name.text)},
                {"node_class", static_cast<unsigned>(node_class)},
                {"node_class_name", node_class_name(node_class)},
                {"children", Json::array()}};

    if (depth >= max_depth) {
        UA_QualifiedName_clear(&browse_name);
        UA_LocalizedText_clear(&display_name);
        return result;
    }

    UA_BrowseRequest request;
    UA_BrowseRequest_init(&request);
    UA_BrowseDescription description;
    UA_BrowseDescription_init(&description);
    UA_NodeId_copy(&node_id, &description.nodeId);
    description.browseDirection = UA_BROWSEDIRECTION_FORWARD;
    description.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HIERARCHICALREFERENCES);
    description.includeSubtypes = true;
    description.resultMask = UA_BROWSERESULTMASK_ALL;
    request.requestedMaxReferencesPerNode = static_cast<UA_UInt32>(max_children);
    request.nodesToBrowseSize = 1;
    request.nodesToBrowse = &description;
    auto response = UA_Client_Service_browse(client, request);
    if (response.resultsSize > 0) {
        const auto &browse_result = response.results[0];
        const auto limit = std::min<std::size_t>(browse_result.referencesSize, max_children);
        for (std::size_t index = 0; index < limit; ++index) {
            const auto &reference = browse_result.references[index];
            Json child{{"node_id", expanded_node_id_text(reference.nodeId)},
                       {"browse_name", ua_string(reference.browseName.name)},
                       {"display_name", ua_string(reference.displayName.text)},
                       {"node_class", static_cast<unsigned>(reference.nodeClass)},
                       {"node_class_name", node_class_name(reference.nodeClass)},
                       {"children", Json::array()}};
            if (UA_ExpandedNodeId_isLocal(&reference.nodeId))
                child = browse_node(client, reference.nodeId.nodeId, depth + 1,
                                    max_depth, max_children);
            result["children"].push_back(std::move(child));
        }
        if (browse_result.continuationPoint.length > 0)
            result["continuation_available"] = true;
    }
    UA_BrowseDescription_clear(&description);
    UA_BrowseResponse_clear(&response);
    UA_QualifiedName_clear(&browse_name);
    UA_LocalizedText_clear(&display_name);
    return result;
}

Json reference_summary(const UA_ReferenceDescription &reference) {
    return Json{{"node_id", expanded_node_id_text(reference.nodeId)},
                {"browse_name", ua_string(reference.browseName.name)},
                {"display_name", ua_string(reference.displayName.text)},
                {"node_class", static_cast<unsigned>(reference.nodeClass)},
                {"node_class_name", node_class_name(reference.nodeClass)}};
}

Json browse_references(UA_Client *client, const UA_NodeId &node_id,
                       const UA_NodeId &reference_type, std::size_t max_children,
                       bool read_values) {
    UA_BrowseRequest request;
    UA_BrowseRequest_init(&request);
    UA_BrowseDescription description;
    UA_BrowseDescription_init(&description);
    UA_NodeId_copy(&node_id, &description.nodeId);
    UA_NodeId_copy(&reference_type, &description.referenceTypeId);
    description.browseDirection = UA_BROWSEDIRECTION_FORWARD;
    description.includeSubtypes = false;
    description.resultMask = UA_BROWSERESULTMASK_ALL;
    request.requestedMaxReferencesPerNode = static_cast<UA_UInt32>(max_children);
    request.nodesToBrowseSize = 1;
    request.nodesToBrowse = &description;
    auto response = UA_Client_Service_browse(client, request);
    Json result = Json::array();
    if (response.resultsSize > 0) {
        const auto &browse_result = response.results[0];
        const auto limit = std::min<std::size_t>(browse_result.referencesSize, max_children);
        for (std::size_t index = 0; index < limit; ++index) {
            const auto &reference = browse_result.references[index];
            auto item = reference_summary(reference);
            if (read_values && UA_ExpandedNodeId_isLocal(&reference.nodeId)) {
                UA_Variant value;
                UA_Variant_init(&value);
                const auto status = UA_Client_readValueAttribute(client, reference.nodeId.nodeId, &value);
                item["value_status"] = status_json(status);
                if (UA_StatusCode_isGood(status)) item["value"] = variant_value(value);
                UA_Variant_clear(&value);
            }
            result.push_back(std::move(item));
        }
    }
    UA_BrowseDescription_clear(&description);
    UA_BrowseResponse_clear(&response);
    return result;
}

Json value_snapshot(UA_Client *client, const UA_NodeId &node_id, UA_NodeClass node_class) {
    if (node_class != UA_NODECLASS_VARIABLE)
        return Json{{"available", false},
                    {"reason", "Value attribute is only applicable to Variable nodes."}};
    UA_ReadValueId read_value;
    UA_ReadValueId_init(&read_value);
    UA_NodeId_copy(&node_id, &read_value.nodeId);
    read_value.attributeId = UA_ATTRIBUTEID_VALUE;
    const auto read_at = UA_DateTime_now();
    auto data_value = UA_Client_read(client, &read_value);
    UA_ReadValueId_clear(&read_value);

    const auto read_status = data_value.hasStatus ? data_value.status : UA_STATUSCODE_GOOD;
    Json snapshot{{"available", true},
                  {"read_at", date_time_text(read_at)},
                  {"value_status", status_json(read_status)},
                  {"value", data_value.hasValue ? variant_value(data_value.value) : Json(nullptr)},
                  {"source_timestamp", data_value.hasSourceTimestamp
                      ? Json(date_time_text(data_value.sourceTimestamp)) : Json(nullptr)},
                  {"server_timestamp", data_value.hasServerTimestamp
                      ? Json(date_time_text(data_value.serverTimestamp)) : Json(nullptr)}};
    UA_DataValue_clear(&data_value);
    return snapshot;
}

Json inspect_node(UA_Client *client, const UA_NodeId &node_id) {
    UA_QualifiedName browse_name;
    UA_LocalizedText display_name;
    UA_LocalizedText description;
    UA_NodeClass node_class = UA_NODECLASS_UNSPECIFIED;
    UA_NodeId data_type;
    UA_Int32 value_rank = -1;
    UA_Byte access_level = 0;
    UA_Byte user_access_level = 0;
    UA_Double minimum_sampling_interval = 0.0;
    UA_Boolean historizing = false;
    UA_QualifiedName_init(&browse_name);
    UA_LocalizedText_init(&display_name);
    UA_LocalizedText_init(&description);
    UA_NodeId_init(&data_type);
    Json statuses = Json::object();
    const auto record_status = [&](const char *name, UA_StatusCode status) {
        statuses[name] = status_json(status);
    };
    record_status("browse_name", UA_Client_readBrowseNameAttribute(client, node_id, &browse_name));
    record_status("display_name", UA_Client_readDisplayNameAttribute(client, node_id, &display_name));
    record_status("description", UA_Client_readDescriptionAttribute(client, node_id, &description));
    record_status("node_class", UA_Client_readNodeClassAttribute(client, node_id, &node_class));
    record_status("data_type", UA_Client_readDataTypeAttribute(client, node_id, &data_type));
    record_status("value_rank", UA_Client_readValueRankAttribute(client, node_id, &value_rank));
    record_status("access_level", UA_Client_readAccessLevelAttribute(client, node_id, &access_level));
    record_status("user_access_level", UA_Client_readUserAccessLevelAttribute(client, node_id, &user_access_level));
    record_status("minimum_sampling_interval", UA_Client_readMinimumSamplingIntervalAttribute(
        client, node_id, &minimum_sampling_interval));
    record_status("historizing", UA_Client_readHistorizingAttribute(client, node_id, &historizing));

    Json result{{"node_id", node_id_text(node_id)},
                {"browse_name", ua_string(browse_name.name)},
                {"display_name", ua_string(display_name.text)},
                {"description", ua_string(description.text)},
                {"node_class", static_cast<unsigned>(node_class)},
                {"node_class_name", node_class_name(node_class)},
                {"data_type", node_id_text(data_type)},
                {"value_rank", value_rank},
                {"access_level", access_level},
                {"user_access_level", user_access_level},
                {"minimum_sampling_interval_ms", minimum_sampling_interval},
                {"historizing", historizing != 0},
                {"value_snapshot", value_snapshot(client, node_id, node_class)},
                {"attribute_status", std::move(statuses)},
                {"type_definitions", browse_references(
                    client, node_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASTYPEDEFINITION), 8, false)},
                {"properties", browse_references(
                    client, node_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY), 64, true)}};
    UA_QualifiedName_clear(&browse_name);
    UA_LocalizedText_clear(&display_name);
    UA_LocalizedText_clear(&description);
    UA_NodeId_clear(&data_type);
    return result;
}

Json namespace_array(UA_Client *client) {
    UA_Variant value;
    UA_Variant_init(&value);
    const auto status = UA_Client_readValueAttribute(
        client, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_NAMESPACEARRAY), &value);
    if (UA_StatusCode_isBad(status) ||
        !UA_Variant_hasArrayType(&value, &UA_TYPES[UA_TYPES_STRING])) {
        UA_Variant_clear(&value);
        return Json::array();
    }
    Json result = Json::array();
    const auto *items = static_cast<const UA_String *>(value.data);
    for (std::size_t index = 0; index < value.arrayLength; ++index)
        result.push_back(Json{{"index", index}, {"uri", ua_string(items[index])}});
    UA_Variant_clear(&value);
    return result;
}

Json explore_opcua_server(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto options = client_options(parameters);
    const auto max_depth = parameters.value("max_depth", 2u);
    const auto max_children = parameters.value("max_children", 64u);
    if (max_depth > 8u || max_children == 0u || max_children > 512u)
        throw std::invalid_argument("max_depth must be 0..8 and max_children must be 1..512");

    ClientHolder holder;
    auto status = configure_client(holder.client, options.timeout_seconds);
    if (UA_StatusCode_isBad(status))
        throw std::runtime_error("failed to configure OPC UA client: " + status_json(status).dump());
    std::size_t endpoint_count = 0;
    UA_EndpointDescription *endpoints = nullptr;
    status = UA_Client_getEndpoints(holder.client, options.endpoint.c_str(),
                                    &endpoint_count, &endpoints);
    Json endpoint_urls = Json::array();
    if (UA_StatusCode_isGood(status)) {
        for (std::size_t index = 0; index < endpoint_count; ++index)
            endpoint_urls.push_back(ua_string(endpoints[index].endpointUrl));
    }
    if (endpoints != nullptr)
        UA_Array_delete(endpoints, endpoint_count, &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
    if (UA_StatusCode_isBad(status))
        throw std::runtime_error("OPC UA endpoint discovery failed: " + status_json(status).dump());
    status = connect_client(holder.client, options);
    if (UA_StatusCode_isBad(status))
        throw std::runtime_error("OPC UA session creation failed: " + status_json(status).dump());

    access.report_progress(0.5, "Browsing OPC UA server");
    auto result = Json{{"endpoint", options.endpoint},
                       {"endpoint_reachable", true},
                       {"session_created", true},
                       {"detail_operation", "sensors.inspect_opcua_node"},
                       {"available_endpoints", std::move(endpoint_urls)},
                       {"namespaces", namespace_array(holder.client)},
                       {"node_tree", browse_node(holder.client,
                                                  UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                                                  0, max_depth, max_children)}};
    access.report_progress(1.0, "OPC UA server exploration complete");
    return result;
}

Json inspect_opcua_node(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto options = client_options(parameters);
    const auto node_text = required_string(parameters, "node_id");
    UA_NodeId node_id;
    UA_NodeId_init(&node_id);
    const auto parse_status = UA_NodeId_parse(
        &node_id, UA_STRING(const_cast<char *>(node_text.c_str())));
    if (UA_StatusCode_isBad(parse_status)) {
        UA_NodeId_clear(&node_id);
        throw std::invalid_argument("invalid OPC UA node id: " + node_text);
    }

    ClientHolder holder;
    auto status = configure_client(holder.client, options.timeout_seconds);
    if (UA_StatusCode_isBad(status)) {
        UA_NodeId_clear(&node_id);
        throw std::runtime_error("failed to configure OPC UA client: " + status_json(status).dump());
    }
    status = connect_client(holder.client, options);
    if (UA_StatusCode_isBad(status)) {
        UA_NodeId_clear(&node_id);
        throw std::runtime_error("OPC UA session creation failed: " + status_json(status).dump());
    }
    access.report_progress(0.5, "Reading OPC UA node definition");
    auto node = inspect_node(holder.client, node_id);
    UA_NodeId_clear(&node_id);
    access.report_progress(1.0, "OPC UA node definition read");
    return Json{{"endpoint", options.endpoint}, {"session_created", true}, {"node", std::move(node)}};
}

Json collect_opcua_data(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto options = client_options(parameters);
    const auto nodes = parameters.find("nodes");
    if (nodes == parameters.end() || !nodes->is_object() || nodes->empty())
        throw std::invalid_argument("nodes must be a non-empty object mapping names to OPC UA node ids");

    ClientHolder holder;
    auto status = configure_client(holder.client, options.timeout_seconds);
    if (UA_StatusCode_isBad(status))
        throw std::runtime_error("failed to configure OPC UA client: " + status_json(status).dump());
    status = connect_client(holder.client, options);
    if (UA_StatusCode_isBad(status))
        throw std::runtime_error("OPC UA session creation failed: " + status_json(status).dump());

    sdk::TableBatchBuilder output({
        {"endpoint", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"node_name", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"node_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"node_class", STREAMFIND_PLUGIN_COLUMN_INT64},
        {"node_class_name", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"value_available", STREAMFIND_PLUGIN_COLUMN_BOOL},
        {"value", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"value_json", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"status_code", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"status_name", STREAMFIND_PLUGIN_COLUMN_UTF8}});
    std::size_t completed = 0;
    for (const auto &[name, node_text] : nodes->items()) {
        if (!node_text.is_string() || node_text.get<std::string>().empty())
            throw std::invalid_argument("nodes must map names to non-empty node-id strings");
        UA_NodeId node_id;
        UA_NodeId_init(&node_id);
        const auto node_string = node_text.get<std::string>();
        const auto parse_status = UA_NodeId_parse(
            &node_id, UA_STRING(const_cast<char *>(node_string.c_str())));
        if (UA_StatusCode_isBad(parse_status)) {
            UA_NodeId_clear(&node_id);
            throw std::invalid_argument("invalid OPC UA node id: " + node_string);
        }
        UA_NodeClass node_class = UA_NODECLASS_UNSPECIFIED;
        const auto node_class_status = UA_Client_readNodeClassAttribute(
            holder.client, node_id, &node_class);
        UA_Variant raw_value;
        UA_Variant_init(&raw_value);
        const bool is_variable = UA_StatusCode_isGood(node_class_status) &&
            node_class == UA_NODECLASS_VARIABLE;
        const auto read_status = is_variable
            ? UA_Client_readValueAttribute(holder.client, node_id, &raw_value)
            : UA_STATUSCODE_GOOD;
        std::string value;
        std::string value_json;
        std::string status_code;
        std::string status_name;
        const bool value_available = is_variable && UA_StatusCode_isGood(read_status);
        if (value_available) {
            const auto converted = variant_value(raw_value);
            value = converted.dump();
            value_json = value;
            const auto status = status_json(read_status);
            status_code = status["status_code"].get<std::string>();
            status_name = status["status_name"].get<std::string>();
        } else if (!UA_StatusCode_isGood(node_class_status)) {
            const auto status_info = status_json(node_class_status);
            status_code = status_info["status_code"].get<std::string>();
            status_name = status_info["status_name"].get<std::string>();
        } else if (!is_variable) {
            status_name = "NotApplicable";
        } else {
            const auto status_info = status_json(read_status);
            status_code = status_info["status_code"].get<std::string>();
            status_name = status_info["status_name"].get<std::string>();
        }
        output.append_row({options.endpoint, name, node_string,
                           static_cast<std::int64_t>(node_class), node_class_name(node_class),
                           value_available, value, value_json, status_code, status_name});
        UA_Variant_clear(&raw_value);
        UA_NodeId_clear(&node_id);
        ++completed;
        access.report_progress(static_cast<double>(completed) / static_cast<double>(nodes->size()),
                               "Classified OPC UA node " + name);
    }
    output.emit(access, "opcuaDataTable");
    return Json{{"endpoint", options.endpoint}, {"nodes_requested", nodes->size()},
                {"rows_emitted", output.row_count()}};
}

double positive_real_parameter(const Json &parameters, const char *name,
                               double default_value, double maximum) {
    const auto found = parameters.find(name);
    if (found == parameters.end()) return default_value;
    if (!found->is_number()) throw std::invalid_argument(std::string(name) + " must be numeric");
    const auto value = found->get<double>();
    if (!std::isfinite(value) || value <= 0.0 || value > maximum)
        throw std::invalid_argument(std::string(name) + " must be finite, positive, and within its limit");
    return value;
}

std::size_t positive_integer_parameter(const Json &parameters, const char *name,
                                       std::size_t default_value, std::size_t maximum) {
    const auto found = parameters.find(name);
    if (found == parameters.end()) return default_value;
    if (!found->is_number_unsigned() && !found->is_number_integer())
        throw std::invalid_argument(std::string(name) + " must be an integer");
    const auto value = found->get<std::int64_t>();
    if (value <= 0 || static_cast<std::uint64_t>(value) > maximum)
        throw std::invalid_argument(std::string(name) + " must be positive and within its limit");
    return static_cast<std::size_t>(value);
}

acquisition::CaptureRequest capture_request(const Json &parameters) {
    const auto duration_seconds = positive_real_parameter(parameters, "capture_duration", 10.0, 3600.0);
    const auto interval_ms = positive_real_parameter(parameters, "capture_interval", 1000.0, 600000.0);
    acquisition::CaptureRequest request;
    request.duration = std::chrono::milliseconds(static_cast<std::int64_t>(duration_seconds * 1000.0));
    request.sample_interval = std::chrono::milliseconds(static_cast<std::int64_t>(interval_ms));
    request.max_samples = positive_integer_parameter(parameters, "max_samples", 1000, 1000000);
    request.max_buffer_bytes = positive_integer_parameter(
        parameters, "max_buffer_bytes", 64u * 1024u * 1024u, 512u * 1024u * 1024u);
    if (request.duration.count() == 0 || request.sample_interval.count() == 0)
        throw std::invalid_argument("capture duration and sample interval must be at least one millisecond");
    return request;
}

acquisition::SensorValue sensor_value(const UA_Variant &value) {
    acquisition::SensorValue result;
    if (UA_Variant_isEmpty(&value)) return result;
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_BOOLEAN])) {
        result.kind = acquisition::SensorValueKind::Boolean;
        result.boolean = *static_cast<const UA_Boolean *>(value.data) != 0;
        return result;
    }
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_SBYTE])) {
        result.kind = acquisition::SensorValueKind::Integer;
        result.integer = *static_cast<const UA_SByte *>(value.data);
        return result;
    }
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_BYTE])) {
        result.kind = acquisition::SensorValueKind::Integer;
        result.integer = *static_cast<const UA_Byte *>(value.data);
        return result;
    }
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_INT16])) {
        result.kind = acquisition::SensorValueKind::Integer;
        result.integer = *static_cast<const UA_Int16 *>(value.data);
        return result;
    }
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_UINT16])) {
        result.kind = acquisition::SensorValueKind::Integer;
        result.integer = *static_cast<const UA_UInt16 *>(value.data);
        return result;
    }
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_INT32])) {
        result.kind = acquisition::SensorValueKind::Integer;
        result.integer = *static_cast<const UA_Int32 *>(value.data);
        return result;
    }
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_UINT32])) {
        result.kind = acquisition::SensorValueKind::Integer;
        result.integer = *static_cast<const UA_UInt32 *>(value.data);
        return result;
    }
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_INT64])) {
        result.kind = acquisition::SensorValueKind::Integer;
        result.integer = *static_cast<const UA_Int64 *>(value.data);
        return result;
    }
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_UINT64])) {
        const auto converted = *static_cast<const UA_UInt64 *>(value.data);
        if (converted <= static_cast<UA_UInt64>(std::numeric_limits<std::int64_t>::max())) {
            result.kind = acquisition::SensorValueKind::Integer;
            result.integer = static_cast<std::int64_t>(converted);
            return result;
        }
    }
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_FLOAT]) ||
        UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_DOUBLE])) {
        result.kind = acquisition::SensorValueKind::Real;
        result.real = UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_FLOAT])
            ? static_cast<double>(*static_cast<const UA_Float *>(value.data))
            : *static_cast<const UA_Double *>(value.data);
        return result;
    }
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_STRING])) {
        result.kind = acquisition::SensorValueKind::Text;
        result.text = ua_string(*static_cast<const UA_String *>(value.data));
        return result;
    }
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_DATETIME])) {
        result.kind = acquisition::SensorValueKind::Text;
        result.text = date_time_text(*static_cast<const UA_DateTime *>(value.data));
        return result;
    }
    result.kind = acquisition::SensorValueKind::Json;
    result.text = variant_value(value).dump();
    return result;
}

sdk::TableBatchBuilder make_capture_batch() {
    return sdk::TableBatchBuilder({
        {"source_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"signal_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"event_time", STREAMFIND_PLUGIN_COLUMN_TIMESTAMP},
        {"received_at", STREAMFIND_PLUGIN_COLUMN_TIMESTAMP},
        {"time_origin", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"value_type", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"value_double", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"value_int", STREAMFIND_PLUGIN_COLUMN_INT64},
        {"value_bool", STREAMFIND_PLUGIN_COLUMN_BOOL},
        {"value_text", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"value_json", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"unit", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"quality", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"status_code", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"capture_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"event_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
    });
}

std::size_t observation_size(const acquisition::SensorObservation &observation) {
    std::size_t size = observation.source_id.size() + observation.signal_id.size() +
        observation.event_time.size() + observation.received_at.size() +
        observation.capture_id.size() + observation.event_id.size() + 128;
    if (observation.value.text.has_value()) size += observation.value.text->size();
    if (observation.unit.has_value()) size += observation.unit->size();
    if (observation.status_code.has_value()) size += observation.status_code->size();
    return size;
}

Json capture_opcua_timeseries(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto options = client_options(parameters);
    const auto nodes = parameters.find("nodes");
    if (nodes == parameters.end() || !nodes->is_object() || nodes->empty())
        throw std::invalid_argument("nodes must be a non-empty object mapping signal names to OPC UA node ids");
    const auto request = capture_request(parameters);
    const auto capture_id = optional_string(parameters, "capture_id").empty()
        ? std::string(access.operation_instance()) : optional_string(parameters, "capture_id");

    ClientHolder holder;
    auto status = configure_client(holder.client, options.timeout_seconds);
    if (UA_StatusCode_isBad(status))
        throw std::runtime_error("failed to configure OPC UA client: " + status_json(status).dump());
    status = connect_client(holder.client, options);
    if (UA_StatusCode_isBad(status))
        throw std::runtime_error("OPC UA session creation failed: " + status_json(status).dump());

    struct RequestedNode {
        std::string signal_id;
        std::string node_text;
        UA_NodeId node_id;
        UA_NodeClass node_class = UA_NODECLASS_UNSPECIFIED;
        UA_StatusCode node_class_status = UA_STATUSCODE_GOOD;
        RequestedNode() { UA_NodeId_init(&node_id); }
        ~RequestedNode() { UA_NodeId_clear(&node_id); }
        RequestedNode(const RequestedNode &) = delete;
        RequestedNode &operator=(const RequestedNode &) = delete;
    };
    std::vector<std::unique_ptr<RequestedNode>> requested;
    requested.reserve(nodes->size());
    for (const auto &[signal_id, node_value] : nodes->items()) {
        if (!node_value.is_string() || node_value.get<std::string>().empty())
            throw std::invalid_argument("nodes must map signal names to non-empty node-id strings");
        auto node = std::make_unique<RequestedNode>();
        node->signal_id = signal_id;
        node->node_text = node_value.get<std::string>();
        const auto parse_status = UA_NodeId_parse(
            &node->node_id, UA_STRING(const_cast<char *>(node->node_text.c_str())));
        if (UA_StatusCode_isBad(parse_status))
            throw std::invalid_argument("invalid OPC UA node id: " + node->node_text);
        node->node_class_status = UA_Client_readNodeClassAttribute(
            holder.client, node->node_id, &node->node_class);
        requested.push_back(std::move(node));
    }

    auto output = make_capture_batch();
    std::size_t buffered_bytes = 0;
    std::uint64_t rows_emitted = 0;
    std::uint64_t sample_count = 0;
    const auto start = std::chrono::steady_clock::now();
    auto next_sample = start;
    auto flush = [&]() {
        if (output.row_count() == 0) return;
        rows_emitted += output.row_count();
        output.emit(access, "sensorTimeseriesTable");
        output = make_capture_batch();
        buffered_bytes = 0;
    };

    while (sample_count < request.max_samples &&
           std::chrono::steady_clock::now() - start <= request.duration) {
        if (access.is_cancelled()) throw std::runtime_error("OPC UA time-series capture cancelled");
        const auto received_at = date_time_text(UA_DateTime_now());
        for (const auto &node : requested) {
            if (sample_count >= request.max_samples) break;
            acquisition::SensorObservation observation;
            observation.source_id = options.endpoint;
            observation.signal_id = node->signal_id;
            observation.received_at = received_at;
            observation.capture_id = capture_id;
            observation.event_id = capture_id + ":" + node->signal_id + ":" +
                std::to_string(sample_count);
            if (UA_StatusCode_isBad(node->node_class_status) ||
                node->node_class != UA_NODECLASS_VARIABLE) {
                observation.event_time = received_at;
                observation.time_origin = "receiver";
                observation.quality = "missing";
                observation.status_code = UA_StatusCode_isBad(node->node_class_status)
                    ? std::optional<std::string>(status_json(node->node_class_status).at("status_code").get<std::string>())
                    : std::optional<std::string>("NotApplicable");
            } else {
                UA_ReadValueId read_value;
                UA_ReadValueId_init(&read_value);
                UA_NodeId_copy(&node->node_id, &read_value.nodeId);
                read_value.attributeId = UA_ATTRIBUTEID_VALUE;
                const auto read_at = UA_DateTime_now();
                auto data_value = UA_Client_read(holder.client, &read_value);
                UA_ReadValueId_clear(&read_value);
                const auto read_status = data_value.hasStatus ? data_value.status : UA_STATUSCODE_GOOD;
                observation.quality = UA_StatusCode_isGood(read_status) && data_value.hasValue
                    ? "good" : UA_StatusCode_isUncertain(read_status) ? "uncertain" : "bad";
                observation.status_code = status_json(read_status).at("status_code").get<std::string>();
                observation.event_time = data_value.hasSourceTimestamp
                    ? date_time_text(data_value.sourceTimestamp)
                    : data_value.hasServerTimestamp ? date_time_text(data_value.serverTimestamp)
                                                    : date_time_text(read_at);
                observation.time_origin = data_value.hasSourceTimestamp
                    ? "source" : data_value.hasServerTimestamp ? "server" : "receiver";
                if (data_value.hasValue && UA_StatusCode_isGood(read_status))
                    observation.value = sensor_value(data_value.value);
                UA_DataValue_clear(&data_value);
            }
            const auto row_bytes = observation_size(observation);
            if (row_bytes > request.max_buffer_bytes)
                throw std::runtime_error("single OPC UA observation exceeds max_buffer_bytes");
            if (buffered_bytes != 0 && buffered_bytes + row_bytes > request.max_buffer_bytes)
                flush();
            const auto &value = observation.value;
            output.append_row({observation.source_id, observation.signal_id, observation.event_time,
                               observation.received_at, observation.time_origin,
                               value.kind == acquisition::SensorValueKind::Real ? "real" :
                               value.kind == acquisition::SensorValueKind::Integer ? "integer" :
                               value.kind == acquisition::SensorValueKind::Boolean ? "boolean" :
                               value.kind == acquisition::SensorValueKind::Text ? "string" :
                               value.kind == acquisition::SensorValueKind::Json ? "json" : "missing",
                               value.real.has_value() ? sdk::TableBatchBuilder::Cell(*value.real) : Cell(std::monostate{}),
                               value.integer.has_value() ? sdk::TableBatchBuilder::Cell(*value.integer) : Cell(std::monostate{}),
                               value.boolean.has_value() ? sdk::TableBatchBuilder::Cell(*value.boolean) : Cell(std::monostate{}),
                               value.kind == acquisition::SensorValueKind::Text && value.text.has_value() ? Cell(*value.text) : Cell(std::monostate{}),
                               value.kind == acquisition::SensorValueKind::Json && value.text.has_value() ? Cell(*value.text) : Cell(std::monostate{}),
                               observation.unit.has_value() ? Cell(*observation.unit) : Cell(std::monostate{}),
                               observation.quality,
                               observation.status_code.has_value() ? Cell(*observation.status_code) : Cell(std::monostate{}),
                               observation.capture_id, observation.event_id});
            buffered_bytes += row_bytes;
            ++sample_count;
            if (output.row_count() >= 1024) flush();
            const auto elapsed = std::chrono::steady_clock::now() - start;
            const auto fraction = std::min(1.0, std::chrono::duration<double>(elapsed).count() /
                                                  std::chrono::duration<double>(request.duration).count());
            access.report_progress(fraction, "Capturing OPC UA time-series observations");
        }
        next_sample += request.sample_interval;
        while (std::chrono::steady_clock::now() < next_sample) {
            if (access.is_cancelled()) throw std::runtime_error("OPC UA time-series capture cancelled");
            const auto remaining = next_sample - std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::min(
                remaining, std::chrono::steady_clock::duration(std::chrono::milliseconds(100))));
        }
    }
    flush();
    access.report_progress(1.0, "OPC UA time-series capture complete");
    return Json{{"endpoint", options.endpoint}, {"capture_id", capture_id},
                {"rows_emitted", rows_emitted}, {"samples_limit", request.max_samples},
                {"duration_seconds", request.duration.count() / 1000.0}};
}

}  // namespace streamfind::sensors::operations::detail
