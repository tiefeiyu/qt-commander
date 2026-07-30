#include <iostream>
#include <string>
#include <cstring>
#include "nlohmann/json.hpp"
#include "server/mcp/protocol.h"

using json = nlohmann::json;

static int passed = 0, failed = 0;

#define TEST(name) do { std::cout << "  " << name << "... "; } while(0)
#define PASS() do { std::cout << "PASS\n"; passed++; } while(0)
#define FAIL(msg) do { std::cout << "FAIL: " << msg << "\n"; failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// Helper: parse JSON safely, return default object on failure.
static json parse_opt(const std::string& s) {
    try {
        return json::parse(s);
    } catch (...) {
        return json::object();
    }
}

// ===========================================================================
// 1. Initialize handshake -- valid initialize request produces capabilities
// ===========================================================================
static void test_initialize_handshake() {
    MCPProtocol proto;
    std::string req = R"({"jsonrpc":"2.0","method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}},"id":1})";
    std::string resp = proto.processMessage(req);
    json j = parse_opt(resp);

    CHECK(!j.is_null(), "response should be valid JSON");
    CHECK(j.contains("result"), "response should contain 'result'");
    CHECK(j["result"].contains("capabilities"), "result should contain 'capabilities'");
    CHECK(j["result"].contains("protocolVersion"), "result should contain 'protocolVersion'");
    CHECK(j["result"].contains("serverInfo"), "result should contain 'serverInfo'");
    CHECK(j["result"]["capabilities"].contains("tools"), "capabilities should contain 'tools'");
    CHECK(j["result"]["capabilities"].contains("resources"), "capabilities should contain 'resources'");
    CHECK(j["result"]["serverInfo"]["name"] == "qt-commander", "server name should match");
    CHECK(j["id"] == 1, "response id should match request id");

    // Verify no error.
    CHECK(!j.contains("error"), "initialize response should not contain error");
    PASS();
}

// ===========================================================================
// 2. Reject tools/call before initialize
// ===========================================================================
static void test_reject_before_initialize() {
    MCPProtocol proto;
    std::string req = R"({"jsonrpc":"2.0","method":"tools/list","params":{},"id":1})";
    std::string resp = proto.processMessage(req);
    json j = parse_opt(resp);

    CHECK(j.contains("error"), "should return error when not initialized");
    CHECK(j["error"].contains("code"), "error should contain 'code'");
    CHECK(j["error"]["code"] == -32002, "error code should be -32002 (Not Initialized)");
    CHECK(j["id"] == 1, "response id should match");
    PASS();
}

// ===========================================================================
// 3. tools/list after initialize returns registered tools
// ===========================================================================
static void test_tools_list_after_init() {
    MCPProtocol proto;

    // Initialize first.
    std::string init_req = R"({"jsonrpc":"2.0","method":"initialize","params":{},"id":1})";
    proto.processMessage(init_req);

    // Register some tools.
    MCPToolDef tool1;
    tool1.name = "test_tool_a";
    tool1.description = "First test tool";
    tool1.inputSchema = {{"type", "object"}, {"properties", json::object()}};
    proto.registerTool(tool1);

    MCPToolDef tool2;
    tool2.name = "test_tool_b";
    tool2.description = "Second test tool";
    tool2.inputSchema = {{"type", "object"}};
    proto.registerTool(tool2);

    // Query tools/list.
    std::string list_req = R"({"jsonrpc":"2.0","method":"tools/list","params":{},"id":2})";
    std::string resp = proto.processMessage(list_req);
    json j = parse_opt(resp);

    CHECK(j.contains("result"), "response should contain 'result'");
    CHECK(j["result"].contains("tools"), "result should contain 'tools'");
    CHECK(j["result"]["tools"].is_array(), "tools should be an array");
    CHECK(j["result"]["tools"].size() == 2, "should have 2 registered tools");
    CHECK(j["result"]["tools"][0]["name"] == "test_tool_a", "first tool name should match");
    CHECK(j["result"]["tools"][1]["name"] == "test_tool_b", "second tool name should match");
    CHECK(j["result"]["tools"][0]["description"] == "First test tool", "description should match");
    CHECK(j["id"] == 2, "response id should match");
    PASS();
}

// ===========================================================================
// 4. tools/call with valid tool dispatches to handler
// ===========================================================================
static void test_tools_call_valid() {
    MCPProtocol proto;

    // Initialize.
    proto.processMessage(R"({"jsonrpc":"2.0","method":"initialize","params":{},"id":1})");

    // Register a tool.
    MCPToolDef tool;
    tool.name = "echo";
    tool.description = "Echo test tool";
    tool.inputSchema = {{"type", "object"}};
    proto.registerTool(tool);

    // Set a handler that echoes back the arguments.
    proto.setToolCallHandler([](const std::string& name, const json& params) -> json {
        json result;
        result["tool"] = name;
        result["args"] = params;
        return result;
    });

    std::string call_req = R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"echo","arguments":{"key":"value"}},"id":2})";
    std::string resp = proto.processMessage(call_req);
    json j = parse_opt(resp);

    CHECK(j.contains("result"), "response should contain result");
    CHECK(j["result"].contains("content"), "result should contain content");
    CHECK(j["result"]["content"].is_array(), "content should be array");
    CHECK(j["result"]["content"].size() >= 1, "content should have at least 1 item");
    CHECK(j["result"]["content"][0]["type"] == "text", "content type should be text");

    std::string text = j["result"]["content"][0]["text"];
    json text_json = json::parse(text);
    CHECK(text_json["tool"] == "echo", "handler should have received 'echo'");
    CHECK(text_json["args"]["key"] == "value", "handler should have received arguments");
    CHECK(j["id"] == 2, "response id should match");
    PASS();
}

// ===========================================================================
// 5. tools/call with unknown tool name -- handler controls response
// ===========================================================================
static void test_tools_call_unknown() {
    MCPProtocol proto;

    // Initialize.
    proto.processMessage(R"({"jsonrpc":"2.0","method":"initialize","params":{},"id":1})");

    // Register one tool but call a different one.
    MCPToolDef tool;
    tool.name = "real_tool";
    tool.description = "The only tool";
    tool.inputSchema = {{"type", "object"}};
    proto.registerTool(tool);

    // Handler that returns an application-level error for unknown tools.
    proto.setToolCallHandler([](const std::string& name, const json&) -> json {
        if (name != "real_tool") {
            return {{"error", "Unknown tool: " + name}};
        }
        return {{"ok", true}};
    });

    std::string call_req = R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"nonexistent_tool","arguments":{}},"id":5})";
    std::string resp = proto.processMessage(call_req);
    json j = parse_opt(resp);

    // The protocol always returns a success JSON-RPC for known methods.
    // Application-level errors appear inside the content text.
    CHECK(j.contains("result"), "response should contain result");
    CHECK(j["result"].contains("content"), "result should contain content");

    std::string text = j["result"]["content"][0]["text"];
    json text_json = json::parse(text);
    CHECK(text_json.contains("error"), "handler should signal error");
    CHECK(text_json["error"].get<std::string>().find("nonexistent_tool") != std::string::npos,
          "error should mention the unknown tool name");
    PASS();
}

// ===========================================================================
// 6. notifications/initialized after initialize
// ===========================================================================
static void test_notification_initialized() {
    MCPProtocol proto;

    // Initialize first.
    proto.processMessage(R"({"jsonrpc":"2.0","method":"initialize","params":{},"id":1})");

    // Send notifications/initialized (notification -- no id).
    std::string notif = R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})";
    std::string resp = proto.processMessage(notif);

    // Notifications return empty string.
    CHECK(resp.empty(), "notification should produce no response");

    // After notifications/initialized, tools/list should still work.
    std::string list_req = R"({"jsonrpc":"2.0","method":"tools/list","params":{},"id":2})";
    resp = proto.processMessage(list_req);
    json j = parse_opt(resp);
    CHECK(j.contains("result"), "tools/list should work after initialized notification");
    CHECK(!j.contains("error"), "no error expected");
    PASS();
}

// ===========================================================================
// 7. Invalid JSON returns parse error
// ===========================================================================
static void test_invalid_json() {
    MCPProtocol proto;

    // Send garbage that is not valid JSON.
    std::string resp = proto.processMessage("not valid json at all");
    json j = parse_opt(resp);

    CHECK(j.contains("error"), "should return error for invalid JSON");
    CHECK(j["error"]["code"] == -32700, "error code should be -32700 (Parse error)");
    CHECK(j["id"].is_null(), "id should be null for parse errors");

    // Also test empty string.
    resp = proto.processMessage("");
    j = parse_opt(resp);
    CHECK(j.contains("error"), "empty string should produce error");
    CHECK(j["error"]["code"] == -32700, "parse error expected for empty input");

    // Test truncated JSON.
    resp = proto.processMessage("{\"jsonrpc\":\"2.0\",\"method\":\"test\"");
    j = parse_opt(resp);
    CHECK(j.contains("error"), "truncated JSON should produce error");
    CHECK(j["error"]["code"] == -32700, "parse error expected for truncated input");
    PASS();
}

// ===========================================================================
// 8. Batch requests (JSON array) are rejected with appropriate error
// ===========================================================================
static void test_batch_request() {
    MCPProtocol proto;

    std::string batch = R"([{"jsonrpc":"2.0","method":"initialize","params":{},"id":1}])";
    std::string resp = proto.processMessage(batch);
    json j = parse_opt(resp);

    // The protocol checks !msg.is_object() and returns invalid request.
    CHECK(j.contains("error"), "batch array should produce error");
    CHECK(j["error"]["code"] == -32600, "error code should be -32600 (Invalid Request)");
    CHECK(j["id"].is_null(), "id should be null for invalid request");
    PASS();
}

// ===========================================================================
// 9. Resources list/read delegation to handlers
// ===========================================================================
static void test_resources_list_read() {
    MCPProtocol proto;

    // Initialize.
    proto.processMessage(R"({"jsonrpc":"2.0","method":"initialize","params":{},"id":1})");

    // Set resource handlers.
    proto.setResourceListHandler([]() -> json {
        json r1;
        r1["uri"] = "test://resource/1";
        r1["name"] = "Resource One";
        json arr = json::array();
        arr.push_back(r1);
        return arr;
    });

    proto.setResourceReadHandler([](const std::string& uri) -> json {
        json result;
        result["uri"] = uri;
        result["text"] = "content of " + uri;
        return result;
    });

    // Test resources/list.
    std::string list_req = R"({"jsonrpc":"2.0","method":"resources/list","params":{},"id":10})";
    std::string resp = proto.processMessage(list_req);
    json j = parse_opt(resp);

    CHECK(j.contains("result"), "resources/list should have result");
    CHECK(j["result"].contains("resources"), "result should contain resources");
    CHECK(j["result"]["resources"].is_array(), "resources should be array");
    CHECK(j["result"]["resources"].size() == 1, "should have 1 resource");
    CHECK(j["result"]["resources"][0]["uri"] == "test://resource/1", "uri should match");

    // Test resources/read.
    std::string read_req = R"({"jsonrpc":"2.0","method":"resources/read","params":{"uri":"test://resource/1"},"id":11})";
    resp = proto.processMessage(read_req);
    j = parse_opt(resp);

    CHECK(j.contains("result"), "resources/read should have result");
    CHECK(j["result"].contains("contents"), "result should contain contents");
    CHECK(j["result"]["contents"].is_array(), "contents should be array");
    CHECK(j["result"]["contents"].size() == 1, "should have 1 content item");
    CHECK(j["result"]["contents"][0]["uri"] == "test://resource/1", "uri should match");
    CHECK(j["result"]["contents"][0]["text"] == "content of test://resource/1", "text should match");

    PASS();
}

// ===========================================================================
// 10. Resources edge cases
// ===========================================================================
static void test_resources_no_handler() {
    TEST("resources/list with no handler returns empty list");
    MCPProtocol proto;
    proto.processMessage(R"({"jsonrpc":"2.0","method":"initialize","params":{},"id":1})");

    // No resource handlers set — should return empty array
    std::string req = R"({"jsonrpc":"2.0","method":"resources/list","params":{},"id":10})";
    std::string resp = proto.processMessage(req);
    json j = parse_opt(resp);

    CHECK(j.contains("result"), "resources/list should have result");
    CHECK(j["result"].contains("resources"), "result should contain resources");
    CHECK(j["result"]["resources"].is_array(), "resources should be array");
    CHECK(j["result"]["resources"].size() == 0, "should have 0 resources with no handler");

    // resources/read with no handler — returns empty contents array
    req = R"({"jsonrpc":"2.0","method":"resources/read","params":{"uri":"test://x"},"id":11})";
    resp = proto.processMessage(req);
    j = parse_opt(resp);
    CHECK(j.contains("result"), "resources/read should have result");
    CHECK(j["result"].contains("contents"), "result should contain contents");
    CHECK(j["result"]["contents"].is_array(), "contents should be array");
    CHECK(j["result"]["contents"].size() == 0, "contents should be empty with no handler");

    PASS();
}

// ===========================================================================
// 11. Tools/call edge cases
// ===========================================================================
static void test_tools_call_edge_cases() {
    TEST("tools/call with empty name or no handler");
    MCPProtocol proto;
    proto.processMessage(R"({"jsonrpc":"2.0","method":"initialize","params":{},"id":1})");

    // Empty tool name — protocol returns empty result (graceful)
    std::string req = R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":""},"id":20})";
    std::string resp = proto.processMessage(req);
    json j = parse_opt(resp);
    CHECK(j.contains("result"), "tools/call with empty name should have result");

    // Tool name with no registered handler — returns empty result
    req = R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"nonexistent_tool"},"id":21})";
    resp = proto.processMessage(req);
    j = parse_opt(resp);
    CHECK(j.contains("result"), "tools/call with unknown tool should have result");

    PASS();
}

// ===========================================================================
// 12. Logging setLevel
// ===========================================================================
static void test_logging_set_level() {
    TEST("logging/setLevel notification is accepted");
    MCPProtocol proto;
    proto.processMessage(R"({"jsonrpc":"2.0","method":"initialize","params":{},"id":1})");

    // logging/setLevel is a notification (no id) — should be accepted silently
    std::string req = R"({"jsonrpc":"2.0","method":"logging/setLevel","params":{"level":"debug"}})";
    std::string resp = proto.processMessage(req);
    CHECK(resp.empty(), "logging/setLevel notification should produce no response");

    // With an id, it should return a result (like any recognized method w/ handler)
    req = R"({"jsonrpc":"2.0","method":"logging/setLevel","params":{"level":"info"},"id":30})";
    resp = proto.processMessage(req);
    json j = parse_opt(resp);
    CHECK(j.contains("result") || j.contains("error"),
          "logging/setLevel with id should produce response");

    PASS();
}

// ===========================================================================
// 13. Error response format verification
// ===========================================================================
static void test_error_response_format() {
    MCPProtocol proto;

    // Send request with invalid method (before init).
    std::string req = R"({"jsonrpc":"2.0","method":"nonexistent","params":{},"id":42})";
    std::string resp = proto.processMessage(req);
    json j = parse_opt(resp);

    // Verify JSON-RPC 2.0 error response format.
    CHECK(j.contains("jsonrpc"), "error response should contain jsonrpc");
    CHECK(j["jsonrpc"] == "2.0", "jsonrpc should be 2.0");
    CHECK(j.contains("error"), "error response should contain error object");
    CHECK(j["error"].is_object(), "error should be an object");
    CHECK(j["error"].contains("code"), "error should contain code");
    CHECK(j["error"]["code"].is_number_integer(), "error code should be integer");
    CHECK(j["error"].contains("message"), "error should contain message");
    CHECK(j["error"]["message"].is_string(), "error message should be string");
    CHECK(j.contains("id"), "error response should contain id");
    CHECK(j["id"] == 42, "id should echo the request id");

    // Verify that jsonrpc, error, id are the only top-level keys.
    CHECK(j.size() == 3, "error response should have exactly 3 keys (jsonrpc, error, id)");
    PASS();
}

// ===========================================================================

int main() {
    std::cout << "test_protocol\n";

    test_initialize_handshake();
    test_reject_before_initialize();
    test_tools_list_after_init();
    test_tools_call_valid();
    test_tools_call_unknown();
    test_notification_initialized();
    test_invalid_json();
    test_batch_request();
    test_resources_list_read();
    test_resources_no_handler();
    test_tools_call_edge_cases();
    test_logging_set_level();
    test_error_response_format();

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
