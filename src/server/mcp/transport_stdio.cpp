#include "transport_stdio.h"

#include <iostream>
#include <string>
#include <exception>
#include <cstdio>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

// ---------------------------------------------------------------------------
// Set stdin/stdout to binary mode so raw bytes aren't mangled on Windows.
// ---------------------------------------------------------------------------
static void ensure_binary_io() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

StdioTransport::~StdioTransport() {
    stop();
}

bool StdioTransport::start(MessageHandler handler) {
    if (running_.exchange(true)) {
        return false; // Already running.
    }

    ensure_binary_io();

    reader_thread_ = std::thread([this, cb = std::move(handler)]() {
        readLoop(cb);
    });

    return true;
}

bool StdioTransport::send(const MCPMessage& msg) {
    try {
        std::cout << msg.data << "\n" << std::flush;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[mcp:stdio] error sending: " << e.what() << std::endl;
        return false;
    }
}

void StdioTransport::stop() {
    running_ = false;
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

void StdioTransport::readLoop(MessageHandler handler) {
    std::string line;

    while (running_) {
        if (!std::getline(std::cin, line)) {
            // EOF or error on stdin.
            break;
        }
        if (!running_) {
            break;
        }
        // Skip empty lines (possible phantom \r from \r\n after binary mode).
        if (line.empty()) {
            continue;
        }
        try {
            MCPMessage msg;
            msg.data = std::move(line);
            handler(msg);
        } catch (const std::exception& e) {
            std::cerr << "[mcp:stdio] error handling message: " << e.what()
                      << std::endl;
        }
    }

    running_ = false;
}
