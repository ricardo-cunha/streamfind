#pragma once

#include "streamfind/export.hpp"
#include "streamfind/data_proxy_protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace streamfind::service {

struct STREAMFIND_CORE_API ServiceProjectAttachment {
    std::string service_instance_id;
    std::string endpoint;
    std::uint16_t port{0};
    std::string session_id;
};

/** A sanitized view of one locally discovered service and its live health. */
STREAMFIND_CORE_API data_proxy::Json list_services();

/** Stop one service, or the unique healthy service when instance_id is empty. */
STREAMFIND_CORE_API data_proxy::Json stop_service(const std::string &instance_id = {});

/** Attach a project to an already running local streamfind service. */
STREAMFIND_CORE_API ServiceProjectAttachment attach_project(
    const std::filesystem::path &database_path);

STREAMFIND_CORE_API ServiceProjectAttachment create_project(
    const std::filesystem::path &database_path,
    const data_proxy::Json &metadata = data_proxy::Json::object());

STREAMFIND_CORE_API data_proxy::Json get_workflow(const ServiceProjectAttachment &attachment);
STREAMFIND_CORE_API data_proxy::Json resolve_workflow_inputs(
    const ServiceProjectAttachment &attachment, const std::string &operation_id);

STREAMFIND_CORE_API data_proxy::Json start_workflow(
    const ServiceProjectAttachment &attachment,
    const std::string &execution_mode = "detailed");

STREAMFIND_CORE_API data_proxy::Json workflow_snapshot(
    const ServiceProjectAttachment &attachment);

STREAMFIND_CORE_API data_proxy::Json cancel_workflow(
    const ServiceProjectAttachment &attachment);

STREAMFIND_CORE_API data_proxy::Json cancel_operation(
    const ServiceProjectAttachment &attachment,
    const std::string &operation_instance);

STREAMFIND_CORE_API data_proxy::Json close_project(
    const ServiceProjectAttachment &attachment);

STREAMFIND_CORE_API data_proxy::Json run_operation(
    const ServiceProjectAttachment &attachment,
    const std::string &operation_id,
    const data_proxy::Json &parameters = data_proxy::Json::object(),
    const std::string &operation_instance = {},
    const data_proxy::Json &inputs = nullptr);

STREAMFIND_CORE_API data_proxy::Json validate_workflow(
    const ServiceProjectAttachment &attachment, const data_proxy::Json &workflow = nullptr);

STREAMFIND_CORE_API data_proxy::Json save_workflow(
    const ServiceProjectAttachment &attachment, const data_proxy::Json &workflow);

STREAMFIND_CORE_API data_proxy::Json get_project(const ServiceProjectAttachment &attachment);
STREAMFIND_CORE_API data_proxy::Json set_metadata(
    const ServiceProjectAttachment &attachment,
    const data_proxy::Json &metadata);
STREAMFIND_CORE_API data_proxy::Json get_audit_trail(
    const ServiceProjectAttachment &attachment);
STREAMFIND_CORE_API data_proxy::Json copy_project(
    const ServiceProjectAttachment &attachment,
    const std::filesystem::path &destination_database_path);

STREAMFIND_CORE_API data_proxy::Json get_results(const ServiceProjectAttachment &attachment);

STREAMFIND_CORE_API data_proxy::Json get_current_results(const ServiceProjectAttachment &attachment);

STREAMFIND_CORE_API data_proxy::Json get_result_data(
    const ServiceProjectAttachment &attachment, const data_proxy::Json &request);

}  // namespace streamfind::service
