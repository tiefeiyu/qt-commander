#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <stdexcept>

// Platform-neutral byte-order conversion
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

static constexpr size_t FRAME_HEADER_SIZE = 4;  // 4-byte BE length
static constexpr size_t MAX_FRAME_PAYLOAD = 16 * 1024 * 1024; // 16 MB

// Encode payload into a framed message: [4-byte BE length][payload]
inline std::vector<uint8_t> frame_encode(const uint8_t* data, size_t len) {
    if (len == 0 || len > MAX_FRAME_PAYLOAD) {
        throw std::invalid_argument("frame_encode: invalid payload length");
    }
    std::vector<uint8_t> out(FRAME_HEADER_SIZE + len);
    uint32_t be = htonl(static_cast<uint32_t>(len));
    std::memcpy(out.data(), &be, FRAME_HEADER_SIZE);
    std::memcpy(out.data() + FRAME_HEADER_SIZE, data, len);
    return out;
}

// Decode frames from a byte stream. Feed bytes incrementally.
class FrameDecoder {
public:
    enum class Result { NeedMore, Complete, Error };

    Result feed(const uint8_t* data, size_t len, std::vector<uint8_t>& out_frame) {
        buffer_.insert(buffer_.end(), data, data + len);
        return try_decode(out_frame);
    }

    void reset() { buffer_.clear(); state_ = State::ReadingHeader; }

private:
    enum class State { ReadingHeader, ReadingPayload };
    State state_ = State::ReadingHeader;
    std::vector<uint8_t> buffer_;
    size_t payload_len_ = 0;

    Result try_decode(std::vector<uint8_t>& out_frame) {
        while (true) {
            if (state_ == State::ReadingHeader) {
                if (buffer_.size() < FRAME_HEADER_SIZE) return Result::NeedMore;
                uint32_t be;
                std::memcpy(&be, buffer_.data(), FRAME_HEADER_SIZE);
                payload_len_ = ntohl(be);
                if (payload_len_ == 0 || payload_len_ > MAX_FRAME_PAYLOAD) {
                    reset();
                    return Result::Error;
                }
                buffer_.erase(buffer_.begin(), buffer_.begin() + FRAME_HEADER_SIZE);
                state_ = State::ReadingPayload;
            }
            if (state_ == State::ReadingPayload) {
                if (buffer_.size() < payload_len_) return Result::NeedMore;
                out_frame.assign(buffer_.begin(), buffer_.begin() + payload_len_);
                buffer_.erase(buffer_.begin(), buffer_.begin() + payload_len_);
                state_ = State::ReadingHeader;
                return Result::Complete;
            }
        }
    }
};
