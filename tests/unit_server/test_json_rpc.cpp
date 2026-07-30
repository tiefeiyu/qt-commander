// JSON-RPC 2.0 protocol parsing tests — tests the JSON handling logic
// This tests the core concepts used by MCPProtocol (not the full nlohmann integration yet)

#include <iostream>
#include <cassert>
#include <string>

static int passed = 0, failed = 0;
#define TEST(n) do { std::cout << "  " << n << "... "; } while(0)
#define PASS() do { std::cout << "PASS\n"; passed++; } while(0)
#define FAIL(m) do { std::cout << "FAIL: " << m << "\n"; failed++; } while(0)
#define CHECK(c,m) do { if(!(c)) { FAIL(m); return; } } while(0)

// Lightweight JSON-RPC 2.0 parsing without nlohmann
// Tests concepts: request detection, notification detection, error response building

bool isJsonRpc(const std::string& json) {
    return json.find("\"jsonrpc\"") != std::string::npos &&
           json.find("\"2.0\"") != std::string::npos;
}

bool hasId(const std::string& json) {
    return json.find("\"id\"") != std::string::npos;
}

bool isRequest(const std::string& json) {
    return isJsonRpc(json) && json.find("\"method\"") != std::string::npos;
}

bool isNotification(const std::string& json) {
    return isRequest(json) && !hasId(json);
}

bool isResponse(const std::string& json) {
    return isJsonRpc(json) && !isRequest(json);
}

std::string buildRequest(const std::string& method, const std::string& params, int id) {
    return "{\"jsonrpc\":\"2.0\",\"method\":\"" + method +
           "\",\"params\":" + params + ",\"id\":" + std::to_string(id) + "}";
}

std::string buildError(int code, const std::string& msg, int id) {
    return "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":" + std::to_string(code) +
           ",\"message\":\"" + msg + "\"},\"id\":" + std::to_string(id) + "}";
}

std::string buildResult(const std::string& result, int id) {
    return "{\"jsonrpc\":\"2.0\",\"result\":" + result + ",\"id\":" + std::to_string(id) + "}";
}

static void test_is_request() {
    TEST("detect JSON-RPC request");
    auto req = buildRequest("test.method", "{}", 1);
    CHECK(isRequest(req), "should be request");
    CHECK(!isNotification(req), "should not be notification (has id)");
    CHECK(!isResponse(req), "should not be response");
    PASS();
}

static void test_is_notification() {
    TEST("detect JSON-RPC notification");
    std::string notif = "{\"jsonrpc\":\"2.0\",\"method\":\"notify\",\"params\":{}}";
    CHECK(isRequest(notif), "should be request");
    CHECK(isNotification(notif), "should be notification (no id)");
    CHECK(!isResponse(notif), "should not be response");
    PASS();
}

static void test_is_response() {
    TEST("detect JSON-RPC response");
    std::string resp = "{\"jsonrpc\":\"2.0\",\"result\":{},\"id\":1}";
    CHECK(isResponse(resp), "should be response");
    CHECK(!isRequest(resp), "should not be request");
    CHECK(!isNotification(resp), "should not be notification");
    PASS();
}

static void test_error_structure() {
    TEST("error response structure");
    auto err = buildError(-32000, "server error", 5);
    CHECK(err.find("\"code\":-32000") != std::string::npos, "error code present");
    CHECK(err.find("\"message\":\"server error\"") != std::string::npos, "error message present");
    CHECK(err.find("\"id\":5") != std::string::npos, "id present");
    PASS();
}

static void test_result_structure() {
    TEST("result response structure");
    auto res = buildResult("{\"ok\":true}", 42);
    CHECK(res.find("\"result\":{\"ok\":true}") != std::string::npos, "result present");
    CHECK(res.find("\"id\":42") != std::string::npos, "id present");
    PASS();
}

static void test_batch_detection() {
    TEST("batch request (array) detection");
    std::string batch = "[{\"jsonrpc\":\"2.0\",\"method\":\"a\",\"id\":1},{\"jsonrpc\":\"2.0\",\"method\":\"b\",\"id\":2}]";
    CHECK(batch[0] == '[', "starts with array bracket");
    CHECK(isJsonRpc(batch), "contains jsonrpc marker");
    PASS();
}

static void test_invalid_json_not_crash() {
    TEST("malformed JSON handling");
    std::string bad[] = {"", "{", "not json", "[1,2,}"};
    for (auto& s : bad) {
        CHECK(!isJsonRpc(s), "non-JSON should not be detected as JSON-RPC");
    }
    PASS();
}

static void test_mcp_initialize_message() {
    TEST("MCP initialize request format");
    auto init = buildRequest("initialize",
        "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}", 1);
    CHECK(isRequest(init), "initialize is a request");
    CHECK(isJsonRpc(init), "contains jsonrpc");
    CHECK(init.find("initialize") != std::string::npos, "contains method name");
    PASS();
}

static void test_tools_list_request() {
    TEST("tools/list request format");
    auto req = buildRequest("tools/list", "{}", 2);
    CHECK(isRequest(req), "tools/list is a request");
    PASS();
}

static void test_tools_call_request() {
    TEST("tools/call request format");
    auto req = buildRequest("tools/call",
        "{\"name\":\"qt_list_processes\",\"arguments\":{}}", 3);
    CHECK(isRequest(req), "tools/call is a request");
    CHECK(req.find("qt_list_processes") != std::string::npos, "contains tool name");
    PASS();
}

int main() {
    std::cout << "test_json_rpc\n";
    test_is_request();
    test_is_notification();
    test_is_response();
    test_error_structure();
    test_result_structure();
    test_batch_detection();
    test_invalid_json_not_crash();
    test_mcp_initialize_message();
    test_tools_list_request();
    test_tools_call_request();

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
