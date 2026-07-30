#pragma once
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

struct BuildResult {
    bool ok;
    std::string error;
    fs::path library_path;  // Path to compiled DLL/SO/DYLIB
    std::string qt_version;
    std::string arch;
    std::string build_key;
};

class BuildManager {
public:
    explicit BuildManager(const fs::path& workspace);

    // Build the injection library for the given Qt environment.
    // qt_env: path to qtenv2.bat (Windows) or Qt install prefix (Linux/macOS)
    // vcvars: path to vcvars64.bat (Windows only)
    // arch: target architecture (auto-detected if empty)
    // generator: CMake generator (auto-detected if empty)
    // qt_major_version: 5 or 6
    BuildResult build(const fs::path& qt_env, const fs::path& vcvars,
                      const std::string& arch, const std::string& generator,
                      int qt_major_version);

private:
    fs::path workspace_;
    std::string detectBuildTool() const;
};
