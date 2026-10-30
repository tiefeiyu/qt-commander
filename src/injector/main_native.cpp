#include "injector.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

void usage()
{
    std::cerr << "usage: qtc-inject --pid <pid> --library <path>\n";
}

std::string jsonEscape(const std::string& value)
{
    std::string escaped;
    for (const char ch : value) {
        if (ch == '\\' || ch == '"')
            escaped += '\\';
        escaped += ch;
    }
    return escaped;
}

} // namespace

int main(int argc, char** argv)
{
    int pid = 0;
    std::filesystem::path library;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--pid" && i + 1 < argc) {
            pid = std::stoi(argv[++i]);
        } else if (arg == "--library" && i + 1 < argc) {
            library = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (pid <= 0 || library.empty()) {
        usage();
        return 2;
    }
    if (!std::filesystem::is_regular_file(library)) {
        std::cout << "{\"ok\":false,\"code\":\"E_INJECTION_FAILED\","
                     "\"message\":\"library not found\"}\n";
        return 4;
    }

    const InjectResult result = injectLibrary(pid, library);
    if (!result.ok) {
        std::string code = "E_INJECTION_FAILED";
        const std::string error = result.error;
        if (error.find("cannot attach") != std::string::npos ||
            error.find("OpenProcess") != std::string::npos ||
            error.find("ptrace") != std::string::npos)
            code = "E_PERMISSION";
        std::cout << "{\"ok\":false,\"code\":\"" << code
                  << "\",\"message\":\"" << jsonEscape(error) << "\"}\n";
        return code == "E_PERMISSION" ? 3 : 4;
    }
    std::cout << "{\"ok\":true,\"pid\":" << pid << "}\n";
    return 0;
}
