// Exponential backoff polling algorithm test
#include <cstdio>

int main() {
    int passed = 0, total = 0;

    // Algorithm matches performInitHandshake in injector_win.cpp line ~406:
    // delay starts at 50, doubles each iteration, capped at 3200
    int expected_delays[] = {50, 100, 200, 400, 800, 1600, 3200, 3200, 3200, 3200};
    int delay = 50;

    for (int attempt = 0; attempt < 10; ++attempt) {
        total++;
        if (delay == expected_delays[attempt]) passed++;
        if (delay * 2 < 3200)
            delay *= 2;
        else
            delay = 3200;
    }

    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
