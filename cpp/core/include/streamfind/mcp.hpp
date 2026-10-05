#pragma once

#include "streamfind/export.hpp"
#include "streamfind/project.hpp"
#include <functional>

namespace streamfind::mcp {

STREAMFIND_CORE_API const OperationRegistry &operations();

class STREAMFIND_CORE_API Session {
public:
    using DependencyList = std::function<Json()>;
    using DependencyInstaller = std::function<Json(const Json &)>;
    explicit Session(const OperationRegistry &operations = mcp::operations(),
                     DependencyList dependencies = {}, DependencyInstaller installer = {});
    Json handle(const Json &request);

private:
    const OperationRegistry &operations_;
    DependencyList dependencies_;
    DependencyInstaller installer_;
    Json project_{Json::object()};

};

/** @brief Handle one MCP JSON-RPC request. */
STREAMFIND_CORE_API Json handle(const Json &request,
                                const OperationRegistry &operations = mcp::operations());

}
