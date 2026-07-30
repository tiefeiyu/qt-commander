#pragma once
#include <string>
#include <functional>
#include <vector>
#include "nlohmann/json.hpp"

using json = nlohmann::json;

// Describes a tool that the AI agent can call.
struct MCPToolDef {
    std::string name;
    std::string description;
    json inputSchema; // JSON Schema object
};

// JSON-RPC 2.0 + MCP protocol handler.
class MCPProtocol {
public:
    using ToolCallHandler =
        std::function<json(const std::string& tool_name, const json& params)>;
    using ResourceReadHandler = std::function<json(const std::string& uri)>;
    using ResourceListHandler = std::function<json()>;

    // Register a tool so the client can discover it.
    void registerTool(const MCPToolDef& tool);

    // Set external handlers.
    void setToolCallHandler(ToolCallHandler h);
    void setResourceReadHandler(ResourceReadHandler h);
    void setResourceListHandler(ResourceListHandler h);

    // Process an incoming JSON-RPC message.
    // Returns the response JSON string, or an empty string for notifications.
    std::string processMessage(const std::string& raw_json);

    // Server capabilities advertised during the initialize handshake.
    json getCapabilities() const;

private:
    // JSON-RPC method handlers.
    json handleInitialize(const json& params);
    json handleToolsList();
    json handleToolsCall(const json& params);
    json handleResourcesList();
    json handleResourcesRead(const json& params);
    json handleLoggingSetLevel(const json& params);

    // Build a JSON-RPC error object (not a full response).
    json makeError(int code, const std::string& message,
                   const json& data = json());

    // Build a complete JSON-RPC success response.
    json makeResponse(const json& id, const json& result);

    // Registered tool definitions.
    std::vector<MCPToolDef> tools_;

    // External handlers (set by the application).
    ToolCallHandler toolCallHandler_;
    ResourceReadHandler resourceReadHandler_;
    ResourceListHandler resourceListHandler_;

    // Server identity.
    std::string serverName_ = "qt-commander";
    std::string protocolVersion_ = "2024-11-05";
    bool initialized_ = false;
};
