#include "build_manager.h"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <array>
#include <regex>
#include <algorithm>
#include <random>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

// ---------------------------------------------------------------------------
// Utility helpers
// ---------------------------------------------------------------------------

static std::string createHash(const std::string& input) {
    // Simple FNV-1a hash for build key derivation
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : input) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }

    // Format as hex string
    const char* hexDigits = "0123456789abcdef";
    std::string result(16, '0');
    for (int i = 0; i < 16; ++i) {
        result[15 - i] = hexDigits[hash & 0xF];
        hash >>= 4;
    }
    return result;
}

static std::string getSourceHash(const fs::path& srcDir) {
    // Compute a basic hash of all .cpp/.h files in the source directory
    // This is used to invalidate stale build outputs
    std::string combined;

    if (!fs::exists(srcDir))
        return combined;

    for (const auto& entry : fs::recursive_directory_iterator(srcDir)) {
        if (!entry.is_regular_file())
            continue;

        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".cxx" &&
            ext != ".hxx" && ext != ".cc" && ext != ".hh")
            continue;

        // Use last write time as a proxy for content hash (fast)
        auto ft = fs::last_write_time(entry.path());
        auto duration = ft.time_since_epoch().count();
        combined += entry.path().filename().string() + std::to_string(duration);
    }

    return createHash(combined);
}

static bool createDirectories(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    return !ec;
}

static int runProcess(const std::string& command,
                      std::string* output = nullptr) {
#ifdef _WIN32
    // Use _popen for capturing output
    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe)
        return -1;

    if (output) {
        std::array<char, 4096> buf{};
        while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
            *output += buf.data();
        }
    }
    return _pclose(pipe);
#else
    // POSIX popen
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe)
        return -1;

    if (output) {
        std::array<char, 4096> buf{};
        while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
            *output += buf.data();
        }
    }
    return pclose(pipe);
#endif
}

// Copy file or directory recursively
static bool copyRecursive(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::copy(from, to, fs::copy_options::recursive |
                            fs::copy_options::overwrite_existing,
             ec);
    return !ec;
}

// ---------------------------------------------------------------------------
// Cross-platform build helpers
// ---------------------------------------------------------------------------

static std::string detectGenerator() {
#ifdef _WIN32
    // Check for Ninja first
    std::string ninjaOut;
    if (runProcess("ninja --version 2>nul", &ninjaOut) == 0 && !ninjaOut.empty())
        return "Ninja";

    // Fallback to Visual Studio (detected by CMake)
    return "";
#elif defined(__APPLE__)
    // Check for Ninja
    std::string ninjaOut;
    if (runProcess("ninja --version 2>/dev/null", &ninjaOut) == 0 && !ninjaOut.empty())
        return "Ninja";
    return "Unix Makefiles";
#else
    // Linux: Ninja or Makefiles
    std::string ninjaOut;
    if (runProcess("ninja --version 2>/dev/null", &ninjaOut) == 0 && !ninjaOut.empty())
        return "Ninja";
    return "Unix Makefiles";
#endif
}

static std::string detectArch() {
#ifdef _WIN32
    SYSTEM_INFO sysInfo{};
    GetNativeSystemInfo(&sysInfo);
    switch (sysInfo.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        return "x86_64";
    case PROCESSOR_ARCHITECTURE_INTEL:
        return "x86";
    case PROCESSOR_ARCHITECTURE_ARM64:
        return "arm64";
    default:
        return "x86_64";
    }
#else
    // On Linux/macOS, use uname -m
    std::string unameOut;
    runProcess("uname -m", &unameOut);
    if (!unameOut.empty()) {
        // Trim trailing newline
        unameOut.erase(std::remove(unameOut.begin(), unameOut.end(), '\n'),
                       unameOut.end());
        // Normalize
        if (unameOut == "x86_64" || unameOut == "amd64")
            return "x86_64";
        if (unameOut == "i386" || unameOut == "i686")
            return "x86";
        if (unameOut == "aarch64" || unameOut == "arm64")
            return "arm64";
        return unameOut;
    }
    return "x86_64";
#endif
}

// ---------------------------------------------------------------------------
// BuildManager implementation
// ---------------------------------------------------------------------------

BuildManager::BuildManager(const fs::path& workspace) : workspace_(workspace) {
}

std::string BuildManager::detectBuildTool() const {
#ifdef _WIN32
    std::string cmakeOut;
    int ret = runProcess("cmake --version 2>nul", &cmakeOut);
    if (ret == 0 && !cmakeOut.empty())
        return "cmake";
    return "";
#else
    std::string cmakeOut;
    int ret = runProcess("cmake --version 2>/dev/null", &cmakeOut);
    if (ret == 0 && !cmakeOut.empty())
        return "cmake";
    return "";
#endif
}

BuildResult BuildManager::build(const fs::path& qt_env, const fs::path& vcvars,
                                const std::string& arch,
                                const std::string& generator,
                                int qt_major_version) {
    BuildResult result;
    result.ok = false;

    // 1. Verify cmake is available
    std::string buildTool = detectBuildTool();
    if (buildTool.empty()) {
        result.error = "cmake not found on PATH";
        return result;
    }

    // 2. Resolve arch and generator
    std::string resolvedArch = arch.empty() ? detectArch() : arch;
    std::string resolvedGenerator = generator.empty() ? detectGenerator() : generator;

    // 3. Build a unique key from version + toolchain + arch + source hash
    std::string qtEnvStr = qt_env.filename().string();
    std::string vcvarsStr = vcvars.filename().string();

    // Combine key components
    std::stringstream keyStream;
    keyStream << "qt" << qt_major_version << "_"
              << resolvedArch << "_"
              << (resolvedGenerator.empty() ? "default" : resolvedGenerator) << "_"
              << qtEnvStr << "_"
              << vcvarsStr << "_";

    // Source hash (library source is assumed to be at workspace/src/library)
    fs::path libSrc = workspace_ / "src" / "library";
    if (fs::exists(libSrc)) {
        keyStream << getSourceHash(libSrc);
    } else {
        // Fallback: use a random token
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        keyStream << createHash(std::to_string(now));
    }

    result.build_key = keyStream.str();

    // 4. Create build directories
    fs::path buildDir = workspace_ / "builds" / result.build_key;
    fs::path buildSrcDir = buildDir / "src";

    if (!createDirectories(buildDir)) {
        result.error = "failed to create build directory: " + buildDir.string();
        return result;
    }

    // 5. Copy library source to build/src
    if (fs::exists(libSrc)) {
        if (!copyRecursive(libSrc, buildSrcDir)) {
            result.error = "failed to copy source to build directory";
            return result;
        }
    } else {
        // If there is no library source directory, create a minimal CMakeLists.txt
        // for the injection library (the caller should have placed it there)
        result.error = "library source not found at: " + libSrc.string();
        return result;
    }

    // 6. Build
#ifdef _WIN32
    // On Windows, chain vcvars + qtenv + cmake via a batch script
    fs::path buildScript = buildDir / "build.bat";

    {
        std::ofstream script(buildScript);
        if (!script.is_open()) {
            result.error = "failed to create build script";
            return result;
        }

        // Write the build script
        script << "@echo off\r\n";
        script << "setlocal enabledelayedexpansion\r\n";
        script << "cd /d \"" << buildDir.string() << "\"\r\n";

        // Chain vcvars if provided
        if (!vcvars.empty() && fs::exists(vcvars)) {
            script << "call \"" << vcvars.string() << "\"\r\n";
            if (!vcvarsStr.empty()) {
                script << "if %errorlevel% neq 0 exit /b %errorlevel%\r\n";
            }
        }

        // Chain qtenv if provided
        if (!qt_env.empty() && fs::exists(qt_env)) {
            script << "call \"" << qt_env.string() << "\"\r\n";
            script << "if %errorlevel% neq 0 exit /b %errorlevel%\r\n";
        }

        // CMake configure
        script << "\"" << buildTool << "\" -S \"" << buildSrcDir.string()
               << "\" -B \"" << buildDir.string() << "/build\"";
        if (!resolvedGenerator.empty()) {
            script << " -G \"" << resolvedGenerator << "\"";
        }
        if (!resolvedArch.empty() && resolvedArch == "x86") {
            script << " -A Win32";
        } else if (!resolvedArch.empty() && resolvedArch == "x86_64") {
            script << " -A x64";
        } else if (!resolvedArch.empty() && resolvedArch == "arm64") {
            script << " -A ARM64";
        }
        script << " -DQT_MAJOR_VERSION=" << qt_major_version << "\r\n";

        script << "if %errorlevel% neq 0 exit /b %errorlevel%\r\n";

        // CMake build
        script << "\"" << buildTool << "\" --build \"" << buildDir.string()
               << "/build\" --config Release\r\n";
        script << "if %errorlevel% neq 0 exit /b %errorlevel%\r\n";

        script << "endlocal\r\n";
    }

    // Execute build script
    // Build a writable command-line buffer for CreateProcessW
    std::string cmdLine = "\"" + buildScript.string() + "\"";
    std::wstring cmdLineW = fs::path(cmdLine).wstring();
    // CreateProcessW modifies the buffer, so make a mutable copy
    std::vector<wchar_t> cmdBuf(cmdLineW.begin(), cmdLineW.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DWORD err = GetLastError();
        result.error = "failed to launch build script: error " +
                       std::to_string(err);
        return result;
    }

    // Wait for completion
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        result.error = "build failed with exit code " + std::to_string(exitCode);
        return result;
    }
#else
    // Linux/macOS: source env script if provided, then cmake
    fs::path buildDirPath = buildDir / "build";
    createDirectories(buildDirPath);

    // Build cmake configure command
    std::string cmd = "cd \"" + buildDir.string() + "\" && ";

    // Source Qt environment if provided
    if (!qt_env.empty() && fs::exists(qt_env)) {
        // If it's a script, source it before cmake
        std::string envStr = qt_env.string();
        if (envStr.find(".sh") != std::string::npos ||
            envStr.find(".bash") != std::string::npos) {
            cmd += "source \"" + envStr + "\" && ";
        }
    }

    cmd += "\"" + buildTool + "\" -S \"" + buildSrcDir.string() +
           "\" -B \"build\"";
    if (!resolvedGenerator.empty()) {
        cmd += " -G \"" + resolvedGenerator + "\"";
    }
    cmd += " -DQT_MAJOR_VERSION=" + std::to_string(qt_major_version) +
           " -DCMAKE_BUILD_TYPE=Release && ";

    cmd += "\"" + buildTool + "\" --build \"build\" --config Release";

    int ret = runProcess(cmd);
    if (ret != 0) {
        result.error = "build failed with exit code " + std::to_string(ret);
        return result;
    }
#endif

    // 7. Resolve output library path
    fs::path outputDir = buildDir / "build";

    // Common library output names
    std::vector<fs::path> candidates;
#ifdef _WIN32
    candidates.push_back(outputDir / "src" / "library" / "Release" / "qt-commander.dll");
    candidates.push_back(outputDir / "src" / "library" / "qt-commander.dll");
    candidates.push_back(outputDir / "src" / "library" / "Debug" / "qt-commander.dll");
#elif defined(__APPLE__)
    candidates.push_back(outputDir / "src" / "library" / "libqt-commander.dylib");
    candidates.push_back(outputDir / "src" / "library" / "Release" / "libqt-commander.dylib");
#else
    candidates.push_back(outputDir / "src" / "library" / "libqt-commander.so");
    candidates.push_back(outputDir / "src" / "library" / "Release" / "libqt-commander.so");
#endif

    fs::path foundLib;
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            foundLib = candidate;
            break;
        }
    }

    // If not found, search recursively (full scan as last resort)
    if (foundLib.empty() && fs::exists(outputDir)) {
        for (const auto& entry :
             fs::recursive_directory_iterator(outputDir)) {
            if (!entry.is_regular_file())
                continue;
            auto ext = entry.path().extension().string();
#ifdef _WIN32
            if (ext == ".dll" &&
                entry.path().filename().string().find("qt_commander") !=
                    std::string::npos) {
                foundLib = entry.path();
                break;
            }
#elif defined(__APPLE__)
            if (ext == ".dylib" &&
                entry.path().filename().string().find("qt_commander") !=
                    std::string::npos) {
                foundLib = entry.path();
                break;
            }
#else
            if (ext == ".so" &&
                entry.path().filename().string().find("qt_commander") !=
                    std::string::npos) {
                foundLib = entry.path();
                break;
            }
#endif
        }
    }

    if (foundLib.empty()) {
        result.error = "build completed but library not found in output";
        return result;
    }

    result.ok = true;
    result.library_path = foundLib;
    result.qt_version = std::to_string(qt_major_version);
    result.arch = resolvedArch;
    return result;
}
