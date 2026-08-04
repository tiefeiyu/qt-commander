// parse_utils unit tests
//
// Tests:
//   - qt_parse_element_id (camelCase, snake_case, integer, missing)

#include <QCoreApplication>
#include <QObject>
#include <QVariant>
#include <QJsonObject>
#include <QJsonDocument>
#include <iostream>
#include <cstdint>

#if defined(_WIN32) && defined(_MSC_VER)
// MSVC-only: crtdbg/_set_abort_behavior are UCRT-only (no MinGW msvcrt)
#include <windows.h>
#include <crtdbg.h>
#endif

#include "rpc/parse_utils.h"

static int passed = 0, failed = 0, skipped = 0;
#define TEST(n)  do { std::cout << "  " << n << "... "; } while(0)
#define PASS()   do { std::cout << "PASS\n"; passed++; } while(0)
#define FAIL(m)  do { std::cout << "FAIL: " << m << "\n"; failed++; } while(0)
#define CHECK(c,m) do { if(!(c)) { FAIL(m); return; } } while(0)

// ============================================================================
// qt_parse_element_id tests
// ============================================================================

static void test_parse_element_id_camel_case()
{
    TEST("qt_parse_element_id accepts 'elementId' (camelCase)");
    QJsonObject params;
    params[QStringLiteral("elementId")] = QStringLiteral("42");
    uint64_t id = 0;
    CHECK(qt_parse_element_id(params, id), "should return true");
    CHECK(id == 42, "id should be 42");
    PASS();
}

static void test_parse_element_id_snake_case()
{
    TEST("qt_parse_element_id accepts 'element_id' (snake_case)");
    QJsonObject params;
    params[QStringLiteral("element_id")] = QStringLiteral("7");
    uint64_t id = 0;
    CHECK(qt_parse_element_id(params, id), "should return true for snake_case");
    CHECK(id == 7, "id should be 7");
    PASS();
}

static void test_parse_element_id_integer()
{
    TEST("qt_parse_element_id accepts integer values directly");
    QJsonObject params;
    params[QStringLiteral("element_id")] = 42;  // int, not string
    uint64_t id = 0;
    CHECK(qt_parse_element_id(params, id), "should return true for integer");
    CHECK(id == 42, "id should be 42 from integer");
    PASS();
}

static void test_parse_element_id_missing()
{
    TEST("qt_parse_element_id returns false when element_id is missing");
    QJsonObject params;
    uint64_t id = 999;
    CHECK(!qt_parse_element_id(params, id), "should return false");
    CHECK(id == 999, "id should be unchanged");
    PASS();
}

static void test_parse_element_id_empty_string()
{
    TEST("qt_parse_element_id returns false for empty string");
    QJsonObject params;
    params[QStringLiteral("elementId")] = QStringLiteral("");
    uint64_t id = 0;
    CHECK(!qt_parse_element_id(params, id), "should return false for empty string");
    PASS();
}

static void test_parse_element_id_zero()
{
    TEST("qt_parse_element_id returns true for id=0");
    QJsonObject params;
    params[QStringLiteral("element_id")] = 0;
    uint64_t id = 999;
    CHECK(!qt_parse_element_id(params, id), "should return false for id=0");
    PASS();
}

static void test_parse_element_id_snake_preferred()
{
    TEST("qt_parse_element_id prefers camelCase 'elementId' when both present");
    QJsonObject params;
    params[QStringLiteral("elementId")] = QStringLiteral("100");
    params[QStringLiteral("element_id")] = QStringLiteral("200");
    uint64_t id = 0;
    CHECK(qt_parse_element_id(params, id), "should return true");
    CHECK(id == 100, "should use camelCase value (100)");
    PASS();
}


// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[])
{
#if defined(_WIN32) && defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    QCoreApplication app(argc, argv);

    std::cout << "test_parse_utils\n";

    test_parse_element_id_camel_case();
    test_parse_element_id_snake_case();
    test_parse_element_id_integer();
    test_parse_element_id_missing();
    test_parse_element_id_empty_string();
    test_parse_element_id_zero();
    test_parse_element_id_snake_preferred();

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
