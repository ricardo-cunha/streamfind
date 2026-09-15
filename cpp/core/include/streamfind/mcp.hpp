#pragma once

#include "streamfind/export.hpp"
#include "streamfind/project.hpp"

namespace streamfind::mcp {

STREAMFIND_CORE_API const OperationRegistry &operations();

class STREAMFIND_CORE_API Session {
public:
    explicit Session(const MethodRegistry &registry = methods(), const OperationRegistry &operations = mcp::operations());
    Json handle(const Json &request);

private:
    const MethodRegistry &registry_;
    const OperationRegistry &operations_;
    Json project_{Json::object()};
    std::string domain_;
};

/** @brief Handle one MCP JSON-RPC request. */
STREAMFIND_CORE_API Json handle(const Json &request,
                                const MethodRegistry &registry = methods());

}
