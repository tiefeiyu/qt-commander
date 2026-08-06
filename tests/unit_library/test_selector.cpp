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
#include "core/event_injector.h"
#include <QApplication>
#include <QMainWindow>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QAbstractItemView>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMetaMethod>
#include <QListWidget>
#include <QMouseEvent>
#include <QPoint>
#include <QProgressBar>
#include <QSlider>
#include <QVBoxLayout>
#ifdef QT_COMMANDER_WITH_QML
#include <QDir>
#include <QQmlApplicationEngine>
#include <QQuickView>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTemporaryFile>
#include "core/event_injector.h"
#endif
// The QPA input interface: flushed synchronously in tests so injected
// coordinate clicks are delivered before assertions run.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtGui/qpa/qwindowsysteminterface_p.h>
#else
#include <QtGui/qpa/qwindowsysteminterface.h>
#endif
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

        // Show the window so the fixture's widgets count as visible
        // (findElement excludes hidden elements by default).
        mainWin->show();
        QApplication::processEvents();
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
    // mainWin + central + btnOk + btnCancel + emailEdit + QMenuBar +
    // QStatusBar; showing the window can add a few internal widgets, so
    // only assert the count grew past the fixture's own elements.
    CHECK(results.size() >= 7, "empty query should return all visible elements");
    PASS();
}

// ---------------------------------------------------------------------------
// 13. Hidden elements are excluded by default; include_hidden=true restores them
// ---------------------------------------------------------------------------
static void test_find_hidden_excluded_by_default()
{
    TEST("hidden elements excluded by default, include_hidden restores them");
    Fixture fx;

    // ---- 13a. all fixture widgets are visible after show()
    {
        QJsonObject q;
        q[QStringLiteral("type")]        = QStringLiteral("QPushButton");
        q[QStringLiteral("ancestor_id")] = 1.0;
        auto results = fx.find(q);
        CHECK_SZ(results, 2, "both buttons visible -> 2 results");
    }

    // ---- 13b. hide one button -> only the visible one matches by default
    fx.btnCancel->hide();
    QApplication::processEvents();
    {
        QJsonObject q;
        q[QStringLiteral("type")]        = QStringLiteral("QPushButton");
        q[QStringLiteral("ancestor_id")] = 1.0;
        auto results = fx.find(q);
        CHECK_SZ(results, 1, "hidden Cancel excluded by default");
        CHECK(results[0].id == 2, "remaining result is btnOk (id=2)");
    }

    // ---- 13c. include_hidden=true returns both
    {
        QJsonObject q;
        q[QStringLiteral("type")]          = QStringLiteral("QPushButton");
        q[QStringLiteral("ancestor_id")]   = 1.0;
        q[QStringLiteral("include_hidden")] = true;
        auto results = fx.find(q);
        CHECK_SZ(results, 2, "include_hidden=true restores hidden button");
    }

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
// Combo popup: ElementSelector::find must not return the same object twice
// (regression: QComboBoxListView appeared twice in find results while the
// popup was open).
// ---------------------------------------------------------------------------
static void test_combo_popup_no_duplicates()
{
    TEST("find does not return duplicates for a combo popup");

    QMainWindow win;
    auto* combo = new QComboBox(&win);
    combo->addItems({QStringLiteral("A"), QStringLiteral("B"),
                     QStringLiteral("C")});
    win.setCentralWidget(combo);
    win.resize(200, 100);
    win.show();
    combo->showPopup();
    QApplication::processEvents();

    // Build the element map the way the snapshot/findElement path does:
    // every top-level widget becomes a root, all descendants get ids.
    QHash<uint64_t, QObject*> map;
    uint64_t nextId = 1;
    std::function<void(QObject*)> addTree = [&](QObject* obj) {
        map.insert(nextId++, obj);
        const QObjectList& kids = obj->children();
        for (QObject* k : kids)
            addTree(k);
    };
    for (QWidget* w : QApplication::topLevelWidgets())
        addTree(w);

    // include_hidden: this test is about duplicate traversal, not
    // visibility — the popup animates in, so its view may not be
    // visible yet when the query runs.
    QJsonObject query;
    query[QStringLiteral("type_inherits")] = QStringLiteral("QAbstractItemView");
    query[QStringLiteral("include_hidden")] = true;
    const QVector<SelectorResult> results =
        ElementSelector::find(query, map);

    // Same object must not appear twice (dup ids mean double traversal).
    QSet<uint64_t> ids;
    for (const SelectorResult& r : results) {
        CHECK(!ids.contains(r.id),
              "duplicate result id " << r.id << " (object walked twice)");
        ids.insert(r.id);
    }
    CHECK(results.size() >= 1,
          "expected at least one QAbstractItemView (combo popup list)");

    combo->hidePopup();
    win.close();
    PASS();
}

// ---------------------------------------------------------------------------
// Combo popup item click: a mouse press+release on the popup list view must
// select the item under the cursor.  Distinguishes Qt popup behavior from
// event-injection timing issues.
// ---------------------------------------------------------------------------
static void test_combo_popup_click_selects()
{
    TEST("mouse click on popup list selects the item under the cursor");

    QMainWindow win;
    auto* combo = new QComboBox(&win);
    combo->addItems({QStringLiteral("A"), QStringLiteral("B"),
                     QStringLiteral("C")});
    win.setCentralWidget(combo);
    win.resize(200, 100);
    win.show();
    combo->showPopup();
    QApplication::processEvents();

    // The popup animates in (QScrollEffect); give it time to become visible.
    QAbstractItemView* view = nullptr;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2000) {
        for (QWidget* top : QApplication::topLevelWidgets()) {
            view = top->findChild<QAbstractItemView*>();
            if (view && view->isVisible())
                break;
            view = nullptr;
        }
        if (view)
            break;
        QCoreApplication::processEvents();
    }
    CHECK(view != nullptr, "popup list view not found or not visible");

    // Pick a target inside the list; compute the row Qt resolves for it.
    const QPoint target(10, 27);
    const QModelIndex expected = view->indexAt(target);
    CHECK(expected.isValid(), "click target must resolve to an item");
    if (!expected.isValid()) {
        combo->hidePopup();
        win.close();
        return;
    }

    // Qt guards the popup against accidental clicks for
    // doubleClickInterval() ms after showPopup (blockMouseReleaseTimer);
    // an injected click arrives faster than a human one, so wait it out.
    QElapsedTimer waitTimer;
    waitTimer.start();
    while (waitTimer.elapsed() < 500)
        QCoreApplication::processEvents();

    // A real click moves the pointer first (the combo popup container sets
    // the view's currentIndex from MouseMove and only selects the item on
    // MouseButtonRelease), and lands on the viewport -- the deepest child
    // under the cursor -- not on the view widget itself.
    QWidget* vp = view->viewport();
    const QPoint vpPos = vp->mapFrom(view, target);
    const QPoint vpGlobal = vp->mapToGlobal(vpPos);

    QMouseEvent move(QEvent::MouseMove, QPointF(vpPos),
                     QPointF(vpPos), QPointF(vpGlobal),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &move);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(vpPos),
                      QPointF(vpPos), QPointF(vpGlobal),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(vpPos),
                        QPointF(vpPos), QPointF(vpGlobal),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &release);
    QApplication::processEvents();

    CHECK(combo->currentIndex() == expected.row(),
          "expected index " << expected.row() << " after clicking, got "
          << combo->currentIndex());

    combo->hidePopup();
    win.close();
    PASS();
}

// ---------------------------------------------------------------------------
// Ctrl+A in QLineEdit must select all; subsequent typed characters replace
// the selection.  Regression for the injected keyboard path.
// ---------------------------------------------------------------------------
static void test_lineedit_ctrl_a_select_all()
{
    TEST("Ctrl+A selects all and typing replaces the selection");

    QLineEdit edit;
    edit.setText(QStringLiteral("hello world"));
    edit.resize(200, 30);
    edit.show();
    QApplication::processEvents();
    edit.setFocus();
    QApplication::processEvents();

    // Qt6 makes QEvent's copy ctor protected -- post fresh objects instead.
    QCoreApplication::postEvent(&edit, new QKeyEvent(QEvent::KeyPress, Qt::Key_A,
                                                     Qt::ControlModifier,
                                                     QStringLiteral("a")));
    QCoreApplication::postEvent(&edit, new QKeyEvent(QEvent::KeyRelease, Qt::Key_A,
                                                     Qt::ControlModifier,
                                                     QStringLiteral("a")));
    QApplication::processEvents();

    CHECK(edit.selectedText() == QStringLiteral("hello world"),
          "Ctrl+A should select all, got: '"
              << edit.selectedText().toStdString() << "'");

    // A printable key while everything is selected replaces the text.
    QCoreApplication::postEvent(&edit, new QKeyEvent(QEvent::KeyPress, Qt::Key_R,
                                                     Qt::NoModifier,
                                                     QStringLiteral("r")));
    QApplication::processEvents();
    CHECK(edit.text() == QStringLiteral("r"),
          "typing after select-all should replace, got: '"
              << edit.text().toStdString() << "'");

    edit.close();
    PASS();
}

// ---------------------------------------------------------------------------
// Typed method invocation: a manual QGenericArgument("int", ...) must be
// able to invoke a slot with an int parameter (regression for the typed
// callMethod path).
// ---------------------------------------------------------------------------
static void test_callmethod_typed_int()
{
    TEST("typed int invocation of a slot (QProgressBar::setValue)");

    QProgressBar bar;
    bar.setRange(0, 100);
    bar.setValue(10);

    int value = 77;
    const bool ok = QMetaObject::invokeMethod(
        &bar, "setValue", Qt::DirectConnection, Q_ARG(int, value));
    CHECK(ok && bar.value() == 77,
          "typed int invoke failed (ok=" << ok << " value=" << bar.value() << ")");

    PASS();
}

static void test_callmethod_non_slot_not_invokable()
{
    TEST("plain public method is not invokable via QMetaObject");

    // QListWidget::setCurrentRow is a plain public method, not a slot;
    // QMetaObject::invokeMethod must reject it (callMethod reports a clear
    // "no invokable overload" error instead of silently failing).
    QListWidget list;
    list.addItems({QStringLiteral("A"), QStringLiteral("B")});

    int row = 1;
    const bool ok = QMetaObject::invokeMethod(
        &list, "setCurrentRow", Qt::DirectConnection, Q_ARG(int, row));
    CHECK(!ok && list.currentRow() == -1,
          "plain method must not be invokable (ok=" << ok << ")");

    PASS();
}

// ---------------------------------------------------------------------------
// QQmlApplicationEngine with a Window root: the QML scene must be reachable
// from the top-level QQuickWindow's contentItem (regression for QML
// snapshots showing only the root).
// ---------------------------------------------------------------------------
#ifdef QT_COMMANDER_WITH_QML
static void test_qml_window_root_reachable()
{
    TEST("Window-root QML scene reachable from QQuickWindow contentItem");

    QTemporaryFile qml(QDir::tempPath() + QStringLiteral("/qtc_XXXXXX.qml"));
    CHECK(qml.open(), "temp qml file");
    if (!qml.isOpen()) {
        PASS();
        return;
    }
    qml.write("import QtQuick 2.0\n"
              "import QtQuick.Window 2.0\n"
              "Window {\n"
              "    objectName: \"sceneRoot\"\n"
              "    width: 400; height: 300; visible: true\n"
              "    Rectangle { objectName: \"childRect\" }\n"
              "}\n");
    qml.flush();

    QQmlApplicationEngine engine;
    engine.load(QUrl::fromLocalFile(qml.fileName()));
    QApplication::processEvents();

    // The Window root creates a top-level QQuickWindow.
    QQuickWindow* sceneWin = nullptr;
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        if (auto* qw = qobject_cast<QQuickWindow*>(w)) {
            sceneWin = qw;
            break;
        }
    }
    CHECK(sceneWin != nullptr, "no top-level QQuickWindow for Window root");

    // The Window root is the QQuickWindow itself; its QML children live
    // under the window's contentItem.
    bool foundChild = false;
    for (QQuickItem* ci : sceneWin->contentItem()->childItems()) {
        if (ci->objectName() == QStringLiteral("childRect"))
            foundChild = true;
    }
    CHECK(foundChild, "QML children not reachable from window contentItem");

    engine.deleteLater();
    PASS();
}

// ---------------------------------------------------------------------------
// Real coordinate click: QWindowSystemInterface routes the click through the
// QPA input pipeline with the real scene-graph hit test -- clicking the
// center of a plain Rectangle must reach the QQuickMouseArea inside it,
// exactly like a real mouse click.
// ---------------------------------------------------------------------------
static void test_click_at_qml_hit_test()
{
    TEST("clickAt lands on MouseArea via real scene-graph hit test");

    QTemporaryFile qml(QDir::tempPath() + QStringLiteral("/qtc_XXXXXX.qml"));
    CHECK(qml.open(), "temp qml file");
    if (!qml.isOpen()) {
        PASS();
        return;
    }
    qml.write("import QtQuick 2.0\n"
              "Item {\n"
              "    id: root\n"
              "    objectName: \"root\"\n"
              "    property int clickCount: 0\n"
              "    Rectangle {\n"
              "        objectName: \"box\"\n"
              "        width: 100; height: 50\n"
              "        MouseArea {\n"
              "            objectName: \"ma\"\n"
              "            anchors.fill: parent\n"
              "            onClicked: root.clickCount += 1\n"
              "        }\n"
              "    }\n"
              "}\n");
    qml.flush();

    QQuickView view;
    view.setSource(QUrl::fromLocalFile(qml.fileName()));
    view.show();
    QApplication::processEvents();

    QQuickItem* root = view.rootObject();
    CHECK(root != nullptr, "scene root");
    if (!root) {
        view.close();
        return;
    }

    // Box occupies (0,0)-(100,50); its center in window coordinates.
    const bool ok = EventInjector::mouseClickAt(
        &view, 50.0, 25.0, QStringLiteral("left"), QStringList());
    QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
    QApplication::processEvents();

    CHECK(ok, "mouseClickAt returned success");
    CHECK(root->property("clickCount").toInt() == 1,
          "MouseArea inside Rectangle must receive the click, got "
              << root->property("clickCount").toInt());

    view.close();
    PASS();
}

// ---------------------------------------------------------------------------
// Region click: clicking the on-screen region of a container Rectangle must
// land on the MouseArea inside it (real hit testing, not class guessing).
// ---------------------------------------------------------------------------
static void test_click_region_qml()
{
    TEST("clickRegion on container reaches inner MouseArea");

    QTemporaryFile qml(QDir::tempPath() + QStringLiteral("/qtc_XXXXXX.qml"));
    CHECK(qml.open(), "temp qml file");
    if (!qml.isOpen()) {
        PASS();
        return;
    }
    qml.write("import QtQuick 2.0\n"
              "Item {\n"
              "    id: root\n"
              "    objectName: \"root\"\n"
              "    property int clickCount: 0\n"
              "    Rectangle {\n"
              "        objectName: \"box\"\n"
              "        width: 100; height: 50\n"
              "        MouseArea {\n"
              "            objectName: \"ma\"\n"
              "            anchors.fill: parent\n"
              "            onClicked: root.clickCount += 1\n"
              "        }\n"
              "    }\n"
              "}\n");
    qml.flush();

    QQuickView view;
    view.setSource(QUrl::fromLocalFile(qml.fileName()));
    view.show();
    QApplication::processEvents();

    QQuickItem* root = view.rootObject();
    QQuickItem* box = root ? root->findChild<QQuickItem*>(
                                 QStringLiteral("box")) : nullptr;
    CHECK(box != nullptr, "box present");
    if (!box) {
        view.close();
        return;
    }

    const bool ok = EventInjector::mouseClickRegion(
        box, QStringLiteral("left"), QStringList());
    QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
    QApplication::processEvents();

    CHECK(ok, "mouseClickRegion returned success");
    CHECK(root->property("clickCount").toInt() == 1,
          "region click on Rectangle must hit the MouseArea, got "
              << root->property("clickCount").toInt());

    view.close();
    PASS();
}
#endif // QT_COMMANDER_WITH_QML

// ---------------------------------------------------------------------------
// Widget coordinate click: the real QPA pipeline hit tests the widget tree,
// so a click at a button's window coordinates fires its clicked() signal.
// ---------------------------------------------------------------------------
static void test_click_at_widget_hit_test()
{
    TEST("clickAt on a widget button fires clicked() via real hit test");

    QWidget win;
    win.resize(300, 200);
    auto* btn = new QPushButton(QStringLiteral("T"), &win);
    btn->setGeometry(50, 50, 80, 30);
    int clicks = 0;
    QObject::connect(btn, &QPushButton::clicked,
                     [&clicks]() { clicks += 1; });
    win.show();
    QApplication::processEvents();

    CHECK(win.windowHandle() != nullptr, "top-level window handle");
    if (!win.windowHandle()) {
        win.close();
        return;
    }

    // Button center in top-level window coordinates: (90, 65).
    const bool ok = EventInjector::mouseClickAt(
        win.windowHandle(), 90.0, 65.0,
        QStringLiteral("left"), QStringList());
    QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
    QApplication::processEvents();

    CHECK(ok, "mouseClickAt returned success");
    CHECK(clicks == 1, "button must receive the click, got " << clicks);

    win.close();
    PASS();
}

// ---------------------------------------------------------------------------
// Mouse press/release primitives: a press alone must NOT complete a click;
// the release does.  This is the foundation for drags.
// ---------------------------------------------------------------------------
static void test_mouse_press_release_split_click()
{
    TEST("mousePress + mouseRelease on a button = exactly one clicked()");

    QWidget win;
    win.resize(300, 200);
    auto* btn = new QPushButton(QStringLiteral("T"), &win);
    btn->setGeometry(50, 50, 80, 30);
    int clicks = 0;
    QObject::connect(btn, &QPushButton::clicked,
                     [&clicks]() { clicks += 1; });
    win.show();
    QApplication::processEvents();

    const QStringList noMods;
    CHECK(EventInjector::mousePress(btn, QStringLiteral("left"),
                                    -1.0, -1.0, noMods, false),
          "mousePress returned success");
    QApplication::processEvents();
    CHECK(clicks == 0, "press alone must NOT fire clicked(), got " << clicks);

    CHECK(EventInjector::mouseRelease(btn, QStringLiteral("left"),
                                      -1.0, -1.0, noMods, false),
          "mouseRelease returned success");
    QApplication::processEvents();
    CHECK(clicks == 1, "press+release must fire clicked() once, got " << clicks);

    win.close();
    PASS();
}

// ---------------------------------------------------------------------------
// Drag: a press -> move -> release sequence delivers every event with the
// right coordinates to the target widget AND moves the probe (real drag
// semantics: the widget follows the pointer).  A plain QWidget records the
// events (style-independent; QSlider's drag depends on style hit-testing
// of the handle geometry and is intentionally not exercised here).
// ---------------------------------------------------------------------------
class DragProbe : public QWidget {
public:
    explicit DragProbe(QWidget* parent = nullptr) : QWidget(parent) {
        // Without tracking, Qt drops button-less MouseMove events.
        setMouseTracking(true);
    }
    int pressX = -1, moveX = -1, releaseX = -1, moveCount = 0;
protected:
    // QMouseEvent::globalPosition() is Qt6-only; Qt5.15 has globalPos().
    static QPoint eventGlobalPos(const QMouseEvent* e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return e->globalPosition().toPoint();
#else
        return e->globalPos();
#endif
    }
    void mousePressEvent(QMouseEvent* e) override {
        pressX = e->pos().x();
        grab_offset_ = e->pos();
        QWidget::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        moveX = e->pos().x();
        ++moveCount;
        // Follow the pointer while the left button is held.  move() takes
        // parent coordinates: mapFromGlobal gives the pointer in *this*
        // widget's coordinates, so add pos() to translate into the parent's.
        if (e->buttons() & Qt::LeftButton)
            move(mapFromGlobal(eventGlobalPos(e)) + pos() - grab_offset_);
        QWidget::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        releaseX = e->pos().x();
        QWidget::mouseReleaseEvent(e);
    }
private:
    QPoint grab_offset_;
};

static void test_mouse_drag_probe()
{
    TEST("press -> move -> release delivers coordinates AND moves the probe");

    QWidget win;
    win.resize(400, 200);
    auto* probe = new DragProbe(&win);
    probe->setGeometry(50, 50, 220, 60);
    win.show();
    QApplication::processEvents();

    const QStringList noMods;
    // Press at the probe center (hasCoords=false), then drag to the right
    // edge with explicit coordinates, then release there.
    CHECK(EventInjector::mousePress(probe, QStringLiteral("left"),
                                    -1.0, -1.0, noMods, false),
          "drag press ok");
    CHECK(EventInjector::mouseMove(probe, 120.0, 30.0), "move 1 ok");
    CHECK(EventInjector::mouseMove(probe, 200.0, 30.0), "move 2 ok");
    CHECK(EventInjector::mouseRelease(probe, QStringLiteral("left"),
                                      200.0, 30.0, noMods, true),
          "drag release ok");
    QApplication::processEvents();

    const int cx = probe->width() / 2;
    CHECK(probe->pressX == cx,
          "press landed at element center, got " << probe->pressX);
    CHECK(probe->moveCount >= 2,
          "both move events delivered, got " << probe->moveCount);
    CHECK(probe->moveX == 200,
          "last move at target x, got " << probe->moveX);
    CHECK(probe->releaseX == 200,
          "release at target x, got " << probe->releaseX);
    // Visible effect: the probe followed the pointer.  All three events are
    // queued (postEvent) and processed at the final processEvents(), each
    // mapping its own global point through the probe's position at the time
    // it is processed; the probe's top-left ends at (50 + (200-110), 50).
    const int expectedX = 50 + (200 - cx);
    const int expectedY = 50;
    CHECK(probe->x() == expectedX,
          "probe moved with the pointer: x()=" << probe->x()
                                               << " expected " << expectedX);
    CHECK(probe->y() == expectedY,
          "probe followed the pointer vertically: y()=" << probe->y()
                                                        << " expected "
                                                        << expectedY);

    win.close();
    PASS();
}

// ---------------------------------------------------------------------------
// keyCombo: "Ctrl+A" delivers a real press/release pair with modifiers.
// ---------------------------------------------------------------------------
static void test_key_combo_ctrl_a_selects_all()
{
    TEST("keyCombo \"Ctrl+A\" selects all text in a line edit");

    QLineEdit edit;
    edit.setText(QStringLiteral("hello world"));
    edit.resize(200, 30);
    edit.show();
    QApplication::processEvents();
    edit.setFocus();
    QApplication::processEvents();

    CHECK(EventInjector::keyCombo(&edit, QStringLiteral("Ctrl+A")),
          "keyCombo Ctrl+A returned success");
    QApplication::processEvents();

    CHECK(edit.selectedText() == QStringLiteral("hello world"),
          "Ctrl+A must select all, got: '"
              << edit.selectedText().toStdString() << "'");

    edit.close();
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
    test_find_hidden_excluded_by_default();
    test_find_no_matches();
    test_find_type_inherits_all_widgets();
    test_combo_popup_no_duplicates();
    test_combo_popup_click_selects();
    test_lineedit_ctrl_a_select_all();
    test_callmethod_typed_int();
    test_callmethod_non_slot_not_invokable();
    test_click_at_widget_hit_test();
    test_mouse_press_release_split_click();
    test_mouse_drag_probe();
    test_key_combo_ctrl_a_selects_all();
#ifdef QT_COMMANDER_WITH_QML
    test_qml_window_root_reachable();
    test_click_at_qml_hit_test();
    test_click_region_qml();
#endif

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
