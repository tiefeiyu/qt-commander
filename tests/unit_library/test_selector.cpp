// ElementSelector unit tests
//
// Tests element finding with various JSON queries against a QWidget tree:
//   - type (exact className match)
//   - type_inherits (IS-A via superClass chain)
//   - text / text_contains
//   - object_name
//   - window_title
//   - depth (exact vs deep)
//   - limit
//   - AND combination
//   - empty query (all descendants)
//   - no matches (empty result)

#include "selector/selector.h"
#include <QApplication>
#include <QMainWindow>
#include <QPushButton>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QWidget>
#include <QJsonObject>
#include <QHash>
#include <iostream>
#include <cstdint>

static int passed = 0, failed = 0;
#define TEST(n)  do { std::cout << "  " << n << "... "; } while(0)
#define PASS()   do { std::cout << "PASS\n"; passed++; } while(0)
#define FAIL(m)  do { std::cout << "FAIL: " << m << "\n"; failed++; } while(0)
#define CHECK(c,m) do { if(!(c)) { FAIL(m); return; } } while(0)
#define CHECK_SZ(a,exp,m) do { \
    int _sz = (a).size(); \
    if (_sz != (exp)) { FAIL(m << " expected " << (exp) << " results, got " << _sz); return; } \
} while(0)

// ---------------------------------------------------------------------------
// Test fixture: builds a known widget tree and element map
//
// Hierarchy:
//   QMainWindow "TestWindow"           (id: 1)
//     QWidget central                  (id: 5)
//       QPushButton "OK"        btnOK  (id: 2)
//       QPushButton "Cancel"           (id: 3)
//       QLineEdit           emailEdit  (id: 4)
// ---------------------------------------------------------------------------
struct Fixture {
    QMainWindow*   mainWin;
    QPushButton*   btnOk;
    QPushButton*   btnCancel;
    QLineEdit*     emailEdit;
    QWidget*       central;
    QHash<uint64_t, QObject*> map;

    Fixture() {
        mainWin   = new QMainWindow();
        mainWin->setWindowTitle(QStringLiteral("TestWindow"));

        btnOk     = new QPushButton(QStringLiteral("OK"));
        btnOk->setObjectName(QStringLiteral("btnOK"));

        btnCancel = new QPushButton(QStringLiteral("Cancel"));

        emailEdit = new QLineEdit();
        emailEdit->setPlaceholderText(QStringLiteral("Email"));
        emailEdit->setObjectName(QStringLiteral("emailEdit"));

        central   = new QWidget();
        auto* lay = new QVBoxLayout(central);
        lay->addWidget(btnOk);
        lay->addWidget(btnCancel);
        lay->addWidget(emailEdit);
        mainWin->setCentralWidget(central);

        map[1] = mainWin;
        map[2] = btnOk;
        map[3] = btnCancel;
        map[4] = emailEdit;
        map[5] = central;
    }

    ~Fixture() {
        delete mainWin;   // Qt parent-child chain handles the rest
    }

    // Helper: build a query with a default ancestor_id=1 (mainWin)
    QJsonObject query(const QJsonObject& extra) const {
        QJsonObject q = extra;
        if (!q.contains(QStringLiteral("ancestor_id")))
            q[QStringLiteral("ancestor_id")] = 1.0;
        return q;
    }

    // Run a find and return the result vector
    QVector<SelectorResult> find(const QJsonObject& queryObj) const {
        return ElementSelector::find(queryObj, map);
    }
};

// ============================================================================
// Tests
// ============================================================================

// ---------------------------------------------------------------------------
// 1. Find by exact type name (className)
// ---------------------------------------------------------------------------
static void test_find_by_type()
{
    TEST("find by exact type name (className)");
    Fixture fx;

    auto results = fx.find(fx.query({
        {QStringLiteral("type"), QStringLiteral("QPushButton")}
    }));

    CHECK_SZ(results, 2, "QPushButton query");
    CHECK(results[0].id == 2 || results[1].id == 2, "result should include btnOk (id=2)");
    CHECK(results[0].id == 3 || results[1].id == 3, "result should include btnCancel (id=3)");
    PASS();
}

// ---------------------------------------------------------------------------
// 2. Find by type_inherits (IS-A via superClass chain)
// ---------------------------------------------------------------------------
static void test_find_by_type_inherits()
{
    TEST("find by type_inherits (IS-A check)");
    Fixture fx;

    // Both QPushButtons inherit QAbstractButton
    auto results = fx.find(fx.query({
        {QStringLiteral("type_inherits"), QStringLiteral("QAbstractButton")}
    }));

    CHECK_SZ(results, 2, "QAbstractButton query");
    PASS();
}

// ---------------------------------------------------------------------------
// 3. Find by text exact match
// ---------------------------------------------------------------------------
static void test_find_by_text_exact()
{
    TEST("find by text exact match");
    Fixture fx;

    auto results = fx.find(fx.query({
        {QStringLiteral("text"), QStringLiteral("OK")}
    }));

    CHECK_SZ(results, 1, "text 'OK'");
    CHECK(results[0].id == 2, "should match btnOk (id=2)");
    PASS();
}

// ---------------------------------------------------------------------------
// 4. Find by text_contains substring
// ---------------------------------------------------------------------------
static void test_find_by_text_contains()
{
    TEST("find by text_contains substring");
    Fixture fx;

    auto results = fx.find(fx.query({
        {QStringLiteral("text_contains"), QStringLiteral("ance")}
    }));

    CHECK_SZ(results, 1, "text_contains 'ance'");
    CHECK(results[0].id == 3, "should match btnCancel (id=3)");
    PASS();
}

// ---------------------------------------------------------------------------
// 5. Find by object_name
// ---------------------------------------------------------------------------
static void test_find_by_object_name()
{
    TEST("find by object_name");
    Fixture fx;

    auto results = fx.find(fx.query({
        {QStringLiteral("object_name"), QStringLiteral("btnOK")}
    }));

    CHECK_SZ(results, 1, "object_name 'btnOK'");
    CHECK(results[0].id == 2, "should match btnOk (id=2)");
    PASS();
}

// ---------------------------------------------------------------------------
// 6. Find by window_title
// ---------------------------------------------------------------------------
static void test_find_by_window_title()
{
    TEST("find by window_title");
    Fixture fx;

    // All descendants of mainWin share window title "TestWindow"
    auto results = fx.find(fx.query({
        {QStringLiteral("window_title"), QStringLiteral("TestWindow")}
    }));

    // All descendants share the window title (QMainWindow has internal children too)
    CHECK(results.size() >= 4, "window_title 'TestWindow' should match 4+ descendants");
    PASS();
}

// ---------------------------------------------------------------------------
// 7. depth=exact (direct children only) vs deep (recursive)
// ---------------------------------------------------------------------------
static void test_depth_exact_vs_deep()
{
    TEST("depth=exact vs deep");

    // ---- 7a. depth=exact from central (ancestor_id=5) -- only direct children
    {
        Fixture fx;
        QJsonObject q;
        q[QStringLiteral("type")]       = QStringLiteral("QPushButton");
        q[QStringLiteral("depth")]      = QStringLiteral("exact");
        q[QStringLiteral("ancestor_id")] = 5.0;

        auto results = fx.find(q);
        CHECK_SZ(results, 2, "depth=exact from central -> 2 buttons (direct children)");
    }

    // ---- 7b. depth=deep (default) from central -- same result (no deeper widgets)
    {
        Fixture fx;
        QJsonObject q;
        q[QStringLiteral("type")]       = QStringLiteral("QPushButton");
        q[QStringLiteral("ancestor_id")] = 5.0;  // no depth -> deep

        auto results = fx.find(q);
        CHECK_SZ(results, 2, "depth=deep (default) from central -> 2 buttons");
    }

    // ---- 7c. depth=exact from mainWin (ancestor_id=1) -> central at depth 1, NOT buttons
    {
        Fixture fx;
        QJsonObject q;
        q[QStringLiteral("object_name")] = QStringLiteral("btnOK");
        q[QStringLiteral("depth")]       = QStringLiteral("exact");
        q[QStringLiteral("ancestor_id")] = 1.0;

        auto results = fx.find(q);
        CHECK_SZ(results, 0, "depth=exact from mainWin -> btnOK is at depth 2, not found");
    }

    PASS();
}

// ---------------------------------------------------------------------------
// 8. Find with limit=N (returns at most N results)
// ---------------------------------------------------------------------------
static void test_find_with_limit()
{
    TEST("find with limit=N");
    Fixture fx;

    QJsonObject q;
    q[QStringLiteral("type")]        = QStringLiteral("QPushButton");
    q[QStringLiteral("limit")]       = 1.0;
    q[QStringLiteral("ancestor_id")] = 1.0;

    auto results = fx.find(q);
    CHECK_SZ(results, 1, "limit=1 should return at most 1 result");
    PASS();
}

// ---------------------------------------------------------------------------
// 9. Find with AND combination of multiple fields
// ---------------------------------------------------------------------------
static void test_find_and_combination()
{
    TEST("find with AND combination of multiple fields");
    Fixture fx;

    auto results = fx.find(fx.query({
        {QStringLiteral("type"),        QStringLiteral("QPushButton")},
        {QStringLiteral("text"),        QStringLiteral("OK")},
        {QStringLiteral("object_name"), QStringLiteral("btnOK")}
    }));

    CHECK_SZ(results, 1, "AND combination should match exactly btnOk");
    CHECK(results[0].id == 2, "should match btnOk (id=2)");
    PASS();
}

// ---------------------------------------------------------------------------
// 10. Find with empty/trivially-true query returns all descendants
// ---------------------------------------------------------------------------
static void test_find_empty_query()
{
    TEST("find with empty query returns all");
    Fixture fx;

    // Empty query plus ancestor_id -> all descendants of mainWin
    QJsonObject q;
    q[QStringLiteral("ancestor_id")] = 1.0;

    auto results = fx.find(q);
    // mainWin + central + btnOk + btnCancel + emailEdit + QMenuBar + QStatusBar
    CHECK_SZ(results, 7, "empty query should return all elements");
    PASS();
}

// ---------------------------------------------------------------------------
// 11. Find with no matches returns empty
// ---------------------------------------------------------------------------
static void test_find_no_matches()
{
    TEST("find with no matches returns empty result");
    Fixture fx;

    auto results = fx.find(fx.query({
        {QStringLiteral("type"), QStringLiteral("NonExistentType")}
    }));

    CHECK_SZ(results, 0, "non-existent type should return empty");
    PASS();
}

// ---------------------------------------------------------------------------
// 12. Find by type_inherits "QWidget" matches all widgets
// ---------------------------------------------------------------------------
static void test_find_type_inherits_all_widgets()
{
    TEST("find by type_inherits 'QWidget' matches all widgets");
    Fixture fx;

    // mainWin, central, btnOk, btnCancel, emailEdit all inherit QWidget
    auto results = fx.find(fx.query({
        {QStringLiteral("type_inherits"), QStringLiteral("QWidget")}
    }));

    CHECK(results.size() >= 4, "type_inherits QWidget matches most elements");
    PASS();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    std::cout << "test_selector\n";
    test_find_by_type();
    test_find_by_type_inherits();
    test_find_by_text_exact();
    test_find_by_text_contains();
    test_find_by_object_name();
    test_find_by_window_title();
    test_depth_exact_vs_deep();
    test_find_with_limit();
    test_find_and_combination();
    test_find_empty_query();
    test_find_no_matches();
    test_find_type_inherits_all_widgets();

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
