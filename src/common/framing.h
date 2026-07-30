#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

// Length-prefixed frame protocol: magic(0xCC) + version(0x01) + 4-byte-BE-length + payload.
// Max payload: 16 MB. Length=0 is a protocol error.

constexpr uint8_t FRAME_MAGIC = 0xCC;
constexpr uint8_t FRAME_VERSION = 0x01;
constexpr size_t FRAME_HEADER_SIZE = 6;
constexpr size_t FRAME_MAX_PAYLOAD = 16 * 1024 * 1024;

// Encode a payload into a framed message. Returns complete frame (header + payload).
std::vector<uint8_t> frame_encode(const uint8_t* payload, size_t len);
inline std::vector<uint8_t> frame_encode(const std::vector<uint8_t>& payload) {
    return frame_encode(payload.data(), payload.size());
}

enum class FrameResult { NeedMore, Complete, Error };

// Streaming frame decoder. Feed bytes incrementally; returns Complete when a full frame is ready.
class FrameDecoder {
public:
    // Feed len bytes. Returns NeedMore (keep feeding), Complete (frame ready in 'out'), or Error.
    FrameResult feed(const uint8_t* data, size_t len, std::vector<uint8_t>& out);

    // Reset decoder state for a new connection.
    void reset();

    // Check how many bytes are currently buffered (for debugging).
    size_t buffered() const { return buffer_.size(); }

private:
    std::vector<uint8_t> buffer_;
    enum State { WaitingHeader, WaitingPayload };
    State state_ = WaitingHeader;
    uint32_t payload_len_ = 0;
};
