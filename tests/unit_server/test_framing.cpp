#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include "framing.h"

static int passed = 0, failed = 0;

#define TEST(name) do { std::cout << "  " << name << "... "; } while(0)
#define PASS() do { std::cout << "PASS\n"; passed++; } while(0)
#define FAIL(msg) do { std::cout << "FAIL: " << msg << "\n"; failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

static void test_encode_decode_roundtrip() {
    TEST("encode/decode roundtrip");
    const char* data = "hello world";
    auto frame = frame_encode(reinterpret_cast<const uint8_t*>(data), 11);
    CHECK(frame.size() == 6 + 11, "frame size mismatch");

    FrameDecoder dec;
    std::vector<uint8_t> out;
    auto r = dec.feed(frame.data(), frame.size(), out);
    CHECK(r == FrameResult::Complete, "not complete");
    CHECK(out.size() == 11, "payload size mismatch");
    CHECK(memcmp(out.data(), data, 11) == 0, "payload mismatch");
    PASS();
}

static void test_empty_payload() {
    TEST("empty payload (length=1) — edge case");
    const char* data = "x";
    auto frame = frame_encode(reinterpret_cast<const uint8_t*>(data), 1);

    FrameDecoder dec;
    std::vector<uint8_t> out;
    auto r = dec.feed(frame.data(), frame.size(), out);
    CHECK(r == FrameResult::Complete, "not complete");
    CHECK(out.size() == 1, "size mismatch");
    CHECK(out[0] == 'x', "content mismatch");
    PASS();
}

static void test_partial_header() {
    TEST("partial header");
    uint8_t bytes[3] = {0xCC, 0x01, 0x00};
    FrameDecoder dec;
    std::vector<uint8_t> out;
    auto r = dec.feed(bytes, 3, out);
    CHECK(r == FrameResult::NeedMore, "should need more for partial header");
    PASS();
}

static void test_partial_payload() {
    TEST("partial payload");
    auto frame = frame_encode(reinterpret_cast<const uint8_t*>("abcdefghij"), 10);

    FrameDecoder dec;
    std::vector<uint8_t> out;
    // Feed header + first 3 bytes of payload
    auto r = dec.feed(frame.data(), 6 + 3, out);
    CHECK(r == FrameResult::NeedMore, "should need more for partial payload");

    // Feed remaining 7 bytes
    r = dec.feed(frame.data() + 9, 7, out);
    CHECK(r == FrameResult::Complete, "should be complete after getting rest");
    CHECK(out.size() == 10, "size mismatch");
    PASS();
}

static void test_multiple_frames() {
    TEST("multiple frames in one buffer");
    const char* d1 = "first";
    const char* d2 = "second";
    auto f1 = frame_encode(reinterpret_cast<const uint8_t*>(d1), 5);
    auto f2 = frame_encode(reinterpret_cast<const uint8_t*>(d2), 6);

    // Concatenate frames
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), f1.begin(), f1.end());
    combined.insert(combined.end(), f2.begin(), f2.end());

    FrameDecoder dec;
    std::vector<uint8_t> out;

    auto r = dec.feed(combined.data(), combined.size(), out);
    CHECK(r == FrameResult::Complete, "first frame should be complete");
    CHECK(out.size() == 5, "first frame size");

    // Second frame should still be in buffer
    out.clear();
    r = dec.feed(nullptr, 0, out);
    CHECK(r == FrameResult::Complete, "second frame should be ready");
    CHECK(out.size() == 6, "second frame size");
    PASS();
}

static void test_magic_mismatch() {
    TEST("magic byte mismatch");
    uint8_t bad[] = {0xDD, 0x01, 0x00, 0x00, 0x00, 0x01, 'x'};
    FrameDecoder dec;
    std::vector<uint8_t> out;
    auto r = dec.feed(bad, 7, out);
    CHECK(r == FrameResult::Error, "should be error on bad magic");
    PASS();
}

static void test_version_mismatch() {
    TEST("version byte mismatch");
    uint8_t bad[] = {0xCC, 0x02, 0x00, 0x00, 0x00, 0x01, 'x'};
    FrameDecoder dec;
    std::vector<uint8_t> out;
    auto r = dec.feed(bad, 7, out);
    CHECK(r == FrameResult::Error, "should be error on bad version");
    PASS();
}

static void test_zero_length() {
    TEST("zero-length payload → error");
    uint8_t bad[] = {0xCC, 0x01, 0x00, 0x00, 0x00, 0x00};
    FrameDecoder dec;
    std::vector<uint8_t> out;
    auto r = dec.feed(bad, 6, out);
    CHECK(r == FrameResult::Error, "should be error on zero length");
    PASS();
}

static void test_exceed_max() {
    TEST("length > 16MB → error");
    // 16 MB + 1 = 0x01000001
    uint8_t bad[] = {0xCC, 0x01, 0x01, 0x00, 0x00, 0x01};
    FrameDecoder dec;
    std::vector<uint8_t> out;
    auto r = dec.feed(bad, 6, out);
    CHECK(r == FrameResult::Error, "should be error on oversized payload");
    PASS();
}

static void test_max_size_valid() {
    TEST("max size (16MB) length field is valid in header");
    uint32_t maxLen = 16 * 1024 * 1024;
    uint8_t hdr[6] = {0xCC, 0x01,
        (uint8_t)(maxLen >> 24), (uint8_t)(maxLen >> 16),
        (uint8_t)(maxLen >> 8), (uint8_t)(maxLen)};
    FrameDecoder dec;
    std::vector<uint8_t> out;
    auto r = dec.feed(hdr, 6, out);
    CHECK(r == FrameResult::NeedMore, "valid 16MB length should not trigger error");
    PASS();
}

static void test_reset() {
    TEST("reset clears decoder state");
    uint8_t bytes[3] = {0xCC, 0x01, 0x00};
    FrameDecoder dec;
    std::vector<uint8_t> out;
    dec.feed(bytes, 3, out);  // partial
    CHECK(dec.buffered() == 3, "should have 3 bytes buffered");
    dec.reset();
    CHECK(dec.buffered() == 0, "should be empty after reset");

    // Now it should accept a full frame
    auto frame = frame_encode(reinterpret_cast<const uint8_t*>("x"), 1);
    auto r = dec.feed(frame.data(), frame.size(), out);
    CHECK(r == FrameResult::Complete, "should complete after reset");
    PASS();
}

int main() {
    std::cout << "test_framing\n";
    test_encode_decode_roundtrip();
    test_empty_payload();
    test_partial_header();
    test_partial_payload();
    test_multiple_frames();
    test_magic_mismatch();
    test_version_mismatch();
    test_zero_length();
    test_exceed_max();
    test_max_size_valid();
    test_reset();

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
