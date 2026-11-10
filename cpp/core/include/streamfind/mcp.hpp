#pragma once

#include "streamfind/export.hpp"
#include "streamfind/project.hpp"
#include <filesystem>
#include <functional>
#include <vector>

namespace streamfind::mcp {

STREAMFIND_CORE_API const OperationRegistry &operations();

class STREAMFIND_CORE_API Session {
public:
    using DependencyList = std::function<Json()>;
    using DependencyInstaller = std::function<Json(const Json &)>;
    using WorkflowDemoDirectories = std::function<std::vector<std::filesystem::path>()>;
    explicit Session(const OperationRegistry &operations = mcp::operations(),
                     DependencyList dependencies = {}, DependencyInstaller installer = {},
                     std::filesystem::path worker_executable = {},
                     std::filesystem::path worker_configuration = {},
                     WorkflowDemoDirectories workflow_demo_directories = {});
    Json handle(const Json &request);

private:
    const OperationRegistry &operations_;
    DependencyList dependencies_;
    DependencyInstaller installer_;
    std::filesystem::path worker_executable_;
    std::filesystem::path worker_configuration_;
    WorkflowDemoDirectories workflow_demo_directories_;
    Json project_ = Json::object();

};

/** @brief Handle one MCP JSON-RPC request. */
STREAMFIND_CORE_API Json handle(const Json &request,
                                const OperationRegistry &operations = mcp::operations());

}
