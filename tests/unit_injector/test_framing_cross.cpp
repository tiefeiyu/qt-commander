// Frame protocol cross-validation (C++ vs Python)
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include "framing.h"

static int passed = 0, total = 0;
#define CHECK(cond, msg) do { total++; if (cond) { passed++; } else { printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); } } while(0)

void test_roundtrip() {
    const char* json = R"({"jsonrpc":"2.0","id":1,"result":{}})";
    size_t len = strlen(json);
    auto frame = frame_encode(reinterpret_cast<const uint8_t*>(json), len);
    CHECK(frame.size() == 4 + len, "frame size correct");
    uint32_t be;
    memcpy(&be, frame.data(), 4);
    CHECK(ntohl(be) == (uint32_t)len, "big-endian length decoded correctly");
}

void test_decoder_incremental() {
    const char* json = R"({"jsonrpc":"2.0","method":"qt.snapshot","params":{},"id":2})";
    auto frame = frame_encode(reinterpret_cast<const uint8_t*>(json), strlen(json));
    FrameDecoder decoder;
    std::vector<uint8_t> out;
    for (size_t i = 0; i < frame.size() - 1; ++i) {
        CHECK(decoder.feed(&frame[i], 1, out) == FrameDecoder::Result::NeedMore, "need more");
    }
    CHECK(decoder.feed(&frame.back(), 1, out) == FrameDecoder::Result::Complete, "complete");
    CHECK(memcmp(out.data(), json, strlen(json)) == 0, "payload matches");
}

void test_reject_zero() {
    uint32_t zero = 0;
    FrameDecoder d; std::vector<uint8_t> o;
    CHECK(d.feed(reinterpret_cast<const uint8_t*>(&zero), 4, o) == FrameDecoder::Result::Error, "reject zero length");
}

void test_reject_excessive() {
    uint32_t huge = htonl(MAX_FRAME_PAYLOAD + 1);
    FrameDecoder d; std::vector<uint8_t> o;
    CHECK(d.feed(reinterpret_cast<const uint8_t*>(&huge), 4, o) == FrameDecoder::Result::Error, "reject excessive");
}

void test_cross_python() {
    uint8_t expected[] = {0x00, 0x00, 0x00, 0x05};
    auto frame = frame_encode(reinterpret_cast<const uint8_t*>("hello"), 5);
    CHECK(memcmp(frame.data(), expected, 4) == 0, "header matches Python struct.pack('!I', 5)");
    CHECK(memcmp(frame.data() + 4, "hello", 5) == 0, "payload intact");
}

int main() {
    test_roundtrip();
    test_decoder_incremental();
    test_reject_zero();
    test_reject_excessive();
    test_cross_python();
    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
