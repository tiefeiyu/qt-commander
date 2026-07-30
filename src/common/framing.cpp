#include "framing.h"
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// frame_encode
// ---------------------------------------------------------------------------
std::vector<uint8_t> frame_encode(const uint8_t* payload, size_t len) {
    std::vector<uint8_t> frame;
    frame.reserve(FRAME_HEADER_SIZE + len);

    // Header: magic + version
    frame.push_back(FRAME_MAGIC);
    frame.push_back(FRAME_VERSION);

    // 4-byte big-endian payload length
    frame.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    frame.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(len & 0xFF));

    // Payload bytes
    frame.insert(frame.end(), payload, payload + len);

    return frame;
}

// ---------------------------------------------------------------------------
// FrameDecoder
// ---------------------------------------------------------------------------

void FrameDecoder::reset() {
    buffer_.clear();
    state_ = WaitingHeader;
    payload_len_ = 0;
}

FrameResult FrameDecoder::feed(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
    // Append incoming bytes to the internal buffer.
    if (len > 0) {
        buffer_.insert(buffer_.end(), data, data + len);
    }

    // Process as many complete frames as possible from the buffer.
    while (true) {
        if (state_ == WaitingHeader) {
            // Not enough data for a full header yet.
            if (buffer_.size() < FRAME_HEADER_SIZE) {
                return FrameResult::NeedMore;
            }

            // Validate magic byte.
            if (buffer_[0] != FRAME_MAGIC) {
                reset();
                return FrameResult::Error;
            }

            // Validate version byte.
            if (buffer_[1] != FRAME_VERSION) {
                reset();
                return FrameResult::Error;
            }

            // Parse 4-byte big-endian length field.
            payload_len_ = (static_cast<uint32_t>(buffer_[2]) << 24) |
                           (static_cast<uint32_t>(buffer_[3]) << 16) |
                           (static_cast<uint32_t>(buffer_[4]) << 8)  |
                           (static_cast<uint32_t>(buffer_[5]));

            // Validate length: zero payload or exceeding the maximum is a protocol error.
            if (payload_len_ == 0 || payload_len_ > FRAME_MAX_PAYLOAD) {
                reset();
                return FrameResult::Error;
            }

            // Consume the header bytes from the buffer.
            buffer_.erase(buffer_.begin(), buffer_.begin() + FRAME_HEADER_SIZE);
            state_ = WaitingPayload;
        }

        if (state_ == WaitingPayload) {
            // Not enough payload data yet.
            if (buffer_.size() < payload_len_) {
                return FrameResult::NeedMore;
            }

            // Extract the complete payload into 'out'.
            out.assign(buffer_.begin(), buffer_.begin() + payload_len_);

            // Consume the payload bytes from the buffer.
            buffer_.erase(buffer_.begin(), buffer_.begin() + payload_len_);

            // Reset state for the next frame.
            state_ = WaitingHeader;
            payload_len_ = 0;

            // If there is still data left in the buffer after consuming this
            // frame, we return Complete now and leave the remainder for the
            // next call to feed().  The caller should keep calling feed()
            // (possibly with an empty data argument) until NeedMore is
            // returned, at which point more bytes from the wire are needed.
            return FrameResult::Complete;
        }
    }
}
