// Exponential backoff polling algorithm test
#include <cstdio>
#include <algorithm>
#include <cstdint>

int main() {
    int tests = 0, passed = 0;

    // Verify: 50ms → 100ms → 200ms → 400ms → 800ms → 1600ms → 3200ms
    int expected[] = {50, 100, 200, 400, 800, 1600, 3200};
    int delay = 50;
    for (int i = 0; i < 7; ++i) {
        tests++;
        if (delay == expected[i]) passed++;
        if (i > 0) delay = (std::min)(delay * 2, 3200);
    }

    // Verify clamping at 3200ms
    delay = 3200;
    delay = (std::min)(delay * 2, 3200);
    tests++;
    if (delay == 3200) passed++;

    printf("%d/%d tests passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
