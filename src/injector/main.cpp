#include "injector.h"
#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

static void print_usage() {
    std::cerr << "Usage:\n"
              << "  qt-injector <pid> <library_path> <port_file_path>\n"
              << "  qt-injector --eject <pid> <library_path>\n";
}

static void print_error_and_exit(int code, const std::string& msg) {
    std::cerr << "error(" << code << "): " << msg << "\n";
    std::exit(code);
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return 1; }

    std::string mode = argv[1];

    if (mode == "--eject") {
        if (argc != 4) { print_usage(); return 1; }
        int pid = std::stoi(argv[2]);
        if (pid <= 0) print_error_and_exit(1, "invalid PID: " + std::to_string(pid));
        fs::path lib_path(argv[3]);
        InjectResult r = ejectLibrary(pid, lib_path);
        if (!r.ok) print_error_and_exit(2, r.error);
        std::cout << "{\"status\":\"ejected\",\"pid\":" << pid << "}\n";
        return 0;
    }

    if (argc != 4) { print_usage(); return 1; }

    int pid = std::stoi(argv[1]);
    if (pid <= 0) print_error_and_exit(1, "invalid PID: " + std::to_string(pid));

    fs::path lib_path(argv[2]);
    fs::path port_file(argv[3]);

    if (!fs::exists(lib_path))
        print_error_and_exit(2, "library not found: " + lib_path.string());

    if (!isQtProcess(pid))
        print_error_and_exit(6, "PID " + std::to_string(pid) + " is not a Qt process");

    InjectResult inject_r = injectLibrary(pid, lib_path);
    if (!inject_r.ok) print_error_and_exit(2, inject_r.error);

    std::string token = generateToken();
    fs::path workspace = port_file.parent_path().parent_path().parent_path();

    uint16_t port = performInitHandshake(
        pid, lib_path, workspace.string(),
        port_file.parent_path().filename().string(), token, port_file);

    if (port == 0) print_error_and_exit(3, "qt_commander_init failed or timed out");

    std::ifstream pf(port_file);
    if (!pf) print_error_and_exit(4, "port file not found: " + port_file.string());

    std::string port_line, token_line;
    std::getline(pf, port_line);
    std::getline(pf, token_line);
    pf.close();

    token_line.erase(0, token_line.find_first_not_of(" \t\r\n"));
    token_line.erase(token_line.find_last_not_of(" \t\r\n") + 1);

    if (token_line != token)
        print_error_and_exit(5, "token mismatch in port file");

    fs::remove(port_file);

    std::cout << "{\"port\":" << port << ",\"token\":\"" << token << "\"}\n";
    return 0;
}
