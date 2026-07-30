#include <iostream>
#include <string>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include "server/build/build_manager.h"

namespace fs = std::filesystem;

static int passed = 0, failed = 0;

#define TEST(name) do { std::cout << "  " << name << "... "; } while(0)
#define PASS() do { std::cout << "PASS\n"; passed++; } while(0)
#define FAIL(msg) do { std::cout << "FAIL: " << msg << "\n"; failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// Helper: create a unique temp workspace with a minimal library source.
static fs::path create_temp_workspace() {
    auto tmp = fs::temp_directory_path();
    auto dir = tmp / ("qt_commander_build_test_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

// Helper: check if cmake is available on PATH by running a quick test.
static bool cmake_available() {
    int ret = std::system("cmake --version > nul 2> nul");
    return ret == 0;
}

// ===========================================================================
// 1. Build key derivation -- verify BuildResult contains key components
//    when a valid qt_env path is provided.  Only reaches build-key logic
//    if cmake is on PATH; otherwise tests the "cmake not found" error path.
// ===========================================================================
static void test_build_key_derivation() {
    auto workspace = create_temp_workspace();

    // Create a minimal library source directory so build() can proceed
    // past the source-existence check.
    fs::path lib_src = workspace / "src" / "library";
    std::error_code ec;
    fs::create_directories(lib_src, ec);
    {
        std::ofstream cmake_file(lib_src / "CMakeLists.txt");
        cmake_file << "cmake_minimum_required(VERSION 3.16)\nproject(test_lib)\n";
    }

    BuildManager mgr(workspace);

    // Use a qt_env path that encodes a version string in its filename.
    fs::path qt_env = workspace / "Qt" / "5.15.2" / "msvc2019_64" / "bin" / "qtenv2.bat";

    // Call build() and check the result.
    BuildResult result = mgr.build(qt_env, "", "", "", 5);

    if (!cmake_available()) {
        // Without cmake, build() should fail early with "cmake not found on PATH".
        CHECK(!result.ok, "build should fail without cmake");
        CHECK(result.error.find("cmake") != std::string::npos,
              "error should mention cmake");
        std::cout << "      (cmake not on PATH -- skipping build-key content check)\n";
    } else {
        // With cmake, the build key should be generated.
        // The result may still fail at later stages (no real Qt env), but build_key
        // should be populated and contain components from the qt_env path.
        CHECK(!result.build_key.empty(), "build_key should not be empty");

        // The build_key should contain the qt major version we passed (5).
        CHECK(result.build_key.find("qt5") != std::string::npos ||
              result.build_key.find("qt_5") != std::string::npos,
              "build_key should reference qt major version");

        // The build_key should contain an architecture string.
        CHECK(result.build_key.find("x86") != std::string::npos ||
              result.build_key.find("arm") != std::string::npos ||
              result.build_key.find("Win32") != std::string::npos,
              "build_key should contain architecture");

        std::cout << "      build_key = " << result.build_key << "\n";
    }

    fs::remove_all(workspace, ec);
    PASS();
}

// ===========================================================================
// 2. Default build tool detection -- called indirectly via build().
//    Verifies detectBuildTool ran correctly by examining the error when
//    cmake is missing, or the build_key when cmake is present.
// ===========================================================================
static void test_build_tool_detection() {
    auto workspace = create_temp_workspace();

    // No library source -- build() should fail at the source check,
    // but only after detectBuildTool() has run.
    BuildManager mgr(workspace);
    BuildResult result = mgr.build("", "", "", "", 5);

    if (!cmake_available()) {
        // build() should return "cmake not found on PATH" because
        // detectBuildTool() returns empty.
        CHECK(!result.ok, "build should fail");
        CHECK(result.error.find("cmake") != std::string::npos,
              "error should mention cmake: " + result.error);
        CHECK(result.error.find("not found") != std::string::npos ||
              result.error.find("not on PATH") != std::string::npos,
              "error should indicate cmake missing");
    } else {
        // cmake IS available, so detectBuildTool returns "cmake".
        // Without library source, it should fail with "library source not found".
        CHECK(!result.ok, "build should fail without library source");
        CHECK(result.error.find("library source") != std::string::npos,
              "error should mention library source: " + result.error);
    }

    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

// ===========================================================================
// 3. Build path resolution -- verify workspace path is used correctly.
// ===========================================================================
static void test_build_path_resolution() {
    auto workspace = create_temp_workspace();
    {
        // Create library source so build() passes the source-check.
        std::error_code ec;
        fs::create_directories(workspace / "src" / "library", ec);
        std::ofstream(workspace / "src" / "library" / "CMakeLists.txt")
            << "cmake_minimum_required(VERSION 3.16)\nproject(test_lib)\n";
    }

    BuildManager mgr(workspace);

    // Use a path that definitely doesn't exist as qt_env.
    fs::path bogus_env = workspace / "nonexistent" / "qtenv.bat";
    BuildResult result = mgr.build(bogus_env, "", "", "", 6);

    if (!cmake_available()) {
        // Without cmake, the early exit should happen.
        CHECK(!result.ok, "build should fail without cmake");
    } else {
        // With cmake, build_key should exist and contain "qt6".
        CHECK(!result.build_key.empty(), "build_key should be populated");

        // Verify build_key contains the qt version.
        CHECK(result.build_key.find("qt6") != std::string::npos ||
              result.build_key.find("qt_6") != std::string::npos,
              "build_key should reference qt6");

        std::cout << "      build_key = " << result.build_key << "\n";
    }

    std::error_code ec;
    fs::remove_all(workspace, ec);
    PASS();
}

// ===========================================================================

int main() {
    std::cout << "test_build_manager\n";

    test_build_key_derivation();
    test_build_tool_detection();
    test_build_path_resolution();

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
