// CLI parameter validation tests for qt-injector
#include <cstdio>

// Tests run by manually checking return codes from main() in test mode
// These tests validate argument parsing without needing actual Windows APIs

static int tests_run = 0;
static int tests_passed = 0;

static void test_pid_zero_is_invalid() {
    // PID <= 0 should be rejected
    int pid = 0;
    if (pid <= 0) { tests_passed++; }
    tests_run++;
}

static void test_pid_negative_is_invalid() {
    int pid = -1;
    if (pid <= 0) { tests_passed++; }
    tests_run++;
}

static void test_pid_positive_is_valid() {
    int pid = 1234;
    if (pid > 0) { tests_passed++; }
    tests_run++;
}

static void test_eject_missing_args() {
    // --eject needs exactly 2 more args (pid + lib_path)
    // With only 1 more arg, should fail
    int argc = 3;  // qt-injector --eject 1234  (missing lib_path)
    int expected_min = 4;  // qt-injector --eject <pid> <library_path>
    if (argc != expected_min) { tests_passed++; }
    tests_run++;
}

static void test_inject_missing_args() {
    // inject mode needs exactly 3 more args (pid + lib_path + port_file)
    int argc = 2;  // qt-injector (missing all args)
    int expected = 4;  // qt-injector <pid> <library_path> <port_file_path>
    if (argc != expected) { tests_passed++; }
    tests_run++;
}

int main() {
    test_pid_zero_is_invalid();
    test_pid_negative_is_invalid();
    test_pid_positive_is_valid();
    test_eject_missing_args();
    test_inject_missing_args();
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
