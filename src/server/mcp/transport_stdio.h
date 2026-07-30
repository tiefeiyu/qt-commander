#pragma once
#include "transport.h"

#include <atomic>
#include <thread>

// stdio transport: reads JSON-RPC from stdin, writes to stdout.
// Stderr is reserved for server logging.
class StdioTransport : public MCPTransport {
public:
    StdioTransport() = default;
    ~StdioTransport() override;

    bool start(MessageHandler handler) override;
    bool send(const MCPMessage& msg) override;
    void stop() override;

private:
    void readLoop(MessageHandler handler);

    std::atomic<bool> running_{false};
    std::thread reader_thread_;
};
