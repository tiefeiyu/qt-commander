#include <iostream>
#include <string>
#include <cctype>
#include <algorithm>
#include <filesystem>
#include "nlohmann/json.hpp"
#include "server/session/session_manager.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

static int passed = 0, failed = 0;

#define TEST(name) do { std::cout << "  " << name << "... "; } while(0)
#define PASS() do { std::cout << "PASS\n"; passed++; } while(0)
#define FAIL(msg) do { std::cout << "FAIL: " << msg << "\n"; failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// Helper: create a unique temp directory.
static fs::path create_temp_workspace() {
    auto tmp = fs::temp_directory_path();
    auto dir = tmp / "qt_commander_test_XXXXXX";
    for (int i = 0; i < 1000; ++i) {
        auto candidate = tmp / ("qt_commander_test_" + std::to_string(i));
        std::error_code ec;
        if (fs::create_directories(candidate, ec)) {
            return candidate;
        }
    }
    // Fallback: use a timestamp-based name.
    auto fallback = tmp / ("qt_commander_test_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(fallback);
    return fallback;
}

// Helper: check if a string is alphanumeric (a-z, 0-9).
static bool is_alphanumeric(const std::string& s) {
    return !s.empty() &&
        std::all_of(s.begin(), s.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        });
}

// Helper: check if a string is hex-only.
static bool is_hex(const std::string& s) {
    return !s.empty() &&
        std::all_of(s.begin(), s.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}

// ===========================================================================
// 1. Session IDs are 12-character alphanumeric
// ===========================================================================
static void test_session_id_format() {
    auto workspace = create_temp_workspace();
    SessionManager mgr(workspace);

    std::string id1 = mgr.createSession(1001);
    std::string id2 = mgr.createSession(1002);
    std::string id3 = mgr.createSession(1003);

    CHECK(id1.length() == 12, "session ID should be 12 chars");
    CHECK(id2.length() == 12, "session ID should be 12 chars");
    CHECK(id3.length() == 12, "session ID should be 12 chars");

    CHECK(is_alphanumeric(id1), "session ID should be alphanumeric");
    CHECK(is_alphanumeric(id2), "session ID should be alphanumeric");
    CHECK(is_alphanumeric(id3), "session ID should be alphanumeric");

    // IDs should be unique.
    CHECK(id1 != id2, "session IDs should be unique");
    CHECK(id2 != id3, "session IDs should be unique");

    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

// ===========================================================================
// 2. Tokens are 64-character hex strings
// ===========================================================================
static void test_token_format() {
    auto workspace = create_temp_workspace();
    SessionManager mgr(workspace);

    std::string id1 = mgr.createSession(2001);
    std::string id2 = mgr.createSession(2002);

    Session* s1 = mgr.getSession(id1);
    Session* s2 = mgr.getSession(id2);
    CHECK(s1 != nullptr, "session 1 should exist");
    CHECK(s2 != nullptr, "session 2 should exist");

    std::string t1 = s1->token();
    std::string t2 = s2->token();

    CHECK(t1.length() == 64, "token should be 64 chars");
    CHECK(t2.length() == 64, "token should be 64 chars");
    CHECK(is_hex(t1), "token should be hex");
    CHECK(is_hex(t2), "token should be hex");
    CHECK(t1 != t2, "tokens should be unique");

    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

// ===========================================================================
// 3. Create session with PID adds to session list
// ===========================================================================
static void test_create_session() {
    auto workspace = create_temp_workspace();
    SessionManager mgr(workspace);

    CHECK(mgr.sessions().empty(), "sessions should be initially empty");

    std::string id = mgr.createSession(3001);
    CHECK(!id.empty(), "session ID should not be empty");

    CHECK(mgr.sessions().size() == 1, "should have 1 session");

    Session* s = mgr.getSession(id);
    CHECK(s != nullptr, "session should be retrievable");
    CHECK(s->id() == id, "session ID should match");
    CHECK(s->pid() == 3001, "session PID should match");
    CHECK(s->snapshotCount() == 0, "initial snapshot count should be 0");

    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

// ===========================================================================
// 4. PID dedup: second attach to same PID returns existing session
// ===========================================================================
static void test_pid_dedup() {
    auto workspace = create_temp_workspace();
    SessionManager mgr(workspace);

    std::string id1 = mgr.createSession(4001);
    CHECK(mgr.sessions().size() == 1, "should have 1 session after first create");

    // Second call with same PID should return the same session ID.
    std::string id2 = mgr.createSession(4001);
    CHECK(id2 == id1, "same PID should return same session ID");
    CHECK(mgr.sessions().size() == 1, "session count should remain 1");

    // Different PID should create a new session.
    std::string id3 = mgr.createSession(4002);
    CHECK(id3 != id1, "different PID should create different session");
    CHECK(mgr.sessions().size() == 2, "session count should be 2");

    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

// ===========================================================================
// 5. List sessions returns all created sessions
// ===========================================================================
static void test_list_sessions() {
    auto workspace = create_temp_workspace();
    SessionManager mgr(workspace);

    // Create multiple sessions.
    std::string id_a = mgr.createSession(5001);
    std::string id_b = mgr.createSession(5002);
    std::string id_c = mgr.createSession(5003);

    // Use the sessions() accessor to verify the list.
    const auto& sessions = mgr.sessions();
    CHECK(sessions.size() == 3, "should have 3 sessions");

    // Verify each session is in the map.
    CHECK(sessions.find(id_a) != sessions.end(), "session A should be in list");
    CHECK(sessions.find(id_b) != sessions.end(), "session B should be in list");
    CHECK(sessions.find(id_c) != sessions.end(), "session C should be in list");

    // Verify properties via getSession.
    Session* s = mgr.getSession(id_b);
    CHECK(s != nullptr, "session B should exist");
    CHECK(s->pid() == 5002, "session B PID should match");

    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

// ===========================================================================
// 6. Destroy session removes from list
// ===========================================================================
static void test_destroy_session() {
    auto workspace = create_temp_workspace();
    SessionManager mgr(workspace);

    std::string id = mgr.createSession(6001);
    CHECK(mgr.sessions().size() == 1, "should have 1 session");

    bool destroyed = mgr.destroySession(id);
    CHECK(destroyed, "destroySession should return true for existing session");
    CHECK(mgr.sessions().empty(), "sessions should be empty after destroy");
    CHECK(mgr.getSession(id) == nullptr, "destroyed session should not be retrievable");

    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

// ===========================================================================
// 7. Destroy non-existent session returns gracefully
// ===========================================================================
static void test_destroy_nonexistent() {
    auto workspace = create_temp_workspace();
    SessionManager mgr(workspace);

    // Destroying a non-existent session should return false, not crash.
    bool destroyed = mgr.destroySession("nonexistent_id");
    CHECK(!destroyed, "destroySession should return false for non-existent session");

    // Verify no exception is thrown (the catch-all handled it gracefully).
    CHECK(mgr.sessions().empty(), "sessions should still be empty");

    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

// ===========================================================================
// 8. Get session by ID
// ===========================================================================
static void test_get_session() {
    auto workspace = create_temp_workspace();
    SessionManager mgr(workspace);

    // getSession on empty manager should return nullptr.
    Session* null_s = mgr.getSession("anything");
    CHECK(null_s == nullptr, "getSession should return nullptr when empty");

    // Create a session and retrieve it.
    std::string id = mgr.createSession(8001);
    Session* s = mgr.getSession(id);
    CHECK(s != nullptr, "getSession should find existing session");
    CHECK(s->id() == id, "session ID should match");
    CHECK(s->pid() == 8001, "session PID should match");

    // After destroy, getSession should return nullptr.
    mgr.destroySession(id);
    CHECK(mgr.getSession(id) == nullptr, "getSession should return nullptr after destroy");

    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

// ===========================================================================
// 9. handleToolCall — dispatch with invalid session
// ===========================================================================
static void test_handle_tool_call_invalid_session() {
    TEST("handleToolCall returns error for missing session");
    auto workspace = create_temp_workspace();
    SessionManager mgr(workspace);

    // Try to call a session-bound tool with a bad session_id
    json params = {
        {"session_id", "nonexistent123"},
        {"element_id", 1}
    };
    json result = mgr.handleToolCall("qt_snapshot", params);
    CHECK(result.contains("error"), "should return error for invalid session");
    CHECK(result["error"].get<std::string>().find("not found") != std::string::npos
          || result["error"].get<std::string>().find("Session") != std::string::npos,
          "error should mention session not found");

    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

static void test_handle_tool_call_placeholder_tools() {
    TEST("handleToolCall handles placeholder tools gracefully");
    auto workspace = create_temp_workspace();
    SessionManager mgr(workspace);

    // list_processes — placeholder returns valid json
    json plist = mgr.handleToolCall("qt_list_processes", json::object());
    CHECK(!plist.is_null(), "list_processes should return non-null result");

    // list_sessions — should work without active sessions
    json slist = mgr.handleToolCall("qt_list_sessions", json::object());
    CHECK(!slist.is_null(),
          "list_sessions should return non-null result");

    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

// ===========================================================================
// 10. Resources (list + read)
// ===========================================================================
static void test_resources_list() {
    TEST("listResources returns resource descriptors");
    auto workspace = create_temp_workspace();
    SessionManager mgr(workspace);

    json resources = mgr.listResources();
    CHECK(resources.is_array(), "listResources should return array");

    // With a session, resources should be discoverable
    std::string sid = mgr.createSession(9999);
    resources = mgr.listResources();
    CHECK(resources.is_array(), "listResources with session should return array");
    // Resources are file-based — if no snapshot/screenshot files exist,
    // the list may be empty. Verify URI structure is correct when present.
    for (const auto& r : resources) {
        if (r.contains("uri")) {
            std::string uri = r["uri"].get<std::string>();
            CHECK((uri.find("session://") != std::string::npos),
                  "resource URI should use session:// scheme");
        }
    }

    mgr.destroySession(sid);
    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

static void test_resources_read() {
    TEST("readResource returns error for missing files");
    auto workspace = create_temp_workspace();
    SessionManager mgr(workspace);

    // Reading a non-existent resource should return an error
    json result = mgr.readResource("session://nonexistent/snapshots/notfound.json");
    CHECK(result.contains("error"), "readResource for missing file should error");

    // Malformed URI
    json badUri = mgr.readResource("not-a-valid-uri");
    CHECK(badUri.contains("error"), "readResource for malformed URI should error");

    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

// ===========================================================================

int main() {
    std::cout << "test_session_manager\n";

    test_session_id_format();
    test_token_format();
    test_create_session();
    test_pid_dedup();
    test_list_sessions();
    test_destroy_session();
    test_destroy_nonexistent();
    test_get_session();
    test_handle_tool_call_invalid_session();
    test_handle_tool_call_placeholder_tools();
    test_resources_list();
    test_resources_read();

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
