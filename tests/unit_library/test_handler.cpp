// Handler unit tests
//
// Tests main-thread operation handler:
//   - makeError / makeOk response construction
//   - validateElement logic (null map, epoch mismatch, not found,
//     visibility, enabled, zero-size checks)
//   - doPing response
//
// validateElement and doGetProperty/doping were made public on the
// Handler class.  element_map_ is set via the public setElementMap().
// A QApplication is required for QWidget creation.

// --- Qt headers ---
#include <QApplication>
#include <QObject>
#include <QWidget>
#include <QVariant>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDateTime>
#include <iostream>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#endif

// Handler helpers (makeError/makeOk/validateElement) are now public
#include "protocol/handler.h"

#include "core/element_map.h"

static int passed = 0, failed = 0, skipped = 0;
#define TEST(n)  do { std::cout << "  " << n << "... "; } while(0)
#define PASS()   do { std::cout << "PASS\n"; passed++; } while(0)
#define FAIL(m)  do { std::cout << "FAIL: " << m << "\n"; failed++; } while(0)
#define CHECK(c,m) do { if(!(c)) { FAIL(m); return; } } while(0)

// ============================================================================
// makeOk tests
// ============================================================================

static void test_make_ok()
{
    TEST("makeOk returns proper QVariantMap");
    QVariantMap m = Handler::makeOk();
    CHECK(m.size() == 1, "makeOk should have exactly 1 entry");
    CHECK(m[QStringLiteral("ok")].toBool() == true,
          "makeOk should contain ok=true");
    PASS();
}

// ============================================================================
// makeError tests
// ============================================================================

static void test_make_error()
{
    TEST("makeError returns proper QVariantMap with error code");
    QVariantMap m = Handler::makeError(-42,
        QStringLiteral("Something went wrong"));
    CHECK(m[QStringLiteral("ok")].toBool() == false,
          "makeError ok should be false");

    QVariantMap err = m[QStringLiteral("error")].toMap();
    CHECK(err[QStringLiteral("code")].toInt() == -42,
          "error code should match");
    CHECK(err[QStringLiteral("message")].toString() ==
              QStringLiteral("Something went wrong"),
          "error message should match");
    PASS();
}

static void test_make_error_multiple_codes()
{
    TEST("makeError handles various error codes");
    {
        QVariantMap m = Handler::makeError(-1,
            QStringLiteral("Element map not initialized"));
        CHECK(m[QStringLiteral("error")].toMap()
                 [QStringLiteral("code")].toInt() == -1,
              "error code -1");
    }
    {
        QVariantMap m = Handler::makeError(-6,
            QStringLiteral("Element has zero size"));
        CHECK(m[QStringLiteral("error")].toMap()
                 [QStringLiteral("code")].toInt() == -6,
              "error code -6");
    }
    PASS();
}

// ============================================================================
// validateElement tests (require QApplication)
// ============================================================================

// -- validateElement: null element_map ---------------------------------------
static void test_validate_element_no_map()
{
    TEST("validateElement returns nullptr / error -1 for null element_map");
    Handler handler(nullptr);
    // element_map_ is nullptr after Handler(nullptr)

    QVariantMap result;
    QObject* obj = handler.validateElement(1, 0, result);
    CHECK(obj == nullptr, "should return nullptr");
    CHECK(result[QStringLiteral("ok")].toBool() == false,
          "result should have ok=false");
    CHECK(result[QStringLiteral("error")].toMap()
             [QStringLiteral("code")].toInt() == -1,
          "error code should be -1");
    PASS();
}

// -- validateElement: epoch mismatch -----------------------------------------
static void test_validate_element_epoch_mismatch()
{
    TEST("validateElement returns error -2 on epoch mismatch");
    Handler handler(nullptr);

    ElementMap map;
    QWidget widget;
    widget.setObjectName(QStringLiteral("w"));
    widget.resize(100, 100);
    widget.setVisible(true);
    map.insert(1, &widget);
    // epoch = 0, capturedEpoch = 42 -> mismatch

    handler.setElementMap(&map);

    QVariantMap result;
    QObject* obj = handler.validateElement(1, 42, result);
    CHECK(obj == nullptr, "should return nullptr on epoch mismatch");
    CHECK(result[QStringLiteral("error")].toMap()
             [QStringLiteral("code")].toInt() == -2,
          "error code should be -2");

    // Restore
    handler.setElementMap(nullptr);
    PASS();
}

// -- validateElement: element not found --------------------------------------
static void test_validate_element_not_found()
{
    TEST("validateElement returns error -3 for non-existent element ID");
    Handler handler(nullptr);

    ElementMap map;
    map.incrementEpoch();  // epoch = 1
    handler.setElementMap(&map);

    QVariantMap result;
    QObject* obj = handler.validateElement(999, 1, result);
    CHECK(obj == nullptr, "should return nullptr for missing element");
    CHECK(result[QStringLiteral("error")].toMap()
             [QStringLiteral("code")].toInt() == -3,
          "error code should be -3");

    handler.setElementMap(nullptr);
    PASS();
}

// -- validateElement: valid element ------------------------------------------
static void test_validate_element_valid()
{
    TEST("validateElement returns valid QObject* for valid element");
    Handler handler(nullptr);

    ElementMap map;
    QWidget widget;
    widget.setObjectName(QStringLiteral("testWidget"));
    widget.resize(100, 100);
    widget.setVisible(true);   // default, but be explicit
    widget.setEnabled(true);   // default, but be explicit
    map.insert(1, &widget);
    map.incrementEpoch();      // epoch = 1

    handler.setElementMap(&map);

    QVariantMap result;
    QObject* obj = handler.validateElement(1, 1, result);
    CHECK(obj != nullptr, "should return non-null for valid element");
    CHECK(obj == &widget, "should return the correct widget pointer");
    CHECK(static_cast<QWidget*>(obj)->objectName() ==
              QStringLiteral("testWidget"),
          "objectName should match");

    handler.setElementMap(nullptr);
    PASS();
}

// -- validateElement: widget not visible -------------------------------------
static void test_validate_element_not_visible()
{
    TEST("validateElement returns error -4 for non-visible widget");
    Handler handler(nullptr);

    ElementMap map;
    QWidget widget;
    widget.resize(100, 100);
    widget.setVisible(false);  // explicitly hidden
    map.insert(1, &widget);
    map.incrementEpoch();

    handler.setElementMap(&map);

    QVariantMap result;
    QObject* obj = handler.validateElement(1, 1, result);
    CHECK(obj == nullptr, "should return nullptr for non-visible widget");
    CHECK(result[QStringLiteral("error")].toMap()
             [QStringLiteral("code")].toInt() == -4,
          "error code should be -4");

    handler.setElementMap(nullptr);
    PASS();
}

// -- validateElement: widget not enabled -------------------------------------
static void test_validate_element_not_enabled()
{
    TEST("validateElement returns error -5 for disabled widget");
    Handler handler(nullptr);

    ElementMap map;
    QWidget widget;
    widget.resize(100, 100);
    widget.setVisible(true);
    widget.setEnabled(false);  // disabled
    map.insert(1, &widget);
    map.incrementEpoch();

    handler.setElementMap(&map);

    QVariantMap result;
    QObject* obj = handler.validateElement(1, 1, result);
    CHECK(obj == nullptr, "should return nullptr for disabled widget");
    CHECK(result[QStringLiteral("error")].toMap()
             [QStringLiteral("code")].toInt() == -5,
          "error code should be -5");

    handler.setElementMap(nullptr);
    PASS();
}

// -- validateElement: widget zero size ---------------------------------------
static void test_validate_element_zero_size()
{
    TEST("validateElement returns error -6 for zero-size widget");
    Handler handler(nullptr);

    ElementMap map;
    QWidget widget;
    widget.setFixedSize(0, 0);  // force zero size
    widget.setVisible(true);
    widget.setEnabled(true);
    map.insert(1, &widget);
    map.incrementEpoch();

    handler.setElementMap(&map);

    QVariantMap result;
    QObject* obj = handler.validateElement(1, 1, result);
    CHECK(obj == nullptr, "should return nullptr for zero-size widget");
    CHECK(result[QStringLiteral("error")].toMap()
             [QStringLiteral("code")].toInt() == -6,
          "error code should be -6");

    handler.setElementMap(nullptr);
    PASS();
}

// -- validateElement through doGetProperty -----------------------------------
static void test_validate_element_through_do_get_property()
{
    TEST("validateElement via doGetProperty public method");
    Handler handler(nullptr);

    ElementMap map;
    QWidget widget;
    widget.setObjectName(QStringLiteral("myObj"));
    widget.resize(200, 50);
    widget.setVisible(true);
    widget.setEnabled(true);
    map.insert(42, &widget);
    map.incrementEpoch();      // epoch = 1

    handler.setElementMap(&map);

    // Success path
    QVariant okResult = handler.doGetProperty(42,
        QStringLiteral("objectName"), nullptr, 1);
    QVariantMap okMap = okResult.toMap();
    CHECK(okMap[QStringLiteral("ok")].toBool() == true,
          "doGetProperty should succeed");
    CHECK(okMap[QStringLiteral("value")].toString() ==
              QStringLiteral("myObj"),
          "objectName property should match");

    // Failure: wrong epoch
    QVariant epochResult = handler.doGetProperty(42,
        QStringLiteral("objectName"), nullptr, 999);
    QVariantMap epochMap = epochResult.toMap();
    CHECK(epochMap[QStringLiteral("ok")].toBool() == false,
          "should fail on epoch mismatch");
    CHECK(epochMap[QStringLiteral("error")].toMap()
             [QStringLiteral("code")].toInt() == -2,
          "error code -2 for epoch mismatch");

    // Failure: missing element
    QVariant missingResult = handler.doGetProperty(999,
        QStringLiteral("objectName"), nullptr, 1);
    QVariantMap missingMap = missingResult.toMap();
    CHECK(missingMap[QStringLiteral("ok")].toBool() == false,
          "should fail for missing element");
    CHECK(missingMap[QStringLiteral("error")].toMap()
             [QStringLiteral("code")].toInt() == -3,
          "error code -3 for missing element");

    handler.setElementMap(nullptr);
    PASS();
}

// ============================================================================
// doPing tests
// ============================================================================

static void test_ping()
{
    TEST("doPing returns valid response with timestamp");
    Handler handler(nullptr);

    QVariant result = handler.doPing(nullptr);
    QVariantMap map = result.toMap();

    CHECK(map[QStringLiteral("ok")].toBool() == true,
          "ping should have ok=true");
    CHECK(map[QStringLiteral("message")].toString() ==
              QStringLiteral("pong"),
          "ping message should be 'pong'");
    CHECK(map.contains(QStringLiteral("timestamp")),
          "ping should include 'timestamp' key");
    QString ts = map[QStringLiteral("timestamp")].toString();
    CHECK(!ts.isEmpty(), "timestamp should not be empty");
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

    std::cout << "test_handler\n";
    test_make_ok();
    test_make_error();
    test_make_error_multiple_codes();

    // Instance tests (validateElement, doGetProperty, doPing)
    test_validate_element_no_map();
    test_validate_element_epoch_mismatch();
    test_validate_element_not_found();
    test_validate_element_valid();
    test_validate_element_not_visible();
    test_validate_element_not_enabled();
    test_validate_element_zero_size();
    test_validate_element_through_do_get_property();
    test_ping();

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
