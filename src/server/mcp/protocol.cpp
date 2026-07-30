#include "protocol.h"

#include <string>
#include <utility>
#include <iostream>

// Forward declarations
static json buildErrorResponse(const json& id, int code, const std::string& message, const json& data = json());

// JSON-RPC 2.0 error codes.
static constexpr int CODE_PARSE_ERROR     = -32700;
static constexpr int CODE_INVALID_REQUEST = -32600;
static constexpr int CODE_METHOD_NOT_FOUND = -32601;
static constexpr int CODE_INVALID_PARAMS   = -32602;
static constexpr int CODE_INTERNAL_ERROR   = -32603;

// MCP-specific error codes (reserved range -32000..-32099).
static constexpr int CODE_NOT_INITIALIZED = -32002;

// ============================================================================
// Public API
// ============================================================================

void MCPProtocol::registerTool(const MCPToolDef& tool) {
    tools_.push_back(tool);
}

void MCPProtocol::setToolCallHandler(ToolCallHandler h) {
    toolCallHandler_ = std::move(h);
}

void MCPProtocol::setResourceReadHandler(ResourceReadHandler h) {
    resourceReadHandler_ = std::move(h);
}

void MCPProtocol::setResourceListHandler(ResourceListHandler h) {
    resourceListHandler_ = std::move(h);
}

// ============================================================================
// processMessage  --  main entry point
// ============================================================================

std::string MCPProtocol::processMessage(const std::string& raw_json) {
    // --- Parse ----------------------------------------------------------------
    json msg;
    try {
        msg = json::parse(raw_json);
    } catch (const json::parse_error&) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"]      = nullptr;
        resp["error"]   = makeError(CODE_PARSE_ERROR, "Parse error");
        return resp.dump();
    }

    // --- Validate JSON-RPC structure ------------------------------------------
    if (!msg.is_object()) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"]      = nullptr;
        resp["error"]   = makeError(CODE_INVALID_REQUEST,
                                     "Request must be a JSON object");
        return resp.dump();
    }

    auto rpc_it = msg.find("jsonrpc");
    if (rpc_it == msg.end() || !rpc_it->is_string() ||
        rpc_it->get<std::string>() != "2.0") {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"]      = nullptr;
        resp["error"]   = makeError(CODE_INVALID_REQUEST,
                                     "Must specify \"jsonrpc\": \"2.0\"");
        return resp.dump();
    }

    auto method_it = msg.find("method");
    if (method_it == msg.end() || !method_it->is_string()) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"]      = nullptr;
        resp["error"]   = makeError(CODE_INVALID_REQUEST,
                                     "Missing or invalid 'method'");
        return resp.dump();
    }

    std::string method = method_it->get<std::string>();
    json params = msg.value("params", json::object());

    // Determine whether this is a request (has id) or notification (no id).
    bool is_notification = (msg.find("id") == msg.end());
    json id = is_notification ? json() : msg["id"];

    // --- Initialization gate --------------------------------------------------
    if (!initialized_ && method != "initialize" &&
        method != "notifications/initialized") {
        return buildErrorResponse(id, CODE_NOT_INITIALIZED,
                                  "Server not initialized").dump();
    }

    // --- Dispatch -------------------------------------------------------------
    json result;
    try {
        if (method == "initialize") {
            result = handleInitialize(params);
            // Mark as initialized after handling the request so that subsequent
            // requests (including notifications/initialized) are accepted.
            initialized_ = true;
        } else if (method == "notifications/initialized") {
            initialized_ = true;
            if (is_notification) return {};
            result = {{"ok", true}};
        } else if (method == "tools/list") {
            result = handleToolsList();
        } else if (method == "tools/call") {
            result = handleToolsCall(params);
        } else if (method == "resources/list") {
            result = handleResourcesList();
        } else if (method == "resources/read") {
            result = handleResourcesRead(params);
        } else if (method == "logging/setLevel") {
            result = handleLoggingSetLevel(params);
        } else {
            return buildErrorResponse(
                id, CODE_METHOD_NOT_FOUND,
                "Method not found: " + method).dump();
        }
    } catch (const std::exception& e) {
        return buildErrorResponse(
            id, CODE_INTERNAL_ERROR,
            std::string("Internal error: ") + e.what()).dump();
    }

    // Notifications get no response.
    if (is_notification) return {};

    return makeResponse(id, result).dump();
}

// ============================================================================
// JSON-RPC method handlers
// ============================================================================

json MCPProtocol::handleInitialize(const json& /*params*/) {
    json result;
    result["protocolVersion"] = protocolVersion_;
    result["capabilities"]    = getCapabilities();
    result["serverInfo"]["name"]    = serverName_;
    result["serverInfo"]["version"] = "1.0.0";
    return result;
}

json MCPProtocol::handleToolsList() {
    json arr = json::array();
    for (const auto& t : tools_) {
        json entry;
        entry["name"]        = t.name;
        entry["description"] = t.description;
        entry["inputSchema"] = t.inputSchema;
        arr.push_back(std::move(entry));
    }
    return {{"tools", std::move(arr)}};
}

json MCPProtocol::handleToolsCall(const json& params) {
    std::string tool_name = params.value("name", "");
    if (tool_name.empty()) {
        return json::object(); // Return empty result, not an error.
    }

    json args = params.value("arguments", json::object());

    if (!toolCallHandler_) {
        return json::object(); // No handler registered.
    }

    json tool_result = toolCallHandler_(tool_name, args);

    json content = json::array();
    json item;
    item["type"] = "text";
    if (tool_result.is_string()) {
        item["text"] = tool_result.get<std::string>();
    } else {
        item["text"] = tool_result.dump();
    }
    content.push_back(std::move(item));

    return {{"content", std::move(content)}};
}

json MCPProtocol::handleResourcesList() {
    if (resourceListHandler_) {
        json resources = resourceListHandler_();
        return {{"resources", resources}};
    }
    return {{"resources", json::array()}};
}

json MCPProtocol::handleResourcesRead(const json& params) {
    std::string uri = params.value("uri", "");
    if (uri.empty()) {
        return {{"contents", json::array()}};
    }

    if (!resourceReadHandler_) {
        return {{"contents", json::array()}};
    }

    json content = resourceReadHandler_(uri);
    json contents = json::array();
    contents.push_back(std::move(content));
    return {{"contents", std::move(contents)}};
}

json MCPProtocol::handleLoggingSetLevel(const json& params) {
    // Accept and acknowledge the logging level. No persistent state needed.
    (void)params;
    return json::object();
}

// ============================================================================
// Capabilities
// ============================================================================

json MCPProtocol::getCapabilities() const {
    json caps;
    caps["tools"]    = json::object();
    caps["resources"] = json::object();
    caps["logging"]  = json::object();
    return caps;
}

// ============================================================================
// JSON-RPC response builders
// ============================================================================

json MCPProtocol::makeError(int code, const std::string& message,
                            const json& data) {
    json err;
    err["code"]    = code;
    err["message"] = message;
    if (!data.is_null()) {
        err["data"] = data;
    }
    return err;
}

json MCPProtocol::makeResponse(const json& id, const json& result) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    resp["result"]  = result;
    return resp;
}

// ---------------------------------------------------------------------------
// Internal helper: build a complete error response JSON object.
// ---------------------------------------------------------------------------
static json buildErrorResponse(const json& id, int code,
                                    const std::string& message,
                                    const json& data) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id.is_null() ? json(nullptr) : id;
    json err;
    err["code"]    = code;
    err["message"] = message;
    if (!data.is_null()) {
        err["data"] = data;
    }
    resp["error"] = std::move(err);
    return resp;
}
