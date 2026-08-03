// RPC server unit tests
//
// Tests:
//   - qt_parse_element_id (camelCase, snake_case, integer, missing)
//   - Screenshot dispatch path (handler.do_screenshot)
//
// A QApplication is required for QWidget creation.

#include <QApplication>
#include <QObject>
#include <QWidget>
#include <QVariant>
#include <QJsonObject>
#include <QJsonDocument>
#include <iostream>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#endif

#include "protocol/handler.h"
#include "rpc/parse_utils.h"
#include "core/element_map.h"

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
// Screenshot handler test via doScreenshot
// ============================================================================

static void test_do_screenshot_not_found()
{
    TEST("doScreenshot returns error when element not found");
    Handler handler(nullptr);

    ElementMap map;
    map.incrementEpoch();
    handler.setElementMap(&map);

    QVariant result = handler.doScreenshot(999, QString(), 0, nullptr, 1);
    QVariantMap m = result.toMap();
    CHECK(m[QStringLiteral("ok")].toBool() == false, "should fail for missing element");
    CHECK(m[QStringLiteral("error")].toMap()
             [QStringLiteral("code")].toInt() == -3,
          "error code should be -3");

    handler.setElementMap(nullptr);
    PASS();
}

static void test_do_screenshot_visible_widget()
{
    TEST("doScreenshot returns ok for valid visible widget");
    Handler handler(nullptr);

    ElementMap map;
    QWidget widget;
    widget.resize(400, 300);
    widget.setVisible(true);
    widget.setEnabled(true);
    map.insert(1, &widget);
    map.incrementEpoch();

    handler.setElementMap(&map);

    QVariant result = handler.doScreenshot(1, QString(), 5, nullptr, 1);
    QVariantMap m = result.toMap();
    CHECK(m[QStringLiteral("ok")].toBool() == true, "should succeed for valid widget");
    CHECK(m[QStringLiteral("seq")].toInt() == 5, "seq should be 5");

    handler.setElementMap(nullptr);
    PASS();
}

// Defined in handler_test_stubs.cpp: last dir passed to Screenshot::capture.
extern QString qtc_test_last_capture_dir;

static void test_do_screenshot_empty_dir_resolved()
{
    TEST("doScreenshot resolves an empty dir to a usable default");
    Handler handler(nullptr);

    ElementMap map;
    QWidget widget;
    widget.resize(400, 300);
    widget.setVisible(true);
    map.insert(1, &widget);
    map.incrementEpoch();
    handler.setElementMap(&map);

    qtc_test_last_capture_dir.clear();
    QVariant result = handler.doScreenshot(1, QString(), 6, nullptr, 1);
    QVariantMap m = result.toMap();
    CHECK(m[QStringLiteral("ok")].toBool() == true, "should succeed");
    // resolvePath("", "") must not forward "" (that wrote to the drive
    // root); it falls back to the current directory.
    CHECK(!qtc_test_last_capture_dir.isEmpty(),
          "capture dir must not be empty");

    handler.setElementMap(nullptr);
    PASS();
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[])
{
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    QApplication app(argc, argv);

    std::cout << "test_rpc_server\n";

    // parse_element_id tests
    test_parse_element_id_camel_case();
    test_parse_element_id_snake_case();
    test_parse_element_id_integer();
    test_parse_element_id_missing();
    test_parse_element_id_empty_string();
    test_parse_element_id_zero();
    test_parse_element_id_snake_preferred();

    // screenshot handler tests
    test_do_screenshot_not_found();
    test_do_screenshot_visible_widget();
    test_do_screenshot_empty_dir_resolved();

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
