#pragma once
#include <string>
#include <functional>
#include <vector>

// MCP transport abstraction. Supports stdio and HTTP/SSE.

struct MCPMessage {
    std::string data;  // Raw JSON-RPC message body
};

class MCPTransport {
public:
    using MessageHandler = std::function<void(const MCPMessage&)>;

    virtual ~MCPTransport() = default;
    virtual bool start(MessageHandler handler) = 0;
    virtual bool send(const MCPMessage& msg) = 0;
    virtual void stop() = 0;
};
