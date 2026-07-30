#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

// Minimal test framework — catch2 not required for basic validation
static int tests_run = 0;
static int tests_passed = 0;
#define TEST(name) void name(); name(); tests_run++
#define CHECK(cond) do { if (!(cond)) { printf("FAIL: %s:%d\n", __FILE__, __LINE__); return; } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { printf("FAIL: %s:%d (%d != %d)\n", __FILE__, __LINE__, (int)(a), (int)(b)); return; } } while(0)

#include "framing.h"

static void test_roundtrip_basic() {
    const char* json = R"({"jsonrpc":"2.0","id":1,"result":{}})";
    size_t len = strlen(json);
    auto frame = frame_encode(reinterpret_cast<const uint8_t*>(json), len);
    CHECK_EQ(frame.size(), 4 + len);

    uint32_t be;
    memcpy(&be, frame.data(), 4);
    uint32_t decoded_len = ntohl(be);
    CHECK_EQ(decoded_len, (uint32_t)len);
    tests_passed++;
}

static void test_decoder_incremental() {
    const char* json = R"({"jsonrpc":"2.0","method":"qt.snapshot","params":{},"id":2})";
    auto frame = frame_encode(reinterpret_cast<const uint8_t*>(json), strlen(json));

    FrameDecoder decoder;
    std::vector<uint8_t> out;

    for (size_t i = 0; i < frame.size() - 1; ++i) {
        auto result = decoder.feed(&frame[i], 1, out);
        if (i < frame.size() - 1) {
            CHECK(result == FrameDecoder::Result::NeedMore);
        }
    }
    auto result = decoder.feed(&frame.back(), 1, out);
    CHECK(result == FrameDecoder::Result::Complete);
    CHECK_EQ(memcmp(out.data(), json, strlen(json)), 0);
    tests_passed++;
}

static void test_decoder_rejects_zero_length() {
    uint32_t zero = 0;
    FrameDecoder decoder;
    std::vector<uint8_t> out;
    auto result = decoder.feed(reinterpret_cast<const uint8_t*>(&zero), 4, out);
    CHECK(result == FrameDecoder::Result::Error);
    tests_passed++;
}

static void test_decoder_rejects_excessive_length() {
    uint32_t huge = htonl(MAX_FRAME_PAYLOAD + 1);
    FrameDecoder decoder;
    std::vector<uint8_t> out;
    auto result = decoder.feed(reinterpret_cast<const uint8_t*>(&huge), 4, out);
    CHECK(result == FrameDecoder::Result::Error);
    tests_passed++;
}

static void test_cross_validate_python_format() {
    uint8_t expected_header[] = {0x00, 0x00, 0x00, 0x05};
    auto frame = frame_encode(reinterpret_cast<const uint8_t*>("hello"), 5);
    CHECK_EQ(memcmp(frame.data(), expected_header, 4), 0);
    CHECK_EQ(memcmp(frame.data() + 4, "hello", 5), 0);
    tests_passed++;
}

int main() {
    test_roundtrip_basic();
    test_decoder_incremental();
    test_decoder_rejects_zero_length();
    test_decoder_rejects_excessive_length();
    test_cross_validate_python_format();
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
