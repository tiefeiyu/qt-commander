// Exponential backoff polling algorithm test
#include <cstdio>

int main() {
    int passed = 0, total = 0;

    int expected[] = {50, 100, 200, 400, 800, 1600, 3200};
    int delay = 50;
    for (int i = 0; i < 7; ++i) {
        total++;
        printf("i=%d delay=%d expected=%d\n", i, delay, expected[i]);
        if (delay == expected[i]) passed++;
        if (delay * 2 < 3200) delay *= 2; else delay = 3200;
    }

    delay = 3200;
    if (delay * 2 < 3200) delay *= 2; else delay = 3200;
    total++;
    printf("clamp: delay=%d (expected 3200)\n", delay);
    if (delay == 3200) passed++;

    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
